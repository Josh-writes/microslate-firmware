#pragma once

#include <cstddef>

// The Settings screen's rows. Five tabs (Josh, 2026-10-28): Editor, Wi-Fi, Bluetooth, Keyboard, Display. Every row
// is one entry in the table in settings_menu.cpp (label, the value shown on the right, what Prev / Next or Open
// do), so the screen and the input handler cannot disagree about a row. The Wi-Fi tab lists the saved
// networks, then "Add a network", "Firmware update" and, on devices with reader builds, "Reader" (dual boot).
// settingsSelection (main.cpp) is 0 for the tab strip and 1.. for the open tab's rows; rows here count from 0.

constexpr int kSettingsTabCount = 5;
constexpr int kSettingsTabWifi = 1;

enum class SettingKind {
  None,        // no such row
  Value,       // a value to step through: Back / Prev / Next (Esc / Left / Right; Enter steps forward)
  Opens,       // opens its own screen: Back / Open (Enter or Right)
  SavedWifi,   // a saved network: Back / Prefer / Forget
  AddWifi,     // "+ Add a network": Back / Open (Enter)
};

const char* settingsTabName(int tab);
int settingsTabRows(int tab);
SettingKind settingKind(int tab, int row);
const char* settingLabel(int tab, int row);
void settingValue(int tab, int row, char* out, size_t size);   // "" when the row shows no value
void settingStep(int tab, int row, int dir);                   // Value rows: dir +1 next, -1 previous
void settingOpen(int tab, int row);                            // Opens and AddWifi rows
int settingWifiIndex(int tab, int row);                        // SavedWifi rows: index in the saved list, else -1

// The editor menu (Ctrl+M) shares these rows: Writing mode, Font size, Word count, Focus mode, Keyboard layout,
// Spellcheck and Autocorrect (builds with a word list), then Shortcuts, which opens its page.
int editorMenuRows();
bool editorMenuOpensPage(int row);   // the Shortcuts row (drawn with ">")
const char* editorMenuLabel(int row);
void editorMenuValue(int row, char* out, size_t size);
void editorMenuChange(int row, int dir);   // dir +1: Enter or Right, -1: Left; the Shortcuts row opens its page
