#pragma once

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
  BLUETOOTH_SETTINGS
};

// --- Display Orientation ---
// Values map to GfxRenderer::Orientation enum
enum class Orientation : uint8_t {
  PORTRAIT = 0,
  LANDSCAPE_CW = 1,    // LandscapeClockwise
  PORTRAIT_INV = 2,     // PortraitInverted
  LANDSCAPE_CCW = 3     // LandscapeCounterClockwise
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
static constexpr int MAX_FILENAME_LEN = 64;
static constexpr int MAX_TITLE_LEN = 40;

struct FileInfo {
  char filename[MAX_FILENAME_LEN];
  char title[MAX_TITLE_LEN];
  unsigned long modTime;
};

// --- Buffer/Queue Sizes ---
static constexpr size_t TEXT_BUFFER_SIZE = 16384;
static constexpr int MAX_FILES = 50;
static constexpr int INPUT_QUEUE_SIZE = 50;
static constexpr int MAX_LINES = 1024;

// --- Font IDs (from crosspoint-reader fontIds.h) ---
#define FONT_BODY    (-1014561631)   // NOTOSANS_14_FONT_ID
#define FONT_UI      (-1559651934)   // NOTOSANS_12_FONT_ID
#define FONT_SMALL   (-1246724383)   // UI_10_FONT_ID (ubuntu 10)

// --- HID Keycodes ---
static constexpr uint8_t HID_KEY_A          = 0x04;
static constexpr uint8_t HID_KEY_D          = 0x07;
static constexpr uint8_t HID_KEY_N          = 0x11;
static constexpr uint8_t HID_KEY_Q          = 0x14;
static constexpr uint8_t HID_KEY_R          = 0x15;
static constexpr uint8_t HID_KEY_S          = 0x16;
static constexpr uint8_t HID_KEY_T          = 0x17;
static constexpr uint8_t HID_KEY_Z          = 0x1D;
static constexpr uint8_t HID_KEY_ENTER      = 0x28;
static constexpr uint8_t HID_KEY_ESCAPE     = 0x29;
static constexpr uint8_t HID_KEY_BACKSPACE  = 0x2A;
static constexpr uint8_t HID_KEY_TAB        = 0x2B;
static constexpr uint8_t HID_KEY_SPACE      = 0x2C;
static constexpr uint8_t HID_KEY_DELETE     = 0x4C;
static constexpr uint8_t HID_KEY_RIGHT      = 0x4F;
static constexpr uint8_t HID_KEY_LEFT       = 0x50;
static constexpr uint8_t HID_KEY_DOWN       = 0x51;
static constexpr uint8_t HID_KEY_UP         = 0x52;
static constexpr uint8_t HID_KEY_HOME       = 0x4A;
static constexpr uint8_t HID_KEY_END        = 0x4D;
static constexpr uint8_t HID_KEY_CAPSLOCK   = 0x39;
static constexpr uint8_t HID_KEY_F2         = 0x3B;

// --- HID Modifier Masks ---
static constexpr uint8_t MOD_CTRL_LEFT   = 0x01;
static constexpr uint8_t MOD_SHIFT_LEFT  = 0x02;
static constexpr uint8_t MOD_ALT_LEFT    = 0x04;
static constexpr uint8_t MOD_CTRL_RIGHT  = 0x10;
static constexpr uint8_t MOD_SHIFT_RIGHT = 0x20;
static constexpr uint8_t MOD_ALT_RIGHT   = 0x40;

inline bool isCtrl(uint8_t mod) {
  return (mod & MOD_CTRL_LEFT) || (mod & MOD_CTRL_RIGHT);
}
inline bool isShift(uint8_t mod) {
  return (mod & MOD_SHIFT_LEFT) || (mod & MOD_SHIFT_RIGHT);
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
