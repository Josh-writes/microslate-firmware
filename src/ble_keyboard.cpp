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
static unsigned long reconnectDelay = 10000;  // Start at 10s (was 5s) — less aggressive
static unsigned long lastReconnectAttempt = 0;
static constexpr unsigned long MAX_RECONNECT_DELAY = 120000;  // Cap at 2min (was 60s)

// BLE connection parameters (in 1.25ms units)
// Active typing: 30-50ms interval — responsive enough for keystroke delivery
static constexpr uint16_t CONN_INTERVAL_ACTIVE_MIN = 24;   // 30ms
static constexpr uint16_t CONN_INTERVAL_ACTIVE_MAX = 40;   // 50ms
// Idle: 100-200ms interval — radio mostly sleeps between events
static constexpr uint16_t CONN_INTERVAL_IDLE_MIN   = 80;   // 100ms
static constexpr uint16_t CONN_INTERVAL_IDLE_MAX   = 160;  // 200ms
static constexpr uint16_t CONN_SLAVE_LATENCY       = 4;    // Keyboard can skip 4 events
static constexpr uint16_t CONN_SUPERVISION_TIMEOUT  = 400;  // 4s (10ms units)
static constexpr unsigned long BLE_IDLE_SWITCH_MS   = 3000; // Switch to idle params after 3s no keystrokes

// Activity tracking for adaptive connection parameters
static unsigned long lastBleKeystrokeMs = 0;
static bool bleConnIdleMode = false;

// Device discovery variables
static std::vector<BleDeviceInfo> discoveredDevices;
static bool isScanning = false;
static bool continuousScanning = false;
static uint32_t scanStartMs = 0;
static constexpr uint32_t DEVICE_STALE_MS = 10000;  // 10 seconds - reduced to prevent UI slowdown

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
  // Keyboard reports can be 7 or 8 bytes
  // 8-byte: [Modifiers] [Reserved] [Key1-Key6]  (standard)
  // 7-byte: [Modifiers] [Key1-Key6]              (compact, used by Keys-To-Go 2)

  if (length != 7 && length != 8) {
    DBG_PRINTF("[KB-Notify] Unexpected length %d, skipping\n", length);
    return;
  }

  uint8_t modifiers = pData[0];
  uint8_t newReport[8] = {0};

  // Normalize to 8-byte format: [Mod] [Reserved=0] [Key1-Key6]
  if (length == 8) {
    // Already 8-byte format
    memcpy(newReport, pData, 8);
  } else {
    // 7-byte format: convert to 8-byte by inserting reserved byte
    newReport[0] = pData[0];  // Modifiers
    newReport[1] = 0;          // Reserved
    memcpy(&newReport[2], &pData[1], 6);  // Key codes
  }

#ifndef RELEASE_BUILD
  Serial.print("KB: ");
  for (int i = 0; i < 8; i++) Serial.printf("%02X ", newReport[i]);
  Serial.println();
#endif

  // Detect newly pressed keys (bytes 2-7 in normalized format)
  for (int i = 2; i < 8; i++) {
    if (newReport[i] == 0) continue;
    bool wasPressed = false;
    for (int j = 2; j < 8; j++) {
      if (lastReport[j] == newReport[i]) { wasPressed = true; break; }
    }
    if (!wasPressed) {
      DBG_PRINTF("  KEY PRESS: 0x%02X mod=0x%02X\n", newReport[i], modifiers);
      enqueueKeyEvent(newReport[i], modifiers, true);
    }
  }

  // Detect released keys
  for (int i = 2; i < 8; i++) {
    if (lastReport[i] == 0) continue;
    bool stillPressed = false;
    for (int j = 2; j < 8; j++) {
      if (newReport[j] == lastReport[i]) { stillPressed = true; break; }
    }
    if (!stillPressed) {
      DBG_PRINTF("  KEY RELEASE: 0x%02X\n", lastReport[i]);
      enqueueKeyEvent(lastReport[i], modifiers, false);
    }
  }

  memcpy(lastReport, newReport, 8);

  // Track activity for adaptive connection parameters
  lastBleKeystrokeMs = millis();
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
    DBG_PRINTLN("[BLE] Connected to device");
    // Don't call secureConnection() here - the connect task handles it
  }

  void onDisconnect(NimBLEClient* pclient) override {
    bleState = BLEState::DISCONNECTED;
    pInputReportChar = nullptr;
    pRemoteService = nullptr;
    authSuccess = false;
    memset(lastReport, 0, 8);
    lastReconnectAttempt = millis();
    DBG_PRINTLN("[BLE] Disconnected");
  }

  bool onConnParamsUpdateRequest(NimBLEClient* pClient,
                                  const ble_gap_upd_params* params) override {
    // Don't blindly accept the keyboard's requested interval — enforce our floor.
    // The keyboard may request 7.5-10ms intervals which wake the radio 100+ times/sec.
    uint16_t floorMin = bleConnIdleMode ? CONN_INTERVAL_IDLE_MIN : CONN_INTERVAL_ACTIVE_MIN;
    uint16_t floorMax = bleConnIdleMode ? CONN_INTERVAL_IDLE_MAX : CONN_INTERVAL_ACTIVE_MAX;
    uint16_t itvlMin = (params->itvl_min > floorMin) ? params->itvl_min : floorMin;
    uint16_t itvlMax = (params->itvl_max > floorMax) ? params->itvl_max : floorMax;
    if (itvlMin > itvlMax) itvlMax = itvlMin;

    pClient->updateConnParams(itvlMin, itvlMax,
                               CONN_SLAVE_LATENCY, CONN_SUPERVISION_TIMEOUT);
    return true;
  }
} clientCallbacks;

