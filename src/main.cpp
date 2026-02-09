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

// Enum for sleep reasons
enum class SleepReason {
  POWER_LONGPRESS,
  IDLE_TIMEOUT,
  MENU_ACTION
};

// Forward declarations
void renderSleepScreen();
void enterDeepSleep(SleepReason reason);

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
  
  // Show a quick wake-up screen to indicate the device is starting up
  renderer.clearScreen();
  
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  
  // Title: "MicroSlate"
  const char* title = "MicroSlate";
  int titleWidth = renderer.getTextAdvanceX(FONT_BODY, title);
  int titleX = (sw - titleWidth) / 2;
  int titleY = sh * 0.35; // 35% down the screen (moved up)
  renderer.drawText(FONT_BODY, titleX, titleY, title, true, EpdFontFamily::BOLD);
  
  // Subtitle: "Starting..."
  const char* subtitle = "Starting...";
  int subTitleWidth = renderer.getTextAdvanceX(FONT_UI, subtitle);
  int subTitleX = (sw - subTitleWidth) / 2;
  int subTitleY = sh * 0.48; // 48% down the screen (moved up)
  renderer.drawText(FONT_UI, subTitleX, subTitleY, subtitle, true);
  
  // Perform a full display refresh
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  
  // Small delay to show the startup screen briefly
  delay(500);
  
  // Clear the screen and proceed with normal UI
  screenDirty = true; // Force a redraw of the main UI
}

// Enter deep sleep - matches crosspoint pattern
void enterDeepSleep(SleepReason reason) {
  Serial.println("Entering deep sleep...");
  
  // Render the sleep screen before entering deep sleep
  renderSleepScreen();

  // Save any unsaved work
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }

  display.deepSleep();     // Power down display first
  gpio.startDeepSleep();   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// Read ADC with multi-sample averaging to combat BLE scanning noise.
// BLE radio activity causes ADC jitter that crosses InputManager thresholds,
// preventing its 5ms debounce from ever completing.
static int readADCAvg(int pin, int samples = 8) {
  int sum = 0;
  for (int i = 0; i < samples; i++) {
    sum += analogRead(pin);
  }
  return sum / samples;
}

// Map averaged ADC1 (pin 1) → button index.  Thresholds match InputManager
// but with a "no button" zone above 3800 to reject noise near 4095.
static int adcToButton1(int adc) {
  if (adc > 3800) return -1;  // No button
  if (adc > 2600) return 0;   // Back     (InputManager: >3100)
  if (adc > 1400) return 1;   // Confirm  (InputManager: >2090)
  if (adc > 400)  return 2;   // Left     (InputManager: >750)
  return 3;                    // Right
}

