#include "diag/mem_diag.h"
#include "wifi_sync.h"
#include "config.h"
#include "file_manager.h"
#include "input_handler.h"
#include "sd_backup.h"
#include "sync/cloud_service.h"

#include <Arduino.h>
#include <WiFi.h>
#include <SDCardManager.h>
#include <Preferences.h>

// --- Internal state ---
static bool syncActive = false;
static SyncState syncState = SyncState::SCANNING;
static char statusText[64] = "";

extern bool screenDirty;

// --- Network list ---
static constexpr int MAX_NETWORKS = 20;
struct NetworkInfo {
  char ssid[33];
  int  rssi;
  bool encrypted;
  bool saved;  // Has stored password in NVS
};
static NetworkInfo networks[MAX_NETWORKS];
static int networkCount = 0;
static int selectedNet = 0;

// --- Password entry ---
static constexpr int MAX_PASSWORD_LEN = 63;
static char passwordBuf[MAX_PASSWORD_LEN + 1] = "";
static int  passwordLen = 0;

// --- NVS credential storage ---
static Preferences wifiPrefs;
static constexpr int MAX_SAVED_NETWORKS = 4;

static void loadSavedCredentials();
static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize);
static void saveCredential(const char* ssid, const char* pass);
static void forgetCredential(const char* ssid);

// --- Connecting state ---
static unsigned long connectStartMs = 0;
static char connectingSSID[33] = "";
static bool usedSavedPassword = false;
static bool autoConnectAttempted = false;  // True if we tried auto-connect with saved creds
static bool skipAutoConnectOnce = false;   // the user chose "Other network": show the list, do not auto-connect again
static bool addMode = false;               // opened from Settings to add a network (wifiAddStart)
static bool savedLoaded = false;           // the Settings list cache below is current

// --- Forward declarations ---
static void beginScan();
static void beginConnect(const char* ssid, const char* pass);
static void enterSyncingState();

// =========================================================================
// SD card backup of the saved networks: their names only. Passwords stay in NVS (Josh, 2026-11-01): a lost or
// lent card must not give away the home network. After a wipe the networks come back without passwords; the first
// join of each asks for it (getSavedPassword says there is none until then).
// =========================================================================

static constexpr char WIFI_BACKUP_PATH[] = "/microslate/wifi.json";

static void writeWifiBackup() {
    static char buf[512];
    int count = wifiPrefs.getInt("wifi_count", 0);
    snprintf(buf, sizeof(buf), "{\"count\":%d", count);
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
        char sKey[16], pKey[16];
        snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
        snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
        String ssid = wifiPrefs.getString(sKey, "");
        char tmp[16];
        snprintf(tmp, sizeof(tmp), ",\"s%d\":\"", i);  strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        jsonAppendStr(buf, sizeof(buf), ssid.c_str());  strncat(buf, "\"", sizeof(buf) - strlen(buf) - 1);
    }
    strncat(buf, "}", sizeof(buf) - strlen(buf) - 1);
    if (!SdMan.exists("/microslate")) SdMan.mkdir("/microslate");
    sdWriteFile(WIFI_BACKUP_PATH, buf);
}

static void restoreWifiBackup() {
    static char buf[512];
    if (!sdReadFile(WIFI_BACKUP_PATH, buf, sizeof(buf))) return;
    int count = jsonGetInt(buf, "count");
    if (count <= 0) return;
    if (count > MAX_SAVED_NETWORKS) count = MAX_SAVED_NETWORKS;   // a damaged or hand-edited backup
    for (int i = 0; i < count; i++) {
        char sKey[16], pKey[16], sNvs[16], pNvs[16];
        snprintf(sKey, sizeof(sKey), "s%d", i);
        snprintf(pKey, sizeof(pKey), "p%d", i);
        snprintf(sNvs, sizeof(sNvs), "wifi_ssid_%d", i);
        snprintf(pNvs, sizeof(pNvs), "wifi_pass_%d", i);
        char ssid[64] = "", pass[128] = "";
        jsonGetStr(buf, sKey, ssid, sizeof(ssid));
        const bool hasPass = jsonGetStr(buf, pKey, pass, sizeof(pass));   // only in a backup from before 2026-11
        if (ssid[0]) {
            wifiPrefs.putString(sNvs, ssid);
            if (hasPass) wifiPrefs.putString(pNvs, pass);
            else if (wifiPrefs.isKey(pNvs)) wifiPrefs.remove(pNvs);   // no password yet: asked on the first join
        }
    }
    wifiPrefs.putInt("wifi_count", count);
    DBG_PRINTF("[SYNC] Restored %d WiFi network(s) from SD backup\n", count);
}

