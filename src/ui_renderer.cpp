#include "ui_renderer.h"
#include "config.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalDisplay.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>

// External variables
extern bool autoReconnectEnabled;

// External functions
bool getStoredDevice(std::string& address, std::string& name);
uint32_t getCurrentPasskey();

// Font data includes
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>

// Font objects (file-scoped)
static EpdFont ns14Regular(&notosans_14_regular);
static EpdFont ns14Bold(&notosans_14_bold);
static EpdFontFamily ns14Family(&ns14Regular, &ns14Bold);

static EpdFont ns12Regular(&notosans_12_regular);
static EpdFont ns12Bold(&notosans_12_bold);
static EpdFontFamily ns12Family(&ns12Regular, &ns12Bold);

static EpdFont u10Regular(&ubuntu_10_regular);
static EpdFont u10Bold(&ubuntu_10_bold);
static EpdFontFamily u10Family(&u10Regular, &u10Bold);

// Extern shared state (defined in main.cpp)
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int bluetoothDeviceSelection;
extern Orientation currentOrientation;
extern int charsPerLine;
extern char renameBuffer[];
extern int renameBufferLen;

void rendererSetup(GfxRenderer& renderer) {
  renderer.insertFont(FONT_BODY, ns14Family);
  renderer.insertFont(FONT_UI, ns12Family);
  renderer.insertFont(FONT_SMALL, u10Family);
}

// Helper: draw battery percentage in top-right
static void drawBattery(GfxRenderer& renderer, HalGPIO& gpio) {
  int pct = gpio.getBatteryPercentage();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  int tw = renderer.getTextAdvanceX(FONT_SMALL, buf);
  int x = renderer.getScreenWidth() - tw - 8;
  renderer.drawText(FONT_SMALL, x, 5, buf);
}

