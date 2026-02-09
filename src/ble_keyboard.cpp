#include "ble_keyboard.h"
#include "input_handler.h"

#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include <NimBLERemoteService.h>
#include <NimBLERemoteCharacteristic.h>
#include <NimBLERemoteDescriptor.h>
#include <Preferences.h>

// HID service and characteristic UUIDs
static NimBLEUUID hidServiceUUID("1812");
static NimBLEUUID reportUUID("2a4d");
static NimBLEUUID reportMapUUID("2a4b");
static NimBLEUUID protocolModeUUID("2a4e");
static NimBLEUUID bootKeyboardInUUID("2a22");

// Module state
static NimBLEClient* pClient = nullptr;
static NimBLERemoteService* pRemoteService = nullptr;
static NimBLERemoteCharacteristic* pInputReportChar = nullptr;

static BLEState bleState = BLEState::DISCONNECTED;
static bool connectToKeyboard = false;
static std::string keyboardAddress = "";
static uint8_t keyboardAddressType = 0; // Track address type (public/random)
static uint8_t lastReport[8] = {0};

// NVS storage for persistent pairing
static Preferences prefs;

// Global flag to control auto-reconnect behavior
bool autoReconnectEnabled = true;

// Reconnection backoff
static unsigned long reconnectDelay = 5000;
static unsigned long lastReconnectAttempt = 0;
static constexpr unsigned long MAX_RECONNECT_DELAY = 60000;

// Device discovery variables
static std::vector<BleDeviceInfo> discoveredDevices;
static bool isScanning = false;
static bool continuousScanning = false; // Keep restarting scan while in BT settings
static uint32_t scanStartMs = 0;
static constexpr uint32_t DEVICE_STALE_MS = 20000; // 20s stale timeout (generous)

// NEW: helper function to upsert device
static void upsertDevice(const BleDeviceInfo& info) {
  for (auto &d : discoveredDevices) {
    if (d.address == info.address) { 
      d = info; 
      return; 
    }
  }
  discoveredDevices.push_back(info);
}

// NEW: function to prune stale devices
static void pruneStaleDevices() {
  uint32_t now = millis();
  for (int i = (int)discoveredDevices.size() - 1; i >= 0; --i) {
    if (now - discoveredDevices[i].lastSeenMs > DEVICE_STALE_MS) {
      discoveredDevices.erase(discoveredDevices.begin() + i);
    }
  }
}

// Connection timeout in seconds
static constexpr uint8_t CONNECT_TIMEOUT_SEC = 5;

// Keyboard notification callback
static void onKeyboardNotify(NimBLERemoteCharacteristic* pRemChar,
                              uint8_t* pData, size_t length, bool isNotify) {
  if (length < 8) return;

  uint8_t modifiers = pData[0];
  uint8_t newReport[8];
  memcpy(newReport, pData, 8);

  // Debug logging for connection troubleshooting
  Serial.print("Keyboard report: ");
  for (int i = 0; i < 8; i++) {
    Serial.printf("%02X ", pData[i]);
  }
  Serial.println();

  // Detect newly pressed keys
  for (int i = 2; i < 8; i++) {
    if (newReport[i] == 0) continue;
    bool wasPressed = false;
    for (int j = 2; j < 8; j++) {
      if (lastReport[j] == newReport[i]) {
        wasPressed = true;
        break;
      }
    }
    if (!wasPressed) {
      enqueueKeyEvent(newReport[i], modifiers, true);
    }
  }

  // Detect released keys
  for (int i = 2; i < 8; i++) {
    if (lastReport[i] == 0) continue;
    bool stillPressed = false;
    for (int j = 2; j < 8; j++) {
      if (newReport[j] == lastReport[i]) {
        stillPressed = true;
        break;
      }
    }
    if (!stillPressed) {
      enqueueKeyEvent(lastReport[i], modifiers, false);
    }
  }

  memcpy(lastReport, newReport, 8);
}

// Static callback instances to avoid memory leaks
static class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    Serial.println("[BLE] onResult()");
    BleDeviceInfo info;
    info.address = dev->getAddress().toString();
    info.name = dev->haveName() ? dev->getName() : info.address;
    info.rssi = dev->getRSSI();
    info.addressType = dev->getAddress().getType(); // Preserve address type
    info.lastSeenMs = millis();
    upsertDevice(info);
  }
} scanCallbacks;

