#include <Arduino.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <SDCardManager.h>
#include "touch_input.h"
#include "touch_targets.h"
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <NimBLEDevice.h>
#include <esp_system.h>
#include <soc/soc_caps.h>
#include <PowerManager.h>
#include <esp_app_format.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <Preferences.h>
#include "sd_backup.h"

#include "config.h"
#include "ble_keyboard.h"
#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ui_renderer.h"
#include "spell/spell_service.h"
#include "safety_probe.h"
#include "wifi_sync.h"
#include "sync/cloud_service.h"
#include "sleep/sleep_screen.h"
#include "transfer/transfer_service.h"
#include "update/fw_update.h"
#include "editor_prof.h"
#include "keyboard_layout.h"
#include "frontlight.h"
#include "screen_refresh.h"
#include "ui_prefs.h"
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
#include <XteinkDetect.h>
#endif

// Why the device goes to sleep (shown in the debug log)
enum class SleepReason {
  POWER_LONGPRESS,
  IDLE_TIMEOUT
};

// Forward declarations
void renderSleepScreen();
void enterDeepSleep(SleepReason reason);

// External variables
extern bool autoReconnectEnabled;

// --- Hardware objects ---
// `display` and `gpio` are defined in lib/hal (FreeInk-backed HAL).
GfxRenderer renderer(display);


// --- Shared UI state ---
UIState currentState = UIState::MAIN_MENU;
int mainMenuSelection = 0;
int selectedFileIndex = 0;
int settingsSelection = 1;        // 0 = the tab strip, 1.. the open tab's rows
int settingsTab = 0;
int sleepPicSel = 0;              // sleep screen list: 0 Stock, 1 Random, 2.. the pictures, last "+ Add a picture"
bool sleepPicPreview = false;     // showing the highlighted picture full screen
bool sleepPicDeletePending = false;              // Editor, Wi-Fi, Bluetooth, Keyboard, Display (config.h)
bool settingsForgetPending = false;   // Wi-Fi tab: Forget was pressed once, the next press removes the network
int bluetoothDeviceSelection = 0;
int uiBarFocus = -1;   // Bluetooth screen: -1 = device list has focus, 0..3 = bottom-bar button
int pairedKeyboardSelection = 0;
bool pairedForgetPending = false;   // true while a "Forget keyboard?" confirmation is armed
Orientation currentOrientation = Orientation::PORTRAIT;

int charsPerLine = 40;
bool screenDirty = true;

// The screen shows everything typed so far: no redraw waiting and no refresh running (key
// repeat paces itself on this).
bool displayCaughtUp();
bool btShowAllDevices = false;   // Bluetooth scan: keyboards only by default, else named devices (a crowded office
                                 // listed dozens of nameless addresses). Some keyboards don't advertise HID before
                                 // connecting, so the list's last row ("Show other devices") reaches everything.

// Rename buffer
char renameBuffer[MAX_FILENAME_LEN] = "";
int renameBufferLen = 0;
char renameExt[5] = ".txt";   // the title screen's format: ".txt" or ".md"
bool newNotesMd = false;      // Settings > Editor > New notes: .md (default .txt)

// In-document find (overlay within the text editor)
bool editorFindMode = false;
char findBuffer[MAX_FIND_LEN] = "";
int findBufferLen = 0;

// UI mode flags
bool darkMode = false;
bool cleanMode = false;
bool deleteConfirmPending = false;
WritingMode writingMode = WritingMode::NORMAL;
FontSize fontSize = FontSize::LARGE;
bool showWordCount = true;

// --- Screen update ---
// Turns currentOrientation (fixed on some boards) into the renderer's orientation.
static void applyOrientation() {
  if (kOrientationLocked) currentOrientation = kLockedOrientation;
  GfxRenderer::Orientation o = GfxRenderer::Portrait;
  switch (currentOrientation) {
    case Orientation::PORTRAIT:      o = GfxRenderer::Portrait; break;
    case Orientation::LANDSCAPE_CW:  o = GfxRenderer::LandscapeClockwise; break;
    case Orientation::PORTRAIT_INV:  o = GfxRenderer::PortraitInverted; break;
    case Orientation::LANDSCAPE_CCW: o = GfxRenderer::LandscapeCounterClockwise; break;
  }
  renderer.setOrientation(o);
}

