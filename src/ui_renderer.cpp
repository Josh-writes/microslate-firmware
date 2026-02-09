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
bool isDeviceScanning();
uint32_t getScanAgeMs();

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

// ---------------------------------------------------------------------------
// Clipped draw helpers — use renderer.truncatedText() so NO pixel ever
// exceeds screen width.  This is how crosspoint-reader prevents GFX errors.
// ---------------------------------------------------------------------------

// Draw text that is guaranteed not to overflow the screen width.
// maxW = available pixel width from x to right edge (caller computes).
// Falls back to sw - x - 5 if maxW <= 0.
static void drawClippedText(GfxRenderer& r, int font, int x, int y,
                            const char* text, int maxW = 0,
                            bool black = true,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0 || x >= sw || y < 0 || y >= sh) return;

  if (maxW <= 0) maxW = sw - x - 5;   // 5px right margin
  if (maxW <= 0) return;

  auto clipped = r.truncatedText(font, text, maxW, style);
  if (!clipped.empty()) {
    r.drawText(font, x, y, clipped.c_str(), black, style);
  }
}

// Draw right-aligned text (e.g. battery %, RSSI, settings values).
// Computes its own X from the measured text width.
static void drawRightText(GfxRenderer& r, int font, int rightEdge, int y,
                          const char* text, bool black = true,
                          EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  int tw = r.getTextAdvanceX(font, text);
  if (tw <= 0) tw = 30;                    // safe fallback
  int x = rightEdge - tw;
  if (x < 5) x = 5;                        // don't go off left edge
  drawClippedText(r, font, x, y, text, rightEdge - x, black, style);
}

// Safe line — just clamp to screen
static void clippedLine(GfxRenderer& r, int x1, int y1, int x2, int y2) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  // Clamp rather than reject
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  x1 = clamp(x1, 0, sw - 1);
  x2 = clamp(x2, 0, sw - 1);
  y1 = clamp(y1, 0, sh - 1);
  y2 = clamp(y2, 0, sh - 1);
  r.drawLine(x1, y1, x2, y2);
}

// Safe fillRect — clamp dimensions to screen
static void clippedFillRect(GfxRenderer& r, int x, int y, int w, int h,
                            bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x;
  if (y + h > sh) h = sh - y;
  if (w > 0 && h > 0) r.fillRect(x, y, w, h, state);
}

// ---------------------------------------------------------------------------
// Helper: draw battery percentage in top-right
// ---------------------------------------------------------------------------
static void drawBattery(GfxRenderer& renderer, HalGPIO& gpio) {
  int pct = gpio.getBatteryPercentage();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  drawRightText(renderer, FONT_SMALL, renderer.getScreenWidth() - 8, 5, buf);
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
  drawClippedText(renderer, FONT_SMALL, x, y, status);
}