// The saved networks: from the card after a wipe; and a backup written before 2026-11 (with passwords) is
// rewritten with names only.
static void wifiBackupCheck() {
    if (!wifiPrefs.isKey("wifi_count")) { restoreWifiBackup(); return; }
    static char buf[512];
    if (sdReadFile(WIFI_BACKUP_PATH, buf, sizeof(buf)) && strstr(buf, "\"p0\"")) {
        writeWifiBackup();
        DBG_PRINTLN("[WIFI] backup on the card rewritten without passwords");
    }
}

// =========================================================================
// NVS credential storage
// =========================================================================

static void loadSavedCredentials() {
  // Mark networks that have saved passwords
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < networkCount; i++) {
    networks[i].saved = false;
    for (int j = 0; j < count && j < MAX_SAVED_NETWORKS; j++) {
      char key[16];
      snprintf(key, sizeof(key), "wifi_ssid_%d", j);
      String savedSSID = wifiPrefs.getString(key, "");
      if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), networks[i].ssid) == 0) {
        networks[i].saved = true;
        break;
      }
    }
  }
}

static bool getSavedPassword(const char* ssid, char* passBuf, int passBufSize) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      if (!wifiPrefs.isKey(pKey)) return false;   // restored from the card without its password: ask for it
      String savedPass = wifiPrefs.getString(pKey, "");
      strncpy(passBuf, savedPass.c_str(), passBufSize - 1);
      passBuf[passBufSize - 1] = '\0';
      return true;
    }
  }
  return false;
}

static void saveCredential(const char* ssid, const char* pass) {
  int count = wifiPrefs.getInt("wifi_count", 0);

  // Check if already saved — update in place
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      wifiPrefs.putString(pKey, pass);
      savedLoaded = false;
      return;
    }
  }

  // Add new entry (wrap around if full)
  int slot = count < MAX_SAVED_NETWORKS ? count : (count % MAX_SAVED_NETWORKS);
  char sKey[16], pKey[16];
  snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", slot);
  snprintf(pKey, sizeof(pKey), "wifi_pass_%d", slot);
  wifiPrefs.putString(sKey, ssid);
  wifiPrefs.putString(pKey, pass);
  if (count < MAX_SAVED_NETWORKS) {
    wifiPrefs.putInt("wifi_count", count + 1);
  }
  savedLoaded = false;
  writeWifiBackup();
}

