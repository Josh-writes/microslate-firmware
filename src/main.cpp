#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <GfxRenderer.h>
#include <esp_pm.h>
#include <Preferences.h>

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

// --- Persistent settings (NVS) ---
static Preferences uiPrefs;

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

// UI mode flags
bool darkMode = false;
RefreshSpeed refreshSpeed = RefreshSpeed::BALANCED;
bool cleanMode = false;
bool deleteConfirmPending = false;

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

  // Auto-compute chars per line from font metrics so text always fills the screen
  {
    int sw = renderer.getScreenWidth();
    int textAreaWidth = sw - 20;  // 10px margins each side
    int avgCharW = renderer.getTextAdvanceX(FONT_BODY, "abcdefghijklmnopqrstuvwxyz") / 26;
    if (avgCharW > 0) charsPerLine = textAreaWidth / avgCharW;
  }
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
  DBG_INIT();
  DBG_PRINTLN("MicroSlate starting...");

  // Reduce CPU clock — 80MHz is plenty for this workload, saves ~30% active power
  setCpuFrequencyMhz(80);

  gpio.begin();
  display.begin();

  renderer.setOrientation(GfxRenderer::Portrait);
  rendererSetup(renderer);

  editorInit();
  inputSetup();
  fileManagerSetup();
  bleSetup();

  // Enable automatic light sleep between loop iterations.
  // FreeRTOS tickless idle will put the CPU to sleep whenever delay() yields
  // the scheduler and no other tasks are runnable.  BLE stays alive, wake
  // latency is <1ms — invisible to the user.
  {
    esp_pm_config_esp32c3_t pm_config = {
      .max_freq_mhz      = 80,   // Never exceed 80MHz
      .min_freq_mhz      = 40,   // Drop to 40MHz when idle
      .light_sleep_enable = true
    };
    esp_err_t err = esp_pm_configure(&pm_config);
    if (err == ESP_OK) {
      DBG_PRINTLN("[PM] Light sleep enabled (80/40MHz)");
    } else {
      DBG_PRINTF("[PM] Light sleep config failed: %d — running at 80MHz\n", err);
    }
  }

  // Load persisted UI settings from NVS
  uiPrefs.begin("ui_prefs", false);
  currentOrientation = static_cast<Orientation>(uiPrefs.getUChar("orient", 0));
  darkMode = uiPrefs.getBool("darkMode", false);
  refreshSpeed = static_cast<RefreshSpeed>(uiPrefs.getUChar("refreshSpd", 1)); // default BALANCED

  // Initialize auto-reconnect to enabled by default
  autoReconnectEnabled = true;

  DBG_PRINTLN("MicroSlate ready.");
  
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
  DBG_PRINTLN("Entering deep sleep...");
  
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

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
static void processPhysicalButtons() {
  static bool btnUpLast = false;
  static bool btnDownLast = false;
  static bool btnLeftLast = false;
  static bool btnRightLast = false;
  static bool btnConfirmLast = false;
  static bool btnBackLast = false;

  // Use isPressed() — persistent debounced state.  With one-shot scanning
  // (radio quiet during navigation), InputManager debounce works reliably.
  bool btnUp      = gpio.isPressed(HalGPIO::BTN_UP);
  bool btnDown    = gpio.isPressed(HalGPIO::BTN_DOWN);
  bool btnLeft    = gpio.isPressed(HalGPIO::BTN_LEFT);
  bool btnRight   = gpio.isPressed(HalGPIO::BTN_RIGHT);
  bool btnConfirm = gpio.isPressed(HalGPIO::BTN_CONFIRM);
  bool btnBack    = gpio.isPressed(HalGPIO::BTN_BACK);

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

  // Back button long-press for restart
  static bool backHeld = false;
  static unsigned long backPressStart = 0;
  static bool restartTriggered = false;

  if (btnBack && !backHeld) {
    backHeld = true;
    restartTriggered = false;
    backPressStart = millis();
  }

  if (btnBack && backHeld && !restartTriggered) {
    if (millis() - backPressStart > 5000) {
      restartTriggered = true;
      DBG_PRINTLN("BACK held for 5s — restarting device...");
      delay(100);
      ESP.restart();
    }
  }

  if (!btnBack && backHeld) {
    backHeld = false;
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

    case UIState::TEXT_EDITOR: {
      // Key repeat state for held navigation/backspace keys
      static uint8_t repeatKey = 0;
      static unsigned long repeatStart = 0;
      static unsigned long lastRepeat = 0;
      const unsigned long REPEAT_DELAY = 400;
      const unsigned long REPEAT_RATE  = 80;

      auto fireKey = [](uint8_t k) {
        enqueueKeyEvent(k, 0, true);
        enqueueKeyEvent(k, 0, false);
      };

      // Map currently held button to HID key (0 = none)
      uint8_t heldKey = 0;
      if      (btnUp)    heldKey = HID_KEY_UP;
      else if (btnDown)  heldKey = HID_KEY_DOWN;
      else if (btnLeft)  heldKey = HID_KEY_LEFT;
      else if (btnRight) heldKey = HID_KEY_RIGHT;

      if (heldKey != repeatKey) {
        // Key changed — fire immediately on press
        if (heldKey != 0) fireKey(heldKey);
        repeatKey   = heldKey;
        repeatStart = millis();
        lastRepeat  = millis();
      } else if (heldKey != 0) {
        unsigned long now = millis();
        if (now - repeatStart > REPEAT_DELAY && now - lastRepeat > REPEAT_RATE) {
          fireKey(heldKey);
          lastRepeat = now;
        }
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
    }

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
    // On first entry to BT settings, do a one-shot scan
    if (lastState != UIState::BLUETOOTH_SETTINGS) {
      cancelPendingConnection();
      startDeviceScan();  // One-shot 5s scan, radio goes quiet after
    }
  } else {
    autoReconnectEnabled = true;
    if (lastState == UIState::BLUETOOTH_SETTINGS && isDeviceScanning()) {
      stopDeviceScan();
    }
  }
  lastState = currentState;

  // Process BLE (connection handling, scan completion detection)
  bleLoop();

  // CRITICAL: Process buttons BEFORE checking wasAnyPressed() to avoid consuming button states
  processPhysicalButtons();
  int inputEventsProcessed = processAllInput(); // Assuming this returns number of events processed

  // Register activity AFTER button processing (don't consume button states prematurely)
  static unsigned long lastInputTime = 0;
  bool hadActivity = gpio.wasAnyPressed() || inputEventsProcessed > 0;
  if (hadActivity) {
    registerActivity();
    lastInputTime = millis();
  }

  // Cooldown-based screen refresh: the e-ink refresh (~430ms) IS the rate limiter.
  // After each refresh completes, wait a configurable cooldown, then show all
  // accumulated keystrokes at once. No artificial debounce — characters appear as
  // fast as the display allows. Longer cooldown = fewer refreshes = more battery savings.
  static unsigned long lastRefreshDoneMs = 0;
  unsigned long now = millis();

  bool criticalUpdate = (currentState == UIState::BLUETOOTH_SETTINGS && getCurrentPasskey() > 0);

  if (screenDirty) {
    bool cooldownMet = (now - lastRefreshDoneMs >= refreshCooldownMs(refreshSpeed));
    if (criticalUpdate || cooldownMet) {
      updateScreen();
      lastRefreshDoneMs = millis();  // Use millis() AFTER refresh (which blocks ~430ms)
    }
  }

  // Persist UI settings to NVS when they change (NVS write only on change, not every loop)
  static Orientation lastSavedOrientation = currentOrientation;
  static bool lastSavedDarkMode = darkMode;
  static RefreshSpeed lastSavedRefreshSpeed = refreshSpeed;
  if (currentOrientation != lastSavedOrientation || darkMode != lastSavedDarkMode
      || refreshSpeed != lastSavedRefreshSpeed) {
    uiPrefs.putUChar("orient", static_cast<uint8_t>(currentOrientation));
    uiPrefs.putBool("darkMode", darkMode);
    uiPrefs.putUChar("refreshSpd", static_cast<uint8_t>(refreshSpeed));
    lastSavedOrientation = currentOrientation;
    lastSavedDarkMode = darkMode;
    lastSavedRefreshSpeed = refreshSpeed;
  }

  // Check for idle timeout
  if (millis() - lastActivityTime > IDLE_TIMEOUT) {
    enterDeepSleep(SleepReason::IDLE_TIMEOUT);
  }

  // Adaptive delay: shorter when active (responsive), longer when idle (saves power).
  // FreeRTOS tickless idle triggers light sleep during delay(), so longer = more sleep.
  // 20ms active aligns with BLE connection interval (30-50ms) — catches every keystroke
  // without polling faster than BLE events arrive. 100ms idle for deeper sleep windows.
  delay((hadActivity || screenDirty) ? 20 : 100);
}
