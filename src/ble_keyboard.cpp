#include "ble_keyboard.h"
#include "input_handler.h"

#include <NimBLEDevice.h>
#include <NimBLEClient.h>
#include <NimBLERemoteService.h>
#include <NimBLERemoteCharacteristic.h>
#include <NimBLERemoteDescriptor.h>

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
static uint8_t lastReport[8] = {0};

// Global flag to control auto-reconnect behavior
bool autoReconnectEnabled = true;

// Reconnection backoff
static unsigned long reconnectDelay = 5000;
static unsigned long lastReconnectAttempt = 0;
static constexpr unsigned long MAX_RECONNECT_DELAY = 60000;

// Device discovery variables
static std::vector<BleDeviceInfo> discoveredDevices;
static bool isScanning = false;

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

// BLE scan callback - lightweight, NO auto-connect
// Auto-connect only happens for stored devices via bleLoop()
class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    // Nothing to do here - device list is built from getResults() which has merged data
    // This callback exists because NimBLE requires one for scanning
  }
};

// Rebuild discoveredDevices list from NimBLE's internal scan results
// NimBLE merges advertisement + scan response data, so names are available here
static void refreshDeviceListFromScanResults() {
  NimBLEScanResults results = NimBLEDevice::getScan()->getResults();
  int count = results.getCount();

  discoveredDevices.clear();
  for (int i = 0; i < count; i++) {
    NimBLEAdvertisedDevice dev = results.getDevice(i);
    BleDeviceInfo info;
    info.address = dev.getAddress().toString();
    info.name = dev.haveName() ? dev.getName() : info.address;
    info.rssi = dev.getRSSI();
    discoveredDevices.push_back(info);
  }
}

// BLE client callback
class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) override {
    Serial.println("BLE client connected, discovering services...");
    bleState = BLEState::CONNECTING; // Keep as connecting until services are fully set up
    
    // Discover services after connection
    if (pclient->getServices(true)) { // true for doDiscover
      Serial.println("Services discovered successfully");
      
      // Find HID service
      pRemoteService = pclient->getService(hidServiceUUID);
      if (!pRemoteService) {
        Serial.println("HID service not found");
        bleState = BLEState::DISCONNECTED;
        pclient->disconnect();
        return;
      }

      // Find input report characteristic
      pInputReportChar = nullptr;
      std::vector<NimBLERemoteCharacteristic*>* chars = pRemoteService->getCharacteristics();
      for (auto& chr : *chars) {
        if (chr->getUUID() != reportUUID) continue;

        std::vector<NimBLERemoteDescriptor*>* descs = chr->getDescriptors();
        for (auto& desc : *descs) {
          if (desc->getUUID() == NimBLEUUID("2908")) {
            std::string refData = desc->readValue();
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
        pclient->disconnect();
        return;
      }

      // Subscribe to notifications
      if (!pInputReportChar->subscribe(true, onKeyboardNotify)) {
        Serial.println("Failed to subscribe to input reports");
        bleState = BLEState::DISCONNECTED;
        pclient->disconnect();
        return;
      }

      // Set report protocol mode
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
      reconnectDelay = 2000; // Reset backoff on successful connection
    } else {
      Serial.println("Failed to discover services");
      bleState = BLEState::DISCONNECTED;
      pclient->disconnect();
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
};

static bool tryConnect() {
  if (keyboardAddress.empty()) return false;

  bleState = BLEState::CONNECTING;
  Serial.printf("Connecting to: %s (timeout %ds)\n", keyboardAddress.c_str(), CONNECT_TIMEOUT_SEC);

  // Reuse existing client or create new one
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(new ClientCallbacks());
  }
  pClient->setConnectTimeout(CONNECT_TIMEOUT_SEC);

  NimBLEAddress addr(keyboardAddress);
  if (!pClient->connect(addr, true)) {
    Serial.println("Failed to connect");
    bleState = BLEState::DISCONNECTED;
    return false;
  }

  // The service discovery and characteristic setup is now handled in the onConnect callback
  // This allows for asynchronous service discovery after connection
  return true;
}

// Global variable to store the passkey for display
static uint32_t currentPasskey = 0;

// BLE security callback
class BleSecurityCallback : public NimBLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    Serial.println("Passkey request received - keyboard should display it");
    // For "Keyboard Display" pairing, the keyboard generates the PIN
    // We return 0 to indicate that the keyboard will display the PIN
    uint32_t passkey = 0; // The keyboard will generate and display the PIN
    currentPasskey = passkey;
    Serial.printf("Waiting for keyboard to display passkey...\n");
    return passkey;
  }

  void onPassKeyNotify(uint32_t passkey) override {
    Serial.printf("Keyboard displayed passkey: %06lu - please enter this on the X4\n", passkey);
    currentPasskey = passkey; // Store the passkey that the keyboard generated for display
  }

  bool onConfirmPIN(uint32_t passkey) override {
    Serial.printf("Confirming passkey: %06lu\n", passkey);
    currentPasskey = passkey;
    return true; // Automatically confirm - in a real app, you might want user confirmation
  }

