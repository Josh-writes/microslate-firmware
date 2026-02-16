#include "ui_renderer.h"
#include "config.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"

#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <HalDisplay.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern bool deleteConfirmPending;
extern WritingMode writingMode;

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
  // Use getTextWidth (bounding box) — same measurement truncatedText uses —
  // so the allocated space always matches what the truncation check expects.
  int tw = r.getTextWidth(font, text, style);
  if (tw <= 0) tw = 30;                    // safe fallback
  int x = rightEdge - tw;
  if (x < 5) x = 5;                        // don't go off left edge
  drawClippedText(r, font, x, y, text, rightEdge - x, black, style);
}

// Safe line — just clamp to screen
static void clippedLine(GfxRenderer& r, int x1, int y1, int x2, int y2,
                        bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  // Clamp rather than reject
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  x1 = clamp(x1, 0, sw - 1);
  x2 = clamp(x2, 0, sw - 1);
  y1 = clamp(y1, 0, sh - 1);
  y2 = clamp(y2, 0, sh - 1);
  r.drawLine(x1, y1, x2, y2, state);
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
  drawRightText(renderer, FONT_SMALL, renderer.getScreenWidth() - 8, 5, buf, !darkMode);
}

// Helper: draw BLE status
static void drawBleStatus(GfxRenderer& renderer, int x, int y) {
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "KB Connected"; break;
    case BLEState::SCANNING:     status = "Scanning..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "KB Disconnected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, x, y, status, 0, !darkMode);
}

// ===========================================================================
// Screen drawing functions
// ===========================================================================

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;  // text color

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Title
  renderer.drawCenteredText(FONT_BODY, 30, "MicroSlate", tc, EpdFontFamily::BOLD);

  // Menu items
  static const char* menuItems[] = {"Browse Files", "New Note", "Settings", "Sync"};
  for (int i = 0; i < 4; i++) {
    int yPos = 90 + (i * 45);
    if (i == mainMenuSelection) {
      clippedFillRect(renderer, 5, yPos - 5, sw - 10, 35, tc);
      drawClippedText(renderer, FONT_UI, 20, yPos, menuItems[i], sw - 40, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 20, yPos, menuItems[i], sw - 40, tc);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 40) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12, "Arrows: Navigate  Enter: Select", 0, tc);
    drawBleStatus(renderer, 20, sh - bm + 28);
  }
  drawBattery(renderer, gpio);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Notes", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  int fc = getFileCount();
  int lineH = 30;
  int listTop = 42;
  int footerH = 28;  // one line of FONT_SMALL with safe bottom margin
  int maxVisible = (sh - listTop - footerH) / lineH;
  int startIdx = 0;
  if (fc > maxVisible && selectedFileIndex >= maxVisible) {
    startIdx = selectedFileIndex - maxVisible + 1;
  }

  if (fc == 0) {
    drawClippedText(renderer, FONT_UI, 20, listTop + 14, "No notes yet.", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, listTop + 36, "Press Ctrl+N to create one.", 0, tc);
  }

  FileInfo* files = getFileList();
  for (int i = startIdx; i < fc && (i - startIdx) < maxVisible; i++) {
    int yPos = listTop + (i - startIdx) * lineH;

    if (i == selectedFileIndex) {
      clippedFillRect(renderer, 5, yPos - 3, sw - 10, lineH - 1, tc);
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30, !tc);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, files[i].title, sw - 30, tc);
    }
  }

  // Footer
  clippedLine(renderer, 5, sh - footerH - 2, sw - 5, sh - footerH - 2, tc);
  if (deleteConfirmPending && fc > 0) {
    drawClippedText(renderer, FONT_SMALL, 10, sh - footerH + 4, "Delete? Enter:Yes  Esc:No", 0, tc);
  } else {
    drawClippedText(renderer, FONT_SMALL, 10, sh - footerH + 4,
                    "Ctrl+N:Title  Ctrl+D:Delete", 0, tc);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Helper: draw a single editor line from the buffer
static void drawEditorLine(GfxRenderer& renderer, int lineIdx, int x, int yPos,
                           int maxW, bool tc) {
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();
  int totalLines = editorGetLineCount();

  int lineStart = editorGetLinePosition(lineIdx);
  int lineEnd = (lineIdx + 1 < totalLines) ? editorGetLinePosition(lineIdx + 1) : (int)bufLen;
  int dispEnd = lineEnd;
  if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

  int len = dispEnd - lineStart;
  if (len > 0) {
    char lineBuf[256];
    int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
    strncpy(lineBuf, buf + lineStart, copyLen);
    lineBuf[copyLen] = '\0';
    drawClippedText(renderer, FONT_BODY, x, yPos, lineBuf, maxW, tc);
  }
}

// Helper: draw cursor at the given screen position
static void drawEditorCursor(GfxRenderer& renderer, int cursorY, int lineHeight,
                             int sw, bool tc) {
  int curLine = editorGetCursorLine();
  int curCol = editorGetCursorCol();
  char* buf = editorGetBuffer();

  int lineStart = editorGetLinePosition(curLine);
  char prefix[256];
  int prefixLen = (curCol < (int)sizeof(prefix) - 1) ? curCol : (int)sizeof(prefix) - 1;
  strncpy(prefix, buf + lineStart, prefixLen);
  prefix[prefixLen] = '\0';

  int cursorX = 10 + renderer.getTextAdvanceX(FONT_BODY, prefix);
  int cursorW = renderer.getSpaceWidth(FONT_BODY);
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 && cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, tc);
  }
}

