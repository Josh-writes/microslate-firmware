#pragma once

#include <sdkconfig.h>
#include <cstdint>
#include <cstddef>

// --- UI State Machine ---
enum class UIState {
  MAIN_MENU,
  FILE_BROWSER,
  TEXT_EDITOR,
  RENAME_FILE,
  NEW_FILE,
  SETTINGS,
  BLUETOOTH_SETTINGS,
  PAIRED_KEYBOARDS,
  WIFI_SYNC,
  SD_MISSING,     // card pulled: nothing works until it is back
  LAYOUT_EDITOR,  // change what keys type (the active keyboard layout file)
  TRANSFER,       // the Transfer screen: a phone or computer copies or downloads notes
  SLEEP_PICTURES, // the sleep pictures: list, preview, pin, delete, add from a phone
  MY_WORDS,       // the writer's own spellcheck words: list and remove (Settings > Editor)
  FW_UPDATE,      // firmware update from the GitHub release (Settings > Wi-Fi)
  READER          // dual boot: install, swap or remove a reader (Settings > Wi-Fi > Reader)
};

// --- Display Orientation ---
// Values map to GfxRenderer::Orientation enum
enum class Orientation : uint8_t {
  PORTRAIT = 0,
  LANDSCAPE_CW = 1,    // LandscapeClockwise
  PORTRAIT_INV = 2,     // PortraitInverted
  LANDSCAPE_CCW = 3     // LandscapeCounterClockwise
};

// Devices that sit on a stand in one fixed position (E1001) lock the orientation: the setting
// stays visible but cannot change it, and a saved value from another build is overridden.
#if defined(FREEINK_DEVICE_E1001) && FREEINK_DEVICE_E1001
constexpr bool kOrientationLocked = true;
constexpr Orientation kLockedOrientation = Orientation::LANDSCAPE_CCW;
#else
constexpr bool kOrientationLocked = false;
constexpr Orientation kLockedOrientation = Orientation::PORTRAIT;
#endif

// Button-only devices (E1001, X4 Classic): the original v2.1 list look (small header, plain rows, compact button
// strip) instead of the touch layout. The E1001 has no Back button: Up and Down together are Back there. The X4 Classic
// has the X4's front row (Left, Right, Confirm, Back) and a side pair (Up, Down), and is held in any orientation.
#if defined(FREEINK_DEVICE_E1001) && FREEINK_DEVICE_E1001
constexpr bool kClassicUi = true;
constexpr bool kUpDownIsBack = true;
constexpr bool kFrontRowAndSidePair = false;
#elif defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC
constexpr bool kClassicUi = true;
constexpr bool kUpDownIsBack = false;
constexpr bool kFrontRowAndSidePair = true;
#else
constexpr bool kClassicUi = false;
constexpr bool kUpDownIsBack = false;
constexpr bool kFrontRowAndSidePair = false;
#endif

// --- Writing Modes ---
enum class WritingMode : uint8_t {
  NORMAL     = 0,   // Standard scrolling editor
  TYPEWRITER = 1,   // Shows only current line centered on screen
  PAGINATION = 2    // Page-based display instead of scrolling
};

// --- BLE Connection State ---
enum class BLEState : uint8_t {
  DISCONNECTED,
  SCANNING,
  CONNECTING,
  CONNECTED
};

// --- Key Event (for input queue) ---
struct KeyEvent {
  uint8_t keyCode;
  uint8_t modifiers;
  bool pressed;
};

// --- File Info ---
static constexpr int MAX_FILENAME_LEN = 64;   // longer names on the card are not listed (sync_names.h agrees)
static constexpr int MAX_TITLE_LEN = 40;
static constexpr int MAX_FIND_LEN = 48;   // in-document find query

struct FileInfo {
  char filename[MAX_FILENAME_LEN];
  char title[MAX_TITLE_LEN];
  unsigned long modTime;
};

// --- Auto-save timing ---
static constexpr unsigned long AUTO_SAVE_IDLE_MS     = 10000;  // Save after 10s of no keystrokes
static constexpr unsigned long AUTO_SAVE_MAX_MS      = 60000;  // Hard cap during continuous typing (shrinks crash loss window)
static constexpr unsigned long AUTO_SAVE_PARA_MIN_MS = 3000;   // Also save on a paragraph break, at most this often
static constexpr unsigned long AUTO_SAVE_RETRY_MS    = 5000;   // After a failed autosave, the next try waits this long

// --- Firmware version ---
// The release tag is "v" + this (release.yml checks it). Settings > Wi-Fi > Firmware update compares it with the
// latest GitHub release. A test build may set another version and MS_UPDATE_REPO (a test repository) with -D.
#ifndef MS_FIRMWARE_VERSION
#define MS_FIRMWARE_VERSION "3.0.4"
#endif