// Static client callback - no heap allocation
static class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) override {
    Serial.println("BLE client connected, requesting security...");
    bleState = BLEState::CONNECTING;
    // Call secureConnection() here in the NimBLE task context.
    // This keeps the main loop free to display the passkey on screen.
    // NimBLE's SMP runs through the controller, not the host event loop,
    // so this does not deadlock.
    if (!pclient->secureConnection()) {
      Serial.println("secureConnection() returned false");
      // Don't disconnect here - onAuthenticationComplete will handle it
    }
  }

  void onDisconnect(NimBLEClient* pclient) override {
    bleState = BLEState::DISCONNECTED;
    pInputReportChar = nullptr;
    pRemoteService = nullptr;
    memset(lastReport, 0, 8);
    lastReconnectAttempt = millis();
    Serial.println("BLE keyboard disconnected");
  }

  bool onConnParamsUpdateRequest(NimBLEClient* pClient,
                                  const ble_gap_upd_params* params) override {
    pClient->updateConnParams(params->itvl_min, params->itvl_max,
                               params->latency, params->supervision_timeout);
    return true;
  }
} clientCallbacks;

static bool tryConnect() {
  if (keyboardAddress.empty()) return false;

  bleState = BLEState::CONNECTING;
  Serial.printf("Connecting to: %s type=%d (timeout %ds)\n",
                keyboardAddress.c_str(), keyboardAddressType, CONNECT_TIMEOUT_SEC);

  // Reuse existing client or create new one
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks, false);
  }
  pClient->setConnectTimeout(CONNECT_TIMEOUT_SEC);

  // Use the correct address type (public vs random)
  NimBLEAddress addr(keyboardAddress, keyboardAddressType);
  if (!pClient->connect(addr, true)) {
    Serial.println("Failed to connect");
    bleState = BLEState::DISCONNECTED;
    return false;
  }

  // Service discovery is handled in onAuthenticationComplete callback
  return true;
}

// Global variable to store the passkey for display
static uint32_t currentPasskey = 0;

// Static security callback
static class BleSecurityCallback : public NimBLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    uint32_t passkey = (uint32_t)(random(100000, 999999));
    currentPasskey = passkey;
    Serial.printf("Display passkey: %06lu (type on keyboard)\n", passkey);
    return passkey;
  }

  void onPassKeyNotify(uint32_t passkey) override {
    currentPasskey = passkey;
    Serial.printf("Passkey notify: %06lu\n", passkey);
  }

  bool onConfirmPIN(uint32_t passkey) override {
    Serial.printf("Confirm PIN: %06lu\n", passkey);
    currentPasskey = passkey;
    return true;
  }

  bool onSecurityRequest() override {
    Serial.println("Security request accepted");
    return true;
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    Serial.printf("Auth complete: encrypted=%d bonded=%d authenticated=%d\n",
                  desc->sec_state.encrypted,
                  desc->sec_state.bonded,
                  desc->sec_state.authenticated);

    // Accept connection if encrypted (bonded is desirable but not required for first connect)
    if (desc->sec_state.encrypted) {
      Serial.println("Authentication successful, discovering services...");

      if (pClient->getServices(true)) {
        Serial.println("Services discovered successfully");

        pRemoteService = pClient->getService(hidServiceUUID);
        if (!pRemoteService) {
          Serial.println("HID service not found");
          bleState = BLEState::DISCONNECTED;
          pClient->disconnect();
          return;
        }

        // Find input report characteristic via Report Reference descriptor
        pInputReportChar = nullptr;
        std::vector<NimBLERemoteCharacteristic*>* chars = pRemoteService->getCharacteristics();
        for (auto& chr : *chars) {
          if (chr->getUUID() != reportUUID) continue;

          std::vector<NimBLERemoteDescriptor*>* descs = chr->getDescriptors();
          for (auto& d : *descs) {
            if (d->getUUID() == NimBLEUUID("2908")) {
              std::string refData = d->readValue();
              if (refData.length() >= 2 && (uint8_t)refData[1] == 1) {
                pInputReportChar = chr;
                break;
              }
            }
          }
          if (pInputReportChar) break;
        }

        // Fallback: any report char with notify
        if (!pInputReportChar) {
          for (auto& chr : *chars) {
            if (chr->getUUID() == reportUUID && chr->canNotify()) {
              pInputReportChar = chr;
              break;
            }
          }
        }

        // Fallback: boot keyboard input
        if (!pInputReportChar) {
          pInputReportChar = pRemoteService->getCharacteristic(bootKeyboardInUUID);
        }

        if (!pInputReportChar) {
          Serial.println("No input report found");
          bleState = BLEState::DISCONNECTED;
          pClient->disconnect();
          return;
        }

        if (!pInputReportChar->subscribe(true, onKeyboardNotify)) {
          Serial.println("Failed to subscribe to input reports");
          bleState = BLEState::DISCONNECTED;
          pClient->disconnect();
          return;
        }

        NimBLERemoteCharacteristic* pProto = pRemoteService->getCharacteristic(protocolModeUUID);
        if (pProto) {
          uint8_t mode = 1;
          pProto->writeValue(&mode, 1, true);
        }

        // Store this device for future auto-reconnection
        std::string storedAddr, storedName;
        if (!getStoredDevice(storedAddr, storedName) || storedAddr != keyboardAddress) {
          std::string devName = keyboardAddress;
          for (auto& d : discoveredDevices) {
            if (d.address == keyboardAddress) {
              devName = d.name;
              break;
            }
          }
          storePairedDevice(keyboardAddress, devName);
        }

        Serial.println("Keyboard ready - receiving input");
        bleState = BLEState::CONNECTED;
        reconnectDelay = 2000;
      } else {
        Serial.println("Failed to discover services after authentication");
        bleState = BLEState::DISCONNECTED;
        pClient->disconnect();
      }
    } else {
      Serial.println("Authentication failed - not encrypted");
      bleState = BLEState::DISCONNECTED;
      pClient->disconnect();
    }

    currentPasskey = 0;
  }
} securityCallback;

