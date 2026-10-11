#pragma once

#include "config.h"
#include <string>

// Device info structure for discovered devices
struct BleDeviceInfo {
  std::string address;
  std::string name;
  int rssi;
  uint8_t addressType;    // BLE address type (public/random)
  uint32_t lastSeenMs;    // from millis()
  bool isHid = false;     // advertises the HID service (0x1812)
  uint16_t appearance = 0; // BLE appearance (0x03C1 = keyboard), 0 if not advertised
};

// True if the device looks like a keyboard (advertises HID or a HID/keyboard
// appearance). Classification is a hint only — never used to make a device
// permanently unreachable (see getVisibleDeviceIndices fallback).
bool bleDeviceIsLikelyKeyboard(const BleDeviceInfo& d);

// Fills idxOut with indices into the discovered-device array that should be
// shown. When showAll is false, only likely keyboards are shown — UNLESS none
// are found, in which case all devices are shown (so the user is never stuck).
// Reports the total device count and the keyboard count via the out params.
// Returns the number of indices written (<= maxOut).
int getVisibleDeviceIndices(bool showAll, int* idxOut, int maxOut,
                            int* outTotal, int* outKeyboards);

void bleSetup();
void bleLoop();
bool isKeyboardConnected();
BLEState getConnectionState();

// Functions for Bluetooth device management
void startDeviceScan();
void stopDeviceScan();
int getDiscoveredDeviceCount();
BleDeviceInfo* getDiscoveredDevices();
void connectToDevice(int deviceIndex);
void disconnectCurrentDevice();
std::string getCurrentDeviceAddress();

// Global flag to control auto-reconnect behavior
extern bool autoReconnectEnabled;

// Max keyboards that can be stored
static constexpr int MAX_PAIRED_KEYBOARDS = 4;

// Multi-keyboard storage API
int  getPairedKeyboardCount();
bool getPairedKeyboard(int index, std::string& addr, std::string& name, uint8_t& addrType);
int  getLastUsedKeyboardIndex();   // -1 if none stored
bool removePairedKeyboard(int index);
void connectToPairedKeyboard(int index);

// Internal helpers (also used by ui_renderer for backwards compat)
void storePairedDevice(const std::string& address, const std::string& name);
bool getStoredDevice(std::string& address, std::string& name);
void clearStoredDevice();

// Function for getting current passkey for UI display
uint32_t getCurrentPasskey();

// BLE transmit power (dBm). Default -9 (lowest step, best battery life). Higher
// steps improve link margin for weak/distant keyboards at a continuous battery
// cost. Cycles through discrete steps; persisted in NVS. Pure BLE — portable.
int8_t      getBleTxPower();
const char* getBleTxPowerLabel();  // e.g. "-9 dBm (Battery)" for the settings UI
void        cycleBleTxPower(int dir);  // dir>=0: more power, dir<0: less

// Bluetooth scanning status functions
bool isDeviceScanning();
uint32_t getScanAgeMs();
void refreshScanNow();
void clearAllBluetoothBonds();
void cancelPendingConnection();