// Map averaged ADC2 (pin 2) → button index.
static int adcToButton2(int adc) {
  if (adc > 3800) return -1;  // No button
  if (adc > 600)  return 0;   // Up       (InputManager: >1120)
  return 1;                    // Down
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

  bool btnBack, btnConfirm, btnLeft, btnRight, btnUp, btnDown;

  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    // BLE scanning causes ADC noise that prevents InputManager's 5ms debounce
    // from completing.  Read ADC directly with averaging to bypass it.
    int adc1 = readADCAvg(1, 8);
    int adc2 = readADCAvg(2, 8);
    int btn1 = adcToButton1(adc1);
    int btn2 = adcToButton2(adc2);

    btnBack    = (btn1 == 0);
    btnConfirm = (btn1 == 1);
    btnLeft    = (btn1 == 2);
    btnRight   = (btn1 == 3);
    btnUp      = (btn2 == 0);
    btnDown    = (btn2 == 1);

    // Diagnostic logging every 2 seconds
    static unsigned long lastRawDebug = 0;
    if (millis() - lastRawDebug > 2000) {
      lastRawDebug = millis();
      Serial.printf("[ADC-BTN] ADC1=%d(%d) ADC2=%d(%d) | Back=%d Confirm=%d Up=%d Down=%d\n",
                    adc1, btn1, adc2, btn2, btnBack, btnConfirm, btnUp, btnDown);
    }
  } else {
    // Normal screens: use InputManager's debounced state (no BLE noise issue)
    btnUp      = gpio.isPressed(HalGPIO::BTN_UP);
    btnDown    = gpio.isPressed(HalGPIO::BTN_DOWN);
    btnLeft    = gpio.isPressed(HalGPIO::BTN_LEFT);
    btnRight   = gpio.isPressed(HalGPIO::BTN_RIGHT);
    btnConfirm = gpio.isPressed(HalGPIO::BTN_CONFIRM);
    btnBack    = gpio.isPressed(HalGPIO::BTN_BACK);
  }

  // Power button state machine for proper long/short press handling
  static bool powerHeld = false;
  static unsigned long powerPressStart = 0;
  static bool sleepTriggered = false;
  
  bool btnPower = gpio.isPressed(HalGPIO::BTN_POWER);
  
  if (btnPower && !powerHeld) {
    // Button just pressed
    powerHeld = true;
    sleepTriggered = false;
    powerPressStart = millis();
  }

  if (btnPower && powerHeld && !sleepTriggered) {
    if (millis() - powerPressStart > 5000) {
      sleepTriggered = true;
      enterDeepSleep(SleepReason::POWER_LONGPRESS);
      return; // Exit early to prevent further processing
    }
  }

  if (!btnPower && powerHeld) {
    // Button released
    unsigned long duration = millis() - powerPressStart;
    powerHeld = false;

    if (!sleepTriggered && duration > 50 && duration < 1000) {
      // Short press - go to main menu (except when already there)
      if (currentState != UIState::MAIN_MENU) {
        if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
          saveCurrentFile();
        }
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
    }
  }

  // Debug: log button state periodically
  static unsigned long lastButtonDebug = 0;
  if (millis() - lastButtonDebug > 2000 && currentState == UIState::BLUETOOTH_SETTINGS) {
    lastButtonDebug = millis();
    Serial.printf("[BTN-DEBUG] State=%d, Up=%d Down=%d Confirm=%d Back=%d\n",
                  (int)currentState, btnUp, btnDown, btnConfirm, btnBack);
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
        Serial.println("[BTN] Physical UP pressed - enqueuing HID_KEY_UP");
        enqueueKeyEvent(HID_KEY_UP, 0, true);
        enqueueKeyEvent(HID_KEY_UP, 0, false);
      }
      if (btnDown && !btnDownLast) {
        Serial.println("[BTN] Physical DOWN pressed - enqueuing HID_KEY_DOWN");
        enqueueKeyEvent(HID_KEY_DOWN, 0, true);
        enqueueKeyEvent(HID_KEY_DOWN, 0, false);
      }
      if (btnConfirm && !btnConfirmLast) {
        Serial.println("[BTN] Physical CONFIRM pressed - enqueuing HID_KEY_ENTER");
        enqueueKeyEvent(HID_KEY_ENTER, 0, true);
        enqueueKeyEvent(HID_KEY_ENTER, 0, false);
      }
      if (btnBack && !btnBackLast) {
        Serial.println("[BTN] Physical BACK pressed - enqueuing HID_KEY_ESCAPE");
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

// Global variable for activity tracking
static unsigned long lastActivityTime = 0;
const unsigned long IDLE_TIMEOUT = 10UL * 60UL * 1000UL; // 10 minutes

void registerActivity() {
  lastActivityTime = millis();
}

// Function to render the sleep screen
void renderSleepScreen() {
  renderer.clearScreen();
  
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  
  // Title: "MicroSlate"
  const char* title = "MicroSlate";
  int titleWidth = renderer.getTextAdvanceX(FONT_BODY, title);
  int titleX = (sw - titleWidth) / 2;
  int titleY = sh * 0.35; // 35% down the screen (moved up)
  renderer.drawText(FONT_BODY, titleX, titleY, title, true, EpdFontFamily::BOLD);
  
  // Subtitle: "Asleep"
  const char* subtitle = "Asleep";
  int subTitleWidth = renderer.getTextAdvanceX(FONT_UI, subtitle);
  int subTitleX = (sw - subTitleWidth) / 2;
  int subTitleY = sh * 0.48; // 48% down the screen (moved up)
  renderer.drawText(FONT_UI, subTitleX, subTitleY, subtitle, true);
  
  // Footer: "Hold Power to wake"
  const char* footer = "Hold Power to wake";
  int footerWidth = renderer.getTextAdvanceX(FONT_SMALL, footer);
  int footerX = (sw - footerWidth) / 2;
  int footerY = sh * 0.75; // 75% down the screen (moved up from bottom)
  renderer.drawText(FONT_SMALL, footerX, footerY, footer);
  
  // Perform a full display refresh to ensure the sleep screen is visible
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  
  // Small delay to ensure the display update is complete
  delay(500);
}

void loop() {
  // --- GPIO first: always poll buttons before anything else ---
  gpio.update();

  // Control auto-reconnect based on UI state
  static UIState lastState = UIState::MAIN_MENU;
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    autoReconnectEnabled = false;
    // On first entry to BT settings, cancel pending connections and auto-start scan
    if (lastState != UIState::BLUETOOTH_SETTINGS) {
      cancelPendingConnection();
      if (!isDeviceScanning()) {
        startDeviceScan();
      }
    }
  } else {
    autoReconnectEnabled = true;
    // Stop scanning when leaving BT settings
    if (lastState == UIState::BLUETOOTH_SETTINGS && isDeviceScanning()) {
      stopDeviceScan();
    }
  }
  lastState = currentState;

  // Process BLE for connection handling in all states
  bleLoop();

  // Periodically refresh BT screen during scanning (every 3s) instead of on
  // every device-count change.  Frequent 430ms display refreshes starve the
  // button debounce state machine, making physical buttons unresponsive.
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    static unsigned long lastBtRefresh = 0;
    unsigned long now2 = millis();
    if (now2 - lastBtRefresh > 3000) {
      lastBtRefresh = now2;
      screenDirty = true;
    }
  }

  // CRITICAL: Process buttons BEFORE checking wasAnyPressed() to avoid consuming button states
  processPhysicalButtons();
  int inputEventsProcessed = processAllInput(); // Assuming this returns number of events processed

  // Register activity AFTER button processing (don't consume button states prematurely)
  if (gpio.wasAnyPressed() || inputEventsProcessed > 0) {
    registerActivity();
  }

  // Rate limit screen updates to prevent excessive redraws that cause GFX spam
  static unsigned long lastScreenUpdate = 0;
  unsigned long now = millis();

  // Check for critical updates that need immediate display (like passkey)
  bool criticalUpdate = (currentState == UIState::BLUETOOTH_SETTINGS && getCurrentPasskey() > 0);

  if (criticalUpdate || now - lastScreenUpdate > 250) { // Max 4 FPS refresh rate, critical updates bypass rate limit
    if (screenDirty) {
      updateScreen();
      lastScreenUpdate = now;
    }
  }

  // Check for idle timeout
  if (millis() - lastActivityTime > IDLE_TIMEOUT) {
    enterDeepSleep(SleepReason::IDLE_TIMEOUT);
  }

  delay(10);
}
