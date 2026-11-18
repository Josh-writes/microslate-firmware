#include "ble_keyboard.h"
#include "ble_dump.h"
#include "input_handler.h"
#include "sd_backup.h"

#include <HidCore.h>
#include <algorithm>
#include <esp_heap_caps.h>

static volatile uint8_t heldModifiers = 0;   // modifier bits of the last keyboard report
uint8_t bleHeldModifiers() { return heldModifiers; }
#include <NimBLEDevice.h>
#include <Preferences.h>
#if defined(FREEINK_DEVICE_E1001) && FREEINK_DEVICE_E1001
#include "driver/Uc8179E1001Driver.h"
#endif

// HID service and characteristic UUIDs
static NimBLEUUID hidServiceUUID("1812");
static NimBLEUUID reportUUID("2a4d");
static NimBLEUUID reportMapUUID("2a4b");
static NimBLEUUID protocolModeUUID("2a4e");
static NimBLEUUID bootKeyboardInUUID("2a22");

// Module state
static NimBLEClient* pClient = nullptr;

// volatile: written from the BLE host task (callbacks), the connect task, and the
// main loop. Without it the compiler can cache a stale value and miss transitions,
// which showed up as reconnect stalls and the occasional race-driven reboot.
static volatile BLEState bleState = BLEState::DISCONNECTED;
static bool connectToKeyboard = false;
static std::string keyboardAddress = "";
static uint8_t keyboardAddressType = 0;

// Input reports are routed by attribute handle from a GAP event listener
// (gapEventListener), not from NimBLE's per-characteristic callbacks. A bonded
// keyboard keeps its notification subscriptions from the last connection, so with
// the handles saved in NVS it can type as soon as the link is encrypted, before
// service discovery has run again. Discovery still runs afterwards and refreshes
// the list. Characteristics are subscribed with no callback, so each report is
// delivered exactly once, here.
static constexpr int MAX_INPUT_HANDLES = 8;
static volatile uint16_t inputHandles[MAX_INPUT_HANDLES];
static volatile int inputHandleCount = 0;
static volatile uint16_t activeConnHandle = BLE_HS_CONN_HANDLE_NONE;

// How each routed handle is decoded: its report ID and the keyboard's Report Map
// (lib/HidCore). ID 0xFF or no usable map = the fixed 7/8-byte format (decodeLegacy).
// Two parsed maps: a new one is built in the spare and swapped in while routing is
// off, so a report being decoded on the host task never sees a half-written map.
static constexpr size_t MAX_REPORT_MAP = 512;   // GATT limit for the Report Map value
static constexpr uint8_t LEGACY_REPORT_ID = 0xFF;
static hidcore::ReportMap reportMaps[2];
static volatile int activeMap = 0;
static volatile int8_t inputReportIndex[MAX_INPUT_HANDLES];  // into reportMaps[activeMap], -1 = legacy
static hidcore::KeyState keyStates[MAX_INPUT_HANDLES];
static uint8_t routeIds[MAX_INPUT_HANDLES];
static uint8_t routeMap[MAX_REPORT_MAP];   // raw map behind the routes, saved to NVS
static size_t routeMapLen = 0;

// NVS storage for persistent pairing
static Preferences prefs;

// Global flag to control auto-reconnect behavior
bool autoReconnectEnabled = true;

// Reconnect: while no keyboard is connected, a low-duty passive scan listens for the
// stored keyboards only (controller allow list), and the Sticky connects the moment one
// advertises, which is what a keyboard does when a key or its channel button is pressed.
// This replaced polling one stored address every 5 s in turn, where a keyboard's turn
// came only every 5 s x (number of stored keyboards) and the radio was busy ~70% of the
// time. 15 ms of every 100 ms: about 15% radio time, and a keyboard's advertising burst
// still lands in a window within a few hundred ms.
static constexpr uint16_t BG_SCAN_INTERVAL_MS = 100;
static constexpr uint16_t BG_SCAN_WINDOW_MS = 15;
static bool bgScanning = false;
static unsigned long bgScanTriedMs = 0;   // a failed start is retried once a second, not every loop
static portMUX_TYPE bgHitMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool bgHitPending = false;
static ble_addr_t bgHitAddr;

// Per-keyboard backoff after failed attempts, so a keyboard that keeps advertising but
// refuses the connection or the pairing is not hammered (some keyboards rate-limit
// pairing attempts). 2, 4, 8, 16, then 30 s; cleared on a good connection.
struct RetryState {
  std::string addr;
  uint8_t fails = 0;
  unsigned long notBefore = 0;
};
static RetryState retryStates[MAX_PAIRED_KEYBOARDS + 1];
static constexpr unsigned long MAX_RETRY_BACKOFF_MS = 30000;

// Activity tracking (used by connect task to initialise idle timer)
static unsigned long lastBleKeystrokeMs = 0;
static bool bleConnIdleMode = false;

// Device discovery variables
static std::vector<BleDeviceInfo> discoveredDevices;
static bool isScanning = false;
static bool continuousScanning = false;

static void storePairedDevice(const std::string& address, const std::string& name);

// FreeRTOS connect task
static volatile TaskHandle_t connectTaskHandle = nullptr;   // set while a connection is being made
// The task that makes connections: created once, at bleSetup(), while memory is plentiful, and
// woken for each connection. Creating it for every connection failed after a long cloud sync
// (E1001, 2026-10-29): no 32 KB block was left, xTaskCreate failed silently and the keyboard
// was never reconnected.
static TaskHandle_t connectWorker = nullptr;
static unsigned long connectRetryAtMs = 0;   // after a failed start, wait before trying again
static volatile bool authSuccess = false;

// When the current connect task started (millis). Used by bleLoop() to detect and
// recover a wedged connect task so auto-reconnect can't deadlock forever.
static volatile unsigned long connectTaskStartMs = 0;

// Connection timeout in seconds
static constexpr uint32_t CONNECT_TIMEOUT_MS = 10000;

// Timeout applied to the NEXT connect attempt. Manual pairing keeps the full 10s;
// auto-reconnect starts only when the keyboard is advertising, so it connects at once
// or not at all, and a short value releases the radio quickly.
static uint32_t nextConnectTimeoutMs = CONNECT_TIMEOUT_MS;
static constexpr uint32_t RECONNECT_CONNECT_TIMEOUT_MS = 3500;

// Hard ceiling for the whole connect task (connect + auth + service discovery).
// Normal worst case is ~17s (10s connect + 5s auth + discovery); past 30s we assume
// the NimBLE host wedged and force the state machine back to DISCONNECTED so the
// auto-reconnect cycle resumes instead of the keyboard staying dead until reboot.
static constexpr unsigned long CONNECT_TASK_TIMEOUT_MS = 30000;

// Global variable to store the passkey for display
static uint32_t currentPasskey = 0;

#if defined(MS_BLE_DUMP)
static volatile unsigned long encryptedAtMs = 0;  // for the first-report timing line
#endif

// Forward declarations
static bool setupHidConnection();
int getLastUsedKeyboardIndex();
static int findPairedSlot(const std::string& addr);
static bool nvs_loadHandles(int idx);
static void nvs_saveHandles(int idx);
static void nvs_setNeedsRepair(int idx, bool on);

// Helper: upsert device into discovered list
static void upsertDevice(const BleDeviceInfo& info) {
  for (auto &d : discoveredDevices) {
    if (d.address == info.address) {
      // Preserve a previously-captured real name. A device sends several
      // advertising packets; the name often arrives only in the scan response,
      // while plain ADV packets have none (name == address). Without this, a
      // later name-less packet would clobber the real name back to the MAC.
      bool hadRealName = (d.name != d.address);
      bool newHasRealName = (info.name != info.address);
      std::string keptName = d.name;
      bool keptHid = d.isHid;
      uint16_t keptAppearance = d.appearance;
      d = info;
      if (hadRealName && !newHasRealName) d.name = keptName;
      d.isHid = d.isHid || keptHid;                       // HID flag is sticky once seen
      if (d.appearance == 0) d.appearance = keptAppearance; // keep appearance if this packet lacked it
      return;
    }
  }
  discoveredDevices.push_back(info);
}

// --- Retry backoff ---

static RetryState* retryFor(const std::string& addr, bool create) {
  for (auto& r : retryStates) if (r.addr == addr) return &r;
  if (!create) return nullptr;
  for (auto& r : retryStates) {
    if (r.addr.empty() || r.fails == 0) { r = RetryState(); r.addr = addr; return &r; }
  }
  return &retryStates[0];
}

static void noteConnectFailure(const std::string& addr) {
  RetryState* r = retryFor(addr, true);
  if (r->fails < 255) r->fails++;
  unsigned long wait = 1000UL << (r->fails < 5 ? r->fails : 5);
  if (wait > MAX_RETRY_BACKOFF_MS) wait = MAX_RETRY_BACKOFF_MS;
  r->notBefore = millis() + wait;
  BLEDUMP("BACKOFF %s fails=%u wait=%lu ms", addr.c_str(), (unsigned)r->fails, wait);
}

