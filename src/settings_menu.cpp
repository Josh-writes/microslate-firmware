#include "settings_menu.h"

#include "ble_keyboard.h"
#include "config.h"
#include "frontlight.h"
#include "screen_refresh.h"
#include "input_handler.h"   // editorMenu (the Shortcuts page)
#include "keyboard_layout.h"
#include "sleep/sleep_screen.h"
#include "spell/spell_service.h"
#include "update/fw_update.h"
#include "wifi_sync.h"

#include <cstdio>

// --- Shared UI state (defined in main.cpp and input_handler.cpp) ---
extern UIState currentState;
extern Orientation currentOrientation;
extern bool darkMode;
extern WritingMode writingMode;
extern FontSize fontSize;
extern bool newNotesMd;
extern bool showWordCount;
extern bool cleanMode;
extern int uiBarFocus;
extern int sleepPicSel;
extern bool sleepPicPreview;
extern bool sleepPicDeletePending;
extern int bluetoothDeviceSelection;
extern bool btShowAllDevices;
extern int pairedKeyboardSelection;
extern bool pairedForgetPending;
extern int myWordsSel;
extern bool myWordsRemovePending;
extern int layoutEdKey;
extern int layoutEdSlot;
extern bool layoutEdEntry;
extern char layoutEdMessage[96];
extern bool layoutEdConfirmAll;