// Function to get the current passkey for UI display
uint32_t getCurrentPasskey() {
    return currentPasskey;
}

void bleSetup() {
  NimBLEDevice::init("MicroSlate");
  // bonding=true, MITM=true, SC=false (allow legacy pairing for keyboard compat)
  NimBLEDevice::setSecurityAuth(true, true, false);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityCallbacks(&securityCallback);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Open NVS for persistent device storage
  prefs.begin("ble_kb", false);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  scan->setInterval(1349);
  scan->setWindow(449);
  scan->setActiveScan(true);

  // Check if we have a stored device to reconnect to
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName) && !storedAddr.empty()) {
    keyboardAddress = storedAddr;
    // Restore address type from NVS
    keyboardAddressType = prefs.getUChar("addrType", 0);
    connectToKeyboard = true;
    Serial.printf("BLE init - will reconnect to stored device: %s (type=%d)\n",
                  storedAddr.c_str(), keyboardAddressType);
  } else {
    bleState = BLEState::DISCONNECTED;
    Serial.println("BLE init - no stored device, use Bluetooth Settings to pair");
  }
}

void bleLoop() {
  // Auto-restart scan if it expired and continuous scanning is active
  if (isScanning && continuousScanning && !NimBLEDevice::getScan()->isScanning()) {
    Serial.println("[BLE] Scan expired, restarting...");
    NimBLEDevice::getScan()->clearResults();
    NimBLEDevice::getScan()->start(15, false);
    scanStartMs = millis();
  }

  // Sync isScanning flag with actual NimBLE state
  if (isScanning && !continuousScanning && !NimBLEDevice::getScan()->isScanning()) {
    isScanning = false;
  }

  // Attempt connection if requested
  if (connectToKeyboard && bleState != BLEState::CONNECTED) {
    connectToKeyboard = false;
    tryConnect();
    return;
  }

  // Auto-reconnect to STORED device only (with exponential backoff)
  if (bleState == BLEState::DISCONNECTED && autoReconnectEnabled) {
    std::string storedAddr, storedName;
    if (getStoredDevice(storedAddr, storedName) && !storedAddr.empty()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= reconnectDelay) {
        lastReconnectAttempt = now;
        keyboardAddress = storedAddr;
        keyboardAddressType = prefs.getUChar("addrType", 0);
        connectToKeyboard = true;
        Serial.printf("Auto-reconnecting to stored device: %s type=%d (next retry in %lums)\n",
                      storedAddr.c_str(), keyboardAddressType, reconnectDelay);

        reconnectDelay = (reconnectDelay * 2 > MAX_RECONNECT_DELAY)
                           ? MAX_RECONNECT_DELAY : reconnectDelay * 2;
      }
    }
  }
}

