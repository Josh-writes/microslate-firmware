#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>

#include "config.h"
#include "ble_keyboard.h"
#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ui_renderer.h"

// External variables
extern bool autoReconnectEnabled;

// --- Hardware objects ---
HalDisplay display;
GfxRenderer renderer(display);
HalGPIO gpio;

// --- Shared UI state ---
UIState currentState = UIState::MAIN_MENU;
int mainMenuSelection = 0;
int selectedFileIndex = 0;
int settingsSelection = 0;
int bluetoothDeviceSelection = 0;  // For Bluetooth device selection
Orientation currentOrientation = Orientation::PORTRAIT;
int charsPerLine = 40;
bool screenDirty = true;

// Rename buffer
char renameBuffer[MAX_FILENAME_LEN] = "";
int renameBufferLen = 0;

// --- Screen update ---
static void updateScreen() {
  if (!screenDirty) return;
  screenDirty = false;

  // Apply orientation
  static Orientation lastOrientation = Orientation::PORTRAIT;
  if (currentOrientation != lastOrientation) {
    GfxRenderer::Orientation gfxOrient;
    switch (currentOrientation) {
      case Orientation::PORTRAIT:      gfxOrient = GfxRenderer::Portrait; break;
      case Orientation::LANDSCAPE_CW:  gfxOrient = GfxRenderer::LandscapeClockwise; break;
      case Orientation::PORTRAIT_INV:  gfxOrient = GfxRenderer::PortraitInverted; break;
      case Orientation::LANDSCAPE_CCW: gfxOrient = GfxRenderer::LandscapeCounterClockwise; break;
    }
    renderer.setOrientation(gfxOrient);
    lastOrientation = currentOrientation;
  }

  // Update editor's chars per line
  editorSetCharsPerLine(charsPerLine);

  switch (currentState) {
    case UIState::MAIN_MENU:         drawMainMenu(renderer, gpio); break;
    case UIState::FILE_BROWSER:      drawFileBrowser(renderer, gpio); break;
    case UIState::TEXT_EDITOR:       drawTextEditor(renderer, gpio); break;
    case UIState::RENAME_FILE:       drawRenameScreen(renderer); break;
    case UIState::SETTINGS:          drawSettingsMenu(renderer, gpio); break;
    case UIState::BLUETOOTH_SETTINGS: drawBluetoothSettings(renderer, gpio); break;
    default: break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("MicroSlate starting...");

  gpio.begin();
  display.begin();

  renderer.setOrientation(GfxRenderer::Portrait);
  rendererSetup(renderer);

  editorInit();
  inputSetup();
  fileManagerSetup();
  bleSetup();

  // Initialize auto-reconnect to enabled by default
  autoReconnectEnabled = true;

  Serial.println("MicroSlate ready.");
}

// Enter deep sleep - matches crosspoint pattern
static void enterDeepSleep() {
  Serial.println("Entering deep sleep...");

  // Save any unsaved work
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }

  display.deepSleep();     // Power down display first
  gpio.startDeepSleep();   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
static void processPhysicalButtons() {
  static bool btnUpLast = false;
  static bool btnDownLast = false;
  static bool btnLeftLast = false;
  static bool btnRightLast = false;
  static bool btnConfirmLast = false;
  static bool btnBackLast = false;

  // Check for button presses (edge detection)
  bool btnUp = gpio.wasPressed(HalGPIO::BTN_UP);
  bool btnDown = gpio.wasPressed(HalGPIO::BTN_DOWN);
  bool btnLeft = gpio.wasPressed(HalGPIO::BTN_LEFT);
  bool btnRight = gpio.wasPressed(HalGPIO::BTN_RIGHT);
  bool btnConfirm = gpio.wasPressed(HalGPIO::BTN_CONFIRM);
  bool btnBack = gpio.wasPressed(HalGPIO::BTN_BACK);

  // Power button handling: distinguish between short tap and long press
  // Short tap (released quickly): go to main menu
  // Long press (held >5s): handled in main loop for sleep
  if (gpio.wasReleased(HalGPIO::BTN_POWER)) {
    unsigned long heldTime = gpio.getHeldTime();
    // Only trigger short press action if it was a quick tap (not a long press)
    if (heldTime < 1000) {
      if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
        saveCurrentFile();
      }
      currentState = UIState::MAIN_MENU;
      screenDirty = true;
    }
  }

  // Map physical buttons to HID key codes based on current UI state
  switch (currentState) {
    case UIState::MAIN_MENU:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      break;

    case UIState::FILE_BROWSER:
      if (btnUp && !btnUpLast && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast && getFileCount() > 0) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::TEXT_EDITOR:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnLeft && !btnLeftLast) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        currentState = UIState::FILE_BROWSER;
        screenDirty = true;
      }
      break;

    case UIState::RENAME_FILE:
    case UIState::NEW_FILE:
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::BLUETOOTH_SETTINGS:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    case UIState::SETTINGS:
      if (btnUp && !btnUpLast) {
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnLeft && !btnLeftLast) {
        enqueueKeyEvent(HID_KEY_LEFT, 0, true);
        enqueueKeyEvent(HID_KEY_LEFT, 0, false);
      }
      if (btnRight && !btnRightLast) {
        enqueueKeyEvent(HID_KEY_RIGHT, 0, true);
        enqueueKeyEvent(HID_KEY_RIGHT, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, true);
        enqueueKeyEvent(HID_KEY_ESCAPE, 0, false);
      }
      break;

    default:
      break;
  }

  // Update last state
  btnUpLast = btnUp;
  btnDownLast = btnDown;
  btnLeftLast = btnLeft;
  btnRightLast = btnRight;
  btnConfirmLast = btnConfirm;
  btnBackLast = btnBack;
}

void loop() {
  // --- GPIO first: always poll buttons before anything else ---
  gpio.update();

  // --- Deep sleep check: power button held for 5 seconds ---
  // This runs BEFORE bleLoop() which can block, matching crosspoint's pattern
  if (gpio.isPressed(HalGPIO::BTN_POWER) && gpio.getHeldTime() > 5000) {
    enterDeepSleep();
    return;  // Never reached
  }

  // Control auto-reconnect based on UI state
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    autoReconnectEnabled = false;
  } else {
    autoReconnectEnabled = true;
  }

  // Process BLE for connection handling in all states
  bleLoop();

  // Periodically refresh Bluetooth settings screen during scanning so names appear
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    static unsigned long lastBtRefresh = 0;
    unsigned long now = millis();
    if (now - lastBtRefresh >= 2000) {
      lastBtRefresh = now;
      screenDirty = true;
    }
  }

  processPhysicalButtons();
  processAllInput();
  updateScreen();
  delay(10);
}