namespace {

int wrap(int v, int dir, int n) { return (v + dir + n) % n; }

// ---- Value rows ----

void orientationValue(char* out, size_t n) {
  static const char* kNames[4] = {"Portrait", "Landscape CW", "Inverted", "Landscape CCW"};
  snprintf(out, n, "%s", kOrientationLocked ? "Landscape (fixed)" : kNames[static_cast<int>(currentOrientation) & 3]);
}
void orientationStep(int dir) { currentOrientation = static_cast<Orientation>(wrap(static_cast<int>(currentOrientation), dir, 4)); }

void darkModeValue(char* out, size_t n) { snprintf(out, n, "%s", darkMode ? "Dark" : "Light"); }
void darkModeStep(int) { darkMode = !darkMode; }

void writingModeValue(char* out, size_t n) {
  static const char* kNames[3] = {"Normal", "Typewriter", "Pagination"};
  snprintf(out, n, "%s", kNames[static_cast<int>(writingMode) % 3]);
}
void writingModeStep(int dir) { writingMode = static_cast<WritingMode>(wrap(static_cast<int>(writingMode), dir, 3)); }

void fontSizeValue(char* out, size_t n) {
  static const char* kNames[3] = {"Small", "Medium", "Large"};
  snprintf(out, n, "%s", kNames[static_cast<int>(fontSize) % 3]);
}
void fontSizeStep(int dir) { fontSize = static_cast<FontSize>(wrap(static_cast<int>(fontSize), dir, 3)); }

void newNotesValue(char* out, size_t n) { snprintf(out, n, "%s", newNotesMd ? ".md" : ".txt"); }
void newNotesStep(int) { newNotesMd = !newNotesMd; }

void spellcheckValue(char* out, size_t n) { snprintf(out, n, "%s", spellsvc::langLabel(spellLang)); }
void spellcheckStep(int dir) { spellLang = static_cast<uint8_t>(wrap(spellLang, dir, spellsvc::kLangCount)); }   // Off / US / UK

void autocorrectValue(char* out, size_t n) { snprintf(out, n, "%s", autocorrectOn ? "On" : "Off"); }
void autocorrectStep(int) { autocorrectOn = !autocorrectOn; }

void layoutValue(char* out, size_t n) {
  // A file with mistakes says so; the details are in /microslate/layout-problems.txt.
  if (keyboardLayoutProblems() > 0) snprintf(out, n, "%s (see problems)", keyboardLayoutShortName());
  else snprintf(out, n, "%s", keyboardLayoutName());
}
void layoutStep(int dir) { dir > 0 ? keyboardLayoutNext() : keyboardLayoutPrev(); }

void blePowerValue(char* out, size_t n) { snprintf(out, n, "%s", getBleTxPowerLabel()); }
void blePowerStep(int dir) { cycleBleTxPower(dir); }   // more or less power, adjustable in place

void frontlightValue(char* out, size_t n) { snprintf(out, n, "%s", light::label()); }
void frontlightStep(int dir) { light::cycle(dir); }

void refreshValue(char* out, size_t n) { snprintf(out, n, "%s", refresh::label()); }
void refreshStep(int dir) { refresh::cycle(dir); }

void wordCountValue(char* out, size_t n) { snprintf(out, n, "%s", showWordCount ? "Shown" : "Hidden"); }
void wordCountStep(int) { showWordCount = !showWordCount; }

void focusValue(char* out, size_t n) { snprintf(out, n, "%s", cleanMode ? "On" : "Off"); }
void focusStep(int) { cleanMode = !cleanMode; }

// ---- Rows that open a screen ----

void bluetoothValue(char* out, size_t n) { snprintf(out, n, "%s", isKeyboardConnected() ? "Connected" : "Not connected"); }
void bluetoothOpen() {
  bluetoothDeviceSelection = 0;
  btShowAllDevices = false;   // filtered by default (see the note at its definition)
  currentState = UIState::BLUETOOTH_SETTINGS;
}

void pairedValue(char* out, size_t n) {
  const int count = getPairedKeyboardCount();
  if (count == 0) snprintf(out, n, "None");
  else if (count == 1) snprintf(out, n, "1 keyboard");
  else snprintf(out, n, "%d keyboards", count);
}
void pairedOpen() {
  pairedKeyboardSelection = 0;
  pairedForgetPending = false;
  currentState = UIState::PAIRED_KEYBOARDS;
}

void layoutEditValue(char* out, size_t n) {
  const int changed = keyboardLayoutChangedCount();
  if (changed > 0) snprintf(out, n, "%d changed", changed);
}
void layoutEditOpen() {
  layoutEdKey = -1;
  layoutEdSlot = 0;
  layoutEdEntry = false;
  layoutEdMessage[0] = '\0';
  layoutEdConfirmAll = false;
  currentState = UIState::LAYOUT_EDITOR;
}

void sleepScreenValue(char* out, size_t n) { snprintf(out, n, "%s", sleepscr::choiceLabel().c_str()); }
void sleepScreenOpen() {
  sleepscr::refresh();
  // Open on the row in use.
  const sleepscr::Choice ch = sleepscr::choice();
  sleepPicSel = ch == sleepscr::Choice::Random ? 1 : 0;
  for (int i = 0; ch == sleepscr::Choice::Picture && i < sleepscr::count(); i++)
    if (sleepscr::isChosen(i)) sleepPicSel = i + 2;
  sleepPicPreview = false;
  sleepPicDeletePending = false;
  currentState = UIState::SLEEP_PICTURES;
}

void myWordsValue(char* out, size_t n) {
  const int count = spellsvc::ownCount();
  if (count == 1) snprintf(out, n, "1 word");
  else snprintf(out, n, "%d words", count);
}
void myWordsOpen() {
  myWordsSel = 0;
  myWordsRemovePending = false;
  currentState = UIState::MY_WORDS;
}

void shortcutsOpen() { editorMenu = 2; }

void updateValue(char* out, size_t n) { snprintf(out, n, "v%s", fwupdate::version()); }
void updateOpen() { fwupdate::open(); }

// ---- The table ----

bool always() { return true; }
bool hasSpellcheck() { return spellsvc::available(); }   // builds with the word list (S3 boards)
bool hasFrontlight() { return light::present(); }        // X4 Pro
bool hasRefreshChoice() { return refresh::present(); }   // E1001

struct Setting {
  const char* label;
  bool (*shown)();                       // whether this device has the row
  void (*value)(char* out, size_t n);    // the value on the right; nullptr: none
  void (*step)(int dir);                 // Value rows
  void (*open)();                        // Opens rows
};

constexpr Setting kWritingMode = {"Writing mode", always, writingModeValue, writingModeStep, nullptr};
constexpr Setting kFontSize = {"Font size", always, fontSizeValue, fontSizeStep, nullptr};
constexpr Setting kSpellcheck = {"Spellcheck", hasSpellcheck, spellcheckValue, spellcheckStep, nullptr};
constexpr Setting kAutocorrect = {"Autocorrect", hasSpellcheck, autocorrectValue, autocorrectStep, nullptr};
constexpr Setting kLayout = {"Keyboard layout", always, layoutValue, layoutStep, nullptr};

constexpr Setting kEditor[] = {
    kWritingMode,
    kFontSize,
    {"New notes", always, newNotesValue, newNotesStep, nullptr},
    kSpellcheck,
    kAutocorrect,
    {"My words", hasSpellcheck, myWordsValue, nullptr, myWordsOpen},
};
constexpr Setting kBluetooth[] = {
    {"Bluetooth", always, bluetoothValue, nullptr, bluetoothOpen},
    {"Paired keyboards", always, pairedValue, nullptr, pairedOpen},
    {"BLE power", always, blePowerValue, blePowerStep, nullptr},
};
constexpr Setting kKeyboard[] = {
    kLayout,
    {"Edit keyboard layout", always, layoutEditValue, nullptr, layoutEditOpen},
};
constexpr Setting kDisplay[] = {
    {"Orientation", always, orientationValue, orientationStep, nullptr},
    {"Dark mode", always, darkModeValue, darkModeStep, nullptr},
    {"Sleep screen", always, sleepScreenValue, nullptr, sleepScreenOpen},
    {"Frontlight", hasFrontlight, frontlightValue, frontlightStep, nullptr},
    {"Refresh", hasRefreshChoice, refreshValue, refreshStep, nullptr},
};

// The editor menu (Ctrl+M): the writing settings, and the Shortcuts page.
constexpr Setting kEditorMenu[] = {
    kWritingMode,
    kFontSize,
    {"Word count", always, wordCountValue, wordCountStep, nullptr},
    {"Focus mode", always, focusValue, focusStep, nullptr},
    kLayout,
    kSpellcheck,
    kAutocorrect,
    {"Shortcuts", always, nullptr, nullptr, shortcutsOpen},
};

// The Wi-Fi tab's last row, under "+ Add a network".
constexpr Setting kUpdate = {"Firmware update", always, updateValue, nullptr, updateOpen};

struct Tab {
  const char* name;
  const Setting* rows;   // nullptr: the Wi-Fi tab, built from the saved networks
  int count;
};

template <size_t N>
constexpr Tab tab(const char* name, const Setting (&rows)[N]) { return {name, rows, static_cast<int>(N)}; }

constexpr Tab kTabs[kSettingsTabCount] = {
    tab("Editor", kEditor), {"Wi-Fi", nullptr, 0}, tab("Bluetooth", kBluetooth), tab("Keyboard", kKeyboard),
    tab("Display", kDisplay),
};

// The entry shown at `row` of a list (rows a device does not have are skipped), and how many are shown.
const Setting* shownRow(const Setting* rows, int count, int row) {
  for (int i = 0; rows && row >= 0 && i < count; i++)
    if (rows[i].shown() && row-- == 0) return &rows[i];
  return nullptr;
}
int shownCount(const Setting* rows, int count) {
  int n = 0;
  for (int i = 0; rows && i < count; i++) n += rows[i].shown() ? 1 : 0;
  return n;
}

// The Wi-Fi tab's rows are the saved networks and Add a network; only its last row (Firmware update) is a Setting.
const Setting* rowSetting(int tab, int row) {
  if (tab == kSettingsTabWifi) return row == wifiSavedCount() + 1 ? &kUpdate : nullptr;
  if (tab < 0 || tab >= kSettingsTabCount) return nullptr;
  return shownRow(kTabs[tab].rows, kTabs[tab].count, row);
}

constexpr int kEditorMenuCount = static_cast<int>(sizeof(kEditorMenu) / sizeof(kEditorMenu[0]));

}  // namespace