static void updateScreen() {
  if (!screenDirty) return;
  screenDirty = false;
  touchTargetsClear();  // list screens re-register their rows as they draw

  // Bottom-bar focus belongs to one screen; never carry it to the next.
  static UIState lastDrawnState = UIState::MAIN_MENU;
  if (currentState != lastDrawnState) {
    uiBarFocus = -1;
    lastDrawnState = currentState;
  }

  // Apply orientation
  if (kOrientationLocked) currentOrientation = kLockedOrientation;
  static Orientation lastOrientation = Orientation::PORTRAIT;
  if (currentOrientation != lastOrientation) {
    applyOrientation();
    lastOrientation = currentOrientation;
  }

  // Auto-compute chars per line from font metrics so text always fills the screen
  {
    int sw = renderer.getScreenWidth();
    int textAreaWidth = sw - 20;  // 10px margins each side
    // Measure ordinary English (spaces and narrow letters included), not the alphabet,
    // which is wider than real prose and made lines wrap early.
    static const char* kSample = "the of and to in is that it was for on are as with his they at be this from i have or by one had not but what all were when we there can an your which their said if do will each about how up out them then she many some so these would other into has more her two like him see time could no make than first been its who now people my made over did down only way find use may water long little very after words called just where most know get through back much before go good new write our used me man too any day same right look think also around another came come work three word must because does part even place well such here take why help put different away again off went old number great tell men say small every found still between name should home big give air line set own under read last never us left end along while might next sound below saw something thought both few those always looked show large often together asked house don't world going want school important until form food keep children feet land side without boy once animals life enough took sometimes four head above kind began almost live page got earth need far hand high year mother light country father let night picture being study second soon story since white ever paper hard near sentence better best across during today however sure knew it's try told young sun thing whole hear example heard several change answer room sea against top turned learn point city play toward five using himself usually";
    int avgCharW = renderer.getTextAdvanceX(editorFontId(fontSize), kSample) / (int)strlen(kSample);
    if (avgCharW > 0) charsPerLine = textAreaWidth / avgCharW;
  }
  editorSetCharsPerLine(charsPerLine);
  {
    // Wrap by real glyph widths so a word that fits is never cut off at the edge
    static uint16_t advW[128];
    static int advFont = -1;
    int fid = editorFontId(fontSize);
    if (fid != advFont) {
      for (int c = 0; c < 128; c++) {
        char t[2] = {(char)((c < 32 || c > 126) ? ' ' : c), 0};
        advW[c] = (uint16_t)renderer.getTextAdvanceX(fid, t);
      }
      advW['\t'] = (uint16_t)editorTabWidth(renderer, fid);
      advFont = fid;
    }
    int fb = renderer.getTextAdvanceX(fid, "n");
    editorSetPixelWrap(renderer.getScreenWidth() - 20 - 6, advW, fb);
  }

  switch (currentState) {
    case UIState::MAIN_MENU:         drawMainMenu(renderer, gpio); break;
    case UIState::FILE_BROWSER:
      if (cloud::browseHoldActive()) drawCloudNotice(renderer, gpio, cloud::holdNotice());
      else if (loadRefusedBytes()) {
        // A note over the editor limit was not opened (file_manager.cpp, loadFile).
        const unsigned limKb = (unsigned)(TEXT_BUFFER_SIZE / 1024);
        const unsigned kb = (unsigned)((loadRefusedBytes() + 1023) / 1024);
        char sub[200];
        if (kb > limKb)
          snprintf(sub, sizeof(sub), "It is %u KB; this device holds up to %u KB.\n", kb, limKb);
        else
          snprintf(sub, sizeof(sub), "It is just over %u KB, the most this device holds.\n", limKb);
        strncat(sub, "Split it into smaller notes on a computer.\nThe file has not been changed.\n\n"
                     "Press any key to go back.", sizeof(sub) - strlen(sub) - 1);
        drawCloudNotice(renderer, gpio, "This note is too large to open", sub);
      } else drawFileBrowser(renderer, gpio);
      break;
    case UIState::TEXT_EDITOR:       drawTextEditor(renderer, gpio); break;
    case UIState::RENAME_FILE:       drawRenameScreen(renderer, gpio); break;
    case UIState::SETTINGS:          drawSettingsMenu(renderer, gpio); break;
    case UIState::BLUETOOTH_SETTINGS: drawBluetoothSettings(renderer, gpio); break;
    case UIState::PAIRED_KEYBOARDS:   drawPairedKeyboardsMenu(renderer, gpio); break;
    case UIState::LAYOUT_EDITOR:      drawLayoutEditor(renderer, gpio); break;
    case UIState::WIFI_SYNC:          drawSyncScreen(renderer, gpio); break;
    case UIState::TRANSFER:           drawTransferScreen(renderer, gpio); break;
    case UIState::FW_UPDATE:          drawFwUpdateScreen(renderer, gpio); break;
    case UIState::SLEEP_PICTURES:     drawSleepPictures(renderer, gpio); break;
    case UIState::MY_WORDS:           drawMyWords(renderer, gpio); break;
    case UIState::SD_MISSING:         drawCloudNotice(renderer, gpio, "SD card removed", "Insert it, then press any button."); break;
    default: break;
  }
  touchScrollDiscard();  // drop scroll requests the screen had no list to take
}

