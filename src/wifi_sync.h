#pragma once

#include <cstdint>

// Sync state machine states (exposed as int via getSyncState())
enum class SyncState : uint8_t {
  SCANNING,
  NETWORK_LIST,
  PASSWORD_ENTRY,
  CONNECTING,
  CONNECT_FAILED,
  SAVE_PROMPT,
  FORGET_PROMPT,
  CLOUD            // Wi-Fi is up: the cloud screen (src/sync/cloud_service)
};

// Lifecycle
void wifiSyncStart();       // Begin scanning (or auto-connect if saved creds)
void wifiSyncStop();         // Stop everything, WiFi off
void wifiSyncLoop();         // Poll scan/connection/HTTP
bool isWifiSyncActive();

// For UI renderer
SyncState getSyncState();
bool wifiAutoConnecting();   // connecting to a saved network by itself (the screen offers "Other network")
int  getNetworkCount();
const char* getNetworkSSID(int i);
int  getNetworkRSSI(int i);
bool isNetworkEncrypted(int i);
bool isNetworkSaved(int i);
int  getSelectedNetwork();
int  getPasswordLen();
const char* getSyncStatusText();

// For input handler
void syncHandleKey(uint8_t keyCode, uint8_t modifiers);

// Wi-Fi without any screen, for background cloud sync. Connects to a saved network (the last one that worked first).
enum class QuickWifi : uint8_t { Idle, Connecting, Connected, Failed };
bool wifiQuickBegin();        // false when no network is saved
QuickWifi wifiQuickPoll();    // call every loop while Connecting
void wifiQuickEnd();          // Wi-Fi off (unless the Sync or Transfer screen is using it)
// Transfer holds Wi-Fi while its screen is open: wifiQuickEnd() leaves it alone. On the hotspot no background
// connection may start at all (it would switch the radio back to joining a network).
void wifiHold(bool on);
void wifiSetHotspot(bool on);
bool wifiHotspotActive();

// Saved networks, for the Wi-Fi tab of Settings (at most 4, in stored order). The preferred one is tried first by
// background sync and Transfer.
void wifiSavedRefresh();          // read them again (after a change made elsewhere)
int  wifiSavedCount();
const char* wifiSavedSsid(int i);
bool wifiSavedPreferred(int i);
bool wifiSavedConnected(int i);   // Wi-Fi is up on this network now
void wifiSavedPrefer(int i);
void wifiSavedForget(int i);
// Add a network from Settings: the Sync screen's scan list without the automatic connect. Joining one saves it and
// returns to Settings (no cloud screen).
void wifiAddStart();
bool wifiAdding();