static void forgetCredential(const char* ssid) {
  int count = wifiPrefs.getInt("wifi_count", 0);
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
    char sKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    String savedSSID = wifiPrefs.getString(sKey, "");
    if (savedSSID.length() > 0 && strcmp(savedSSID.c_str(), ssid) == 0) {
      // Shift remaining entries down
      for (int j = i; j < count - 1 && j < MAX_SAVED_NETWORKS - 1; j++) {
        char srcS[16], srcP[16], dstS[16], dstP[16];
        snprintf(srcS, sizeof(srcS), "wifi_ssid_%d", j + 1);
        snprintf(srcP, sizeof(srcP), "wifi_pass_%d", j + 1);
        snprintf(dstS, sizeof(dstS), "wifi_ssid_%d", j);
        snprintf(dstP, sizeof(dstP), "wifi_pass_%d", j);
        wifiPrefs.putString(dstS, wifiPrefs.getString(srcS, ""));
        if (wifiPrefs.isKey(srcP)) wifiPrefs.putString(dstP, wifiPrefs.getString(srcP, ""));
        else if (wifiPrefs.isKey(dstP)) wifiPrefs.remove(dstP);   // a network still waiting for its password
      }
      // Clear last slot
      int lastIdx = count - 1;
      char lastS[16], lastP[16];
      snprintf(lastS, sizeof(lastS), "wifi_ssid_%d", lastIdx);
      snprintf(lastP, sizeof(lastP), "wifi_pass_%d", lastIdx);
      wifiPrefs.remove(lastS);
      wifiPrefs.remove(lastP);
      wifiPrefs.putInt("wifi_count", count - 1);
      if (wifiPrefs.getString("wifi_last", "") == ssid) wifiPrefs.remove("wifi_last");
      savedLoaded = false;
      writeWifiBackup();
      return;
    }
  }
}

// =========================================================================
// WiFi scanning
// =========================================================================

static void beginScan() {
  syncState = SyncState::SCANNING;
  strcpy(statusText, "Scanning...");
  networkCount = 0;
  selectedNet = 0;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.scanNetworks(true);  // async scan
  screenDirty = true;
  DBG_PRINTLN("[SYNC] WiFi scan started");
}

static void processScanResults() {
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;  // Still scanning

  if (n <= 0) {
    // Scan failed or no networks found
    networkCount = 0;
    syncState = SyncState::NETWORK_LIST;
    strcpy(statusText, n == 0 ? "No networks found" : "Scan failed");
    WiFi.scanDelete();
    screenDirty = true;
    return;
  }

  // Deduplicate by SSID, keeping strongest signal
  networkCount = 0;
  for (int i = 0; i < n && networkCount < MAX_NETWORKS; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;  // Skip hidden networks

    // Check for duplicate
    bool duplicate = false;
    for (int j = 0; j < networkCount; j++) {
      if (strcmp(networks[j].ssid, ssid.c_str()) == 0) {
        duplicate = true;
        if (WiFi.RSSI(i) > networks[j].rssi) {
          networks[j].rssi = WiFi.RSSI(i);
        }
        break;
      }
    }
    if (duplicate) continue;

    strncpy(networks[networkCount].ssid, ssid.c_str(), 32);
    networks[networkCount].ssid[32] = '\0';
    networks[networkCount].rssi = WiFi.RSSI(i);
    networks[networkCount].encrypted = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
    networks[networkCount].saved = false;
    networkCount++;
  }

  WiFi.scanDelete();

  // Mark saved networks
  loadSavedCredentials();

  // Sort: saved networks first, then by signal strength
  for (int i = 0; i < networkCount - 1; i++) {
    for (int j = i + 1; j < networkCount; j++) {
      bool swap = false;
      if (networks[j].saved && !networks[i].saved) {
        swap = true;
      } else if (networks[j].saved == networks[i].saved && networks[j].rssi > networks[i].rssi) {
        swap = true;
      }
      if (swap) {
        NetworkInfo tmp = networks[i];
        networks[i] = networks[j];
        networks[j] = tmp;
      }
    }
  }

  selectedNet = 0;
  syncState = SyncState::NETWORK_LIST;
  statusText[0] = '\0';
  screenDirty = true;
  DBG_PRINTF("[SYNC] Found %d networks\n", networkCount);

  // Intelligent auto-connect: if the strongest available network is saved, auto-connect
  const bool skipAuto = skipAutoConnectOnce;
  skipAutoConnectOnce = false;
  if (!skipAuto && networkCount > 0 && networks[0].saved) {
    char savedPass[MAX_PASSWORD_LEN + 1];
    if (getSavedPassword(networks[0].ssid, savedPass, sizeof(savedPass))) {
      usedSavedPassword = true;
      autoConnectAttempted = true;
      beginConnect(networks[0].ssid, savedPass);
      DBG_PRINTF("[SYNC] Auto-connecting to strongest saved network: %s\n", networks[0].ssid);
      return;
    }
  }
}