void setup() {
  BoardConfig::holdPowerRails();  // Sticky: PWR_HOLD/PWR_LOCK must be driven before anything else
  DBG_INIT();
  DBG_PRINTLN("MicroSlate starting...");
#ifndef RELEASE_BUILD
  // Every failed allocation, with what was left: out-of-memory reboots during sync (2026-10-30) gave no culprit.
  // esp_rom_printf allocates nothing and is safe wherever the allocation failed.
  heap_caps_register_failed_alloc_callback([](size_t size, uint32_t caps, const char* fn) {
    esp_rom_printf("[HEAP] %s: %u bytes (caps 0x%x) failed, internal %u free / %u largest\n", fn ? fn : "?",
                   (unsigned)size, (unsigned)caps, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                   (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  });
  {
    const auto aborted = freeink::PowerManager::takeAbortedSleepInfo();
    DBG_PRINTF("Boot: reset=%d wake=%d ext1=0x%llx sleepAborted=%d (cause=%d pin=%d)\n",
               (int)esp_reset_reason(), (int)esp_sleep_get_wakeup_cause(),
#if SOC_PM_SUPPORT_EXT1_WAKEUP
               (unsigned long long)esp_sleep_get_ext1_wakeup_status(),
#else
               0ULL,
#endif
               (int)aborted.aborted, aborted.wakeupCause, aborted.wakePinLevel);
  }
#endif

#if FREEINK_MCU_C3
  setCpuFrequencyMhz(80);  // C3 only: the S3 keeps its default clock; esp_pm scales it down later
#endif

  gpio.begin();
  powerManager.begin();
#if !FREEINK_MCU_C3
  // S3 boards (Sticky): the SD card and the e-ink panel share one SPI bus and the
  // card must be mounted BEFORE the display starts, as CrossPoint does. Starting
  // the display first leaves the card undetectable and the panel un-driven. The
  // C3 X4 keeps its original order (display, then SD in fileManagerSetup()).
  SdMan.begin();
#endif
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  // X4 Pro batches carry an SSD1677, UC8179 or UC8279: probe the bus before the display starts (the C3 X4 does this in
  // HalGPIO::begin(), as CrossPoint does). The case covers a band at the panel edges (Draftling measured about 12 px
  // on one side and 8 px on the other on its unit); keep the drawing clear of it on both sides of each axis.
  freeink::applyXteinkDisplayController();
  GfxRenderer::setPanelInset(12, 8);
#endif
  display.begin();
  light::begin();
  refresh::begin();

  renderer.setFadingFix(true);  // Power down display analog circuits after each refresh — reduces idle drain
  rendererSetup(renderer);

  // Load the UI settings early so the startup screen uses the saved orientation
  uiprefs::load();
  applyOrientation();

  editorInit();
  inputSetup();
  fileManagerSetup();
  cloud::init();   // OneDrive sync service (idle until the user connects an account)
  sleepscr::init();   // sleep screen choice (Stock or My pictures)

  // NVS wiped by a firmware flash: take the settings from the SD copy
  if (uiprefs::restoreFromSd()) {
    applyOrientation();
    DBG_PRINTLN("UI prefs restored from SD backup");
  }
  keyboardLayoutsInit(uiprefs::layoutFile(), uiprefs::legacyLayout());   // reads /microslate/layouts on the SD card

  bleSetup();

  // Enable automatic light sleep between loop iterations.
  // CONFIG_PM_ENABLE and CONFIG_FREERTOS_USE_TICKLESS_IDLE come from
  // custom_sdkconfig in platformio.ini. BLE modem sleep keeps the radio alive
  // across sleep/wake cycles.
#ifdef CONFIG_SPIRAM
  constexpr int kMaxCpuMhz = 240;   // S3 (PSRAM): the editor runs on it; the 80 MHz cap was the X4's battery tuning
#else
  constexpr int kMaxCpuMhz = 80;
#endif
  esp_pm_config_t pm_config = {
    .max_freq_mhz = kMaxCpuMhz,
    .min_freq_mhz = 10,
    .light_sleep_enable = true
  };
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  DBG_PRINTF("PM configure: %s\n", esp_err_to_name(pm_err));

  // --- Hardware task watchdog -----------------------------------------------
  // The IDF inits the Task WDT at boot (CONFIG_ESP_TASK_WDT=y). custom_sdkconfig sets it to 30s
  // with panic-reboot (CONFIG_ESP_TASK_WDT_TIMEOUT_S=30, CONFIG_ESP_TASK_WDT_PANIC=y);
  // here we subscribe the Arduino loop task. A loop-task hang (BLE stall, display BUSY
  // pin stuck, SD wedge) then becomes a clean reboot instead of a freeze that needs the
  // manual 5s BACK hold. 30s clears the longest blocking loop op — the e-ink BUSY waits
  // cap at 10s (EInkDisplay.cpp); the BLE connect runs on its own, unwatched task.
  esp_task_wdt_add(NULL);
  esp_task_wdt_reset();
  DBG_PRINTLN("Task watchdog: loop task subscribed (30s, panic)");

  // Initialize auto-reconnect to enabled by default
  autoReconnectEnabled = true;

  // No keyboard paired yet (first start, or all forgotten): start on the Bluetooth screen, which scans on entry.
  if (getPairedKeyboardCount() == 0) currentState = UIState::BLUETOOTH_SETTINGS;

  DBG_PRINTLN("MicroSlate ready.");

  // The display needs one FULL_REFRESH after power-on to initialize its analog
  // circuits before FAST_REFRESH will work.
  renderer.clearScreen();
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);

  screenDirty = true;
}

// Enter deep sleep - matches crosspoint pattern
void enterDeepSleep(SleepReason reason) {
  DBG_PRINTF("Entering deep sleep (%s)...\n",
             reason == SleepReason::POWER_LONGPRESS ? "power button" : "idle timeout");
  light::off();

  // Get the last edit to OneDrive first (bounded wait, about 10 s). Save the open note so there is something to push.
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) saveCurrentFile();
  transfer::stop();   // hotspot and page off; the push below may then use a saved network
  cloud::flushBeforeSleep();

  // Stop watching the loop task — the sleep prep below (full refresh + waiting for
  // the power button to be released) blocks intentionally and must not trip the WDT.
  esp_task_wdt_delete(NULL);

  // Render the sleep screen before entering deep sleep
  renderSleepScreen();

  // Save any unsaved work
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }

  // Stop the BLE controller: left running it arms an RTC timer wakeup, so the
  // device woke itself a few seconds after "sleeping" (wake cause = TIMER).
  if (NimBLEDevice::isInitialized()) NimBLEDevice::deinit(true);

  display.deepSleep();     // Power down display first
  powerManager.startDeepSleep(gpio);   // Waits for power button release, then sleeps
  // Will not return - device is asleep
}