// --- Buffer/Queue Sizes ---
// Note capacity. Boards with PSRAM (the S3 devices) hold much longer notes; the X4 keeps its small buffer.
#if defined(CONFIG_SPIRAM)
// 512 KB, about 75,000 to 90,000 words: a whole novel draft (Josh, 2026-10-29; a book belongs in chapter notes, and
// autosaving multi-megabyte files costs time and SD wear). A key only re-wraps the lines around it (editor_layout.h).
// The line table grows as needed; its ceiling covers the worst case of one line per byte.
static constexpr size_t TEXT_BUFFER_SIZE = 524288;
static constexpr int MAX_LINES = (int)TEXT_BUFFER_SIZE + 1;
// Undo history (edit_history.h): 1 MB in PSRAM, oldest steps dropped when full (work order 1.2).
static constexpr size_t UNDO_HISTORY_SIZE = 1048576;
#else
static constexpr size_t TEXT_BUFFER_SIZE = 16384;
static constexpr int MAX_LINES = 1024;
// The X4 has no PSRAM: a small history (about a hundred typed words), to be checked against its free heap.
static constexpr size_t UNDO_HISTORY_SIZE = 4096;
#endif
// Notes the list holds. The list grows as notes are found (PSRAM on the S3 boards), so these are ceilings, not
// reservations: 108 bytes a note. Past the ceiling the newest notes are kept.
#if defined(CONFIG_SPIRAM)
static constexpr int MAX_FILES = 2000;
#else
static constexpr int MAX_FILES = 200;
#endif
static constexpr int INPUT_QUEUE_SIZE = 128;  // a touch tap on a far row queues up to ~60 events

// --- Font IDs (from crosspoint-reader fontIds.h) ---
#define FONT_BODY    (-1014561631)   // NOTOSANS_14_FONT_ID
#define FONT_UI      (-1559651934)   // NOTOSANS_12_FONT_ID
#define FONT_SMALL   (-1246724383)   // UI_10_FONT_ID (ubuntu 10)
#define FONT_LARGE   (-1422711852)   // NOTOSANS_16_FONT_ID

// --- Font Size ---
enum class FontSize : uint8_t { SMALL = 0, MEDIUM = 1, LARGE = 2 };

inline int editorFontId(FontSize size) {
  switch (size) {
    case FontSize::SMALL:  return FONT_UI;    // notosans 12
    case FontSize::MEDIUM: return FONT_BODY;  // notosans 14
    default:               return FONT_LARGE; // notosans 16
  }
}