static class BleSecurityCallback : public NimBLESecurityCallbacks {
  uint32_t onPassKeyRequest() override {
    DBG_PRINTLN("[BLE] PassKeyRequest received - returning 123456");
    return 123456;
  }

  void onPassKeyNotify(uint32_t passkey) override {
    currentPasskey = passkey;
    DBG_PRINTF("[BLE] Passkey notify: %06lu\n", passkey);
    // Force immediate screen update to show passkey
    extern bool screenDirty;
    screenDirty = true;
  }

  bool onConfirmPIN(uint32_t passkey) override {
    DBG_PRINTF("[BLE] Confirm PIN: %06lu - auto-accepting\n", passkey);
    currentPasskey = passkey;
    // Force immediate screen update to show passkey
    extern bool screenDirty;
    screenDirty = true;
    // Auto-accept like micro-journal does
    return true;
  }

  bool onSecurityRequest() override {
    DBG_PRINTLN("[BLE] Security request - accepting");
    return true;
  }

  void onAuthenticationComplete(ble_gap_conn_desc* desc) override {
    DBG_PRINTF("[BLE] Auth complete: status enc=%d bond=%d auth=%d\n",
               desc->sec_state.encrypted,
               desc->sec_state.bonded,
               desc->sec_state.authenticated);

    if (desc->sec_state.encrypted) {
      authSuccess = true;
      DBG_PRINTLN("[BLE] Auth success");
    } else {
      authSuccess = false;
      DBG_PRINTLN("[BLE] Auth failed - not encrypted");
    }
    currentPasskey = 0;
    // Force screen update to clear passkey display
    extern bool screenDirty;
    screenDirty = true;
  }
} securityCallback;

// --- HID service discovery and subscription ---