static void noteConnectSuccess(const std::string& addr) {
  if (RetryState* r = retryFor(addr, false)) { r->fails = 0; r->notBefore = 0; }
}

static bool retryAllowed(const std::string& addr) {
  RetryState* r = retryFor(addr, false);
  return !r || r->fails == 0 || (long)(millis() - r->notBefore) >= 0;
}

// Pairing errors that mean the stored key or the pairing itself was refused, as opposed
// to radio trouble (timeouts, a dropped link). Only these drop a stored bond. NimBLE
// already handles "key missing" (deletes the bond and pairs again), so reaching here
// with one means that retry failed too. Too many attempts (SMP 0x09) is a rate limit,
// not a bad key: back off instead.
static bool isPairingRefused(int err) {
  if (err == BLE_HS_SM_US_ERR(BLE_SM_ERR_REPEATED) || err == BLE_HS_SM_PEER_ERR(BLE_SM_ERR_REPEATED)) return false;
  if (err >= BLE_HS_ERR_SM_US_BASE && err < BLE_HS_ERR_SM_PEER_BASE + 0x100) return true;
  return err == BLE_HS_HCI_ERR(BLE_ERR_AUTH_FAIL) || err == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING) ||
         err == BLE_HS_HCI_ERR(BLE_ERR_CONN_TERM_MIC) || err == BLE_HS_HCI_ERR(BLE_ERR_INSUFFICIENT_SEC);
}

// --- Keyboard reports ---

static void processKeyboardReport(int slot, uint16_t handle, const uint8_t* pData, size_t length) {
#if defined(MS_BLE_DUMP)
  {
    if (encryptedAtMs) {
      BLEDUMP("FIRST report %lu ms after encryption", (unsigned long)(millis() - encryptedAtMs));
      encryptedAtMs = 0;
    }
    char tag[24];
    snprintf(tag, sizeof(tag), "RPT h=%u n=%u", (unsigned)handle, (unsigned)length);
    bleDumpHex(tag, pData, length);
  }
#endif
  // Decode with the keyboard's own report layout, or the fixed 7/8-byte format when it
  // has no usable Report Map. Media, system and vendor reports produce no events.
  hidcore::KeyEvent ev[2 * hidcore::kMaxKeys];
  const int ri = inputReportIndex[slot];
  const int n = ri >= 0
      ? hidcore::decodeReport(reportMaps[activeMap].reports[ri], pData, length, keyStates[slot], ev, 2 * hidcore::kMaxKeys)
      : hidcore::decodeLegacy(pData, length, keyStates[slot], ev, 2 * hidcore::kMaxKeys);
  heldModifiers = keyStates[slot].mods;   // a modifier-only change gives no event; key repeat reads this
  if (n == 0) return;

#ifndef RELEASE_BUILD
  Serial.print("KB: ");
  for (size_t i = 0; i < length && i < 16; i++) Serial.printf("%02X ", pData[i]);
  Serial.println();
#endif
  for (int i = 0; i < n; i++) {
    DBG_PRINTF("  KEY %s: 0x%02X mod=0x%02X\n", ev[i].pressed ? "PRESS" : "RELEASE", ev[i].usage, ev[i].mods);
    // Win/Cmd shortcuts are passed on; the input handler keeps them from typing text.
    enqueueKeyEvent(ev[i].usage, ev[i].mods, ev[i].pressed);
  }

  // Track activity for adaptive connection parameters
  lastBleKeystrokeMs = millis();
}

// GAP event listener (NimBLEDevice::setCustomGapHandler). Runs on the NimBLE host task
// before the client's own handler, for every GAP event; only notifications on the
// connected keyboard's input handles are used. The return value is ignored by NimBLE.
static int gapEventListener(ble_gap_event* event, void* arg) {
  if (event->type != BLE_GAP_EVENT_NOTIFY_RX) return 0;
  if (event->notify_rx.conn_handle != activeConnHandle) return 0;
  const uint16_t handle = event->notify_rx.attr_handle;
  const int n = inputHandleCount;
  int slot = -1;
  for (int i = 0; i < n; i++) {
    if (inputHandles[i] == handle) { slot = i; break; }
  }
  if (slot < 0) return 0;
  uint8_t buf[64];
  uint16_t len = OS_MBUF_PKTLEN(event->notify_rx.om);
  if (len > sizeof(buf)) len = sizeof(buf);
  if (os_mbuf_copydata(event->notify_rx.om, 0, len, buf) != 0) return 0;
  processKeyboardReport(slot, handle, buf, len);
  return 0;
}

#if defined(MS_BLE_DUMP)
static const char* reportKindName(hidcore::ReportKind k) {
  switch (k) {
    case hidcore::ReportKind::Keyboard: return "keys";
    case hidcore::ReportKind::Consumer: return "media";
    case hidcore::ReportKind::Vendor: return "vendor";
    default: return "other";
  }
}

static void dumpLayout(const hidcore::ReportMap& m) {
  for (int r = 0; r < m.count; r++) {
    const hidcore::ReportLayout& l = m.reports[r];
    char f[96] = "";
    for (int i = 0; i < l.fieldCount; i++) {
      const hidcore::KeyField& k = l.fields[i];
      char t[32];
      if (k.type == hidcore::FieldType::Bitmap)
        snprintf(t, sizeof(t), " bits@%u x%u from 0x%02X", (unsigned)k.bitOffset, (unsigned)k.count, k.usageMin);
      else
        snprintf(t, sizeof(t), " keys@%u %ux%u", (unsigned)k.bitOffset, (unsigned)k.count, (unsigned)k.bitSize);
      strncat(f, t, sizeof(f) - strlen(f) - 1);
    }
    BLEDUMP("LAYOUT id=%u %s bytes=%u%s", l.id, reportKindName(l.kind), (unsigned)((l.bits + 7) / 8), f);
  }
}
#endif

// Installs the routed handles with their report IDs and the Report Map they refer to
// (mapLen 0: decode every handle with the fixed format). Unchanged routes, the usual
// case when discovery confirms what a fast reconnect loaded, are left alone so nothing
// typed meanwhile is lost and no held key is forgotten.
static void setRoutes(const uint16_t* handles, const uint8_t* ids, int count, const uint8_t* map, size_t mapLen) {
  if (count > MAX_INPUT_HANDLES) count = MAX_INPUT_HANDLES;
  if (mapLen > MAX_REPORT_MAP) mapLen = 0;
  bool same = count == inputHandleCount && mapLen == routeMapLen && memcmp(map, routeMap, mapLen) == 0;
  for (int i = 0; same && i < count; i++) same = inputHandles[i] == handles[i] && routeIds[i] == ids[i];
  if (same) return;

  const int spare = activeMap ^ 1;
  const bool parsed = mapLen > 0 && hidcore::parseReportMap(map, mapLen, reportMaps[spare]) == hidcore::ParseResult::Ok;
  inputHandleCount = 0;   // readers stop matching before the routes change
  if (parsed) activeMap = spare;
  for (int i = 0; i < count; i++) {
    inputHandles[i] = handles[i];
    routeIds[i] = ids[i];
    inputReportIndex[i] = (parsed && ids[i] != LEGACY_REPORT_ID) ? (int8_t)reportMaps[activeMap].find(ids[i]) : -1;
    memset(&keyStates[i], 0, sizeof(keyStates[i]));
  }
  routeMapLen = parsed ? mapLen : 0;
  if (routeMapLen) memcpy(routeMap, map, routeMapLen);
  inputHandleCount = count;
}

static void clearRoutes() {
  inputHandleCount = 0;
  memset(keyStates, 0, sizeof(keyStates));
}

// --- Background scan for stored keyboards ---

static void bgScanStop() {
  if (!bgScanning) return;
  NimBLEDevice::getScan()->stop();
  bgScanning = false;
  // Drop a hit from a repeated advert that arrived just before the stop; left set, it
  // started a pointless connect as soon as the keyboard next disconnected.
  portENTER_CRITICAL(&bgHitMux);
  bgHitPending = false;
  portEXIT_CRITICAL(&bgHitMux);
}

static int nvs_loadCount();
static std::string nvs_loadAddr(int idx);
static uint8_t nvs_loadType(int idx);

