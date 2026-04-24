#pragma once

#include <cstdint>

// Sync state machine states (exposed as int via getSyncState())
enum class SyncState : uint8_t {
  SCANNING,
  NETWORK_LIST,
  PASSWORD_ENTRY,
  CONNECTING,
  SYNCING,         // Server running, tracking transfers
  DONE,            // Summary shown, WiFi off, auto-return to menu
  CONNECT_FAILED,
  SAVE_PROMPT,
  FORGET_PROMPT
};

// Lifecycle
void wifiSyncStart();       // Begin scanning (or auto-connect if saved creds)
void wifiSyncStop();         // Stop everything, WiFi off
void wifiSyncLoop();         // Poll scan/connection/HTTP
bool isWifiSyncActive();

// For UI renderer
SyncState getSyncState();
int  getNetworkCount();
const char* getNetworkSSID(int i);
int  getNetworkRSSI(int i);
bool isNetworkEncrypted(int i);
bool isNetworkSaved(int i);
int  getSelectedNetwork();
const char* getPasswordBuffer();
int  getPasswordLen();
const char* getSyncStatusText();

// Sync activity log (for SYNCING/DONE screens)
int  getSyncFilesSent();
int  getSyncFilesReceived();
int  getSyncTotalFiles();
int  getSyncLogCount();
const char* getSyncLogLine(int i);

// For input handler
void syncHandleKey(uint8_t keyCode, uint8_t modifiers);