// =========================================================================
// Connection
// =========================================================================

static void beginConnect(const char* ssid, const char* pass) {
  strncpy(connectingSSID, ssid, 32);
  connectingSSID[32] = '\0';
  syncState = SyncState::CONNECTING;
  snprintf(statusText, sizeof(statusText), "Connecting to %s...", ssid);
  connectStartMs = millis();

  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(ssid, pass);
  screenDirty = true;
  DBG_PRINTF("[SYNC] Connecting to %s\n", ssid);
}

// Wi-Fi is up: open the cloud screen (it starts a sync by itself when signed in). The old PC sync over the local
// network (Python script, /api/files) is retired; Transfer replaces it (2026-10-29).
static void enterSyncingState() {
  syncState = SyncState::CLOUD;
  snprintf(statusText, sizeof(statusText), "%s", cloud::providerName());
  cloud::screenOpen();
  screenDirty = true;
}

static void pollConnection() {
  if (WiFi.status() == WL_CONNECTED) {
    if (addMode) {
      // Added from Settings: joining it is the test; save it, make it the one tried first, back to Settings.
      if (!usedSavedPassword) saveCredential(connectingSSID, passwordBuf);
      wifiPrefs.putString("wifi_last", connectingSSID);
      savedLoaded = false;
      DBG_PRINTF("[WIFI] added %s\n", connectingSSID);
      wifiSyncStop();
      return;
    }
    if (usedSavedPassword) wifiPrefs.putString("wifi_last", connectingSSID);
    // If we used a manually entered password, prompt to save first
    if (!usedSavedPassword) {
      syncState = SyncState::SAVE_PROMPT;
      snprintf(statusText, sizeof(statusText), "%s",
               WiFi.localIP().toString().c_str());
      screenDirty = true;
    } else {
      enterSyncingState();
    }
    return;
  }

  if (millis() - connectStartMs > 25000UL) {
    WiFi.disconnect(true);
    strcpy(statusText, "Connection failed");

    if (usedSavedPassword) {
      syncState = SyncState::FORGET_PROMPT;
    } else {
      syncState = SyncState::CONNECT_FAILED;
    }

    screenDirty = true;
    DBG_PRINTLN("[SYNC] Connection timed out");
  }
}

// =========================================================================
// Input handling — called from input_handler for all key events
// =========================================================================