static bool setupHidConnection() {
  if (!pClient || !pClient->isConnected()) return false;

  DBG_PRINTLN("[BLE] Discovering services...");
  if (!pClient->getServices(true)) {
    DBG_PRINTLN("[BLE] Service discovery failed");
    return false;
  }

  pRemoteService = pClient->getService(hidServiceUUID);
  if (!pRemoteService) {
    DBG_PRINTLN("[BLE] HID service not found");
    return false;
  }

  // Set report protocol mode FIRST (before subscribing)
  NimBLEUUID protocolModeUUID((uint16_t)0x2a4e);  // Protocol Mode
  NimBLERemoteCharacteristic* pProto = pRemoteService->getCharacteristic(protocolModeUUID);
  if (pProto) {
    uint8_t mode = 1;  // 1 = Report Protocol
    pProto->writeValue(&mode, 1, true);
    DBG_PRINTLN("[BLE] Set Protocol Mode to Report Protocol (1)");
  } else {
    DBG_PRINTLN("[BLE] WARNING: No Protocol Mode characteristic found");
  }

  // Find input report via Report Reference descriptor (type=1 means Input)
  pInputReportChar = nullptr;
  std::vector<NimBLERemoteCharacteristic*>* chars = pRemoteService->getCharacteristics(true);  // true = refresh from device
  DBG_PRINTF("[BLE] Found %d characteristics in HID service\n", chars->size());

  for (auto& chr : *chars) {
    DBG_PRINTF("[BLE]   Char UUID: %s, canNotify=%d\n",
               chr->getUUID().toString().c_str(), chr->canNotify());

    if (chr->getUUID() != reportUUID) continue;

    std::vector<NimBLERemoteDescriptor*>* descs = chr->getDescriptors();
    for (auto& d : *descs) {
      if (d->getUUID() == NimBLEUUID("2908")) {
        std::string refData = d->readValue();
        if (refData.length() >= 2) {
          DBG_PRINTF("[BLE]     Report ref: ID=%d Type=%d\n",
                     (uint8_t)refData[0], (uint8_t)refData[1]);
          if ((uint8_t)refData[1] == 1) {
            pInputReportChar = chr;
            DBG_PRINTLN("[BLE]     -> Selected as input report");
            break;
          }
        }
      }
    }
    if (pInputReportChar) break;
  }

  // Fallback: subscribe to ALL notifiable report chars to find keyboard input
  if (!pInputReportChar) {
    DBG_PRINTLN("[BLE] No report ref found, subscribing to ALL notifiable report chars");
    int reportCount = 0;
    for (auto& chr : *chars) {
      if (chr->getUUID() == reportUUID && chr->canNotify()) {
        DBG_PRINTF("[BLE] Attempting subscribe to Report handle=%d...\n", chr->getHandle());
        if (chr->subscribe(true, onKeyboardNotify)) {
          reportCount++;
          DBG_PRINTF("[BLE] SUCCESS - Subscribed to report char #%d (handle=%d)\n",
                     reportCount, chr->getHandle());
          if (reportCount == 1) {
            pInputReportChar = chr;  // Keep first one as primary reference
          }
        } else {
          DBG_PRINTF("[BLE] FAILED to subscribe to report char handle=%d\n", chr->getHandle());
        }
      }
    }
    if (reportCount > 0) {
      DBG_PRINTF("[BLE] Total: Subscribed to %d report characteristics\n", reportCount);
    } else {
      DBG_PRINTLN("[BLE] WARNING: Failed to subscribe to any report characteristics!");
    }
  }

  // Fallback: boot keyboard input
  if (!pInputReportChar) {
    DBG_PRINTLN("[BLE] No report char found, trying boot keyboard input");
    pInputReportChar = pRemoteService->getCharacteristic(bootKeyboardInUUID);
    if (pInputReportChar) {
      DBG_PRINTLN("[BLE] Using boot keyboard input");
    }
  }

  if (!pInputReportChar) {
    DBG_PRINTLN("[BLE] No input report found");
    return false;
  }

  // If we have a specific input report char (from report ref or boot keyboard),
  // and we haven't already subscribed to it, subscribe now
  if (pInputReportChar->getUUID() != reportUUID || !pInputReportChar->canNotify()) {
    DBG_PRINTF("[BLE] Subscribing to char %s\n", pInputReportChar->getUUID().toString().c_str());
    if (!pInputReportChar->subscribe(true, onKeyboardNotify)) {
      DBG_PRINTLN("[BLE] Subscribe failed");
      return false;
    }
    DBG_PRINTLN("[BLE] Subscribe succeeded");
  } else {
    DBG_PRINTLN("[BLE] Already subscribed to report char(s)");
  }

  DBG_PRINTLN("[BLE] HID setup complete");
  return true;
}

// --- FreeRTOS task: runs connect + security + HID setup off the main loop ---