// Get the mode indicator string for the current writing mode
static const char* getModeIndicator() {
  switch (writingMode) {
    case WritingMode::TYPEWRITER: return "[T]";
    case WritingMode::PAGINATION: return "[P]";
    default:                      return "[S]";
  }
}

// Helper: draw the standard editor header, returns textAreaTop
// centerText is optional text drawn centered in the header (e.g. "Page 1/3")
static int drawEditorHeader(GfxRenderer& renderer, HalGPIO& gpio, int sw, bool tc,
                            const char* centerText = nullptr) {
  if (cleanMode) return 8;

  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }
  drawClippedText(renderer, FONT_SMALL, 10, 5, headerBuf, sw - 100, tc, EpdFontFamily::BOLD);

  // Centered text (e.g. page indicator)
  if (centerText) {
    int ctW = renderer.getTextAdvanceX(FONT_SMALL, centerText);
    drawClippedText(renderer, FONT_SMALL, (sw - ctW) / 2, 5, centerText, ctW + 5, tc);
  }

  // Mode indicator (always shown, left of battery with gap)
  const char* modeInd = getModeIndicator();
  int indW = renderer.getTextAdvanceX(FONT_SMALL, modeInd);
  drawClippedText(renderer, FONT_SMALL, sw - 70 - indW, 5, modeInd, indW + 5, tc);

  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);
  return 38;
}