// --- HID Keycodes ---
static constexpr uint8_t HID_KEY_A          = 0x04;
static constexpr uint8_t HID_KEY_B          = 0x05;
static constexpr uint8_t HID_KEY_C          = 0x06;
static constexpr uint8_t HID_KEY_D          = 0x07;
static constexpr uint8_t HID_KEY_E          = 0x08;
static constexpr uint8_t HID_KEY_F          = 0x09;
static constexpr uint8_t HID_KEY_G          = 0x0A;
static constexpr uint8_t HID_KEY_H          = 0x0B;
static constexpr uint8_t HID_KEY_I          = 0x0C;
static constexpr uint8_t HID_KEY_J          = 0x0D;
static constexpr uint8_t HID_KEY_K          = 0x0E;
static constexpr uint8_t HID_KEY_L          = 0x0F;
static constexpr uint8_t HID_KEY_M          = 0x10;
static constexpr uint8_t HID_KEY_N          = 0x11;
static constexpr uint8_t HID_KEY_O          = 0x12;
static constexpr uint8_t HID_KEY_P          = 0x13;
static constexpr uint8_t HID_KEY_Q          = 0x14;
static constexpr uint8_t HID_KEY_R          = 0x15;
static constexpr uint8_t HID_KEY_S          = 0x16;
static constexpr uint8_t HID_KEY_T          = 0x17;
static constexpr uint8_t HID_KEY_U          = 0x18;
static constexpr uint8_t HID_KEY_V          = 0x19;
static constexpr uint8_t HID_KEY_W          = 0x1A;
static constexpr uint8_t HID_KEY_X          = 0x1B;
static constexpr uint8_t HID_KEY_Y          = 0x1C;
static constexpr uint8_t HID_KEY_Z          = 0x1D;
static constexpr uint8_t HID_KEY_1          = 0x1E;
static constexpr uint8_t HID_KEY_2          = 0x1F;
static constexpr uint8_t HID_KEY_3          = 0x20;
static constexpr uint8_t HID_KEY_4          = 0x21;
static constexpr uint8_t HID_KEY_5          = 0x22;
static constexpr uint8_t HID_KEY_6          = 0x23;
static constexpr uint8_t HID_KEY_7          = 0x24;
static constexpr uint8_t HID_KEY_8          = 0x25;
static constexpr uint8_t HID_KEY_9          = 0x26;
static constexpr uint8_t HID_KEY_0          = 0x27;
static constexpr uint8_t HID_KEY_ENTER      = 0x28;
static constexpr uint8_t HID_KEY_ESCAPE     = 0x29;
// Pseudo keys for the four bottom-bar buttons (0xF0..0xF3): produced by a tap on the
// button or by Enter on the focused one. Above every real HID usage code.
static constexpr uint8_t HID_KEY_BAR0       = 0xF0;
// Pseudo keys for a tap on one of the Settings tabs (0xF4..0xF8).
static constexpr uint8_t HID_KEY_TAB0       = 0xF4;
// Pseudo key for a tap on the editor's menu icon (the same as Ctrl+M).
static constexpr uint8_t HID_KEY_EDITOR_MENU = 0xF9;
// Pseudo key for a press and hold on the highlighted row of the notes list: select notes for deleting.
static constexpr uint8_t HID_KEY_ROW_HOLD   = 0xFA;
static constexpr uint8_t HID_KEY_BACKSPACE  = 0x2A;
static constexpr uint8_t HID_KEY_TAB        = 0x2B;
static constexpr uint8_t HID_KEY_SPACE      = 0x2C;
static constexpr uint8_t HID_KEY_MINUS      = 0x2D;
static constexpr uint8_t HID_KEY_EQUAL      = 0x2E;
static constexpr uint8_t HID_KEY_LEFTBRACE  = 0x2F;
static constexpr uint8_t HID_KEY_RIGHTBRACE = 0x30;
static constexpr uint8_t HID_KEY_BACKSLASH  = 0x31;
static constexpr uint8_t HID_KEY_SEMICOLON  = 0x33;
static constexpr uint8_t HID_KEY_APOSTROPHE = 0x34;
static constexpr uint8_t HID_KEY_GRAVE      = 0x35;
static constexpr uint8_t HID_KEY_COMMA      = 0x36;
static constexpr uint8_t HID_KEY_DOT        = 0x37;
static constexpr uint8_t HID_KEY_SLASH      = 0x38;
static constexpr uint8_t HID_KEY_DELETE     = 0x4C;
static constexpr uint8_t HID_KEY_RIGHT      = 0x4F;
static constexpr uint8_t HID_KEY_LEFT       = 0x50;
static constexpr uint8_t HID_KEY_DOWN       = 0x51;
static constexpr uint8_t HID_KEY_UP         = 0x52;
static constexpr uint8_t HID_KEY_HOME       = 0x4A;
static constexpr uint8_t HID_KEY_END        = 0x4D;
static constexpr uint8_t HID_KEY_CAPSLOCK   = 0x39;
static constexpr uint8_t HID_KEY_F2         = 0x3B;
static constexpr uint8_t HID_KEY_NON_US_BACKSLASH = 0x64;

// --- HID Modifier Masks ---
static constexpr uint8_t MOD_CTRL_LEFT   = 0x01;
static constexpr uint8_t MOD_SHIFT_LEFT  = 0x02;
static constexpr uint8_t MOD_ALT_LEFT    = 0x04;
static constexpr uint8_t MOD_GUI_LEFT    = 0x08;   // Win / Cmd
static constexpr uint8_t MOD_CTRL_RIGHT  = 0x10;
static constexpr uint8_t MOD_SHIFT_RIGHT = 0x20;
static constexpr uint8_t MOD_ALT_RIGHT   = 0x40;
static constexpr uint8_t MOD_GUI_RIGHT   = 0x80;

inline bool isCtrl(uint8_t mod) {
  return (mod & MOD_CTRL_LEFT) || (mod & MOD_CTRL_RIGHT);
}
inline bool isShift(uint8_t mod) {
  return (mod & MOD_SHIFT_LEFT) || (mod & MOD_SHIFT_RIGHT);
}
inline bool isAltGr(uint8_t mod) {
  const bool rightAlt = (mod & MOD_ALT_RIGHT) != 0;
  const bool ctrlAlt = ((mod & (MOD_CTRL_LEFT | MOD_CTRL_RIGHT)) != 0)
                    && ((mod & (MOD_ALT_LEFT | MOD_ALT_RIGHT)) != 0);
  return rightAlt || ctrlAlt;
}

// --- Debug Logging ---
// Define RELEASE_BUILD in platformio.ini to disable all serial output.
// This saves significant power by keeping the UART peripheral inactive.
#ifdef RELEASE_BUILD
  #define DBG_INIT()
  #define DBG_PRINTF(fmt, ...)
  #define DBG_PRINTLN(s)
  #define DBG_PRINT(s)
#else
  #define DBG_INIT()            Serial.begin(115200)
  #define DBG_PRINTF(fmt, ...)  Serial.printf(fmt, ##__VA_ARGS__)
  #define DBG_PRINTLN(s)        Serial.println(s)
  #define DBG_PRINT(s)          Serial.print(s)
#endif
