#pragma once

#include "config.h"
#include <string>

// Device info structure for discovered devices
struct BleDeviceInfo {
  std::string address;
  std::string name;
  int rssi;
};

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

// Functions for managing stored device
void storePairedDevice(const std::string& address, const std::string& name);
bool getStoredDevice(std::string& address, std::string& name);
void clearStoredDevice();

// Function for getting current passkey for UI display
uint32_t getCurrentPasskey();