void syncHandleKey(uint8_t keyCode, uint8_t modifiers) {
  switch (syncState) {
    case SyncState::SCANNING:
      // No input during scan
      if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::NETWORK_LIST:
      if (keyCode == HID_KEY_DOWN && networkCount > 0) {
        selectedNet = (selectedNet + 1) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_UP && networkCount > 0) {
        selectedNet = (selectedNet - 1 + networkCount) % networkCount;
        screenDirty = true;
      } else if (keyCode == HID_KEY_ENTER && networkCount > 0) {
        // Try saved password first
        char savedPass[MAX_PASSWORD_LEN + 1];
        if (getSavedPassword(networks[selectedNet].ssid, savedPass, sizeof(savedPass))) {
          usedSavedPassword = true;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, savedPass);
        } else if (!networks[selectedNet].encrypted) {
          // Open network — connect directly
          usedSavedPassword = false;
          autoConnectAttempted = false;
          beginConnect(networks[selectedNet].ssid, "");
        } else {
          // Need password
          usedSavedPassword = false;
          autoConnectAttempted = false;
          passwordBuf[0] = '\0';
          passwordLen = 0;
          syncState = SyncState::PASSWORD_ENTRY;
          screenDirty = true;
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::PASSWORD_ENTRY:
      if (keyCode == HID_KEY_ENTER) {
        if (passwordLen > 0) {
          beginConnect(networks[selectedNet].ssid, passwordBuf);
        }
      } else if (keyCode == HID_KEY_ESCAPE) {
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      } else if (keyCode == HID_KEY_BACKSPACE) {
        if (passwordLen > 0) {
          passwordLen--;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      } else {
        // Printable character — Wi-Fi passwords always use US QWERTY input.
        char c = hidToAscii(keyCode, modifiers);
        if (c != 0 && c >= ' ' && c != '\n' && c != '\t' && passwordLen < MAX_PASSWORD_LEN) {
          passwordBuf[passwordLen++] = c;
          passwordBuf[passwordLen] = '\0';
          screenDirty = true;
        }
      }
      break;

    case SyncState::CONNECTING:
      // No input while connecting (timeout handles failure)
      if (keyCode == HID_KEY_ESCAPE) {
        WiFi.disconnect(true);
        if (autoConnectAttempted) {
          // Was auto-connecting ("Other network"): show the list without connecting to the saved one again
          skipAutoConnectOnce = true;
          beginScan();
        } else {
          syncState = SyncState::NETWORK_LIST;
          screenDirty = true;
        }
      }
      break;

    case SyncState::CLOUD:
      if (cloud::screenKey(keyCode, modifiers)) wifiSyncStop();
      break;

    case SyncState::CONNECT_FAILED:
      if (keyCode == HID_KEY_ENTER) {
        // Back to network list, re-scan
        beginScan();
      } else if (keyCode == HID_KEY_ESCAPE) {
        wifiSyncStop();
      }
      break;

    case SyncState::SAVE_PROMPT:
      // Up = Yes (save), Down = No (skip)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        saveCredential(connectingSSID, passwordBuf);
        DBG_PRINTF("[SYNC] Saved credentials for %s\n", connectingSSID);
        enterSyncingState();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        enterSyncingState();
      }
      break;

    case SyncState::FORGET_PROMPT:
      // Up = Yes (forget), Down = No (keep)
      if (keyCode == HID_KEY_UP || keyCode == HID_KEY_ENTER) {
        forgetCredential(connectingSSID);
        DBG_PRINTF("[SYNC] Forgot credentials for %s\n", connectingSSID);
        beginScan();
      } else if (keyCode == HID_KEY_DOWN || keyCode == HID_KEY_ESCAPE) {
        // Keep credentials — go to network list so user can retry manually
        // rather than triggering auto-connect again immediately
        syncState = SyncState::NETWORK_LIST;
        screenDirty = true;
      }
      break;
  }
}

// =========================================================================
// Quick connect to a saved network (no scan, no list) and the no-UI Wi-Fi used by background sync
// =========================================================================

static constexpr int QUICK_MAX = MAX_SAVED_NETWORKS;

// Saved networks, the last one that worked first.
static int loadSavedList(Preferences& prefs, char (*ssids)[33], char (*passes)[64]) {
  const int count = prefs.getInt("wifi_count", 0);
  String last = prefs.getString("wifi_last", "");
  int n = 0;
  auto add = [&](int i) {
    char sKey[16], pKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    snprintf(pKey, sizeof(pKey), "wifi_pass_%d", i);
    String s = prefs.getString(sKey, "");
    if (s.length() == 0 || n >= QUICK_MAX) return;
    for (int k = 0; k < n; k++) if (strcmp(ssids[k], s.c_str()) == 0) return;
    strncpy(ssids[n], s.c_str(), 32);
    ssids[n][32] = 0;
    String p = prefs.getString(pKey, "");
    strncpy(passes[n], p.c_str(), 63);
    passes[n][63] = 0;
    n++;
  };
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {   // last used first
    char sKey[16];
    snprintf(sKey, sizeof(sKey), "wifi_ssid_%d", i);
    if (last.length() && last == prefs.getString(sKey, "")) add(i);
  }
  for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) add(i);
  return n;
}

static struct {
  char ssids[QUICK_MAX][33];
  char passes[QUICK_MAX][64];
  int n = 0, idx = 0;
  unsigned long startMs = 0;
  bool scanning = false;
  QuickWifi state = QuickWifi::Idle;
} quick;