static void bgScanStart() {
  if (bgScanning) return;
  NimBLEScan* scan = NimBLEDevice::getScan();
  if (scan->isScanning()) return;   // a device scan for pairing owns the radio
  if (bgScanTriedMs && millis() - bgScanTriedMs < 1000) return;
  bgScanTriedMs = millis();

  // The allow list can only change while the controller is not scanning or connecting.
  while (NimBLEDevice::getWhiteListCount() > 0) {
    if (!NimBLEDevice::whiteListRemove(NimBLEDevice::getWhiteListAddress(0))) break;
  }
  int count = nvs_loadCount();
  int listed = 0;
  for (int i = 0; i < count; i++) {
    std::string a = nvs_loadAddr(i);
    if (!a.empty() && NimBLEDevice::whiteListAdd(NimBLEAddress(a, nvs_loadType(i)))) listed++;
  }
  if (listed == 0) return;

  scan->setFilterPolicy(BLE_HCI_SCAN_FILT_USE_WL);
  scan->setActiveScan(false);     // passive: nothing to ask, the advert itself is the signal
  scan->setInterval(BG_SCAN_INTERVAL_MS);
  scan->setWindow(BG_SCAN_WINDOW_MS);
  scan->setDuplicateFilter(0);    // keep reporting a keyboard that advertises again
  scan->setMaxResults(0);         // callbacks only, nothing stored
  bgHitPending = false;
  bgScanning = scan->start(0, false, true);
  if (bgScanning) bgScanTriedMs = 0;
  BLEDUMP("WAIT background scan %s, %d keyboard(s) on the allow list", bgScanning ? "on" : "FAILED", listed);
}

// --- Callbacks (static instances, no heap allocation) ---

static class ScanCallbacks : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    if (bgScanning) {
      // Allow-listed, so this is one of the stored keyboards advertising.
      if (!dev->isConnectable() || bgHitPending || connectTaskHandle != nullptr) return;
      portENTER_CRITICAL(&bgHitMux);
      bgHitAddr = *dev->getAddress().getBase();
      bgHitPending = true;
      portEXIT_CRITICAL(&bgHitMux);
#if defined(MS_BLE_DUMP)
      uint8_t advType = dev->getAdvType();
      bool directed = (advType == BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_HD || advType == BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_LD);
      BLEDUMP("WAKE %s type=%u adv=%u%s rssi=%d bonded=%d", dev->getAddress().toString().c_str(),
              (unsigned)dev->getAddress().getType(), (unsigned)advType, directed ? " DIRECTED" : "",
              dev->getRSSI(), NimBLEDevice::isBonded(dev->getAddress()));
#endif
      return;
    }
    BleDeviceInfo info;
    info.address = dev->getAddress().toString();
    info.name = dev->haveName() ? dev->getName() : info.address;
    info.rssi = dev->getRSSI();
    info.addressType = dev->getAddress().getType();
    // Classify so the UI can show keyboards first and hide clutter. Active scan
    // folds scan-response data in here too. Not guaranteed (some keyboards only
    // expose HID after connecting), so this only MARKS — never hides permanently.
    info.isHid = dev->isAdvertisingService(NimBLEUUID((uint16_t)0x1812));
    info.appearance = dev->getAppearance();
#if defined(MS_BLE_DUMP)
    {
      bool seen = false;
      for (auto& d : discoveredDevices) if (d.address == info.address) { seen = true; break; }
      uint8_t advType = dev->getAdvType();
      bool directed = (advType == BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_HD || advType == BLE_HCI_ADV_TYPE_ADV_DIRECT_IND_LD);
      if (!seen || directed) {
        BLEDUMP("SCAN %s type=%u adv=%u%s conn=%d rssi=%d hid=%d app=0x%04X bonded=%d name=\"%s\"",
                info.address.c_str(), (unsigned)info.addressType, (unsigned)advType,
                directed ? " DIRECTED" : "", dev->isConnectable(), info.rssi, info.isHid,
                (unsigned)info.appearance, NimBLEDevice::isBonded(dev->getAddress()),
                dev->haveName() ? dev->getName().c_str() : "");
      }
    }
#endif
    upsertDevice(info);
    extern bool screenDirty;
    screenDirty = true;
  }
} scanCallbacks;