// ===========================================================================
// Screen drawing functions
// ===========================================================================

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  // Title
  renderer.drawCenteredText(FONT_BODY, 30, "MicroSlate", true, EpdFontFamily::BOLD);

  // Menu items
  static const char* menuItems[] = {"Browse Files", "New Note", "Settings"};
  for (int i = 0; i < 3; i++) {
    int yPos = 90 + (i * 45);
    if (i == mainMenuSelection) {
      clippedFillRect(renderer, 5, yPos - 5, sw - 10, 35, true);
      drawClippedText(renderer, FONT_UI, 20, yPos, menuItems[i], sw - 40, false);
    } else {
      drawClippedText(renderer, FONT_UI, 20, yPos, menuItems[i], sw - 40);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 40) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12, "Arrows: Navigate  Enter: Select");
    drawBleStatus(renderer, 20, sh - bm + 28);
  }
  drawBattery(renderer, gpio);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  // Header
  drawClippedText(renderer, FONT_UI, 10, 10, "Notes", 0, true, EpdFontFamily::BOLD);

  int fc = getFileCount();
  char countBuf[16];
  snprintf(countBuf, sizeof(countBuf), "%d files", fc);
  drawRightText(renderer, FONT_SMALL, sw - 10, 14, countBuf);
  drawBattery(renderer, gpio);

  clippedLine(renderer, 5, 32, sw - 5, 32);

  // File list
  int lineH = 28;
  int maxVisible = (sh - 80) / lineH;
  int startIdx = 0;
  if (fc > maxVisible && selectedFileIndex >= maxVisible) {
    startIdx = selectedFileIndex - maxVisible + 1;
  }

  if (fc == 0) {
    drawClippedText(renderer, FONT_UI, 20, 60, "No notes found.");
    drawClippedText(renderer, FONT_SMALL, 20, 85, "Press Ctrl+N to create one.");
  }

  for (int i = startIdx; i < fc && (i - startIdx) < maxVisible; i++) {
    FileInfo* files = getFileList();
    int yPos = 40 + (i - startIdx) * lineH;

    if (i == selectedFileIndex) {
      clippedFillRect(renderer, 5, yPos - 2, sw - 10, lineH - 2, true);
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30, false);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30);
      drawClippedText(renderer, FONT_SMALL, 15, yPos + 15, files[i].filename, sw - 30);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12,
                    "Enter:Open  Ctrl+N:New  Ctrl+R:Rename  Esc:Back");
  }

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
  drawClippedText(renderer, FONT_UI, 10, 5, headerBuf, sw - 80, true, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 25, sw - 5, 25);

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

  // Draw visible lines — truncate to screen width
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int lineIdx = vpStart + i;
    int lineStart = editorGetLinePosition(lineIdx);
    int lineEnd;
    if (lineIdx + 1 < totalLines) {
      lineEnd = editorGetLinePosition(lineIdx + 1);
    } else {
      lineEnd = (int)bufLen;
    }

    int dispEnd = lineEnd;
    if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

    int len = dispEnd - lineStart;
    if (len > 0) {
      char lineBuf[256];
      int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
      strncpy(lineBuf, buf + lineStart, copyLen);
      lineBuf[copyLen] = '\0';

      int yPos = textAreaTop + (i * lineHeight);
      drawClippedText(renderer, FONT_BODY, 10, yPos, lineBuf, sw - 20);
    }
  }

  // --- Draw cursor ---
  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorScreenLine = curLine - vpStart;
    int cursorY = textAreaTop + (cursorScreenLine * lineHeight);

    int lineStart = editorGetLinePosition(curLine);
    char prefix[256];
    int prefixLen = curCol;
    if (prefixLen > (int)sizeof(prefix) - 1) prefixLen = (int)sizeof(prefix) - 1;
    strncpy(prefix, buf + lineStart, prefixLen);
    prefix[prefixLen] = '\0';

    int cursorX = 10 + renderer.getTextAdvanceX(FONT_BODY, prefix);
    int cursorW = renderer.getSpaceWidth(FONT_BODY);
    if (cursorW < 2) cursorW = 8;

    // Clamp cursor to screen
    if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 && cursorY + lineHeight <= sh) {
      renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, true);
    }
  }

  // --- Status bar ---
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm);

    char statusLeft[64];
    snprintf(statusLeft, sizeof(statusLeft), "L%d C%d  %d/%d  %dcpl",
             curLine + 1, curCol + 1,
             editorGetCursorPosition(), (int)bufLen, charsPerLine);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12, statusLeft, sw / 2 - 20);

    drawRightText(renderer, FONT_SMALL, sw - 20, sh - bm + 12, "Ctrl+S:Save  Ctrl+Q:Back");
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();

  drawClippedText(renderer, FONT_UI, 10, 20, "Rename File", 0, true, EpdFontFamily::BOLD);
  clippedLine(renderer, 5, 42, sw - 5, 42);

  FileInfo* files = getFileList();
  drawClippedText(renderer, FONT_SMALL, 20, 60, "Current:");
  drawClippedText(renderer, FONT_UI, 20, 78, files[selectedFileIndex].filename, sw - 40);

  drawClippedText(renderer, FONT_SMALL, 20, 110, "New name:");
  renderer.drawRect(15, 128, sw - 30, 30);
  drawClippedText(renderer, FONT_UI, 20, 132, renameBuffer, sw - 50);

  // Cursor
  int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, renameBuffer);
  int cursorW = renderer.getSpaceWidth(FONT_UI);
  if (cursorW < 2) cursorW = 8;
  if (cursorX + cursorW < sw)
    renderer.fillRect(cursorX, 132, cursorW, 20, true);

  drawClippedText(renderer, FONT_SMALL, 20, 170, ".txt will be added automatically");
  drawClippedText(renderer, FONT_SMALL, 20, 200, "Enter: Confirm   Esc: Cancel");

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  drawClippedText(renderer, FONT_UI, 10, 10, "Settings", 0, true, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32);

  // Setting items
  static const char* labels[] = {"Orientation", "Line Width", "Back", "Bluetooth", "Clear Paired Device"};
  for (int i = 0; i < 5; i++) {
    int yPos = 50 + (i * 45);

    if (i == settingsSelection) {
      clippedFillRect(renderer, 5, yPos - 5, sw - 10, 38, true);
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, false);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15);
    }

    // Value display on the right
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
    } else if (i == 4) {
      std::string storedAddr, storedName;
      if (getStoredDevice(storedAddr, storedName)) {
        snprintf(val, sizeof(val), "Paired: %s", storedName.c_str());
      } else {
        strcpy(val, "None paired");
      }
    }

    if (val[0] != '\0') {
      bool inverted = (i == settingsSelection);
      drawRightText(renderer, FONT_UI, sw - 20, yPos, val, !inverted);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12,
                    "Arrows:Nav  L/R:Change  Enter:Select  Esc:Back");
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  renderer.clearScreen();

  // Header
  drawClippedText(renderer, FONT_UI, 10, 10, "Bluetooth Devices", 0, true, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32);

  // Connection status
  const char* status;
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "Connected to keyboard"; break;
    case BLEState::SCANNING:     status = "Scanning for devices..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "Not connected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, 10, 45, status, sw / 2 - 10);

  // Paired device info
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName)) {
    char pairedStr[64];
    snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
    drawClippedText(renderer, FONT_SMALL, sw / 2, 45, pairedStr, sw / 2 - 10);
  }

  // Passkey display
  uint32_t passkey = getCurrentPasskey();
  if (passkey > 0) {
    char passkeyStr[32];
    drawClippedText(renderer, FONT_UI, 20, 100, "PAIRING CODE:", 0, true, EpdFontFamily::BOLD);
    snprintf(passkeyStr, sizeof(passkeyStr), "%06lu", passkey);
    drawClippedText(renderer, FONT_BODY, 20, 130, passkeyStr, 0, true, EpdFontFamily::BOLD);
    drawClippedText(renderer, FONT_SMALL, 20, 160, "Type this code on your keyboard");
    drawClippedText(renderer, FONT_SMALL, 20, 180, "then press Enter");
  } else if (isDeviceScanning()) {
    static uint8_t dotPhase = 0;
    static uint32_t lastAnimMs = 0;
    if (millis() - lastAnimMs > 900) {
      dotPhase = (dotPhase + 1) % 4;
      lastAnimMs = millis();
    }
    std::string dots(dotPhase, '.');
    char scanningStr[64];
    int deviceCount = getDiscoveredDeviceCount();
    snprintf(scanningStr, sizeof(scanningStr), "Searching for devices%s", dots.c_str());
    drawClippedText(renderer, FONT_SMALL, 10, 60, scanningStr, sw / 2 - 10);

    char foundStr[32];
    snprintf(foundStr, sizeof(foundStr), "Found: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, sw / 2, 60, foundStr, sw / 2 - 10);
  }

  // Device list
  int deviceCount = getDiscoveredDeviceCount();
  if (deviceCount > 0) {
    BleDeviceInfo* devices = getDiscoveredDevices();

    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "Available devices: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, 10, 70, headerStr, 0, true, EpdFontFamily::BOLD);

    // Show up to 10 devices (pagination via scrolling)
    int maxDevicesToShow = 10;
    int startIndex = 0;
    if (bluetoothDeviceSelection >= maxDevicesToShow) {
      startIndex = bluetoothDeviceSelection - maxDevicesToShow + 1;
    }
    int devicesToShow = (deviceCount - startIndex < maxDevicesToShow)
                        ? deviceCount - startIndex : maxDevicesToShow;

    for (int i = 0; i < devicesToShow; i++) {
      int deviceIndex = startIndex + i;
      int yPos = 90 + (i * 30);

      // Stop drawing if we'd go into the footer zone
      if (yPos > sh - 100) break;

      bool isSelected = (bluetoothDeviceSelection == deviceIndex);
      bool isConnected = (getCurrentDeviceAddress() == devices[deviceIndex].address);

      const char* displayName = devices[deviceIndex].name.empty()
                                ? devices[deviceIndex].address.c_str()
                                : devices[deviceIndex].name.c_str();

      // Available width: leave room for RSSI on the right (~80px)
      int nameMaxW = sw - 100;

      if (isSelected || isConnected) {
        clippedFillRect(renderer, 5, yPos - 5, sw - 10, 25, true);
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW, false);
      } else {
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW);
      }

      // RSSI on the right
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%ddBm", devices[deviceIndex].rssi);
      drawRightText(renderer, FONT_SMALL, sw - 10, yPos, rssiStr);
    }

    // Page indicator
    if (deviceCount > maxDevicesToShow) {
      char navHint[32];
      int pageNum = (bluetoothDeviceSelection / maxDevicesToShow) + 1;
      int totalPages = (deviceCount + maxDevicesToShow - 1) / maxDevicesToShow;
      snprintf(navHint, sizeof(navHint), "Page %d/%d", pageNum, totalPages);
      int navY = 90 + (devicesToShow * 30);
      if (navY < sh - 100)
        drawClippedText(renderer, FONT_SMALL, 15, navY, navHint);
    }
  } else {
    drawClippedText(renderer, FONT_UI, 20, 80, "No devices found");
    drawClippedText(renderer, FONT_SMALL, 20, 100, "Press Enter to scan for devices");
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12,
                    "Enter:Scan/Select  Left:Disconnect  Esc:Back");
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