void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  int lineHeight = renderer.getLineHeight(FONT_BODY);
  if (lineHeight <= 0) lineHeight = 20;
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();

  // --- TYPEWRITER MODE ---
  if (writingMode == WritingMode::TYPEWRITER) {
    // In clean mode (Ctrl+Z): just text on blank screen, no header
    int textAreaTop = cleanMode ? 0 : drawEditorHeader(renderer, gpio, sw, tc);

    // Center the current line vertically
    int textAreaHeight = sh - textAreaTop;
    int centerY = textAreaTop + (textAreaHeight / 2) - (lineHeight / 2);

    // Draw only the current line
    if (curLine < totalLines) {
      drawEditorLine(renderer, curLine, 10, centerY, sw - 20, tc);
    }

    // Draw cursor
    drawEditorCursor(renderer, centerY, lineHeight, sw, tc);

    editorSetVisibleLines(1);

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- PAGINATION MODE ---
  if (writingMode == WritingMode::PAGINATION) {
    // Pre-compute page info for the header
    // Use a temporary linesPerPage estimate (will be exact since header height is fixed)
    int tempTextTop = cleanMode ? 8 : 38;
    int tempLinesPerPage = (sh - 5 - tempTextTop) / lineHeight;
    if (tempLinesPerPage < 1) tempLinesPerPage = 1;
    int currentPage = curLine / tempLinesPerPage;
    int totalPages = (totalLines + tempLinesPerPage - 1) / tempLinesPerPage;
    if (totalPages < 1) totalPages = 1;

    char pageStr[16];
    snprintf(pageStr, sizeof(pageStr), "Pg %d/%d", currentPage + 1, totalPages);
    int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc, pageStr);

    int textAreaBottom = sh - 5;
    int textAreaHeight = textAreaBottom - textAreaTop;
    int linesPerPage = textAreaHeight / lineHeight;
    if (linesPerPage < 1) linesPerPage = 1;

    // Recompute with actual linesPerPage if it differs
    currentPage = curLine / linesPerPage;
    int pageStart = currentPage * linesPerPage;

    editorSetVisibleLines(linesPerPage);

    // Draw lines for this page
    for (int i = 0; i < linesPerPage && (pageStart + i) < totalLines; i++) {
      int yPos = textAreaTop + (i * lineHeight);
      drawEditorLine(renderer, pageStart + i, 10, yPos, sw - 20, tc);
    }

    // Draw cursor if on this page
    if (curLine >= pageStart && curLine < pageStart + linesPerPage) {
      int cursorY = textAreaTop + ((curLine - pageStart) * lineHeight);
      drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
    }

    renderer.displayBuffer(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- NORMAL MODE ---
  int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc);

  int textAreaBottom = sh - 5;
  int textAreaHeight = textAreaBottom - textAreaTop;
  int visibleLines = textAreaHeight / lineHeight;

  editorSetVisibleLines(visibleLines);

  int vpStart = editorGetViewportStart();
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();

  // Draw visible lines
  for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
    int yPos = textAreaTop + (i * lineHeight);
    drawEditorLine(renderer, vpStart + i, 10, yPos, sw - 20, tc);
  }

  // Draw cursor
  if (curLine >= vpStart && curLine < vpStart + visibleLines) {
    int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
    drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Edit Title", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  drawClippedText(renderer, FONT_SMALL, 20, 42, "Note title:", 0, tc);
  int boxY = 64, boxH = 36;
  int textY = boxY + 8;
  renderer.drawRect(15, boxY, sw - 30, boxH, tc);
  drawClippedText(renderer, FONT_UI, 20, textY, renameBuffer, sw - 50, tc);

  // Cursor — thin bar aligned with text
  int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, renameBuffer);
  if (cursorX + 2 < sw - 15)
    renderer.fillRect(cursorX, textY, 2, 16, tc);

  // Footer
  clippedLine(renderer, 5, sh - 36, sw - 5, sh - 36, tc);
  drawClippedText(renderer, FONT_SMALL, 10, sh - 30, "Enter: Confirm   Esc: Cancel", 0, tc);

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawClippedText(renderer, FONT_SMALL, 10, 5, "Settings", 0, !darkMode, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, !darkMode);

  // Setting items: Orientation, Dark Mode, Writing Mode, Bluetooth, Clear Paired
  static const char* labels[] = {
    "Orientation", "Dark Mode", "Writing Mode", "Bluetooth", "Clear Paired"
  };
  const int SETTINGS_COUNT = 5;

  // Compute line height to fit all items — use smaller spacing if needed
  int lineH = 38;
  int listTop = 50;
  if (listTop + SETTINGS_COUNT * lineH > sh - 70) {
    lineH = (sh - 70 - listTop) / SETTINGS_COUNT;
    if (lineH < 24) lineH = 24;
  }

  for (int i = 0; i < SETTINGS_COUNT; i++) {
    int yPos = listTop + (i * lineH);
    bool sel = (i == settingsSelection);

    if (sel) {
      clippedFillRect(renderer, 5, yPos - 5, sw - 10, lineH - 6, !darkMode);
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, darkMode);
    } else {
      drawClippedText(renderer, FONT_UI, 15, yPos, labels[i], sw / 2 - 15, !darkMode);
    }

    // Value on the right
    char val[32] = "";
    if (i == 0) {
      switch (currentOrientation) {
        case Orientation::PORTRAIT:      strcpy(val, "Portrait"); break;
        case Orientation::LANDSCAPE_CW:  strcpy(val, "Landscape CW"); break;
        case Orientation::PORTRAIT_INV:  strcpy(val, "Inverted"); break;
        case Orientation::LANDSCAPE_CCW: strcpy(val, "Landscape CCW"); break;
      }
    } else if (i == 1) {
      strcpy(val, darkMode ? "Dark" : "Light");
    } else if (i == 2) {
      switch (writingMode) {
        case WritingMode::NORMAL:     strcpy(val, "Normal"); break;
        case WritingMode::TYPEWRITER: strcpy(val, "Typewriter"); break;
        case WritingMode::PAGINATION: strcpy(val, "Pagination"); break;
      }
    } else if (i == 4) {
      std::string storedAddr, storedName;
      if (getStoredDevice(storedAddr, storedName)) {
        snprintf(val, sizeof(val), "%s", storedName.c_str());
      } else {
        strcpy(val, "None");
      }
    }

    if (val[0] != '\0') {
      drawRightText(renderer, FONT_UI, sw - 20, yPos, val, sel ? darkMode : !darkMode);
    }
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, !darkMode);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12,
                    "Arrows:Navigate  Enter:Change  Esc:Back", 0, !darkMode);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();

  renderer.clearScreen();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Bluetooth Devices", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  // Connection status
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "Connected to keyboard"; break;
    case BLEState::SCANNING:     status = "Scanning for devices..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "Not connected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, 10, 45, status, sw / 2 - 10, tc);

  // Paired device info
  std::string storedAddr, storedName;
  if (getStoredDevice(storedAddr, storedName)) {
    char pairedStr[64];
    snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
    drawClippedText(renderer, FONT_SMALL, sw / 2, 45, pairedStr, sw / 2 - 10, tc);
  }

  // Passkey display
  uint32_t passkey = getCurrentPasskey();
  if (passkey > 0) {
    char passkeyStr[32];
    drawClippedText(renderer, FONT_UI, 20, 100, "PAIRING CODE:", 0, tc, EpdFontFamily::BOLD);
    snprintf(passkeyStr, sizeof(passkeyStr), "%06lu", passkey);
    drawClippedText(renderer, FONT_BODY, 20, 130, passkeyStr, 0, tc, EpdFontFamily::BOLD);
    drawClippedText(renderer, FONT_SMALL, 20, 160, "Type this code on your keyboard", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 180, "then press Enter", 0, tc);
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
    drawClippedText(renderer, FONT_SMALL, 10, 60, scanningStr, sw / 2 - 10, tc);

    char foundStr[32];
    snprintf(foundStr, sizeof(foundStr), "Found: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, sw / 2, 60, foundStr, sw / 2 - 10, tc);
  }

  // Device list
  int deviceCount = getDiscoveredDeviceCount();
  if (deviceCount > 0) {
    BleDeviceInfo* devices = getDiscoveredDevices();

    char headerStr[64];
    snprintf(headerStr, sizeof(headerStr), "Available devices: %d", deviceCount);
    drawClippedText(renderer, FONT_SMALL, 10, 70, headerStr, 0, tc, EpdFontFamily::BOLD);

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
        clippedFillRect(renderer, 5, yPos - 5, sw - 10, 25, tc);
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW, !tc);
      } else {
        drawClippedText(renderer, FONT_UI, 15, yPos, displayName, nameMaxW, tc);
      }

      // RSSI on the right
      char rssiStr[16];
      snprintf(rssiStr, sizeof(rssiStr), "%ddBm", devices[deviceIndex].rssi);
      drawRightText(renderer, FONT_SMALL, sw - 10, yPos, rssiStr, tc);
    }

    // Page indicator
    if (deviceCount > maxDevicesToShow) {
      char navHint[32];
      int pageNum = (bluetoothDeviceSelection / maxDevicesToShow) + 1;
      int totalPages = (deviceCount + maxDevicesToShow - 1) / maxDevicesToShow;
      snprintf(navHint, sizeof(navHint), "Page %d/%d", pageNum, totalPages);
      int navY = 90 + (devicesToShow * 30);
      if (navY < sh - 100)
        drawClippedText(renderer, FONT_SMALL, 15, navY, navHint, 0, tc);
    }
  } else {
    drawClippedText(renderer, FONT_UI, 20, 80, "No devices found", 0, tc);
    drawClippedText(renderer, FONT_SMALL, 20, 100, "Press Enter to scan for devices", 0, tc);
  }

  // Footer
  constexpr int bm = 60;
  if (sh > bm + 30) {
    clippedLine(renderer, 10, sh - bm, sw - 10, sh - bm, tc);
    drawClippedText(renderer, FONT_SMALL, 20, sh - bm + 12,
                    "Enter:Connect  Right:Scan  Left:Disconnect  Esc:Back", 0, tc);
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}

