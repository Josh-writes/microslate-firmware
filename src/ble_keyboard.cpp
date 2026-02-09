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
static uint8_t keyboardAddressType = 0;
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
static bool continuousScanning = false;
static uint32_t scanStartMs = 0;
static constexpr uint32_t DEVICE_STALE_MS = 20000;

// FreeRTOS connect task
static TaskHandle_t connectTaskHandle = nullptr;
static volatile bool authSuccess = false;

// Connection timeout in seconds
static constexpr uint8_t CONNECT_TIMEOUT_SEC = 10;

// Global variable to store the passkey for display
static uint32_t currentPasskey = 0;

// Forward declarations
static bool setupHidConnection();

// Helper: upsert device into discovered list
static void upsertDevice(const BleDeviceInfo& info) {
  for (auto &d : discoveredDevices) {
    if (d.address == info.address) {
      d = info;
      return;
    }
  }
  discoveredDevices.push_back(info);
}

// Helper: prune devices not seen recently
static void pruneStaleDevices() {
  uint32_t now = millis();
  for (int i = (int)discoveredDevices.size() - 1; i >= 0; --i) {
    if (now - discoveredDevices[i].lastSeenMs > DEVICE_STALE_MS) {
      discoveredDevices.erase(discoveredDevices.begin() + i);
    }
  }
}

// Keyboard notification callback
static void onKeyboardNotify(NimBLERemoteCharacteristic* pRemChar,
                              uint8_t* pData, size_t length, bool isNotify) {
  if (length < 8) return;

  uint8_t modifiers = pData[0];
  uint8_t newReport[8];
  memcpy(newReport, pData, 8);

  Serial.print("KB: ");
  for (int i = 0; i < 8; i++) Serial.printf("%02X ", pData[i]);
  Serial.println();

  // Detect newly pressed keys
  for (int i = 2; i < 8; i++) {
    if (newReport[i] == 0) continue;
    bool wasPressed = false;
    for (int j = 2; j < 8; j++) {
      if (lastReport[j] == newReport[i]) { wasPressed = true; break; }
    }
    if (!wasPressed) enqueueKeyEvent(newReport[i], modifiers, true);
  }

  // Detect released keys
  for (int i = 2; i < 8; i++) {
    if (lastReport[i] == 0) continue;
    bool stillPressed = false;
    for (int j = 2; j < 8; j++) {
      if (newReport[j] == lastReport[i]) { stillPressed = true; break; }
    }
    if (!stillPressed) enqueueKeyEvent(lastReport[i], modifiers, false);
  }

  memcpy(lastReport, newReport, 8);
}

// --- Callbacks (static instances, no heap allocation) ---

static class ScanCallbacks : public NimBLEAdvertisedDeviceCallbacks {
  void onResult(NimBLEAdvertisedDevice* dev) override {
    BleDeviceInfo info;
    info.address = dev->getAddress().toString();
    info.name = dev->haveName() ? dev->getName() : info.address;
    info.rssi = dev->getRSSI();
    info.addressType = dev->getAddress().getType();
    info.lastSeenMs = millis();
    upsertDevice(info);
  }
} scanCallbacks;

static class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) override {
    Serial.println("[BLE] Connected to device");
    // Don't call secureConnection() here - the connect task handles it
  }

  void onDisconnect(NimBLEClient* pclient) override {
    bleState = BLEState::DISCONNECTED;
    pInputReportChar = nullptr;
    pRemoteService = nullptr;
    authSuccess = false;
    memset(lastReport, 0, 8);
    lastReconnectAttempt = millis();
    Serial.println("[BLE] Disconnected");
  }

  bool onConnParamsUpdateRequest(NimBLEClient* pClient,
                                  const ble_gap_upd_params* params) override {
    pClient->updateConnParams(params->itvl_min, params->itvl_max,
                               params->latency, params->supervision_timeout);
    return true;
  }
} clientCallbacks;