static void bleConnectTask(void* param) {
  bleState = BLEState::CONNECTING;
  authSuccess = false;

  DBG_PRINTF("[BLE-Task] Connecting to %s type=%d\n",
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
    DBG_PRINTLN("[BLE-Task] Connection failed");
    bleState = BLEState::DISCONNECTED;
    connectTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  DBG_PRINTLN("[BLE-Task] Connected, attempting security...");

  // Step 2: Try security pairing (optional for some keyboards)
  // If this fails, we'll still try HID setup in case the keyboard doesn't require auth
  bool secureAttempted = pClient->secureConnection();

  if (secureAttempted) {
    // Wait for auth callbacks
    unsigned long secStart = millis();
    while (!authSuccess && (millis() - secStart < 5000)) {
      vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (authSuccess) {
      DBG_PRINTLN("[BLE-Task] Security succeeded");
    } else {
      DBG_PRINTLN("[BLE-Task] Security failed/timeout - trying HID anyway");
    }
  } else {
    DBG_PRINTLN("[BLE-Task] secureConnection() returned false - trying HID anyway");
  }

  DBG_PRINTLN("[BLE-Task] Setting up HID...");

  // Step 4: Service discovery + HID subscription (blocks this task)
  if (!setupHidConnection()) {
    DBG_PRINTLN("[BLE-Task] HID setup failed, disconnecting");
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

  // Request our preferred connection parameters (active typing mode)
  pClient->updateConnParams(CONN_INTERVAL_ACTIVE_MIN, CONN_INTERVAL_ACTIVE_MAX,
                             CONN_SLAVE_LATENCY, CONN_SUPERVISION_TIMEOUT);
  bleConnIdleMode = false;
  lastBleKeystrokeMs = millis();

  DBG_PRINTLN("[BLE-Task] Keyboard ready!");
  bleState = BLEState::CONNECTED;
  reconnectDelay = 10000;  // Reset backoff after successful connection

  connectTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// Launch the connect task (non-blocking from main loop's perspective)
static void startConnectTask() {
  if (connectTaskHandle != nullptr) {
    DBG_PRINTLN("[BLE] Connect task already running");
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
  // bonding=true, MITM=FALSE, SC=FALSE - "Just Works" pairing
  NimBLEDevice::setSecurityAuth(true, false, false);
  // NO_INPUT_OUTPUT forces "Just Works" pairing (no passkey)
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityCallbacks(&securityCallback);
  NimBLEDevice::setPower(ESP_PWR_LVL_N9);  // -9dBm — lowest verified working power level

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
    DBG_PRINTF("[BLE] Will reconnect to: %s type=%d\n", storedAddr.c_str(), keyboardAddressType);
  } else {
    bleState = BLEState::DISCONNECTED;
    DBG_PRINTLN("[BLE] No stored device");
  }
}

void bleLoop() {
  // Detect when a one-shot scan finishes
  if (isScanning && !NimBLEDevice::getScan()->isScanning()) {
    isScanning = false;
    continuousScanning = false;
    DBG_PRINTF("[BLE] Scan complete — found %d devices\n", (int)discoveredDevices.size());
    // Trigger screen refresh to show final results
    extern bool screenDirty;
    screenDirty = true;
  }

  // Launch connect task if requested (non-blocking)
  if (connectToKeyboard && bleState != BLEState::CONNECTED && connectTaskHandle == nullptr) {
    connectToKeyboard = false;
    startConnectTask();
    return;
  }

  // Adaptive BLE connection parameters: fast interval while typing, slow when idle.
  // Saves significant radio power during pauses between typing bursts.
  if (bleState == BLEState::CONNECTED && pClient && pClient->isConnected()) {
    unsigned long now = millis();
    if (!bleConnIdleMode && (now - lastBleKeystrokeMs > BLE_IDLE_SWITCH_MS)) {
      // No keystrokes for 3s — switch to slow polling to save radio power
      pClient->updateConnParams(CONN_INTERVAL_IDLE_MIN, CONN_INTERVAL_IDLE_MAX,
                                 CONN_SLAVE_LATENCY, CONN_SUPERVISION_TIMEOUT);
      bleConnIdleMode = true;
    } else if (bleConnIdleMode && (now - lastBleKeystrokeMs <= BLE_IDLE_SWITCH_MS)) {
      // Keystroke arrived — switch back to fast polling for responsive typing
      pClient->updateConnParams(CONN_INTERVAL_ACTIVE_MIN, CONN_INTERVAL_ACTIVE_MAX,
                                 CONN_SLAVE_LATENCY, CONN_SUPERVISION_TIMEOUT);
      bleConnIdleMode = false;
    }
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
        DBG_PRINTF("[BLE] Auto-reconnect: %s (retry in %lums)\n",
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
    DBG_PRINTLN("[BLE] Connection in progress, will complete in background");
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
  scan->start(5, false);  // One-shot 5-second scan

  isScanning = true;
  continuousScanning = false;  // One-shot: no auto-restart
  DBG_PRINTLN("[BLE] Started one-shot scan (5s)");
}

void stopDeviceScan() {
  NimBLEDevice::getScan()->stop();
  isScanning = false;
  continuousScanning = false;
}

int getDiscoveredDeviceCount() {
  return discoveredDevices.size();
}

BleDeviceInfo* getDiscoveredDevices() {
  return discoveredDevices.empty() ? nullptr : discoveredDevices.data();
}

void connectToDevice(int deviceIndex) {
  if (deviceIndex < 0 || deviceIndex >= (int)discoveredDevices.size()) {
    DBG_PRINTLN("[BLE] Invalid device index");
    return;
  }

  stopDeviceScan();

  if (pClient && pClient->isConnected()) {
    pClient->disconnect();
  }

  keyboardAddress = discoveredDevices[deviceIndex].address;
  keyboardAddressType = discoveredDevices[deviceIndex].addressType;
  connectToKeyboard = true;

  DBG_PRINTF("[BLE] Will connect to: %s type=%d (%s)\n",
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
  DBG_PRINTF("[BLE] Stored to NVS: %s (%s) type=%d\n",
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
  DBG_PRINTLN("[BLE] Deleted all bonds + cleared stored device");
}

void clearStoredDevice() {
  prefs.remove("addr");
  prefs.remove("name");
  prefs.remove("addrType");
  DBG_PRINTLN("[BLE] Cleared stored device from NVS");
  NimBLEDevice::deleteAllBonds();
  DBG_PRINTLN("[BLE] Cleared all stored bonds");
}
