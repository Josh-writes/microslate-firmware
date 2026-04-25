#include "ble_keyboard.h"
#include "input_handler.h"

#include <NimBLEDevice.h>
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
static uint8_t inputReportId = 0;   // Non-zero if keyboard prefixes reports with a report ID

// NVS storage for persistent pairing
static Preferences prefs;

// Global flag to control auto-reconnect behavior
bool autoReconnectEnabled = true;

// Reconnection backoff
static unsigned long reconnectDelay = 5000;   // 5s initial — gives keyboard time to release old connection after deep sleep
static unsigned long lastReconnectAttempt = 0;
static constexpr unsigned long MAX_RECONNECT_DELAY = 120000;  // Cap at 2min (was 60s)

// Multi-keyboard cycling: index of the keyboard to try on the next auto-reconnect attempt
static int reconnectKeyboardIndex = 0;

// Activity tracking (used by connect task to initialise idle timer)
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
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;

// Global variable to store the passkey for display
static uint32_t currentPasskey = 0;

// Forward declarations
static bool setupHidConnection();
int getLastUsedKeyboardIndex();

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
  // Strip report ID prefix if the keyboard uses one (learned during HID discovery).
  // e.g. 9-byte: [ReportID][Mod][Reserved][K1-K6]  →  strip to 8-byte standard
  //      8-byte: [ReportID][Mod][K1-K6]             →  strip to 7-byte compact
  if (inputReportId != 0 && length > 0 && pData[0] == inputReportId) {
    pData++;
    length--;
  }

  // Must be 7 or 8 bytes after stripping. Consumer-control / media-key reports
  // from other report IDs arrive here too — silently ignore them.
  if (length < 7 || length > 8) {
    DBG_PRINTF("[KB-Notify] Ignoring %d-byte report (not keyboard format)\n", (int)length);
    return;
  }

  uint8_t modifiers = pData[0];
  uint8_t newReport[8] = {0};

  // Normalize to 8-byte format: [Mod] [Reserved=0] [Key1-Key6]
  if (length == 8) {
    memcpy(newReport, pData, 8);
  } else {
    // 7-byte compact: insert reserved byte
    newReport[0] = pData[0];  // Modifiers
    newReport[1] = 0;          // Reserved
    memcpy(&newReport[2], &pData[1], 6);
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

static class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    BleDeviceInfo info;
    info.address = dev->getAddress().toString();
    info.name = dev->haveName() ? dev->getName() : info.address;
    info.rssi = dev->getRSSI();
    info.addressType = dev->getAddress().getType();
    info.lastSeenMs = millis();
    upsertDevice(info);
    extern bool screenDirty;
    screenDirty = true;
  }
} scanCallbacks;

static class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) override {
    DBG_PRINTLN("[BLE] Connected to device");
    // Don't call secureConnection() here - the connect task handles it
  }

  void onDisconnect(NimBLEClient* pclient, int reason) override {
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
    // Reject the keyboard's connection parameter update request.
    // Keychron keyboards send a conn param update on the first keypress (idle→active
    // interval switch). Returning true makes NimBLE substitute our m_connParams into
    // the response, which mismatches what the Keychron expects and causes an immediate
    // crash/disconnect. Returning false tells the keyboard to keep the current params
    // (30–50 ms interval negotiated at connect time), which works fine for all keyboards.
    return false;
  }

  // Security callbacks (merged from NimBLESecurityCallbacks — removed in 2.x)
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    DBG_PRINTLN("[BLE] PassKeyEntry received - entering 123456");
    NimBLEDevice::injectPassKey(connInfo, 123456);
  }

  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    DBG_PRINTF("[BLE] Confirm passkey: %06lu - auto-accepting\n", (unsigned long)pin);
    currentPasskey = pin;
    extern bool screenDirty;
    screenDirty = true;
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    DBG_PRINTF("[BLE] Auth complete: encrypted=%d bonded=%d\n",
               connInfo.isEncrypted(), connInfo.isBonded());

    if (connInfo.isEncrypted()) {
      authSuccess = true;
      DBG_PRINTLN("[BLE] Auth success");
    } else {
      authSuccess = false;
      DBG_PRINTLN("[BLE] Auth failed - not encrypted");
    }
    currentPasskey = 0;
    extern bool screenDirty;
    screenDirty = true;
  }
} clientCallbacks;