static class ClientCallbacks : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient* pclient) override {
    DBG_PRINTLN("[BLE] Connected to device");
#if defined(MS_BLE_DUMP)
    {
      NimBLEConnInfo ci = pclient->getConnInfo();
      BLEDUMP("LINK up peer=%s id=%s itvl=%u lat=%u tmo=%u", ci.getAddress().toString().c_str(),
              ci.getIdAddress().toString().c_str(), (unsigned)ci.getConnInterval(),
              (unsigned)ci.getConnLatency(), (unsigned)ci.getConnTimeout());
    }
#endif
    // Don't call secureConnection() here - the connect task handles it
  }

  void onDisconnect(NimBLEClient* pclient, int reason) override {
    bleState = BLEState::DISCONNECTED;
    activeConnHandle = BLE_HS_CONN_HANDLE_NONE;
    clearRoutes();
    authSuccess = false;
    DBG_PRINTLN("[BLE] Disconnected");
    BLEDUMP("LINK down reason=0x%X (%d)", (unsigned)reason, reason);
  }

  bool onConnParamsUpdateRequest(NimBLEClient* pclient,
                                  const ble_gap_upd_params* params) override {
    // Accept the keyboard's own parameters (Logitech asks for slave latency to save its
    // battery). NimBLE answers an accepted request with the client's stored parameters,
    // not the peer's, so copy the peer's values in first. Returning true without that
    // answered with our defaults instead, which is what disconnected Keychron boards and
    // why requests used to be rejected. Not yet retested on a Keychron.
    pclient->setConnectionParams(params->itvl_min, params->itvl_max, params->latency,
                                 params->supervision_timeout);
    BLEDUMP("CONNPARAM request itvl=%u-%u lat=%u tmo=%u -> accepted",
            (unsigned)params->itvl_min, (unsigned)params->itvl_max,
            (unsigned)params->latency, (unsigned)params->supervision_timeout);
    return true;
  }

  // Security callbacks (merged from NimBLESecurityCallbacks — removed in 2.x)
  void onPassKeyEntry(NimBLEConnInfo& connInfo) override {
    DBG_PRINTLN("[BLE] PassKeyEntry received - entering 123456");
    BLEDUMP("SEC passkey entry requested -> injecting 123456");
    NimBLEDevice::injectPassKey(connInfo, 123456);
  }

  void onConfirmPasskey(NimBLEConnInfo& connInfo, uint32_t pin) override {
    DBG_PRINTF("[BLE] Confirm passkey: %06lu - auto-accepting\n", (unsigned long)pin);
    BLEDUMP("SEC numeric comparison %06lu -> accepted", (unsigned long)pin);
    currentPasskey = pin;
    extern bool screenDirty;
    screenDirty = true;
    NimBLEDevice::injectConfirmPasskey(connInfo, true);
  }

  void onAuthenticationComplete(NimBLEConnInfo& connInfo) override {
    DBG_PRINTF("[BLE] Auth complete: encrypted=%d bonded=%d\n",
               connInfo.isEncrypted(), connInfo.isBonded());
    BLEDUMP("SEC done enc=%d auth=%d bonded=%d keysize=%u peer=%s id=%s storeBonded=%d bonds=%d",
            connInfo.isEncrypted(), connInfo.isAuthenticated(), connInfo.isBonded(),
            (unsigned)connInfo.getSecKeySize(), connInfo.getAddress().toString().c_str(),
            connInfo.getIdAddress().toString().c_str(),
            NimBLEDevice::isBonded(connInfo.getIdAddress()), NimBLEDevice::getNumBonds());

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

// Report Reference (0x2908) of a Report characteristic: id and type (1 input, 2 output, 3 feature).
static bool readReportRef(NimBLERemoteCharacteristic* chr, uint8_t& id, uint8_t& type) {
  NimBLERemoteDescriptor* ref = chr->getDescriptor(NimBLEUUID((uint16_t)0x2908));
  if (!ref) return false;
  NimBLEAttValue v = ref->readValue();
  if (v.size() < 2) return false;
  id = v[0];
  type = v[1];
  return true;
}

#if defined(MS_BLE_DUMP)
static void dumpHidService(NimBLERemoteService* svc, const std::vector<NimBLERemoteCharacteristic*>& chars) {
  for (auto& chr : chars) {
    bool isReport = chr->getUUID() == reportUUID;
    uint8_t id = 0, type = 0;
    bool hasRef = isReport && readReportRef(chr, id, type);
    BLEDUMP("CHAR %s h=%u read=%d notify=%d indicate=%d write=%d wnr=%d%s",
            chr->getUUID().toString().c_str(), (unsigned)chr->getHandle(), chr->canRead(),
            chr->canNotify(), chr->canIndicate(), chr->canWrite(), chr->canWriteNoResponse(),
            (isReport && !hasRef) ? " ref=none" : "");
    if (hasRef) BLEDUMP("REPORTREF h=%u id=%u type=%u", (unsigned)chr->getHandle(), id, type);
  }
  if (!pClient->isConnected()) return;
  if (NimBLERemoteCharacteristic* info = svc->getCharacteristic(NimBLEUUID((uint16_t)0x2A4A))) {
    NimBLEAttValue v = info->readValue();
    bleDumpHex("HIDINFO", v.data(), v.size());
  }
  if (NimBLERemoteCharacteristic* map = svc->getCharacteristic(reportMapUUID)) {
    NimBLEAttValue v = map->readValue();
    BLEDUMP("MAP len=%u", (unsigned)v.size());
    bleDumpHex("MAP", v.data(), v.size());
  } else {
    BLEDUMP("MAP none");
  }
  BLEDUMP("BOOTKB present=%d", svc->getCharacteristic(bootKeyboardInUUID) != nullptr);
}

static void dumpReadString(NimBLERemoteService* svc, uint16_t uuid, const char* label) {
  if (NimBLERemoteCharacteristic* c = svc->getCharacteristic(NimBLEUUID(uuid))) {
    std::string v = c->readValue();
    BLEDUMP("DEVINFO %s=\"%s\"", label, v.c_str());
  }
}

static void dumpDeviceInfo() {
  if (!pClient || !pClient->isConnected()) return;
  if (NimBLERemoteService* dis = pClient->getService(NimBLEUUID((uint16_t)0x180A))) {
    if (NimBLERemoteCharacteristic* pnp = dis->getCharacteristic(NimBLEUUID((uint16_t)0x2A50))) {
      NimBLEAttValue v = pnp->readValue();
      if (v.size() >= 7) {
        BLEDUMP("DEVINFO pnp src=%u vid=0x%04X pid=0x%04X ver=0x%04X", (unsigned)v[0],
                (unsigned)(v[1] | (v[2] << 8)), (unsigned)(v[3] | (v[4] << 8)), (unsigned)(v[5] | (v[6] << 8)));
      } else {
        bleDumpHex("DEVINFO pnp", v.data(), v.size());
      }
    }
    dumpReadString(dis, 0x2A29, "maker");
    dumpReadString(dis, 0x2A24, "model");
    dumpReadString(dis, 0x2A26, "firmware");
  } else {
    BLEDUMP("DEVINFO none");
  }
  if (NimBLERemoteService* bas = pClient->getService(NimBLEUUID((uint16_t)0x180F))) {
    if (NimBLERemoteCharacteristic* lvl = bas->getCharacteristic(NimBLEUUID((uint16_t)0x2A19))) {
      NimBLEAttValue v = lvl->readValue();
      BLEDUMP("BATTERY %d%%", v.size() ? (int)v[0] : -1);
    }
  }
  NimBLEConnInfo ci = pClient->getConnInfo();
  BLEDUMP("LINK ready itvl=%u lat=%u tmo=%u enc=%d bonded=%d", (unsigned)ci.getConnInterval(),
          (unsigned)ci.getConnLatency(), (unsigned)ci.getConnTimeout(), ci.isEncrypted(), ci.isBonded());
}

static void dumpBonds(const char* when) {
  int n = NimBLEDevice::getNumBonds();
  BLEDUMP("BONDS %s count=%d", when, n);
  for (int i = 0; i < n; i++) {
    NimBLEAddress a = NimBLEDevice::getBondedAddress(i);
    BLEDUMP("BOND %d %s type=%u", i, a.toString().c_str(), (unsigned)a.getType());
  }
}
#endif

// --- HID service discovery and subscription ---

// Discovers every HID service (a keyboard with a touchpad, or a ZMK board, can have more than
// one), puts each in report protocol and subscribes every input report (without a callback:
// reports reach gapEventListener by handle), as the keyboard expects from any host. Nothing
// is unsubscribed: B2 first switched off the media, system and Logitech HID++ reports, and
// the MX Keys Mini then ignored its own Easy-Switch keys while linked to the Sticky.
// The first service whose Report Map has key reports is the one decoded; the rest arrive and
// are ignored. Without one, the keyboard is switched to boot protocol and its boot keyboard
// input (always the fixed 8-byte format) is used; only a keyboard without boot support gets
// every input report decoded with the fixed 7/8-byte format. Report References are fetched
// per characteristic with getDescriptor(); B0 found that the old selection only reached
// "subscribe all" by accident, because it never fetched them.
static constexpr int MAX_HID_SERVICES = 4;

static bool setupHidConnection() {
  if (!pClient || !pClient->isConnected()) return false;

  DBG_PRINTLN("[BLE] Discovering services...");
  const auto& services = pClient->getServices(true);
  if (services.empty()) {
    DBG_PRINTLN("[BLE] Service discovery failed");
    return false;
  }
  // Local pointers: the service objects live until the next discovery, while a link that
  // drops mid-setup (the keyboard switched away) must not leave this task holding null.
  NimBLERemoteService* hid[MAX_HID_SERVICES];
  int hidCount = 0;
  for (NimBLERemoteService* sv : services)
    if (sv && sv->getUUID() == hidServiceUUID && hidCount < MAX_HID_SERVICES) hid[hidCount++] = sv;
  if (hidCount == 0) {
    DBG_PRINTLN("[BLE] HID service not found");
    return false;
  }
  BLEDUMP("HID services=%d", hidCount);

  // The keyboard's own description of its reports. Static: this runs on the connect
  // task, and the parsed map is over 1 KB.
  static uint8_t map[MAX_REPORT_MAP];
  static hidcore::ReportMap parsed;
  size_t mapLen = 0;
  hidcore::ParseResult pr = hidcore::ParseResult::Empty;

  uint16_t handles[MAX_INPUT_HANDLES];
  uint8_t ids[MAX_INPUT_HANDLES];
  int count = 0;
  int keyService = -1;               // the service whose key reports are decoded
  uint16_t all[MAX_INPUT_HANDLES];   // every subscribed input report, for the last fallback
  int allCount = 0;

  for (int si = 0; si < hidCount; si++) {
    NimBLERemoteService* svc = hid[si];
    // Report protocol first (before subscribing).
    if (NimBLERemoteCharacteristic* proto = svc->getCharacteristic(protocolModeUUID)) {
      uint8_t mode = 1;  // 1 = Report Protocol
      proto->writeValue(&mode, 1, true);
    } else {
      DBG_PRINTF("[BLE] No Protocol Mode characteristic in HID service %d\n", si);
    }
    const auto& chars = svc->getCharacteristics(true);  // true = refresh from device
    DBG_PRINTF("[BLE] HID service %d: %d characteristics\n", si, (int)chars.size());

    // This service's Report Map, until one service has given the key reports.
    const bool looking = keyService < 0;
    if (looking) {
      mapLen = 0;
      pr = hidcore::ParseResult::Empty;
      if (NimBLERemoteCharacteristic* mc = svc->getCharacteristic(reportMapUUID)) {
        NimBLEAttValue v = mc->readValue();
        mapLen = v.size() <= sizeof(map) ? v.size() : 0;
        if (mapLen) memcpy(map, v.data(), mapLen);
        pr = hidcore::parseReportMap(map, mapLen, parsed);
      }
      DBG_PRINTF("[BLE] Report Map %u bytes: %s\n", (unsigned)mapLen, hidcore::parseResultName(pr));
      BLEDUMP("MAP svc=%d parsed len=%u result=%s reports=%u keyboard=%d", si, (unsigned)mapLen,
              hidcore::parseResultName(pr), (unsigned)parsed.count, parsed.keyboardReports());
#if defined(MS_BLE_DUMP)
      if (pr == hidcore::ParseResult::Ok) dumpLayout(parsed);
#endif
    }

    for (auto& chr : chars) {
      if (chr->getUUID() != reportUUID || !chr->canNotify()) continue;
      if (!chr->subscribe(true, nullptr)) {
        DBG_PRINTF("[BLE] FAILED to subscribe to report handle=%d\n", chr->getHandle());
        continue;
      }
      if (allCount < MAX_INPUT_HANDLES) all[allCount++] = chr->getHandle();
      if (!looking || pr != hidcore::ParseResult::Ok) continue;
      uint8_t id = 0, type = 0;
      if (!readReportRef(chr, id, type) || type != 1) continue;
      const int ri = parsed.find(id);
      const bool keys = ri >= 0 && parsed.reports[ri].kind == hidcore::ReportKind::Keyboard;
      if (keys && count < MAX_INPUT_HANDLES) {
        handles[count] = chr->getHandle();
        ids[count++] = id;
        DBG_PRINTF("[BLE] Key report id=%u handle=%d\n", id, chr->getHandle());
      }
      BLEDUMP("ROUTE svc=%d h=%u id=%u %s", si, (unsigned)chr->getHandle(), id,
              keys ? "keys" : ri >= 0 ? reportKindName(parsed.reports[ri].kind) : "unknown");
    }
    if (looking && count > 0) keyService = si;   // keep this service's map
#if defined(MS_BLE_DUMP)
    dumpHidService(svc, chars);
#endif
  }

#if defined(MS_BLE_FORCE_BOOT)
  // Test builds only: skip the Report Map path to prove the boot fallback on a keyboard
  // whose map works.
  BLEDUMP("ROUTE forced fallback (MS_BLE_FORCE_BOOT)");
  count = 0;
  keyService = -1;
#endif

  // Fallback 1: no service gave key reports. Boot protocol: every keyboard that supports it
  // sends its keys on the boot keyboard input in the fixed 8-byte format.
  if (count == 0) {
    mapLen = 0;
    if (pr == hidcore::ParseResult::Ok) BLEDUMP("ROUTE no key report matched the map, falling back");
    for (int si = 0; si < hidCount && count == 0; si++) {
      NimBLERemoteCharacteristic* boot = hid[si]->getCharacteristic(bootKeyboardInUUID);
      NimBLERemoteCharacteristic* proto = hid[si]->getCharacteristic(protocolModeUUID);
      if (!boot || !proto || !boot->canNotify()) continue;
      uint8_t mode = 0;  // 0 = Boot Protocol
      if (!proto->writeValue(&mode, 1, true) || !boot->subscribe(true, nullptr)) continue;
      handles[count] = boot->getHandle();
      ids[count++] = LEGACY_REPORT_ID;
      DBG_PRINTF("[BLE] Using boot protocol (service %d)\n", si);
      BLEDUMP("ROUTE boot protocol svc=%d h=%u", si, (unsigned)boot->getHandle());
    }
  }

  // Fallback 2: no boot support either. Every input report, fixed 7/8-byte format.
  if (count == 0) {
    for (int i = 0; i < allCount; i++) {
      handles[count] = all[i];
      ids[count++] = LEGACY_REPORT_ID;
    }
    if (count) BLEDUMP("ROUTE fixed format on %d input report(s)", count);
  }

  if (count == 0) {
    DBG_PRINTLN("[BLE] No input report found");
    return false;
  }
  if (!pClient->isConnected()) return false;
  setRoutes(handles, ids, count, map, keyService >= 0 ? mapLen : 0);

  DBG_PRINTF("[BLE] HID setup complete, %d input report(s)\n", count);
  BLEDUMP("HID subscribed %d input report(s), %s", count, routeMapLen ? "decoded by the map" : "fixed format");
  return true;
}

// --- FreeRTOS task: runs connect + security + HID setup off the main loop ---

static void connectTaskExit(bool failed) {
  if (failed) {
    noteConnectFailure(keyboardAddress);
    if (pClient && pClient->isConnected()) pClient->disconnect();
    bleState = BLEState::DISCONNECTED;
  }
  connectTaskHandle = nullptr;   // the worker waits for the next connection
}

static void runConnect() {
  bleState = BLEState::CONNECTING;
  authSuccess = false;

  NimBLEAddress addr(keyboardAddress, keyboardAddressType);
  const int slot = findPairedSlot(keyboardAddress);
  const bool hadBond = NimBLEDevice::isBonded(addr);

  DBG_PRINTF("[BLE-Task] Connecting to %s type=%d\n",
             keyboardAddress.c_str(), keyboardAddressType);
  BLEDUMP("CONNECT try %s type=%u timeout=%lu bonded=%d slot=%d", keyboardAddress.c_str(),
          (unsigned)keyboardAddressType, (unsigned long)nextConnectTimeoutMs, hadBond, slot);

  // Guard: if the link layer is already up (bleState flipped to DISCONNECTED spuriously
  // while the BLE connection is still alive), don't call connect() again — that crashes
  // the NimBLE stack. Just re-sync state and exit.
  if (pClient && pClient->isConnected()) {
    DBG_PRINTLN("[BLE-Task] Already connected - re-syncing state");
    bleState = BLEState::CONNECTED;
    lastBleKeystrokeMs = millis();
    bleConnIdleMode = false;
    connectTaskExit(false);
    return;
  }

  // Create/reuse client
  if (!pClient) {
    pClient = NimBLEDevice::createClient();
    pClient->setClientCallbacks(&clientCallbacks, false);
  }
  pClient->setConnectTimeout(nextConnectTimeoutMs);
  // Back to the initial parameters: an accepted request from the last keyboard changed them.
  pClient->setConnectionParams(BLE_GAP_INITIAL_CONN_ITVL_MIN, BLE_GAP_INITIAL_CONN_ITVL_MAX,
                               BLE_GAP_INITIAL_CONN_LATENCY, BLE_GAP_INITIAL_SUPERVISION_TIMEOUT);

  // A stored keyboard we still hold a bond for can type as soon as the link is
  // encrypted: load its input handles now so the first report is not lost.
  const bool fastPath = hadBond && slot >= 0 && nvs_loadHandles(slot);

  // Step 1: Connect (blocks this task, main loop continues).
  // Bonds are kept across connects and boots. They were once deleted on every connect,
  // then at every boot, to dodge an old NimBLE crash on encrypted reconnects; that made
  // every keyboard pair from scratch after each restart, and a keyboard that only
  // re-pairs in pairing mode never came back. A bond is now dropped only when the
  // keyboard refuses it (below).
  if (!pClient->connect(addr, true)) {
    DBG_PRINTLN("[BLE-Task] Connection failed");
    BLEDUMP("CONNECT failed err=%d", pClient->getLastError());
    clearRoutes();
    connectTaskExit(true);
    return;
  }
  activeConnHandle = pClient->getConnHandle();

  DBG_PRINTLN("[BLE-Task] Connected, attempting security...");

  // Step 2: Encrypt with the stored bond, or pair (Just Works). Blocks until the
  // encryption result or a disconnect. NimBLE itself deletes the bond and pairs again
  // once when the keyboard says it no longer has the key.
  bool secured = pClient->secureConnection();
  int secErr = secured ? 0 : pClient->getLastError();
  for (int i = 0; i < 10 && secured && !authSuccess; i++) vTaskDelay(pdMS_TO_TICKS(100));
  BLEDUMP("SEC secureConnection=%d err=%d encrypted=%d", secured, secErr, (int)authSuccess);

  if (!authSuccess) {
    // A keyboard does not send reports on an unencrypted link; continuing only showed
    // "Connected" with a keyboard that typed nothing.
    if (isPairingRefused(secErr)) {
      if (NimBLEDevice::isBonded(addr)) {
        NimBLEDevice::deleteBond(addr);
        BLEDUMP("BOND dropped for %s: the keyboard refused it (err=%d)", keyboardAddress.c_str(), secErr);
      }
      if (slot >= 0) nvs_setNeedsRepair(slot, true);
      DBG_PRINTF("[BLE-Task] Pairing refused (err=%d): keyboard needs pairing mode\n", secErr);
    } else {
      DBG_PRINTF("[BLE-Task] Security failed (err=%d)\n", secErr);
    }
    connectTaskExit(true);
    return;
  }
#if defined(MS_BLE_DUMP)
  encryptedAtMs = millis();
#endif
  DBG_PRINTLN("[BLE-Task] Security succeeded");
  noteConnectSuccess(keyboardAddress);
  if (slot >= 0) nvs_setNeedsRepair(slot, false);

  // Fast path: the keyboard kept its subscriptions with the bond, so it is ready now.
  if (fastPath) {
    bleState = BLEState::CONNECTED;
    bleConnIdleMode = false;
    lastBleKeystrokeMs = millis();
    BLEDUMP("READY fast after %lu ms, %d stored input handle(s)",
            (unsigned long)(millis() - connectTaskStartMs), (int)inputHandleCount);
  }

  // Step 3: Service discovery + subscriptions. On the fast path this refreshes the
  // handles while the keyboard already types.
  DBG_PRINTLN("[BLE-Task] Setting up HID...");
  const bool hidReady = setupHidConnection();
  // A read that failed because the keyboard is leaving can return before the
  // disconnect event arrives; give the link a moment to settle before judging.
  for (int i = 0; i < 5 && !hidReady && pClient->isConnected(); i++) vTaskDelay(pdMS_TO_TICKS(100));
  if (!pClient->isConnected()) {
    // The keyboard ended the link during setup (switched to another device). Not a
    // failure of this keyboard: no backoff, wait for its next advert.
    BLEDUMP("HID setup cut short: link down");
    connectTaskExit(false);
    return;
  }
  if (!hidReady) {
    DBG_PRINTLN("[BLE-Task] HID setup failed, disconnecting");
    BLEDUMP("HID setup failed");
    connectTaskExit(true);
    return;
  }

  // Mark connected before the NVS writes below so bleLoop doesn't start a second task.
  bleState = BLEState::CONNECTED;
  bleConnIdleMode = false;
  if (!fastPath) lastBleKeystrokeMs = millis();  // Start the 3s idle timer from now, not from boot
  DBG_PRINTLN("[BLE-Task] Keyboard ready!");
  BLEDUMP("READY after %lu ms", (unsigned long)(millis() - connectTaskStartMs));

  // NOTE: updateConnParams() is intentionally NOT called here.  Calling it immediately
  // after connecting triggers a reentrancy crash with Keychron keyboards (the keyboard
  // sends its own BLE_GAP_EVENT_CONN_UPDATE_REQ at the same time).

  // Name for the stored list: the advertised name, else the GATT Device Name (Generic
  // Access 0x1800 / 0x2A00), else whatever name is stored already (storePairedDevice).
  std::string devName;
  for (auto& d : discoveredDevices) {
    if (d.address == keyboardAddress && d.name != keyboardAddress) { devName = d.name; break; }
  }
  if (devName.empty()) {
    NimBLERemoteService* gasSvc = pClient->getService(NimBLEUUID((uint16_t)0x1800));
    NimBLERemoteCharacteristic* nameChr =
        gasSvc ? gasSvc->getCharacteristic(NimBLEUUID((uint16_t)0x2A00)) : nullptr;
    if (nameChr) devName = nameChr->readValue();
  }
  storePairedDevice(keyboardAddress, devName.empty() ? keyboardAddress : devName);
  int stored = findPairedSlot(keyboardAddress);
  if (stored >= 0) nvs_saveHandles(stored);
#if defined(MS_BLE_DUMP)
  dumpDeviceInfo();
#endif

  connectTaskExit(false);
}

// Launch the connect task (non-blocking from main loop's perspective)
static void connectWorkerLoop(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    runConnect();
    connectTaskHandle = nullptr;   // every path ends in connectTaskExit(); this is a guard
  }
}