bool isKeyboardConnected() {
  return bleState == BLEState::CONNECTED;
}

BLEState getConnectionState() {
  return bleState;
}

void cancelPendingConnection() {
  connectToKeyboard = false;
  if (pClient && pClient->isConnected() && bleState == BLEState::CONNECTING) {
    pClient->disconnect();
  }
  if (bleState == BLEState::CONNECTING) {
    bleState = BLEState::DISCONNECTED;
  }
}

void startDeviceScan() {
  // Cancel any pending connection that would block the scan
  cancelPendingConnection();

  NimBLEDevice::getScan()->stop();
  discoveredDevices.clear();
  NimBLEDevice::getScan()->clearResults();

  scanStartMs = millis();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  scan->setActiveScan(true);

  scan->start(15, false);  // 15 second non-blocking scan, auto-restarted by bleLoop()
  isScanning = true;
  continuousScanning = true;
  Serial.println("[BLE] Started continuous BLE device scan");
}

void stopDeviceScan() {
  NimBLEDevice::getScan()->stop();
  isScanning = false;
  continuousScanning = false;
}

int getDiscoveredDeviceCount() {
  // Prune stale devices before returning count
  pruneStaleDevices();
  return discoveredDevices.size();
}

BleDeviceInfo* getDiscoveredDevices() {
  if (discoveredDevices.empty()) {
    return nullptr;
  }
  return discoveredDevices.data();
}

void connectToDevice(int deviceIndex) {
  if (deviceIndex < 0 || deviceIndex >= (int)discoveredDevices.size()) {
    Serial.println("Invalid device index");
    return;
  }

  // Stop continuous scanning before connecting
  stopDeviceScan();

  if (pClient && pClient->isConnected()) {
    pClient->disconnect();
  }

  keyboardAddress = discoveredDevices[deviceIndex].address;
  keyboardAddressType = discoveredDevices[deviceIndex].addressType;
  connectToKeyboard = true;

  Serial.printf("Connecting to: %s type=%d (%s)\n",
                keyboardAddress.c_str(), keyboardAddressType,
                discoveredDevices[deviceIndex].name.c_str());
}

void disconnectCurrentDevice() {
  if (pClient && pClient->isConnected()) {
    pClient->disconnect();
  }

  bleState = BLEState::DISCONNECTED;
  pInputReportChar = nullptr;
  pRemoteService = nullptr;
  memset(lastReport, 0, 8);
  lastReconnectAttempt = millis();
  keyboardAddress = "";
}

std::string getCurrentDeviceAddress() {
  return keyboardAddress;
}

void storePairedDevice(const std::string& address, const std::string& name) {
  prefs.putString("addr", address.c_str());
  prefs.putString("name", name.c_str());
  prefs.putUChar("addrType", keyboardAddressType);
  Serial.printf("Stored paired device to NVS: %s (%s) type=%d\n",
                address.c_str(), name.c_str(), keyboardAddressType);
}

bool getStoredDevice(std::string& address, std::string& name) {
  String addr = prefs.getString("addr", "");
  if (addr.length() > 0) {
    address = addr.c_str();
    String n = prefs.getString("name", "");
    name = (n.length() > 0) ? n.c_str() : address;
    return true;
  }
  return false;
}

bool isDeviceScanning() {
  return isScanning;
}

uint32_t getScanAgeMs() {
  return isScanning ? (millis() - scanStartMs) : 0;
}

void refreshScanNow() {
  stopDeviceScan();
  discoveredDevices.clear();
  NimBLEDevice::getScan()->clearResults();
  startDeviceScan();
}

void clearAllBluetoothBonds() {
  NimBLEDevice::deleteAllBonds();
  clearStoredDevice();
  Serial.println("[BLE] Deleted all bonds + cleared stored device");
}

void clearStoredDevice() {
  prefs.remove("addr");
  prefs.remove("name");
  prefs.remove("addrType");
  Serial.println("Cleared stored device from NVS");

  NimBLEDevice::deleteAllBonds();
  Serial.println("Cleared all stored bonds");
}