// --- HID service discovery and subscription ---

static bool setupHidConnection() {
  if (!pClient || !pClient->isConnected()) return false;

  DBG_PRINTLN("[BLE] Discovering services...");
  if (pClient->getServices(true).empty()) {
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
  inputReportId = 0;
  const auto& chars = pRemoteService->getCharacteristics(true);  // true = refresh from device
  DBG_PRINTF("[BLE] Found %d characteristics in HID service\n", (int)chars.size());

  for (auto& chr : chars) {
    DBG_PRINTF("[BLE]   Char UUID: %s, canNotify=%d\n",
               chr->getUUID().toString().c_str(), chr->canNotify());

    if (chr->getUUID() != reportUUID) continue;

    const auto& descs = chr->getDescriptors();
    for (auto& d : descs) {
      if (d->getUUID() == NimBLEUUID("2908")) {
        NimBLEAttValue refData = d->readValue();
        if (refData.size() >= 2) {
          DBG_PRINTF("[BLE]     Report ref: ID=%d Type=%d\n",
                     (uint8_t)refData[0], (uint8_t)refData[1]);
          if ((uint8_t)refData[1] == 1) {
            pInputReportChar = chr;
            inputReportId = (uint8_t)refData[0];  // 0 = no ID prefix, non-zero = ID byte present
            DBG_PRINTF("[BLE]     -> Selected as input report (reportId=%d)\n", inputReportId);
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
    for (auto& chr : chars) {
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

  // Guard: if the link layer is already up (bleState flipped to DISCONNECTED spuriously
  // while the BLE connection is still alive), don't call connect() again — that crashes
  // the NimBLE stack. Just re-sync state and exit.
  if (pClient && pClient->isConnected()) {
    DBG_PRINTLN("[BLE-Task] Already connected - re-syncing state");
    bleState = BLEState::CONNECTED;
    lastBleKeystrokeMs = millis();
    bleConnIdleMode = false;
    connectTaskHandle = nullptr;
    vTaskDelete(NULL);
    return;
  }

  // Create/reuse client
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks, false);
  }
  pClient->setConnectTimeout(CONNECT_TIMEOUT_MS);

  // Step 1: Connect (blocks this task, main loop continues)
  NimBLEAddress addr(keyboardAddress, keyboardAddressType);
  // Delete any stored bond before connecting — forces a fresh "Just Works" pairing
  // instead of an encrypted reconnect. Prevents a NimBLE security-state crash when
  // the keyboard still holds a stale connection from a previous unclean disconnect.
  NimBLEDevice::deleteBond(addr);
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

  // Step 5: Mark connected immediately so bleLoop doesn't start a second connect task.
  // Do this BEFORE storePairedDevice (NVS write) to minimise the window where the link
  // is live but bleState is still CONNECTING.
  bleState = BLEState::CONNECTED;
  reconnectDelay = 5000;  // Reset backoff after successful connection
  bleConnIdleMode = false;
  lastBleKeystrokeMs = millis();  // Start the 3s idle timer from now, not from boot
  DBG_PRINTLN("[BLE-Task] Keyboard ready!");

  // NOTE: updateConnParams() is intentionally NOT called here.  Calling it immediately
  // after connecting triggers a reentrancy crash with Keychron keyboards (the keyboard
  // sends its own BLE_GAP_EVENT_CONN_UPDATE_REQ at the same time).  The adaptive params
  // logic in bleLoop() handles this safely from the main task after a stable delay.

  // If the device didn't broadcast a name during scanning, read it from GATT now.
  // Generic Access service (0x1800) Device Name characteristic (0x2A00).
  {
    NimBLERemoteService* gasSvc = pClient->getService(NimBLEUUID((uint16_t)0x1800));
    if (gasSvc) {
      NimBLERemoteCharacteristic* nameChr = gasSvc->getCharacteristic(NimBLEUUID((uint16_t)0x2A00));
      if (nameChr) {
        std::string gattName = nameChr->readValue();
        if (!gattName.empty()) {
          for (auto& d : discoveredDevices) {
            if (d.address == keyboardAddress && d.name == keyboardAddress) {
              d.name = gattName;
              break;
            }
          }
        }
      }
    }
  }

  // Store device to NVS for future reconnects, then reset cycling index to this keyboard
  {
    std::string devName = keyboardAddress;
    for (auto& d : discoveredDevices) {
      if (d.address == keyboardAddress) { devName = d.name; break; }
    }
    storePairedDevice(keyboardAddress, devName);
    // After a successful connect, next auto-reconnect attempt starts from this keyboard
    int newLastKb = getLastUsedKeyboardIndex();
    reconnectKeyboardIndex = (newLastKb >= 0) ? newLastKb : 0;
  }

  connectTaskHandle = nullptr;
  vTaskDelete(NULL);
}

// Launch the connect task (non-blocking from main loop's perspective)
static void startConnectTask() {
  if (connectTaskHandle != nullptr) {
    DBG_PRINTLN("[BLE] Connect task already running");
    return;
  }
  // 20480 bytes: Logitech has a complex service tree (keyboard + media + battery reports
  // + many descriptors). NimBLE 2.x uses more per-call stack than 1.4.x — 12288 was
  // sufficient before but overflows during full service discovery on the Logitech.
  xTaskCreate(bleConnectTask, "ble_conn", 20480, NULL, 1, &connectTaskHandle);
}

// --- NVS multi-keyboard helpers ---

static void nvs_kbKey(char* buf, int idx, const char* suffix) {
  snprintf(buf, 16, "kb_%d_%s", idx, suffix);
}

static int nvs_loadCount() {
  return (int)prefs.getUChar("kb_count", 0);
}

static std::string nvs_loadAddr(int idx) {
  char key[16]; nvs_kbKey(key, idx, "addr");
  return std::string(prefs.getString(key, "").c_str());
}

static std::string nvs_loadName(int idx) {
  char key[16]; nvs_kbKey(key, idx, "name");
  return std::string(prefs.getString(key, "").c_str());
}

static uint8_t nvs_loadType(int idx) {
  char key[16]; nvs_kbKey(key, idx, "type");
  return prefs.getUChar(key, 0);
}

static void nvs_saveKb(int idx, const std::string& addr, const std::string& name, uint8_t type) {
  char key[16];
  nvs_kbKey(key, idx, "addr"); prefs.putString(key, addr.c_str());
  nvs_kbKey(key, idx, "name"); prefs.putString(key, name.c_str());
  nvs_kbKey(key, idx, "type"); prefs.putUChar(key, type);
}

static void nvs_clearSlot(int idx) {
  char key[16];
  nvs_kbKey(key, idx, "addr"); prefs.remove(key);
  nvs_kbKey(key, idx, "name"); prefs.remove(key);
  nvs_kbKey(key, idx, "type"); prefs.remove(key);
}

// --- Public API ---

uint32_t getCurrentPasskey() {
  return currentPasskey;
}

void bleSetup() {
  NimBLEDevice::init("MicroSlate");
  // bond=true, MITM=false (we don't require it), SC=false (legacy compat for Logitech etc.)
  // DISPLAY_YESNO: we can show a number and confirm — lets keyboards that *do* want
  // numeric comparison (Apple Magic Keyboard, etc.) initiate it while still falling
  // back to "Just Works" for keyboards that don't need it.
  NimBLEDevice::setSecurityAuth(true, false, false);
  // NO_INPUT_OUTPUT = "Just Works" pairing — works for both Logitech and Keychron
  // since MITM=false means we never require authenticated pairing regardless of IO cap.
  // DISPLAY_YESNO was tried for Keychron but broke Logitech (bond/security-state mismatch
  // in NimBLE 2.x caused crashes on reconnect). Just Works is sufficient for all targets.
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  // ENC-only key distribution — omit ID (IRK) which some keyboards don't support
  // and which caused bond-validation failures after the security param change.
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setPower(-9);  // -9dBm — lowest verified working power level

  prefs.begin("ble_kb", false);

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);
  scan->setInterval(1349);
  scan->setWindow(449);
  scan->setActiveScan(true);

  // Migrate from old single-keyboard NVS format on first boot after firmware update.
  // Sentinel: if "kb_count" key is absent (returns 0xFF default), run migration.
  if (prefs.getUChar("kb_count", 0xFF) == 0xFF) {
    String oldAddr = prefs.getString("addr", "");
    if (oldAddr.length() > 0) {
      String oldName = prefs.getString("name", oldAddr);
      uint8_t oldType = prefs.getUChar("addrType", 0);
      nvs_saveKb(0, std::string(oldAddr.c_str()), std::string(oldName.c_str()), oldType);
      prefs.putUChar("kb_count", 1);
      prefs.putUChar("last_kb", 0);
      prefs.remove("addr");
      prefs.remove("name");
      prefs.remove("addrType");
      DBG_PRINTLN("[BLE] Migrated single-keyboard NVS to multi-keyboard format");
    } else {
      prefs.putUChar("kb_count", 0);
    }
  }

  // Prime auto-reconnect: start with last-used keyboard, give it 5s before first attempt
  // so the keyboard's supervision timeout from the previous session has time to expire.
  int pairedCount = nvs_loadCount();
  int lastKb = (pairedCount > 0) ? (int)prefs.getUChar("last_kb", 0) : -1;
  if (lastKb >= pairedCount) lastKb = 0;
  reconnectKeyboardIndex = (lastKb >= 0) ? lastKb : 0;

  if (pairedCount > 0 && lastKb >= 0) {
    keyboardAddress = nvs_loadAddr(lastKb);
    keyboardAddressType = nvs_loadType(lastKb);
    lastReconnectAttempt = millis();
    DBG_PRINTF("[BLE] Will reconnect to: %s (in %lums, %d paired total)\n",
               keyboardAddress.c_str(), reconnectDelay, pairedCount);
  } else {
    bleState = BLEState::DISCONNECTED;
    DBG_PRINTLN("[BLE] No paired keyboards");
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

  // Auto-reconnect: cycle through all paired keyboards with exponential backoff.
  // Backoff increases only after a full cycle through all keyboards.
  if (bleState == BLEState::DISCONNECTED && autoReconnectEnabled && connectTaskHandle == nullptr) {
    int count = nvs_loadCount();
    if (count > 0) {
      unsigned long now = millis();
      if (now - lastReconnectAttempt >= reconnectDelay) {
        int idx = reconnectKeyboardIndex % count;
        keyboardAddress = nvs_loadAddr(idx);
        keyboardAddressType = nvs_loadType(idx);
        connectToKeyboard = true;
        lastReconnectAttempt = now;
        DBG_PRINTF("[BLE] Auto-reconnect: trying keyboard %d/%d (%s)\n",
                   idx + 1, count, keyboardAddress.c_str());
        reconnectKeyboardIndex = (reconnectKeyboardIndex + 1) % count;
        // Increase backoff only after a full cycle through all keyboards
        if (reconnectKeyboardIndex == 0) {
          reconnectDelay = (reconnectDelay * 2 > MAX_RECONNECT_DELAY)
                             ? MAX_RECONNECT_DELAY : reconnectDelay * 2;
        }
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
  scan->setScanCallbacks(&scanCallbacks, true);
  scan->setActiveScan(true);
  scan->start(5000, false);  // One-shot 5-second scan (2.x API takes milliseconds)

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

// --- Multi-keyboard public API ---

int getPairedKeyboardCount() {
  return nvs_loadCount();
}

bool getPairedKeyboard(int index, std::string& addr, std::string& name, uint8_t& addrType) {
  if (index < 0 || index >= nvs_loadCount()) return false;
  addr = nvs_loadAddr(index);
  name = nvs_loadName(index);
  addrType = nvs_loadType(index);
  return !addr.empty();
}

int getLastUsedKeyboardIndex() {
  int count = nvs_loadCount();
  if (count == 0) return -1;
  int last = (int)prefs.getUChar("last_kb", 0);
  return (last < count) ? last : 0;
}

bool removePairedKeyboard(int index) {
  int count = nvs_loadCount();
  if (index < 0 || index >= count) return false;

  // Delete NimBLE bond for the keyboard being removed
  std::string addr = nvs_loadAddr(index);
  if (!addr.empty()) {
    NimBLEDevice::deleteBond(NimBLEAddress(addr, nvs_loadType(index)));
  }

  // Shift remaining slots down
  for (int i = index; i < count - 1; i++) {
    nvs_saveKb(i, nvs_loadAddr(i + 1), nvs_loadName(i + 1), nvs_loadType(i + 1));
  }
  nvs_clearSlot(count - 1);
  prefs.putUChar("kb_count", (uint8_t)(count - 1));

  // Fix last_kb
  int lastKb = (int)prefs.getUChar("last_kb", 0);
  if (count - 1 == 0) {
    prefs.remove("last_kb");
  } else if (lastKb >= count - 1) {
    prefs.putUChar("last_kb", 0);
  } else if (lastKb > index) {
    prefs.putUChar("last_kb", (uint8_t)(lastKb - 1));
  }

  // If this was the current connection, reset state
  if (keyboardAddress == addr) {
    if (pClient && pClient->isConnected()) pClient->disconnect();
    keyboardAddress = "";
  }

  reconnectKeyboardIndex = 0;
  DBG_PRINTF("[BLE] Removed paired keyboard slot %d (%s)\n", index, addr.c_str());
  return true;
}

void connectToPairedKeyboard(int index) {
  int count = nvs_loadCount();
  if (index < 0 || index >= count) return;

  stopDeviceScan();
  if (pClient && pClient->isConnected()) pClient->disconnect();

  keyboardAddress = nvs_loadAddr(index);
  keyboardAddressType = nvs_loadType(index);
  prefs.putUChar("last_kb", (uint8_t)index);
  reconnectKeyboardIndex = (count > 1) ? (index + 1) % count : 0;
  reconnectDelay = 5000;
  connectToKeyboard = true;
  DBG_PRINTF("[BLE] Switching to paired keyboard %d: %s\n", index, keyboardAddress.c_str());
}

void storePairedDevice(const std::string& address, const std::string& name) {
  int count = nvs_loadCount();

  // Update name if already stored
  for (int i = 0; i < count; i++) {
    if (nvs_loadAddr(i) == address) {
      nvs_saveKb(i, address, name, keyboardAddressType);
      prefs.putUChar("last_kb", (uint8_t)i);
      DBG_PRINTF("[BLE] Updated paired keyboard slot %d: %s\n", i, name.c_str());
      return;
    }
  }

  int idx;
  if (count < MAX_PAIRED_KEYBOARDS) {
    idx = count;
    prefs.putUChar("kb_count", (uint8_t)(count + 1));
  } else {
    // List full: evict slot 0 (oldest), shift everything down
    for (int i = 0; i < MAX_PAIRED_KEYBOARDS - 1; i++) {
      nvs_saveKb(i, nvs_loadAddr(i + 1), nvs_loadName(i + 1), nvs_loadType(i + 1));
    }
    idx = MAX_PAIRED_KEYBOARDS - 1;
  }

  nvs_saveKb(idx, address, name, keyboardAddressType);
  prefs.putUChar("last_kb", (uint8_t)idx);
  DBG_PRINTF("[BLE] Stored new paired keyboard slot %d: %s (%s)\n",
             idx, name.c_str(), address.c_str());
}

bool getStoredDevice(std::string& address, std::string& name) {
  int idx = getLastUsedKeyboardIndex();
  if (idx < 0) return false;
  uint8_t addrType;
  return getPairedKeyboard(idx, address, name, addrType);
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
  int count = nvs_loadCount();
  for (int i = 0; i < count; i++) nvs_clearSlot(i);
  prefs.putUChar("kb_count", 0);
  prefs.remove("last_kb");
  NimBLEDevice::deleteAllBonds();
  keyboardAddress = "";
  reconnectKeyboardIndex = 0;
  DBG_PRINTLN("[BLE] Cleared all paired keyboards and bonds");
}