static bool g_wifiHeld = false;      // Transfer is using Wi-Fi
static bool g_hotspotOn = false;     // Transfer runs the device's own hotspot

void wifiHold(bool on) { g_wifiHeld = on; }
void wifiSetHotspot(bool on) { g_hotspotOn = on; }
bool wifiHotspotActive() { return g_hotspotOn; }

static void quickAttempt() {
  WiFi.disconnect(true);
  delay(20);
  WiFi.mode(WIFI_STA);
  WiFi.begin(quick.ssids[quick.idx], quick.passes[quick.idx]);
  quick.startMs = millis();
  DBG_PRINTF("[WIFI] quick: trying %s\n", quick.ssids[quick.idx]);
  memdiag::stagef("Wi-Fi join %d of %d", quick.idx + 1, quick.n);
}

bool wifiQuickBegin() {
#if !defined(RELEASE_BUILD)
  // Why Wi-Fi drops (the 802.11 reason code), so a slow or lost connection shows in the log.
  static bool eventsOn = false;
  if (!eventsOn) {
    eventsOn = true;
    WiFi.onEvent([](arduino_event_id_t, arduino_event_info_t info) {
      DBG_PRINTF("[WIFI] disconnected, reason %d\n", (int)info.wifi_sta_disconnected.reason);
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  }
#endif
  if (g_hotspotOn) return false;
  if (WiFi.status() == WL_CONNECTED) { quick.state = QuickWifi::Connected; return true; }
  if (quick.state == QuickWifi::Connecting) return true;   // already looking (sync and Transfer share one attempt)
  Preferences prefs;
  if (!prefs.begin("wifi_creds", true)) return false;
  if (!prefs.isKey("wifi_count")) { prefs.end(); return false; }
  quick.n = loadSavedList(prefs, quick.ssids, quick.passes);
  prefs.end();
  if (quick.n <= 0) return false;
  quick.idx = 0;
  quick.state = QuickWifi::Connecting;
  // Look first: only saved networks that are actually in range are tried (no 10 s wait on one that is not).
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  WiFi.scanNetworks(true);
  quick.scanning = true;
  quick.startMs = millis();
  memdiag::stage("Wi-Fi scan");
  return true;
}

// The scan finished: keep the saved networks that were seen (the last one that worked stays first).
static void quickFilterVisible(int found) {
  int keep = 0;
  for (int k = 0; k < quick.n; k++) {
    bool seen = false;
    for (int i = 0; i < found && !seen; i++) seen = (WiFi.SSID(i) == quick.ssids[k]);
    if (!seen) continue;
    if (keep != k) {
      memcpy(quick.ssids[keep], quick.ssids[k], sizeof(quick.ssids[0]));
      memcpy(quick.passes[keep], quick.passes[k], sizeof(quick.passes[0]));
    }
    keep++;
  }
  quick.n = keep;
}

QuickWifi wifiQuickPoll() {
  if (quick.state != QuickWifi::Connecting) return quick.state;
  if (quick.scanning) {
    // A full scan takes about 9 s where 20 networks are in range and Bluetooth shares the radio
    // (Sticky and E1001, 2026-10-29); the old 8 s limit cut every scan off at home, so every
    // background sync failed with "No Wi-Fi".
    const int found = WiFi.scanComplete();
    const bool timedOut = millis() - quick.startMs > 20000;
    if (found == WIFI_SCAN_RUNNING && !timedOut) return quick.state;
    quick.scanning = false;
    DBG_PRINTF("[SYNC] Quick scan: %d network(s) in %lu ms%s\n", found > 0 ? found : 0,
               (unsigned long)(millis() - quick.startMs), timedOut ? " (timed out)" : "");
    if (found > 0) quickFilterVisible(found); else quick.n = 0;
    WiFi.scanDelete();
    if (quick.n <= 0) {
      DBG_PRINTF("[WIFI] quick: no saved network in range\n");
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      quick.state = QuickWifi::Failed;
      memdiag::wifiOff();
      return quick.state;
    }
    quick.idx = 0;
    quickAttempt();
    return quick.state;
  }
  if (WiFi.status() == WL_CONNECTED) {
    quick.state = QuickWifi::Connected;
    DBG_PRINTF("[WIFI] quick: joined %s after %lu ms, signal %d dBm, channel %d\n", quick.ssids[quick.idx],
               (unsigned long)(millis() - quick.startMs), (int)WiFi.RSSI(), (int)WiFi.channel());
    memdiag::stage("Wi-Fi joined");
    Preferences prefs;
    if (prefs.begin("wifi_creds", false)) { prefs.putString("wifi_last", quick.ssids[quick.idx]); prefs.end(); }
    return quick.state;
  }
  if (millis() - quick.startMs > 10000) {
    DBG_PRINTF("[WIFI] quick: %s did not join in 10 s (status %d)\n", quick.ssids[quick.idx], (int)WiFi.status());
    if (++quick.idx >= quick.n) {
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      quick.state = QuickWifi::Failed;
      memdiag::wifiOff();
    } else {
      quickAttempt();
    }
  }
  return quick.state;
}

void wifiQuickEnd() {
  if (g_wifiHeld) return;   // the Transfer screen is serving on this network
  quick.state = QuickWifi::Idle;
  quick.scanning = false;
  if (syncActive) return;   // the Sync screen owns Wi-Fi while it is open
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  memdiag::wifiOff();
}

// =========================================================================
// Public API
// =========================================================================

void wifiSyncStart() {
  if (syncActive) return;
  syncActive = true;
  wifiPrefs.begin("wifi_creds", false);
  wifiBackupCheck();

  // Already connected (a recent background sync left Wi-Fi up): go straight to the OneDrive screen.
  if (WiFi.status() == WL_CONNECTED) {
    usedSavedPassword = true;
    enterSyncingState();
    return;
  }
  // Otherwise scan: the strongest saved network in range is connected to automatically, and the list only shows
  // when none is (see processScanResults).
  skipAutoConnectOnce = false;
  beginScan();

  DBG_PRINTLN("[SYNC] WiFi sync started");
}

void wifiSyncStop() {
  if (!syncActive) return;
  const bool adding = addMode;
  addMode = false;

  cloud::screenClose();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  wifiPrefs.end();
  syncActive = false;
  networkCount = 0;
  passwordBuf[0] = '\0';
  passwordLen = 0;
  statusText[0] = '\0';

  // Back where it was opened from: Settings (adding a network) or the main menu
  extern UIState currentState;
  currentState = adding ? UIState::SETTINGS : UIState::MAIN_MENU;
  screenDirty = true;

  DBG_PRINTLN("[SYNC] WiFi sync stopped");
}

void wifiSyncLoop() {
  if (!syncActive) return;

  switch (syncState) {
    case SyncState::SCANNING:
      processScanResults();
      break;

    case SyncState::CONNECTING:
      pollConnection();
      break;

    case SyncState::CLOUD:   // the cloud screen runs from cloud::loop()
      break;

    case SyncState::SAVE_PROMPT:
      // Server is NOT running during save prompt (will start after user responds)
      break;

    default:
      break;
  }
}

bool isWifiSyncActive() {
  return syncActive;
}

SyncState getSyncState() {
  return syncState;
}

int getNetworkCount() {
  return networkCount;
}

const char* getNetworkSSID(int i) {
  if (i < 0 || i >= networkCount) return "";
  return networks[i].ssid;
}

int getNetworkRSSI(int i) {
  if (i < 0 || i >= networkCount) return -100;
  return networks[i].rssi;
}

bool isNetworkEncrypted(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].encrypted;
}

bool isNetworkSaved(int i) {
  if (i < 0 || i >= networkCount) return false;
  return networks[i].saved;
}

int getSelectedNetwork() {
  return selectedNet;
}

int getPasswordLen() {
  return passwordLen;
}

const char* getSyncStatusText() {
  return statusText;
}

bool wifiAutoConnecting() { return syncActive && syncState == SyncState::CONNECTING && autoConnectAttempted; }

// =========================================================================
// Saved networks for Settings, and adding one
// =========================================================================

static struct {
  char ssid[MAX_SAVED_NETWORKS][33];
  char last[33];
  int n = 0;
} saved;

// Runs f with wifiPrefs open: the Sync screen keeps it open, Settings opens it for the moment.
template <typename F> static void withWifiPrefs(F f) {
  const bool wasOpen = syncActive;
  if (!wasOpen && !wifiPrefs.begin("wifi_creds", false)) return;
  wifiBackupCheck();
  f();
  if (!wasOpen) wifiPrefs.end();
}

static void savedLoad() {
  if (savedLoaded) return;
  savedLoaded = true;
  saved.n = 0;
  saved.last[0] = '\0';
  withWifiPrefs([] {
    const int count = wifiPrefs.getInt("wifi_count", 0);
    for (int i = 0; i < count && i < MAX_SAVED_NETWORKS; i++) {
      char key[16];
      snprintf(key, sizeof(key), "wifi_ssid_%d", i);
      String s = wifiPrefs.getString(key, "");
      if (s.length() == 0) continue;
      snprintf(saved.ssid[saved.n++], sizeof(saved.ssid[0]), "%s", s.c_str());
    }
    snprintf(saved.last, sizeof(saved.last), "%s", wifiPrefs.getString("wifi_last", "").c_str());
  });
}

void wifiSavedRefresh() { savedLoaded = false; }
int wifiSavedCount() { savedLoad(); return saved.n; }
const char* wifiSavedSsid(int i) { savedLoad(); return (i >= 0 && i < saved.n) ? saved.ssid[i] : ""; }

bool wifiSavedPreferred(int i) {
  savedLoad();
  if (i < 0 || i >= saved.n) return false;
  // No choice made yet: the first one is what is tried first.
  bool lastSaved = false;
  for (int k = 0; k < saved.n; k++) lastSaved = lastSaved || strcmp(saved.ssid[k], saved.last) == 0;
  return lastSaved ? strcmp(saved.ssid[i], saved.last) == 0 : i == 0;
}

bool wifiSavedConnected(int i) {
  savedLoad();
  if (i < 0 || i >= saved.n || g_hotspotOn || WiFi.status() != WL_CONNECTED) return false;
  return WiFi.SSID() == saved.ssid[i];
}

void wifiSavedPrefer(int i) {
  savedLoad();
  if (i < 0 || i >= saved.n) return;
  char ssid[33];
  snprintf(ssid, sizeof(ssid), "%s", saved.ssid[i]);
  withWifiPrefs([&] { wifiPrefs.putString("wifi_last", ssid); });
  savedLoaded = false;
}

void wifiSavedForget(int i) {
  savedLoad();
  if (i < 0 || i >= saved.n) return;
  char ssid[33];
  snprintf(ssid, sizeof(ssid), "%s", saved.ssid[i]);
  withWifiPrefs([&] { forgetCredential(ssid); });
  savedLoaded = false;
  DBG_PRINTF("[WIFI] forgot %s\n", ssid);
}

void wifiAddStart() {
  if (syncActive || g_hotspotOn) return;
  if (cloud::wifiInUse()) cloud::radioTakenOver();   // a background sync gives the radio up; its push runs later
  syncActive = true;
  addMode = true;
  wifiPrefs.begin("wifi_creds", false);
  wifiBackupCheck();
  skipAutoConnectOnce = true;   // show the list; the user picks the network to add
  autoConnectAttempted = false;
  beginScan();
  DBG_PRINTLN("[WIFI] add a network");
}

bool wifiAdding() { return syncActive && addMode; }