const char* settingsTabName(int tab) { return (tab >= 0 && tab < kSettingsTabCount) ? kTabs[tab].name : ""; }

int settingsTabRows(int tab) {
  if (tab == kSettingsTabWifi) return wifiSavedCount() + 2;   // saved networks, Add a network, Firmware update
  if (tab < 0 || tab >= kSettingsTabCount) return 0;
  return shownCount(kTabs[tab].rows, kTabs[tab].count);
}

SettingKind settingKind(int tab, int row) {
  if (tab == kSettingsTabWifi) {
    if (row < 0 || row >= settingsTabRows(tab)) return SettingKind::None;
    if (row < wifiSavedCount()) return SettingKind::SavedWifi;
    return row == wifiSavedCount() ? SettingKind::AddWifi : SettingKind::Opens;
  }
  const Setting* s = rowSetting(tab, row);
  if (!s) return SettingKind::None;
  return s->open ? SettingKind::Opens : SettingKind::Value;
}

const char* settingLabel(int tab, int row) {
  switch (settingKind(tab, row)) {
    case SettingKind::SavedWifi: return wifiSavedSsid(row);
    case SettingKind::AddWifi: return "+  Add a network";
    case SettingKind::None: return "";
    default: return rowSetting(tab, row)->label;
  }
}