// The open note has text that cannot be saved because the SD card is gone. Saves it first when it can (a failed save
// is how the editor finds out the card was pulled), so false also means "saved, safe to leave".
static bool unsavedWithoutCard() {
  if (currentState != UIState::TEXT_EDITOR || !editorHasUnsavedChanges()) return false;
  if (!sdLost()) saveCurrentFile();
  return sdLost() && editorHasUnsavedChanges();
}

// Translate physical button presses to HID key codes
// NOTE: gpio.update() is called in loop() before this function
// One press and release of a key, as if typed: how the physical buttons reach the input handler.
static void tapKey(uint8_t k) {
  enqueueKeyEvent(k, 0, true);
  enqueueKeyEvent(k, 0, false);
}

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
  // Boards with one shared confirm/power key (E1001, Sticky) report a click as a one-tick press event,
  // not a held state, so accept either.
  bool btnConfirm = gpio.isPressed(HalGPIO::BTN_CONFIRM) || gpio.wasPressed(HalGPIO::BTN_CONFIRM);
  // The X4 Pro has no Back button: its touch Home key below the screen is Back.
  bool btnBack    = gpio.isPressed(HalGPIO::BTN_BACK) || (gpio.hasHomeKey() && gpio.wasHomeKeyPressed());

  // Follow the screen orientation: with the picture upside down (PortraitInverted)
  // or in LandscapeClockwise the physical Up/Down side buttons and the Left/Right
  // front buttons sit the opposite way round on screen, so swap them. Verified on
  // the X4: CrossPoint's rule (CCW instead of CW) read backwards for MicroSlate.
  {
    const auto o = renderer.getOrientation();
    if (o == GfxRenderer::PortraitInverted || o == GfxRenderer::LandscapeClockwise) {
      std::swap(btnUp, btnDown);
      std::swap(btnLeft, btnRight);
    }
  }

  // Button-only devices (E1001) have no Back key: Up and Down together send Escape. Whichever
  // button lands first scrolls once; the chord then owns both until they are released.
  if (kClassicUi) {
    static bool chordActive = false;
    if (btnUp && btnDown) {
      if (!chordActive) {
        chordActive = true;
        tapKey(HID_KEY_ESCAPE);
      }
      btnUp = btnDown = false;
    } else if (chordActive) {
      if (!btnUp && !btnDown) chordActive = false;
      else btnUp = btnDown = false;
    }
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
    if (millis() - powerPressStart > 3000) {
      sleepTriggered = true;
      // Card gone with unsaved text: the first hold warns, a second one (while the warning shows) turns off anyway.
      if (editorSdWarn != 2 && unsavedWithoutCard()) {
        editorSdWarn = 2;
        screenDirty = true;
        return;
      }
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
      if (unsavedWithoutCard()) {   // the text could not be saved: stay in the editor and say why
        editorSdWarn = 1;
        screenDirty = true;
      } else if (currentState != UIState::MAIN_MENU) {
        currentState = UIState::MAIN_MENU;   // unsavedWithoutCard() saved the open note
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
      // Card gone with unsaved text: the first hold warns, a second one (while the warning shows) restarts anyway.
      if (editorSdWarn != 3 && unsavedWithoutCard()) {
        editorSdWarn = 3;
        screenDirty = true;
        return;
      }
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
    // List screens (all with the shared bottom bar): Up/Down scroll; the front Left/Right
    // buttons scroll too (Right = up, Left = down) until the bar has focus, then they
    // step along it. Confirm presses the highlighted row / focused bar button.
    case UIState::MAIN_MENU:
    case UIState::FILE_BROWSER:
    case UIState::SETTINGS:
    case UIState::BLUETOOTH_SETTINGS:
    case UIState::PAIRED_KEYBOARDS:
    case UIState::SLEEP_PICTURES:
    case UIState::MY_WORDS:
    case UIState::LAYOUT_EDITOR: {
      if (btnUp && !btnUpLast) tapKey(HID_KEY_UP);
      if (btnDown && !btnDownLast) tapKey(HID_KEY_DOWN);
      if (btnRight && !btnRightLast) tapKey(uiBarFocus >= 0 ? HID_KEY_RIGHT : HID_KEY_UP);
      if (btnLeft && !btnLeftLast) tapKey(uiBarFocus >= 0 ? HID_KEY_LEFT : HID_KEY_DOWN);
      if (btnConfirm && !btnConfirmLast) tapKey(HID_KEY_ENTER);
      if (btnBack && !btnBackLast) tapKey(HID_KEY_ESCAPE);
      break;
    }

    case UIState::TEXT_EDITOR: {
      // Key repeat state for held navigation/backspace keys
      static uint8_t repeatKey = 0;
      static unsigned long repeatStart = 0;
      static unsigned long lastRepeat = 0;
      const unsigned long REPEAT_DELAY = 400;
      const unsigned long REPEAT_RATE  = 80;

      // Map currently held button to HID key (0 = none)
      uint8_t heldKey = 0;
      if      (btnUp)    heldKey = HID_KEY_UP;
      else if (btnDown)  heldKey = HID_KEY_DOWN;
      else if (btnLeft)  heldKey = HID_KEY_LEFT;
      else if (btnRight) heldKey = HID_KEY_RIGHT;

      if (heldKey != repeatKey) {
        // Key changed — fire immediately on press
        if (heldKey != 0) tapKey(heldKey);
        repeatKey   = heldKey;
        repeatStart = millis();
        lastRepeat  = millis();
      } else if (heldKey != 0) {
        unsigned long now = millis();
        if (now - repeatStart > REPEAT_DELAY && now - lastRepeat > REPEAT_RATE) {
          tapKey(heldKey);
          lastRepeat = now;
        }
      }

      if (btnConfirm && !btnConfirmLast) tapKey(HID_KEY_ENTER);
      if (btnBack && !btnBackLast) tapKey(HID_KEY_ESCAPE);   // Esc: the Transfer / Exit choice (Back again exits)
      break;
    }

    case UIState::RENAME_FILE:
    case UIState::NEW_FILE:
      if (btnConfirm && !btnConfirmLast) tapKey(HID_KEY_ENTER);
      if (btnBack && !btnBackLast) tapKey(HID_KEY_ESCAPE);
      break;

    case UIState::WIFI_SYNC:
    case UIState::TRANSFER:
    case UIState::FW_UPDATE:
      if ((btnUp && !btnUpLast) || (btnRight && !btnRightLast)) tapKey(HID_KEY_UP);
      if ((btnDown && !btnDownLast) || (btnLeft && !btnLeftLast)) tapKey(HID_KEY_DOWN);
      if (btnConfirm && !btnConfirmLast) tapKey(HID_KEY_ENTER);
      if (btnBack && !btnBackLast) tapKey(HID_KEY_ESCAPE);
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
const unsigned long IDLE_TIMEOUT = 5UL * 60UL * 1000UL; // 5 minutes

void registerActivity() {
  lastActivityTime = millis();
}

// Function to render the sleep screen
void renderSleepScreen() {
  renderer.clearScreen();
  sleepscr::drawForSleep(renderer);   // a sleep picture, or the stock screen (sleep/sleep_screen.h)

  // Perform a full display refresh to ensure the sleep screen is visible
  renderer.displayBuffer(HalDisplay::FULL_REFRESH);
  
  // Small delay to ensure the display update is complete
  delay(500);
}

void loop() {
  // Feed the hardware watchdog every iteration. If the loop ever wedges for >30s
  // this stops firing and the chip reboots cleanly instead of freezing.
  esp_task_wdt_reset();

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

  // First pairing (no keyboard stored): the Bluetooth screen keeps searching, and when a search finds exactly one
  // keyboard it connects without a key press, as soon as the user puts the keyboard in pairing mode. Each keyboard is
  // tried once per visit, so a refused pairing does not repeat. Two or more keyboards: the list, nearest first, and
  // the user picks. Once connected, on to the main menu.
  static bool firstPair = false;
  static std::string autoTried;
  if (currentState == UIState::BLUETOOTH_SETTINGS) {
    if (lastState != UIState::BLUETOOTH_SETTINGS) {
      firstPair = getPairedKeyboardCount() == 0;
      autoTried.clear();
    }
    if (firstPair && isKeyboardConnected()) {
      firstPair = false;
      currentState = UIState::MAIN_MENU;
      screenDirty = true;
    } else if (firstPair && !isDeviceScanning() && !bleConnectBusy() && getCurrentPasskey() == 0) {
      int idx[32], total = 0, kbd = 0, hidden = 0;
      getVisibleDeviceIndices(false, idx, 32, &total, &kbd, &hidden);
      if (kbd == 1 && getDiscoveredDevices()[idx[0]].address != autoTried) {
        autoTried = getDiscoveredDevices()[idx[0]].address;
        connectToDevice(idx[0]);
      } else {
        startDeviceScan(true);   // keep searching; the list stays so the user can pick from it
      }
      screenDirty = true;
    }
  }
  lastState = currentState;

  // Process BLE (connection handling, scan completion detection)
  bleLoop();

  // OneDrive sync: runs the engine and decides when to sync (idle save, note closed, notes list opened).
  cloud::loop();
  editorProfReport();
  if (currentState != UIState::TEXT_EDITOR && !renderer.isRefreshing()) fileManagerPoll();
  safetyProbePoll();   // timing builds only: the safety audit's serial commands
  {
    static UIState cloudPrev = UIState::MAIN_MENU;
    if (currentState != cloudPrev) {
      if (cloudPrev == UIState::TEXT_EDITOR) cloud::onNoteClosed();
      if (currentState == UIState::FILE_BROWSER && cloudPrev == UIState::MAIN_MENU) cloud::onBrowse();
      cloudPrev = currentState;
    }
    if (cloud::browseHoldActive() && currentState != UIState::FILE_BROWSER) cloud::browseHoldCancel();
    // A note opened during a sync waited for its own transfer: open it now.
    char held[MAX_FILENAME_LEN];
    if (cloud::takeHeldNote(held, sizeof(held)) && currentState == UIState::FILE_BROWSER) {
      loadFile(held);
      screenDirty = true;
    }
  }

  // Process WiFi sync HTTP clients when active
  if (isWifiSyncActive()) wifiSyncLoop();
  transfer::loop();
  fwupdate::loop();

  // CRITICAL: Process buttons BEFORE checking wasAnyPressed() to avoid consuming button states
  processPhysicalButtons();
  processTouch();  // touch panel (Sticky): swipes/taps become menu key events
  int inputEventsProcessed = processAllInput(); // Assuming this returns number of events processed

  // Register activity AFTER button processing (don't consume button states prematurely)
  static unsigned long lastInputTime = 0;
  bool hadActivity = gpio.wasAnyPressed() || inputEventsProcessed > 0;
  if (hadActivity) {
    registerActivity();
    lastInputTime = millis();
  }

  // SD card pulled: everything hinges on it, so park. The editor is the exception: the text lives in RAM, so it stays
  // on screen (header says "No SD") and is saved as soon as the card is back. Anywhere else a key press tries the card
  // and restarts the device when it answers (a restart goes through the boot path that is known to work).
  if (sdLost()) {
    if (currentState == UIState::TEXT_EDITOR) {
      if (hadActivity) sdResume();
    } else if (editorHasUnsavedChanges() && editorGetLength() > 0) {
      currentState = UIState::TEXT_EDITOR;   // something tried to leave the editor with unsaved text: keep it open
      screenDirty = true;
    } else {
      if (currentState != UIState::SD_MISSING) {
        if (isWifiSyncActive()) wifiSyncStop();
        cloud::screenClose();
        currentState = UIState::SD_MISSING;
        screenDirty = true;
      }
      if (hadActivity && sdProbe()) {
        DBG_PRINTLN("SD card back, restarting");
        esp_restart();
      }
    }
  }

  // "Note full" in the editor header goes away by itself: redraw once when its time is up.
  {
    static bool noteFullShown = false;
    const bool shown = editorNoteFullShowing();
    if (noteFullShown && !shown && currentState == UIState::TEXT_EDITOR) screenDirty = true;
    noteFullShown = shown;
  }

  // Auto-save: hybrid idle + hard cap for crash protection.
  // - Saves after AUTO_SAVE_IDLE_MS of no keystrokes (catches natural pauses between sentences)
  // - Hard cap every AUTO_SAVE_MAX_MS (1 min) during continuous typing
  static unsigned long lastAutoSaveMs = 0;
  if (currentState == UIState::TEXT_EDITOR
      && editorHasUnsavedChanges()
      && editorGetCurrentFile()[0] != '\0'
      && !sdLost()) {
    unsigned long now = millis();
    const unsigned long pauseMs = cloud::pauseMs();   // 10 s, or 1 s when the cloud backup is overdue
    bool idleTrigger = (now - lastInputTime) > pauseMs
                    && (now - lastAutoSaveMs) > pauseMs;
    bool capTrigger  = (now - lastAutoSaveMs) > AUTO_SAVE_MAX_MS;
    // Flush at paragraph breaks (newline), rate-limited so holding Enter / fast
    // paragraphs don't hammer the SD. Shrinks the worst-case crash loss to a line.
    bool paraTrigger = editorHasParagraphBreakPending()
                    && (now - lastAutoSaveMs) > AUTO_SAVE_PARA_MIN_MS;
    if (idleTrigger || capTrigger || paraTrigger) {
      lastAutoSaveMs = now;
      editorClearParagraphBreak();
      saveCurrentFile(false);  // Skip refreshFileList — file list unchanged by content update
      cloud::onAutosave();
    }
  }

  if (currentState == UIState::TEXT_EDITOR) cloud::editorTick(millis() - lastInputTime, editorHasUnsavedChanges());

  // Periodically refresh sync screen to show status changes (every 2s)
  if (currentState == UIState::WIFI_SYNC) {
    static unsigned long lastSyncRefresh = 0;
    if (millis() - lastSyncRefresh > 2000) {
      screenDirty = true;
      lastSyncRefresh = millis();
    }
  }

  // Poll display refresh — non-blocking check of BUSY pin
  if (renderer.isRefreshing()) {
    renderer.pollRefresh();
  }

  // Don't start a new screen update while display is still refreshing
  if (screenDirty && !renderer.isRefreshing()) {
    updateScreen();
  }

  uiprefs::saveIfChanged();   // NVS and the SD copy, only after a change

  // Check for idle timeout (skip while WiFi sync is active)
  if (!isWifiSyncActive() && !fwupdate::keepAwake() && millis() - lastActivityTime > IDLE_TIMEOUT
      && !(sdLost() && currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges())) {
    enterDeepSleep(SleepReason::IDLE_TIMEOUT);
  }

  // Adaptive delay with recently-active window for button responsiveness.
  // BLE keystrokes wake from light sleep via modem interrupt (delay value irrelevant).
  // Physical buttons are polled, so the idle delay must be short enough to catch a
  // quick tap (~80-150ms). 50ms idle guarantees 1-2 samples per press.
  // Stay at fast polling for 2s after any activity for snappy consecutive presses.
  static constexpr unsigned long ACTIVE_WINDOW_MS = 2000;
  bool recentlyActive = (millis() - lastInputTime) < ACTIVE_WINDOW_MS;
  delay((hadActivity || screenDirty || recentlyActive) ? 10 : 50);
}

bool displayCaughtUp() { return !screenDirty && !renderer.isRefreshing(); }