static class BleSecurityCallback : public NimBLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    // For DISPLAY_ONLY, we should typically *display* the passkey we are notified of,
    // not generate one here. If this gets called, log it for debugging.
    Serial.println("[BLE] PassKeyRequest received (unexpected for DISPLAY_ONLY)");
    return 0;
  }

  void onPassKeyNotify(uint32_t passkey) override {
    currentPasskey = passkey;
    Serial.printf("[BLE] Passkey notify: %06lu\n", passkey);
  }

  bool onConfirmPIN(uint32_t passkey) override {
    Serial.printf("[BLE] Confirm PIN: %06lu\n", passkey);
    currentPasskey = passkey;
    return true;
  }

  bool onSecurityRequest() override {
    return true;
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    Serial.printf("[BLE] Auth complete: status enc=%d bond=%d auth=%d\n",
                  desc->sec_state.encrypted,
                  desc->sec_state.bonded,
                  desc->sec_state.authenticated);

    if (desc->sec_state.encrypted) {
      authSuccess = true;
      Serial.println("[BLE] Auth success");
    } else {
      authSuccess = false;
      Serial.println("[BLE] Auth failed - not encrypted");
    }
    currentPasskey = 0;
  }
} securityCallback;

// --- HID service discovery and subscription ---

static bool setupHidConnection() {
  if (!pClient || !pClient->isConnected()) return false;

  Serial.println("[BLE] Discovering services...");
  if (!pClient->getServices(true)) {
    Serial.println("[BLE] Service discovery failed");
    return false;
  }

  pRemoteService = pClient->getService(hidServiceUUID);
  if (!pRemoteService) {
    Serial.println("[BLE] HID service not found");
    return false;
  }

  // Find input report via Report Reference descriptor (type=1 means Input)
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
    Serial.println("[BLE] No input report found");
    return false;
  }

  if (!pInputReportChar->subscribe(true, onKeyboardNotify)) {
    Serial.println("[BLE] Subscribe failed");
    return false;
  }

  // Set report protocol mode
  NimBLERemoteCharacteristic* pProto = pRemoteService->getCharacteristic(protocolModeUUID);
  if (pProto) {
    uint8_t mode = 1;
    pProto->writeValue(&mode, 1, true);
  }

  Serial.println("[BLE] HID setup complete");
  return true;
}

// --- FreeRTOS task: runs connect + security + HID setup off the main loop ---