// Helper: draw BLE status
static void drawBleStatus(GfxRenderer& renderer, int x, int y) {
  const char* status;
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "KB Connected"; break;
    case BLEState::SCANNING:     status = "Scanning..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "KB Disconnected"; break;
  }
  renderer.drawText(FONT_SMALL, x, y, status);
}

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();

  // Title
  renderer.drawCenteredText(FONT_BODY, 30, "MicroSlate", true, EpdFontFamily::BOLD);

  // Menu items
  static const char* menuItems[] = {"Browse Files", "New Note", "Settings"};
  for (int i = 0; i < 3; i++) {
    int yPos = 90 + (i * 45);
    if (i == mainMenuSelection) {
      renderer.fillRect(5, yPos - 5, renderer.getScreenWidth() - 10, 35, true);
      // Draw inverted text (white on black)
      renderer.drawText(FONT_UI, 20, yPos, menuItems[i], false);
    } else {
      renderer.drawText(FONT_UI, 20, yPos, menuItems[i]);
    }
  }

  // Status area at bottom
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  renderer.drawLine(5, sh - 50, sw - 5, sh - 50);
  renderer.drawText(FONT_SMALL, 10, sh - 40, "Arrows: Navigate  Enter: Select");
  drawBleStatus(renderer, 10, sh - 25);
  drawBattery(renderer, gpio);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  // Header
  renderer.drawText(FONT_UI, 10, 10, "Notes", true, EpdFontFamily::BOLD);

  int fc = getFileCount();
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d files", fc);
  int cw = renderer.getTextAdvanceX(FONT_SMALL, countBuf);
  renderer.drawText(FONT_SMALL, sw - cw - 10, 14, countBuf);
  drawBattery(renderer, gpio);

  renderer.drawLine(5, 32, sw - 5, 32);

  // File list
  int lineH = 28;
  int maxVisible = (sh - 80) / lineH;
  int startIdx = 0;
  if (fc > maxVisible && selectedFileIndex >= maxVisible) {
    startIdx = selectedFileIndex - maxVisible + 1;
  }

  if (fc == 0) {
    renderer.drawText(FONT_UI, 20, 60, "No notes found.");
    renderer.drawText(FONT_SMALL, 20, 85, "Press Ctrl+N to create one.");
  }

  for (int i = startIdx; i < fc && (i - startIdx) < maxVisible; i++) {
    FileInfo* files = getFileList();
    int yPos = 40 + (i - startIdx) * lineH;

    if (i == selectedFileIndex) {
      renderer.fillRect(5, yPos - 2, sw - 10, lineH - 2, true);
      renderer.drawText(FONT_UI, 15, yPos, files[i].title, false);
    } else {
      renderer.drawText(FONT_UI, 15, yPos, files[i].title);
      // Show filename below title in small font
      renderer.drawText(FONT_SMALL, 15, yPos + 15, files[i].filename);
    }
  }

  // Footer
  renderer.drawLine(5, sh - 30, sw - 5, sh - 30);
  renderer.drawText(FONT_SMALL, 10, sh - 22,
                    "Enter:Open  Ctrl+N:New  Ctrl+R:Rename  Esc:Back");

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  // --- Header bar ---
  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }
  renderer.drawText(FONT_UI, 10, 5, headerBuf, true, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  renderer.drawLine(5, 25, sw - 5, 25);

  // --- Text area ---
  int textAreaTop = 30;
  int textAreaBottom = sh - 30;
  int textAreaHeight = textAreaBottom - textAreaTop;
  int lineHeight = renderer.getLineHeight(FONT_BODY);
  if (lineHeight <= 0) lineHeight = 20;
  int visibleLines = textAreaHeight / lineHeight;

  editorRecalculateLines();
  int vpStart = editorGetViewportStart();
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();
  int curCol = editorGetCursorCol();
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();

  // Draw visible lines
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int lineIdx = vpStart + i;
    int lineStart = editorGetLinePosition(lineIdx);
    int lineEnd;
    if (lineIdx + 1 < totalLines) {
      lineEnd = editorGetLinePosition(lineIdx + 1);
    } else {
      lineEnd = (int)bufLen;
    }

    // Don't include trailing newline in display
    int dispEnd = lineEnd;
    if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

    int len = dispEnd - lineStart;
    if (len > 0) {
      char lineBuf[256];
      int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
      strncpy(lineBuf, buf + lineStart, copyLen);
      lineBuf[copyLen] = '\0';

      int yPos = textAreaTop + (i * lineHeight);
      renderer.drawText(FONT_BODY, 10, yPos, lineBuf);
    }
  }

  // --- Draw cursor ---
  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorScreenLine = curLine - vpStart;
    int cursorY = textAreaTop + (cursorScreenLine * lineHeight);

    // Calculate X position using font metrics
    int lineStart = editorGetLinePosition(curLine);
    char prefix[256];
    int prefixLen = curCol;
    if (prefixLen > (int)sizeof(prefix) - 1) prefixLen = (int)sizeof(prefix) - 1;
    strncpy(prefix, buf + lineStart, prefixLen);
    prefix[prefixLen] = '\0';

    int cursorX = 10 + renderer.getTextAdvanceX(FONT_BODY, prefix);
    int cursorW = renderer.getSpaceWidth(FONT_BODY);
    if (cursorW < 2) cursorW = 8;

    // Block cursor (inverted rectangle)
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, true);
  }

  // --- Status bar ---
  renderer.drawLine(5, sh - 28, sw - 5, sh - 28);

  char statusLeft[64];
  snprintf(statusLeft, sizeof(statusLeft), "L%d C%d  %d/%d  %dcpl",
           curLine + 1, curCol + 1,
           editorGetCursorPosition(), (int)bufLen, charsPerLine);
  renderer.drawText(FONT_SMALL, 10, sh - 20, statusLeft);

  const char* statusRight = "Ctrl+S:Save  Ctrl+Q:Back";
  int srw = renderer.getTextAdvanceX(FONT_SMALL, statusRight);
  renderer.drawText(FONT_SMALL, sw - srw - 10, sh - 20, statusRight);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer) {
  renderer.clearScreen();

  int sw = renderer.getScreenWidth();

  renderer.drawText(FONT_UI, 10, 20, "Rename File", true, EpdFontFamily::BOLD);
  renderer.drawLine(5, 42, sw - 5, 42);

  // Current filename
  FileInfo* files = getFileList();
  renderer.drawText(FONT_SMALL, 20, 60, "Current:");
  renderer.drawText(FONT_UI, 20, 78, files[selectedFileIndex].filename);

  // New name input
  renderer.drawText(FONT_SMALL, 20, 110, "New name:");
  renderer.drawRect(15, 128, sw - 30, 30);
  renderer.drawText(FONT_UI, 20, 132, renameBuffer);

  // Cursor
  int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, renameBuffer);
  int cursorW = renderer.getSpaceWidth(FONT_UI);
  if (cursorW < 2) cursorW = 8;
  renderer.fillRect(cursorX, 132, cursorW, 20, true);

  // Extension hint
  renderer.drawText(FONT_SMALL, 20, 170, ".txt will be added automatically");

  // Instructions
  renderer.drawText(FONT_SMALL, 20, 200, "Enter: Confirm   Esc: Cancel");

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  renderer.drawText(FONT_UI, 10, 10, "Settings", true, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  renderer.drawLine(5, 32, sw - 5, 32);

  // Setting items
  static const char* labels[] = {"Orientation", "Line Width", "Back", "Bluetooth", "Clear Paired Device"};
  for (int i = 0; i < 5; i++) {
    int yPos = 50 + (i * 45);

    if (i == settingsSelection) {
      renderer.fillRect(5, yPos - 5, sw - 10, 38, true);
      renderer.drawText(FONT_UI, 15, yPos, labels[i], false);
    } else {
      renderer.drawText(FONT_UI, 15, yPos, labels[i]);
    }

    // Value display
    char val[32] = "";
    if (i == 0) {
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      strcpy(val, "Portrait"); break;
        case Orientation::LANDSCAPE_CW:  strcpy(val, "Landscape CW"); break;
        case Orientation::PORTRAIT_INV:  strcpy(val, "Inverted"); break;
        case Orientation::LANDSCAPE_CCW: strcpy(val, "Landscape CCW"); break;
      }
    } else if (i == 1) {
      snprintf(val, sizeof(val), "%d chars", charsPerLine);
    } else if (i == 4) { // Show if device is paired
      std::string storedAddr, storedName;
      if (getStoredDevice(storedAddr, storedName)) {
        snprintf(val, sizeof(val), "Paired: %s", storedName.c_str());
      } else {
        strcpy(val, "None paired");
      }
    }

    if (val[0] != '\0') {
      int vw = renderer.getTextAdvanceX(FONT_UI, val);
      bool inverted = (i == settingsSelection);
      renderer.drawText(FONT_UI, sw - vw - 20, yPos, val, !inverted);
    }
  }

  // Instructions
  renderer.drawLine(5, sh - 30, sw - 5, sh - 30);
  renderer.drawText(FONT_SMALL, 10, sh - 22,
                    "Arrows:Nav  Left/Right:Change  Enter:Select  Esc:Back");

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();

  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  renderer.drawText(FONT_UI, 10, 10, "Bluetooth Devices", true, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  renderer.drawLine(5, 32, sw - 5, 32);

  // Display connection status
  const char* status;
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    
      status = "Connected to keyboard"; 
      break;
    case BLEState::SCANNING:     
      status = "Scanning for devices..."; 
      break;
    case BLEState::CONNECTING:   
      status = "Connecting..."; 
      break;
    case BLEState::DISCONNECTED: 
      status = "Not connected"; 
      break;
  }
  
  renderer.drawText(FONT_SMALL, 10, 45, status);

  // Show if there's a stored/paired device
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName)) {
    char pairedStr[64];
    snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
    renderer.drawText(FONT_SMALL, sw/2, 45, pairedStr);
  }

  // Show passkey if pairing is in progress
  uint32_t passkey = getCurrentPasskey();
  if (passkey > 0) {
    char passkeyStr[16];
    snprintf(passkeyStr, sizeof(passkeyStr), "PIN: %06lu", passkey);
    renderer.drawText(FONT_UI, 10, 60, passkeyStr, true, EpdFontFamily::BOLD);
    renderer.drawText(FONT_SMALL, 10, 75, "Enter this PIN on the keyboard", true);
  }

  // Show discovered devices
  int deviceCount = getDiscoveredDeviceCount();
  if (deviceCount > 0) {
    BleDeviceInfo* devices = getDiscoveredDevices();
    
    // Header for device list
    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "Available devices: %d", deviceCount);
    renderer.drawText(FONT_SMALL, 10, 70, headerStr, true, EpdFontFamily::BOLD);
    
    // Calculate which devices to show based on selection to enable scrolling
    int maxDevicesToShow = 5;
    int startIndex = 0;
    
    // Adjust start index so selected device is visible
    if (bluetoothDeviceSelection >= maxDevicesToShow) {
      startIndex = bluetoothDeviceSelection - maxDevicesToShow + 1;
    }
    
    int devicesToShow = (deviceCount - startIndex < maxDevicesToShow) ? 
                        deviceCount - startIndex : maxDevicesToShow;
    
    for (int i = 0; i < devicesToShow; i++) {
      int deviceIndex = startIndex + i;
      int yPos = 90 + (i * 30);
      
      // Highlight current connection if it matches
      bool isConnectedDevice = (getCurrentDeviceAddress() == devices[deviceIndex].address);
      // Highlight selected device for potential connection
      bool isSelectedDevice = (bluetoothDeviceSelection == deviceIndex);
      
      if (isConnectedDevice) {
        renderer.fillRect(5, yPos - 5, sw - 10, 25, true);
        // Show name if available, otherwise show address
        const char* displayName = devices[deviceIndex].name.empty() ? 
                                 devices[deviceIndex].address.c_str() : 
                                 devices[deviceIndex].name.c_str();
        renderer.drawText(FONT_UI, 15, yPos, displayName, false);
      } else if (isSelectedDevice) {
        // Highlight selected device with a filled rectangle
        renderer.fillRect(5, yPos - 5, sw - 10, 25, true);
        // Show name if available, otherwise show address
        const char* displayName = devices[deviceIndex].name.empty() ? 
                                 devices[deviceIndex].address.c_str() : 
                                 devices[deviceIndex].name.c_str();
        renderer.drawText(FONT_UI, 15, yPos, displayName, false);
      } else {
        // Regular device display
        const char* displayName = devices[deviceIndex].name.empty() ? 
                                 devices[deviceIndex].address.c_str() : 
                                 devices[deviceIndex].name.c_str();
        renderer.drawText(FONT_UI, 15, yPos, displayName);
      }
      
      // Show signal strength indicator
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%ddBm", devices[deviceIndex].rssi);
      int rssiWidth = renderer.getTextAdvanceX(FONT_SMALL, rssiStr);
      renderer.drawText(FONT_SMALL, sw - rssiWidth - 10, yPos, rssiStr);
    }
    
    // Show navigation hint if there are more devices than can be displayed
    if (deviceCount > maxDevicesToShow) {
      char navHint[32];
      int pageNum = (bluetoothDeviceSelection / maxDevicesToShow) + 1;
      int totalPages = (deviceCount + maxDevicesToShow - 1) / maxDevicesToShow; // Ceiling division
      snprintf(navHint, sizeof(navHint), "Page %d/%d", pageNum, totalPages);
      renderer.drawText(FONT_SMALL, 15, 90 + (maxDevicesToShow * 30), navHint);
    }
  } else {
    renderer.drawText(FONT_UI, 20, 80, "No devices found", true);
    renderer.drawText(FONT_SMALL, 20, 100, "Press Enter to scan for devices");
  }

  // Instructions
  renderer.drawLine(5, sh - 30, sw - 5, sh - 30);
  renderer.drawText(FONT_SMALL, 10, sh - 22, "Enter: Scan/Select  Esc: Back");

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}