void settingValue(int tab, int row, char* out, size_t size) {
  if (size == 0) return;
  out[0] = '\0';
  switch (settingKind(tab, row)) {
    case SettingKind::SavedWifi:
      if (wifiSavedConnected(row)) snprintf(out, size, "Connected");
      else if (wifiSavedPreferred(row) && wifiSavedCount() > 1) snprintf(out, size, "Tried first");
      break;
    case SettingKind::Value:
    case SettingKind::Opens: {
      const Setting* s = rowSetting(tab, row);
      if (s->value) s->value(out, size);
      break;
    }
    default: break;
  }
}

void settingStep(int tab, int row, int dir) {
  if (settingKind(tab, row) == SettingKind::Value) rowSetting(tab, row)->step(dir > 0 ? 1 : -1);
}

void settingOpen(int tab, int row) {
  const SettingKind kind = settingKind(tab, row);
  if (kind == SettingKind::Opens) {
    uiBarFocus = -1;
    rowSetting(tab, row)->open();
  } else if (kind == SettingKind::AddWifi) {
    wifiAddStart();   // the scan list; joining a network saves it and comes back here
    if (isWifiSyncActive()) currentState = UIState::WIFI_SYNC;
  }
}

int settingWifiIndex(int tab, int row) { return settingKind(tab, row) == SettingKind::SavedWifi ? row : -1; }

// ---- The editor menu (Ctrl+M) ----

int editorMenuRows() { return shownCount(kEditorMenu, kEditorMenuCount); }

bool editorMenuOpensPage(int row) {
  const Setting* s = shownRow(kEditorMenu, kEditorMenuCount, row);
  return s && s->open;
}

const char* editorMenuLabel(int row) {
  const Setting* s = shownRow(kEditorMenu, kEditorMenuCount, row);
  return s ? s->label : "";
}

void editorMenuValue(int row, char* out, size_t size) {
  if (size == 0) return;
  out[0] = '\0';
  const Setting* s = shownRow(kEditorMenu, kEditorMenuCount, row);
  if (s && s->value) s->value(out, size);
}

void editorMenuChange(int row, int dir) {
  const Setting* s = shownRow(kEditorMenu, kEditorMenuCount, row);
  if (!s) return;
  if (s->open) s->open();
  else s->step(dir > 0 ? 1 : -1);
}