static void bleConnectTask(void* param) {
  bleState = BLEState::CONNECTING;
  authSuccess = false;

  Serial.printf("[BLE-Task] Connecting to %s type=%d\n",
                keyboardAddress.c_str(), keyboardAddressType);

  // Create/reuse client
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks, false);
  }
  pClient->setConnectTimeout(CONNECT_TIMEOUT_SEC);

  // Step 1: Connect (blocks this task, main loop continues)
  NimBLEAddress addr(keyboardAddress, keyboardAddressType);
  if (!pClient->connect(addr, true)) {
    Serial.println("[BLE-Task] Connection failed");
    bleState = BLEState::DISCONNECTED;
    connectTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[BLE-Task] Connected, starting security...");

  // Step 2: Security (blocks this task while passkey exchange happens)
  // Main loop stays responsive and can display the passkey on screen
  if (!pClient->secureConnection()) {
    Serial.println("[BLE-Task] secureConnection() failed");
    // Give a moment for onAuthenticationComplete to fire
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  // Step 3: Check auth result
  if (!authSuccess) {
    Serial.println("[BLE-Task] Auth not successful, disconnecting");
    if (pClient->isConnected()) pClient->disconnect();
    bleState = BLEState::DISCONNECTED;
    connectTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  Serial.println("[BLE-Task] Auth OK, setting up HID...");

  // Step 4: Service discovery + HID subscription (blocks this task)
  if (!setupHidConnection()) {
    Serial.println("[BLE-Task] HID setup failed, disconnecting");
    if (pClient->isConnected()) pClient->disconnect();
    bleState = BLEState::DISCONNECTED;
    connectTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  // Step 5: Store device and mark connected
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

  Serial.println("[BLE-Task] Keyboard ready!");
  bleState = BLEState::CONNECTED;
  reconnectDelay = 2000;

  connectTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// Launch the connect task (non-blocking from main loop's perspective)
static void startConnectTask() {
  if (connectTaskHandle != nullptr) {
    Serial.println("[BLE] Connect task already running");
    return;
  }
  xTaskCreate(bleConnectTask, "ble_conn", 8192, NULL, 1, &connectTaskHandle);
}

// --- Public API ---

uint32_t getCurrentPasskey() {
  return currentPasskey;
}

void bleSetup() {
  NimBLEDevice::init("MicroSlate");
  // bonding=true, MITM=true, SC=true (modern keyboards prefer Secure Connections)
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityCallbacks(&securityCallback);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  prefs.begin("ble_kb", false);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  scan->setInterval(1349);
  scan->setWindow(449);
  scan->setActiveScan(true);

  // Check for stored device to auto-reconnect
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName) && !storedAddr.empty()) {
    keyboardAddress = storedAddr;
    keyboardAddressType = prefs.getUChar("addrType", 0);
    connectToKeyboard = true;
    Serial.printf("[BLE] Will reconnect to: %s type=%d\n", storedAddr.c_str(), keyboardAddressType);
  } else {
    bleState = BLEState::DISCONNECTED;
    Serial.println("[BLE] No stored device");
  }
}

void bleLoop() {
  // Auto-restart scan if expired and continuous mode active
  if (isScanning && continuousScanning && !NimBLEDevice::getScan()->isScanning()) {
    NimBLEDevice::getScan()->clearResults();
    NimBLEDevice::getScan()->start(15, false);
    scanStartMs = millis();
  }

  // Sync isScanning with actual NimBLE state
  if (isScanning && !continuousScanning && !NimBLEDevice::getScan()->isScanning()) {
    isScanning = false;
  }

  // Launch connect task if requested (non-blocking)
  if (connectToKeyboard && bleState != BLEState::CONNECTED && connectTaskHandle == nullptr) {
    connectToKeyboard = false;
    startConnectTask();
    return;
  }

  // Auto-reconnect to stored device (exponential backoff)
  if (bleState == BLEState::DISCONNECTED && autoReconnectEnabled && connectTaskHandle == nullptr) {
    std::string storedAddr, storedName;
    if (getStoredDevice(storedAddr, storedName) && !storedAddr.empty()) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= reconnectDelay) {
        lastReconnectAttempt = now;
        keyboardAddress = storedAddr;
        keyboardAddressType = prefs.getUChar("addrType", 0);
        connectToKeyboard = true;
        Serial.printf("[BLE] Auto-reconnect: %s (retry in %lums)\n",
                      storedAddr.c_str(), reconnectDelay);
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
  if (connectTaskHandle != nullptr) {
    // Can't safely kill the task mid-connection, but prevent new attempts
    Serial.println("[BLE] Connection in progress, will complete in background");
  }
  if (bleState == BLEState::CONNECTING && connectTaskHandle == nullptr) {
    bleState = BLEState::DISCONNECTED;
  }
}

void startDeviceScan() {
  cancelPendingConnection();

  NimBLEDevice::getScan()->stop();
  discoveredDevices.clear();
  NimBLEDevice::getScan()->clearResults();

  scanStartMs = millis();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanCallbacks, true);
  scan->setActiveScan(true);
  scan->start(15, false);

  isScanning = true;
  continuousScanning = true;
  Serial.println("[BLE] Started continuous scan");
}

void stopDeviceScan() {
  NimBLEDevice::getScan()->stop();
  isScanning = false;
  continuousScanning = false;
}

int getDiscoveredDeviceCount() {
  pruneStaleDevices();
  return discoveredDevices.size();
}

BleDeviceInfo* getDiscoveredDevices() {
  return discoveredDevices.empty() ? nullptr : discoveredDevices.data();
}

void connectToDevice(int deviceIndex) {
  if (deviceIndex < 0 || deviceIndex >= (int)discoveredDevices.size()) {
    Serial.println("[BLE] Invalid device index");
    return;
  }

  stopDeviceScan();

  if (pClient && pClient->isConnected()) {
    pClient->disconnect();
  }

  keyboardAddress = discoveredDevices[deviceIndex].address;
  keyboardAddressType = discoveredDevices[deviceIndex].addressType;
  connectToKeyboard = true;

  Serial.printf("[BLE] Will connect to: %s type=%d (%s)\n",
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
  Serial.printf("[BLE] Stored to NVS: %s (%s) type=%d\n",
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
  Serial.println("[BLE] Cleared stored device from NVS");
  NimBLEDevice::deleteAllBonds();
  Serial.println("[BLE] Cleared all stored bonds");
}