// Helper: draw signal strength indicator (1-4 bars)
static void drawSignalBars(GfxRenderer& r, int x, int y, int rssi, bool color) {
  // RSSI to bars: > -50 = 4, > -65 = 3, > -75 = 2, else 1
  int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 3;  // bar heights: 4, 7, 10, 13
    int by = y + 13 - bh;
    if (i < bars) {
      clippedFillRect(r, x + i * 5, by, 3, bh, color);
    } else {
      clippedFillRect(r, x + i * 5, by + bh - 2, 3, 2, color);
    }
  }
}

void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  drawClippedText(renderer, FONT_SMALL, 10, 5, "Sync", 0, tc, EpdFontFamily::BOLD);
  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);

  SyncState state = getSyncState();

  switch (state) {
    case SyncState::SCANNING: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Scanning for networks...", sw - 40, tc);
      break;
    }

    case SyncState::NETWORK_LIST: {
      int nc = getNetworkCount();
      int sel = getSelectedNetwork();

      if (nc == 0) {
        const char* st = getSyncStatusText();
        drawClippedText(renderer, FONT_UI, 20, 60, st[0] ? st : "No networks found", sw - 40, tc);
        drawClippedText(renderer, FONT_SMALL, 20, 90, "Enter: Rescan  Esc: Back", 0, tc);
      } else {
        drawClippedText(renderer, FONT_SMALL, 10, 38, "Select network:", 0, tc);

        int lineH = 28;
        int listTop = 56;
        int footerH = 28;
        int maxVisible = (sh - listTop - footerH) / lineH;
        int startIdx = 0;
        if (nc > maxVisible && sel >= maxVisible) {
          startIdx = sel - maxVisible + 1;
        }

        for (int i = startIdx; i < nc && (i - startIdx) < maxVisible; i++) {
          int yPos = listTop + (i - startIdx) * lineH;
          bool isSel = (i == sel);

          // Build display string: signal indicator + lock + saved + SSID
          char label[48];
          snprintf(label, sizeof(label), "%s%s%s",
                   isNetworkEncrypted(i) ? "* " : "  ",
                   isNetworkSaved(i) ? "+ " : "",
                   getNetworkSSID(i));

          if (isSel) {
            clippedFillRect(renderer, 5, yPos - 3, sw - 10, lineH - 2, tc);
            drawClippedText(renderer, FONT_UI, 15, yPos, label, sw - 50, !tc);
            drawSignalBars(renderer, sw - 30, yPos, getNetworkRSSI(i), !tc);
          } else {
            drawClippedText(renderer, FONT_UI, 15, yPos, label, sw - 50, tc);
            drawSignalBars(renderer, sw - 30, yPos, getNetworkRSSI(i), tc);
          }
        }
      }

      // Footer
      constexpr int bm = 28;
      clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4,
                      "*=encrypted +=saved  Enter:Select  Esc:Back", 0, tc);
      break;
    }

    case SyncState::PASSWORD_ENTRY: {
      int sel = getSelectedNetwork();
      char heading[48];
      snprintf(heading, sizeof(heading), "Password for %s", getNetworkSSID(sel));
      drawClippedText(renderer, FONT_SMALL, 20, 42, heading, sw - 40, tc);

      // Password field box
      renderer.drawRect(15, 62, sw - 30, 30, tc);

      // Show dots for password characters (privacy)
      const char* pass = getPasswordBuffer();
      int pLen = getPasswordLen();
      char dots[MAX_TITLE_LEN + 1];
      int dotLen = pLen < MAX_TITLE_LEN ? pLen : MAX_TITLE_LEN;
      for (int i = 0; i < dotLen; i++) dots[i] = '*';
      dots[dotLen] = '\0';
      drawClippedText(renderer, FONT_UI, 20, 66, dots, sw - 50, tc);

      // Cursor
      int cursorX = 20 + renderer.getTextAdvanceX(FONT_UI, dots);
      int cursorW = renderer.getSpaceWidth(FONT_UI);
      if (cursorW < 2) cursorW = 8;
      if (cursorX + cursorW < sw)
        renderer.fillRect(cursorX, 66, cursorW, 20, tc);

      drawClippedText(renderer, FONT_SMALL, 20, 110, "Enter: Connect   Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::CONNECTING: {
      const char* st = getSyncStatusText();
      drawClippedText(renderer, FONT_UI, 20, 80, st, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 110, "Esc: Cancel", 0, tc);
      break;
    }

    case SyncState::SYNCING: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 42, ip, sw - 40, tc, EpdFontFamily::BOLD);

      int logCount = getSyncLogCount();
      if (logCount == 0) {
        drawClippedText(renderer, FONT_UI, 20, 75, "Waiting for PC...", sw - 40, tc);
        drawClippedText(renderer, FONT_SMALL, 20, 110, "Run microslate_sync.py on PC", sw - 40, tc);
        drawClippedText(renderer, FONT_SMALL, 20, 130, "See README for setup", sw - 40, tc);
      } else {
        // Show activity log
        int yPos = 68;
        for (int i = 0; i < logCount && yPos < sh - 50; i++) {
          drawClippedText(renderer, FONT_SMALL, 20, yPos, getSyncLogLine(i), sw - 40, tc);
          yPos += 20;
        }
      }

      // Footer
      constexpr int bm = 28;
      clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
      char countStr[48];
      snprintf(countStr, sizeof(countStr), "Sent: %d  Recv: %d   Esc: Cancel",
               getSyncFilesSent(), getSyncFilesReceived());
      drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4, countStr, sw - 20, tc);
      break;
    }

    case SyncState::DONE: {
      const char* summary = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Sync Complete", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 85, summary, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 125, "Returning to menu...", 0, tc);
      break;
    }

    case SyncState::CONNECT_FAILED: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Connection failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 110, "Enter: Retry   Esc: Back", 0, tc);
      break;
    }

    case SyncState::SAVE_PROMPT: {
      const char* ip = getSyncStatusText();
      drawClippedText(renderer, FONT_SMALL, 20, 50, "Connected!", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_UI, 20, 80, ip, sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Save password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }

    case SyncState::FORGET_PROMPT: {
      drawClippedText(renderer, FONT_UI, 20, 80, "Saved password failed", sw - 40, tc);
      drawClippedText(renderer, FONT_SMALL, 20, 120, "Forget saved password?", 0, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, FONT_SMALL, 20, 145, "Enter/Up: Yes   Down/Esc: No", 0, tc);
      break;
    }
  }

  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