// 32768 bytes: Logitech has a complex service tree (keyboard + media + battery reports
// + many descriptors). NimBLE 2.x uses more per-call stack than 1.4.x — 12288 was
// sufficient before but overflows during full service discovery on the Logitech.
// Raised from 20480: keyboards with even heavier service trees (e.g. some split/ZMK
// boards) still panicked during discovery at 20480.
static bool ensureConnectWorker() {
  if (connectWorker) return true;
  if (xTaskCreate(connectWorkerLoop, "ble_conn", 32768, NULL, 1, &connectWorker) == pdPASS) return true;
  connectWorker = nullptr;
  DBG_PRINTF("[BLE] Could not start the connect task: %u bytes free, largest block %u\n",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  return false;
}

// Starts a connection on the worker. False when the worker is busy or could not be made.
static bool startConnectTask() {
  if (connectTaskHandle != nullptr) {
    DBG_PRINTLN("[BLE] Connect task already running");
    return false;
  }
  if (!ensureConnectWorker()) {
    connectRetryAtMs = millis() + 2000;   // do not retry on every advert
    return false;
  }
  bgScanStop();   // the radio connects; a scan in progress would refuse it
  connectTaskStartMs = millis();  // for the wedged-task recovery guard in bleLoop()
  connectTaskHandle = connectWorker;
  xTaskNotifyGive(connectWorker);
  return true;
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

// Routing of a stored keyboard, for the fast reconnect: kb_N_hnd (uint16 input report
// handles), kb_N_rid (report ID per handle, 0xFF = fixed format) and kb_N_map (its
// Report Map). They are attribute handles and the keyboard's own report description,
// not keys, but stay in NVS with the rest of the per-keyboard data; the SD backup does
// not carry them. A keyboard saved before B2 has no rid/map and is decoded with the
// fixed format until its next full setup saves them.
static size_t nvs_blobLen(const char* key) {
  return prefs.isKey(key) ? prefs.getBytesLength(key) : 0;
}

// Writes a blob only when it changed (len 0 removes it): no flash wear on every connect.
static void nvs_putBlobIfChanged(const char* key, const void* data, size_t len) {
  static uint8_t old[MAX_REPORT_MAP];
  if (len == 0) { if (prefs.isKey(key)) prefs.remove(key); return; }
  if (nvs_blobLen(key) == len && len <= sizeof(old)) {
    prefs.getBytes(key, old, len);
    if (memcmp(old, data, len) == 0) return;
  }
  prefs.putBytes(key, data, len);
}

static bool nvs_loadHandles(int idx) {
  char key[16]; nvs_kbKey(key, idx, "hnd");
  uint16_t h[MAX_INPUT_HANDLES];
  size_t len = nvs_blobLen(key);
  if (len < sizeof(uint16_t) || len > sizeof(h) || len % sizeof(uint16_t)) return false;
  prefs.getBytes(key, h, len);
  const int n = (int)(len / sizeof(uint16_t));

  uint8_t ids[MAX_INPUT_HANDLES];
  memset(ids, LEGACY_REPORT_ID, sizeof(ids));
  static uint8_t map[MAX_REPORT_MAP];
  size_t mapLen = 0;
  nvs_kbKey(key, idx, "rid");
  if (nvs_blobLen(key) == (size_t)n) {
    prefs.getBytes(key, ids, n);
    nvs_kbKey(key, idx, "map");
    mapLen = nvs_blobLen(key);
    if (mapLen > sizeof(map)) mapLen = 0;
    if (mapLen) prefs.getBytes(key, map, mapLen);
  }
  if (!mapLen) memset(ids, LEGACY_REPORT_ID, sizeof(ids));
  setRoutes(h, ids, n, map, mapLen);
  return true;
}

static void nvs_saveHandles(int idx) {
  uint16_t h[MAX_INPUT_HANDLES];
  int n = inputHandleCount;
  if (n == 0) return;   // the link dropped meanwhile: keep what is stored
  for (int i = 0; i < n; i++) h[i] = inputHandles[i];
  char key[16];
  nvs_kbKey(key, idx, "hnd"); nvs_putBlobIfChanged(key, h, n * sizeof(uint16_t));
  nvs_kbKey(key, idx, "rid"); nvs_putBlobIfChanged(key, routeIds, routeMapLen ? (size_t)n : 0);
  nvs_kbKey(key, idx, "map"); nvs_putBlobIfChanged(key, routeMap, n ? routeMapLen : 0);
}

// kb_N_rp: the keyboard refused its bond or a new pairing; it has to be put in pairing
// mode. Shown as "Re-pair" in Paired keyboards; cleared on the next good connection.
static bool nvs_loadNeedsRepair(int idx) {
  char key[16]; nvs_kbKey(key, idx, "rp");
  return prefs.getUChar(key, 0) != 0;
}

static void nvs_setNeedsRepair(int idx, bool on) {
  if (nvs_loadNeedsRepair(idx) == on) return;
  char key[16]; nvs_kbKey(key, idx, "rp");
  if (on) prefs.putUChar(key, 1);
  else prefs.remove(key);
  extern bool screenDirty;
  screenDirty = true;
}

static void nvs_clearSlot(int idx) {
  static const char* const kSuffixes[] = {"addr", "name", "type", "hnd", "rid", "map", "rp"};
  char key[16];
  for (const char* s : kSuffixes) {
    nvs_kbKey(key, idx, s);
    if (prefs.isKey(key)) prefs.remove(key);
  }
}

// Moves everything stored for slot src into slot dst (list shifts on remove/evict).
static void nvs_moveSlot(int dst, int src) {
  nvs_saveKb(dst, nvs_loadAddr(src), nvs_loadName(src), nvs_loadType(src));
  static uint8_t buf[MAX_REPORT_MAP];
  char sk[16], dk[16];
  for (const char* s : {"hnd", "rid", "map"}) {
    nvs_kbKey(sk, src, s); nvs_kbKey(dk, dst, s);
    size_t len = nvs_blobLen(sk);
    if (len > 0 && len <= sizeof(buf)) { prefs.getBytes(sk, buf, len); prefs.putBytes(dk, buf, len); }
    else if (prefs.isKey(dk)) prefs.remove(dk);
  }
  nvs_kbKey(dk, dst, "rp");
  if (nvs_loadNeedsRepair(src)) prefs.putUChar(dk, 1);
  else if (prefs.isKey(dk)) prefs.remove(dk);
}

static int findPairedSlot(const std::string& addr) {
  int count = nvs_loadCount();
  for (int i = 0; i < count; i++) {
    if (nvs_loadAddr(i) == addr) return i;
  }
  return -1;
}

// --- SD card backup for BLE pairing ---

static constexpr char BLE_BACKUP_PATH[] = "/microslate/ble_kb.json";

// Set from the ble_conn task when a new keyboard is stored; consumed by bleLoop()
// on the main task, which owns all SPI (display + SD) access.
static volatile bool bleBackupPending = false;

static void writeBleBackup() {
    static char buf[512];
    int count = nvs_loadCount();
    int last  = (int)prefs.getUChar("last_kb", 0);
    snprintf(buf, sizeof(buf), "{\"count\":%d,\"last\":%d", count, last);
    for (int i = 0; i < count; i++) {
        std::string addr = nvs_loadAddr(i);
        std::string name = nvs_loadName(i);
        uint8_t     type = nvs_loadType(i);
        char tmp[16];
        snprintf(tmp, sizeof(tmp), ",\"a%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), addr.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
        snprintf(tmp, sizeof(tmp), ",\"n%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), name.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
        snprintf(tmp, sizeof(tmp), ",\"t%d\":%d", i, type);
        strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, "}", sizeof(buf) - strlen(buf) - 1);
    if (!SdMan.exists("/microslate")) SdMan.mkdir("/microslate");
    sdWriteFile(BLE_BACKUP_PATH, buf);
}

static void restoreBleBackup() {
    static char buf[512];
    if (!sdReadFile(BLE_BACKUP_PATH, buf, sizeof(buf))) return;
    int count = jsonGetInt(buf, "count");
    int last  = jsonGetInt(buf, "last");
    if (count < 0) return;
    if (count > MAX_PAIRED_KEYBOARDS) count = MAX_PAIRED_KEYBOARDS;   // a damaged or hand-edited backup
    if (last >= count) last = -1;
    for (int i = 0; i < count && i < MAX_PAIRED_KEYBOARDS; i++) {
        char aKey[8], nKey[8], tKey[8];
        snprintf(aKey, sizeof(aKey), "a%d", i);
        snprintf(nKey, sizeof(nKey), "n%d", i);
        snprintf(tKey, sizeof(tKey), "t%d", i);
        char addr[32] = "", name[64] = "";
        jsonGetStr(buf, aKey, addr, sizeof(addr));
        jsonGetStr(buf, nKey, name, sizeof(name));
        int type = jsonGetInt(buf, tKey);
        if (addr[0]) nvs_saveKb(i, addr, name, (uint8_t)(type >= 0 ? type : 0));
    }
    prefs.putUChar("kb_count", (uint8_t)count);
    if (last >= 0) prefs.putUChar("last_kb", (uint8_t)last);
    DBG_PRINTF("[BLE] Restored %d paired keyboard(s) from SD backup\n", count);
}

// --- Public API ---

uint32_t getCurrentPasskey() {
  return currentPasskey;
}

// --- BLE transmit power -----------------------------------------------------
// Discrete steps in dBm, low->high. -9 is the battery-friendly default (verified
// rock-solid with the author's keyboards); +9 is the chip maximum for weak or
// distant keyboards whose own transmitter / antenna leaves the link at the edge.
// Radio power is a real share of this device's battery budget, so the default
// stays low and higher steps are strictly opt-in via Settings -> BLE Power.
static const int8_t kTxPowerSteps[] = { -9, -3, 0, 3, 9 };
static constexpr int kTxPowerStepCount =
    sizeof(kTxPowerSteps) / sizeof(kTxPowerSteps[0]);
static int g_txPowerIdx = 0;  // index into kTxPowerSteps; 0 => -9 dBm

static void applyBleTxPower() {
  NimBLEDevice::setPower(kTxPowerSteps[g_txPowerIdx]);
}

const char* getBleTxPowerLabel() {
  static char buf[20];
  const char* tag = "";
  if (g_txPowerIdx == 0) tag = " (Battery)";
  else if (g_txPowerIdx == kTxPowerStepCount - 1) tag = " (Max)";
  snprintf(buf, sizeof(buf), "%+d dBm%s", (int)kTxPowerSteps[g_txPowerIdx], tag);
  return buf;
}

void cycleBleTxPower(int dir) {
  g_txPowerIdx =
      (g_txPowerIdx + (dir >= 0 ? 1 : -1) + kTxPowerStepCount) % kTxPowerStepCount;
  applyBleTxPower();                                  // takes effect live (global)
  prefs.putChar("tx_power", kTxPowerSteps[g_txPowerIdx]);  // persist chosen dBm
}

void bleSetup() {
  NimBLEDevice::init("MicroSlate");
  // bond=true, MITM=false (we don't require it), SC=true (LE Secure Connections).
  // Upstream offered only Legacy Pairing (SC=false); keyboards advertising BT 5.0
  // often *require* SC and refuse Legacy outright — the GAP link comes up but
  // pairing is rejected, so the device reports "Connected" while the keyboard
  // sends nothing and no bond is stored. SC=true fixes that while MITM=false +
  // NO_INPUT_OUTPUT keeps it "Just Works" (no passkey), so Logitech/Keychron etc.
  // all still pair without prompting.
  NimBLEDevice::setSecurityAuth(true, false, true);
  // NO_INPUT_OUTPUT = "Just Works" pairing — works for both Logitech and Keychron
  // since MITM=false means we never require authenticated pairing regardless of IO cap.
  // DISPLAY_YESNO was tried for Keychron but broke Logitech (bond/security-state mismatch
  // in NimBLE 2.x caused crashes on reconnect). Just Works is sufficient for all targets.
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);
  // ENC-only key distribution — omit ID (IRK) which some keyboards don't support
  // and which caused bond-validation failures after the security param change.
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC);

  // Keyboard reports are taken from the GAP event stream by handle (gapEventListener).
  NimBLEDevice::setCustomGapHandler(gapEventListener);

  prefs.begin("ble_kb", false);

  // TX power: restore saved level (default -9 dBm). Applied after prefs opens so
  // the user's Settings -> BLE Power choice survives reboots.
  int8_t savedTx = prefs.getChar("tx_power", -9);
  g_txPowerIdx = 0;
  for (int i = 0; i < kTxPowerStepCount; i++) {
    if (kTxPowerSteps[i] == savedTx) { g_txPowerIdx = i; break; }
  }
  applyBleTxPower();

  // Restore from SD backup if NVS was wiped (e.g. after a firmware flash)
  BLEDUMP("BOOT own=%s nvsList=%d", NimBLEDevice::getAddress().toString().c_str(), prefs.isKey("kb_count"));
  if (!prefs.isKey("kb_count")) {
    BLEDUMP("BOOT keyboard list missing in NVS, restoring from SD backup");
    restoreBleBackup();
  }

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

  int pairedCount = nvs_loadCount();
#if defined(MS_BLE_DUMP)
  for (int i = 0; i < pairedCount; i++) {
    char hk[16], mk[16]; nvs_kbKey(hk, i, "hnd"); nvs_kbKey(mk, i, "map");
    BLEDUMP("LIST %d %s type=%u bonded=%d repair=%d handles=%u map=%u name=\"%s\"", i, nvs_loadAddr(i).c_str(),
            (unsigned)nvs_loadType(i), NimBLEDevice::isBonded(NimBLEAddress(nvs_loadAddr(i), nvs_loadType(i))),
            nvs_loadNeedsRepair(i), (unsigned)(nvs_blobLen(hk) / 2), (unsigned)nvs_blobLen(mk),
            nvs_loadName(i).c_str());
  }
  dumpBonds("at boot (kept)");
#endif

  bleState = BLEState::DISCONNECTED;
  ensureConnectWorker();   // now, while memory is plentiful
  if (pairedCount > 0) {
    DBG_PRINTF("[BLE] %d paired keyboard(s); waiting for one to advertise\n", pairedCount);
  } else {
    DBG_PRINTLN("[BLE] No paired keyboards");
  }
}

void bleLoop() {
  // Flush a deferred SD backup requested by the ble_conn task (first-time pairing).
  // Runs here on the main task so it can never overlap an e-ink refresh on the
  // shared SPI bus. Wait until the connect task has fully exited.
  if (bleBackupPending && connectTaskHandle == nullptr) {
    bleBackupPending = false;
    writeBleBackup();
  }

  // Detect when a one-shot scan finishes
  if (isScanning && !NimBLEDevice::getScan()->isScanning()) {
    isScanning = false;
    continuousScanning = false;
    DBG_PRINTF("[BLE] Scan complete — found %d devices\n", (int)discoveredDevices.size());
    // Trigger screen refresh to show final results
    extern bool screenDirty;
    screenDirty = true;
  }
  // The background scan ends on its own if the host resets; start it again below.
  if (bgScanning && !NimBLEDevice::getScan()->isScanning()) bgScanning = false;

  // Recover from a wedged connect task: if it has run far longer than its own internal
  // timeouts allow (10s connect + 5s auth + discovery), assume the NimBLE host stalled
  // and release the gate so auto-reconnect can try again instead of the keyboard staying
  // dead until reboot. Best-effort: we can't safely kill the task mid-call; when it
  // eventually unwinds it takes the next connection, and the main loop keeps running.
  if (connectTaskHandle != nullptr && bleState != BLEState::CONNECTED
      && millis() - connectTaskStartMs > CONNECT_TASK_TIMEOUT_MS) {
    DBG_PRINTLN("[BLE] Connect task exceeded 30s — assuming wedged, resetting state");
    BLEDUMP("CONNECT task wedged over 30 s, state reset");
    connectTaskHandle = nullptr;       // release the reconnect gate
    bleState = BLEState::DISCONNECTED;
    if (pClient && pClient->isConnected()) pClient->disconnect();
  }

  // Launch connect task if requested (non-blocking)
  if (connectToKeyboard && bleState != BLEState::CONNECTED && connectTaskHandle == nullptr) {
    connectToKeyboard = false;
    startConnectTask();
    return;
  }

  // Auto-reconnect: listen for any stored keyboard and connect when one advertises.
  const bool waiting = bleState == BLEState::DISCONNECTED && autoReconnectEnabled &&
                       connectTaskHandle == nullptr && !isScanning && nvs_loadCount() > 0;
  if (!waiting) {
    if (bgScanning && (bleState == BLEState::CONNECTED || !autoReconnectEnabled)) bgScanStop();
    return;
  }
  if (connectRetryAtMs && (long)(millis() - connectRetryAtMs) < 0) return;
  connectRetryAtMs = 0;
  if (bgHitPending) {
    ble_addr_t hit;
    portENTER_CRITICAL(&bgHitMux);
    hit = bgHitAddr;
    bgHitPending = false;
    portEXIT_CRITICAL(&bgHitMux);
    NimBLEAddress a(hit);
    std::string addr = a.toString();
    if (retryAllowed(addr)) {
      keyboardAddress = addr;
      keyboardAddressType = a.getType();
      nextConnectTimeoutMs = RECONNECT_CONNECT_TIMEOUT_MS;
      DBG_PRINTF("[BLE] Stored keyboard advertising, connecting: %s\n", addr.c_str());
      startConnectTask();
      return;
    }
  }
  if (!bgScanning) bgScanStart();
}

bool isKeyboardConnected() {
  return bleState == BLEState::CONNECTED;
}

bool bleConnectBusy() {
  return connectToKeyboard || connectTaskHandle != nullptr;
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

void startDeviceScan(bool keepList) {
  cancelPendingConnection();

  bgScanStop();
  NimBLEDevice::getScan()->stop();
  if (!keepList) discoveredDevices.clear();
  NimBLEDevice::getScan()->clearResults();

  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&scanCallbacks, true);
  // Undo the background scan's settings: every device, active, results kept.
  scan->setFilterPolicy(BLE_HCI_SCAN_FILT_NO_WL);
  scan->setInterval(1349);
  scan->setWindow(449);
  scan->setMaxResults(0xFF);
  scan->setDuplicateFilter(1);
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
  bgScanning = false;
}

int getDiscoveredDeviceCount() {
  return discoveredDevices.size();
}

BleDeviceInfo* getDiscoveredDevices() {
  return discoveredDevices.empty() ? nullptr : discoveredDevices.data();
}

bool bleDeviceIsLikelyKeyboard(const BleDeviceInfo& d) {
  if (d.isHid) return true;
  // BLE appearance values: 0x03C0 = Generic HID, 0x03C1 = Keyboard
  if (d.appearance == 0x03C0 || d.appearance == 0x03C1) return true;
  return false;
}

static bool bleDeviceHasName(const BleDeviceInfo& d) {
  return !d.name.empty() && d.name != d.address;
}

// 0 = likely keyboard, 1 = has a name, 2 = address only.
static int bleDeviceRank(const BleDeviceInfo& d) {
  if (bleDeviceIsLikelyKeyboard(d)) return 0;
  return bleDeviceHasName(d) ? 1 : 2;
}

int getVisibleDeviceIndices(bool showAll, int* idxOut, int maxOut,
                            int* outTotal, int* outKeyboards, int* outHidden) {
  const int total = (int)discoveredDevices.size();
  int kbd = 0, named = 0;
  for (auto& d : discoveredDevices) {
    const int r = bleDeviceRank(d);
    if (r == 0) kbd++;
    else if (r == 1) named++;
  }
  // The filter keeps keyboards; with none, devices that have a name (a crowded room is full of nameless phones and
  // beacons, and a keyboard in pairing mode nearly always sends its name); with neither, everything.
  const int keepRank = kbd > 0 ? 0 : (named > 0 ? 1 : 2);
  const int filtered = keepRank == 0 ? kbd : (keepRank == 1 ? named : total);
  if (outTotal) *outTotal = total;
  if (outKeyboards) *outKeyboards = kbd;
  if (outHidden) *outHidden = total - filtered;

  // Best first, sorted before the cap so a keyboard is never cut off by a long list: keyboards, named devices, the
  // rest; the strongest signal (the nearest device) first within each.
  std::vector<int> order;
  order.reserve(total);
  for (int i = 0; i < total; i++) {
    if (showAll || bleDeviceRank(discoveredDevices[i]) <= keepRank) order.push_back(i);
  }
  std::stable_sort(order.begin(), order.end(), [](int a, int b) {
    const BleDeviceInfo& da = discoveredDevices[a];
    const BleDeviceInfo& db = discoveredDevices[b];
    const int ra = bleDeviceRank(da), rb = bleDeviceRank(db);
    if (ra != rb) return ra < rb;
    return da.rssi > db.rssi;
  });
  const int n = (int)order.size() < maxOut ? (int)order.size() : maxOut;
  for (int i = 0; i < n; i++) idxOut[i] = order[i];
  return n;
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
  nextConnectTimeoutMs = CONNECT_TIMEOUT_MS;   // user-initiated pairing: allow full time
  noteConnectSuccess(keyboardAddress);         // a deliberate pick skips any backoff
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
  clearRoutes();
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

// True when the keyboard refused its bond, or when there is no bond at all (an entry
// restored from the SD backup, or one left from before bonds were kept): either way it
// has to be put in pairing mode before it can type here.
bool pairedKeyboardNeedsRepair(int index) {
  if (index < 0 || index >= nvs_loadCount()) return false;
  if (nvs_loadNeedsRepair(index)) return true;
  std::string addr = nvs_loadAddr(index);
  return !addr.empty() && !NimBLEDevice::isBonded(NimBLEAddress(addr, nvs_loadType(index)));
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
    nvs_moveSlot(i, i + 1);
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

  bgScanStop();   // the allow list is rebuilt from the new list on the next start
  DBG_PRINTF("[BLE] Removed paired keyboard slot %d (%s)\n", index, addr.c_str());
  writeBleBackup();
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
  nextConnectTimeoutMs = CONNECT_TIMEOUT_MS;   // user-initiated: allow full time
  noteConnectSuccess(keyboardAddress);         // a deliberate pick skips any backoff
  connectToKeyboard = true;
  DBG_PRINTF("[BLE] Switching to paired keyboard %d: %s\n", index, keyboardAddress.c_str());
  writeBleBackup();
}

static void storePairedDevice(const std::string& address, const std::string& name) {
  int count = nvs_loadCount();

  // Update name if already stored. An address-only name (nothing better was found)
  // never replaces a real stored name; that is how names used to turn into addresses.
  for (int i = 0; i < count; i++) {
    if (nvs_loadAddr(i) == address) {
      std::string keep = nvs_loadName(i);
      bool keepStored = (name == address && !keep.empty() && keep != address);
      nvs_saveKb(i, address, keepStored ? keep : name, keyboardAddressType);
      prefs.putUChar("last_kb", (uint8_t)i);
      DBG_PRINTF("[BLE] Updated paired keyboard slot %d: %s\n", i, (keepStored ? keep : name).c_str());
      if (!keepStored && keep != name) bleBackupPending = true;
      return;
    }
  }

  // The same keyboard paired again under a new address: reuse the slot of a stored entry
  // that has no bond and the same real name, since that entry can never connect again.
  // Not by address: the MX Keys Mini's channels use neighbouring addresses (...be:5d,
  // be:61, be:63), and a nearly equal address turned out to be another live channel.
  int idx = -1;
  for (int i = 0; i < count && idx < 0; i++) {
    std::string a = nvs_loadAddr(i);
    if (a.empty() || NimBLEDevice::isBonded(NimBLEAddress(a, nvs_loadType(i)))) continue;
    if (name != address && nvs_loadName(i) == name) idx = i;
  }
  if (idx >= 0) {
    DBG_PRINTF("[BLE] Replacing stale entry slot %d (%s) with %s\n", idx, nvs_loadAddr(idx).c_str(), address.c_str());
    BLEDUMP("LIST replace slot %d %s -> %s", idx, nvs_loadAddr(idx).c_str(), address.c_str());
    nvs_clearSlot(idx);
  } else if (count < MAX_PAIRED_KEYBOARDS) {
    idx = count;
    prefs.putUChar("kb_count", (uint8_t)(count + 1));
  } else {
    // List full: evict slot 0 (oldest), shift everything down
    std::string evicted = nvs_loadAddr(0);
    if (!evicted.empty()) NimBLEDevice::deleteBond(NimBLEAddress(evicted, nvs_loadType(0)));
    for (int i = 0; i < MAX_PAIRED_KEYBOARDS - 1; i++) {
      nvs_moveSlot(i, i + 1);
    }
    idx = MAX_PAIRED_KEYBOARDS - 1;
    nvs_clearSlot(idx);
  }

  nvs_saveKb(idx, address, name, keyboardAddressType);
  prefs.putUChar("last_kb", (uint8_t)idx);
  DBG_PRINTF("[BLE] Stored new paired keyboard slot %d: %s (%s)\n",
             idx, name.c_str(), address.c_str());
  // Do NOT write the SD backup here: this function runs inside the ble_conn task
  // during a first-time pairing, while the main task may be mid e-ink refresh.
  // The SD card and display share the SPI bus, and concurrent access from two
  // tasks corrupts the bus and crashes the device. Defer to bleLoop() (main task).
  bleBackupPending = true;
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
