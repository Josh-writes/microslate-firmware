#pragma once
// Dual boot with a reader (Settings > Wi-Fi > Reader). MicroSlate stays in one app slot of the stock partition
// table; the writer can install one reader, CrossPoint or CrossInk, into the other slot from its GitHub release
// (Josh-writes/crosspoint-microslate, Josh-writes/crossink-microslate: <id>-<device>.bin). The installed reader then
// appears in the main menu under its own name, and its home screen has a "MicroSlate" entry back
// (src/microslate_switch.h, shared with the readers). A MicroSlate update writes over the reader; the new version
// brings the same reader back after the restart.
// Every downloaded reader is also saved on the SD card (/microslate/readers). Installing it again (a swap back,
// Reinstall, or after a MicroSlate update) uses that copy unless GitHub has a newer release, and without Wi-Fi.
// The reader's first sector is written last, so an interrupted download never leaves a reader that looks installed.

#include <cstddef>
#include <cstdint>

namespace readers {

enum class Phase : uint8_t {
  Idle,        // screen closed
  List,        // CrossPoint and CrossInk cards (and a Remove button when one is installed)
  Confirm,     // Install / Check for updates / Remove?
  NoWifi,      // no saved network, or none could be joined
  Connecting,  // joining a saved network
  Preparing,   // finding the file and clearing the slot
  Installing,  // downloading (pct)
  Saving,      // copying the downloaded reader to the SD card (pct)
  Restoring,   // installing the saved copy from the SD card (pct)
  KeepCopy,    // after Remove: keep the saved copy on the SD card, or delete it
  Done,        // installed (or updated): shown for a moment, then the main menu
  UpToDate,    // Check for updates: nothing newer than the installed release; shown for a moment, then the main menu
  Failed       // error text
};

struct View {
  Phase phase;
  int sel;              // List: the card with the outline
  int pct;              // Installing: 0..100
  int target;           // Confirm / Preparing / Installing / Done / Failed / KeepCopy: the reader (index), -1 for Remove
  bool reinstall;       // Installing after a MicroSlate update
  bool update;          // Check for updates on the installed reader (not an install)
  char message[96];     // NoWifi / Failed: the line under the title
};

// The readers on offer.
int count();
const char* name(int i);    // "CrossPoint"
const char* blurb(int i);   // one line under the name

bool supported();           // this device has reader builds (reTerminal Sticky, Xteink X4 Pro, X4 Classic)
bool saved(int i);          // a copy of it is on the SD card
uint32_t savedBytes(int i); // its size
const char* installedTag(); // the installed reader's release ("v2.0.0"), or "" when not known
const char* installed();    // the installed reader's name, or nullptr (the main menu shows it when set)
int installedIndex();       // its index in the list, or -1 (a reader from elsewhere)
bool reinstallNeeded();     // a reader was chosen but is not in the slot (an update wrote over it, or it failed)
void openInstalled();       // boots the reader; returns only on failure

void settingValue(char* out, size_t n);   // the Settings row: "CrossInk", "None", "Reinstall CrossInk"
void open();                              // the Settings row: the list
void boot();                              // once in setup(): resumes the reinstall after a MicroSlate update
void beforeMicroSlateUpdate();            // fw_update, when Install is tapped (before the reader is written over)
void key(uint8_t keyCode);                // arrows move, Enter or 1 / 2 asks, Delete asks to remove, Esc back
void loop();                              // every main loop
const View& view();
uint8_t barMask();                        // which bar buttons the screen has
bool keepAwake();                         // a download is running: no idle sleep

}  // namespace readers