  bool onSecurityRequest() override {
    Serial.println("Security Request - accepting");
    return true;
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    Serial.printf("Authentication complete - success: %s\n", desc->sec_state.authenticated ? "true" : "false");
    currentPasskey = 0; // Clear the passkey after authentication
  }
};

// Function to get the current passkey for UI display
uint32_t getCurrentPasskey() {
    return currentPasskey;
}

void bleSetup() {
  NimBLEDevice::init("MicroSlate");
  NimBLEDevice::setSecurityAuth(true, true, true); // Enable bonding, MITM protection
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO); // Set IO capabilities for numeric comparison
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID); // Set key distribution
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID); // Set key distribution
  NimBLEDevice::setSecurityCallbacks(new BleSecurityCallback());
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Configure scan but DON'T start scanning on boot
  // Only auto-reconnect to stored device, or user manually scans in BT Settings
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  scan->setInterval(1349);
  scan->setWindow(449);
  scan->setActiveScan(true);

  // Check if we have a stored device to reconnect to
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName) && !storedAddr.empty()) {
    // Try to reconnect to stored device
    keyboardAddress = storedAddr;
    connectToKeyboard = true;
    Serial.printf("BLE init - will reconnect to stored device: %s\n", storedAddr.c_str());
  } else {
    bleState = BLEState::DISCONNECTED;
    Serial.println("BLE init - no stored device, use Bluetooth Settings to pair");
  }
}

void bleLoop() {
  // Attempt connection if requested
  if (connectToKeyboard && bleState != BLEState::CONNECTED) {
    connectToKeyboard = false;
    tryConnect();
    return;
  }

  // Auto-reconnect to STORED device only (with exponential backoff)
  // Never auto-connect to random devices - user must pair manually first
  if (bleState == BLEState::DISCONNECTED && autoReconnectEnabled) {
    std::string storedAddr, storedName;
    if (getStoredDevice(storedAddr, storedName) && !storedAddr.empty()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= reconnectDelay) {
        lastReconnectAttempt = now;
        keyboardAddress = storedAddr;
        connectToKeyboard = true;
        Serial.printf("Auto-reconnecting to stored device: %s (next retry in %lums)\n",
                      storedAddr.c_str(), reconnectDelay);

        reconnectDelay = (reconnectDelay * 2 > MAX_RECONNECT_DELAY)
                           ? MAX_RECONNECT_DELAY : reconnectDelay * 2;
      }
    }
    // If no stored device: do nothing. User must go to BT Settings to pair.
  }
}

bool isKeyboardConnected() {
  return bleState == BLEState::CONNECTED;
}

BLEState getConnectionState() {
  return bleState;
}

void startDeviceScan() {
  // Stop any existing scan
  NimBLEDevice::getScan()->stop();

  // Clear previous discoveries
  discoveredDevices.clear();
  NimBLEDevice::getScan()->clearResults();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);

  scan->start(15, false);  // 15 second non-blocking scan
  isScanning = true;
  Serial.println("Started BLE device scan");
}

void stopDeviceScan() {
  NimBLEDevice::getScan()->stop();
  refreshDeviceListFromScanResults();
  isScanning = false;
}

int getDiscoveredDeviceCount() {
  if (isScanning) {
    if (NimBLEDevice::getScan()->isScanning()) {
      // Scan still running - refresh with latest merged results
      refreshDeviceListFromScanResults();
    } else {
      // Scan just finished naturally - get final results and mark done
      refreshDeviceListFromScanResults();
      isScanning = false;
    }
  }
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

  // Stop scanning first
  if (isScanning) {
    stopDeviceScan();
  }

  // Disconnect current connection if any
  if (pClient && pClient->isConnected()) {
    pClient->disconnect();
  }

  keyboardAddress = discoveredDevices[deviceIndex].address;
  connectToKeyboard = true;

  // Store this device as the paired device
  storePairedDevice(discoveredDevices[deviceIndex].address, discoveredDevices[deviceIndex].name);

  Serial.printf("Connecting to: %s (%s)\n",
                keyboardAddress.c_str(),
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

// Storage for paired device (in-memory, lost on reboot)
static std::string storedDeviceAddress = "";
static std::string storedDeviceName = "";

void storePairedDevice(const std::string& address, const std::string& name) {
  storedDeviceAddress = address;
  storedDeviceName = name;
  Serial.printf("Stored paired device: %s (%s)\n", address.c_str(), name.c_str());
}

bool getStoredDevice(std::string& address, std::string& name) {
  if (!storedDeviceAddress.empty()) {
    address = storedDeviceAddress;
    name = storedDeviceName.empty() ? storedDeviceAddress : storedDeviceName;
    return true;
  }
  return false;
}

void clearStoredDevice() {
  storedDeviceAddress = "";
  storedDeviceName = "";
  Serial.println("Cleared stored device");
}
