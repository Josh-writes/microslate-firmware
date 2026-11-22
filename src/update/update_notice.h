#pragma once
// Check for updates (Settings > Wi-Fi, on by default; Josh, 2026-11-22). When on, the first wake of the day asks GitHub whether a newer
// MicroSlate release is out (Josh, 2026-11-22: MicroSlate only, not the reader). It joins a saved network for this
// itself and switches Wi-Fi off again, only while the main menu is open (never while the writer types: Wi-Fi and
// Bluetooth share the radio). The newest version seen is kept in NVS; a notice shows on the right of the main menu's
// footer and on the Settings > Wi-Fi > Firmware update row until that version is installed. The footer says
// "Checking for updates..." while it runs, then "Up to date" or "Couldn't check for updates" for a few seconds.

namespace updnotice {

extern bool enabled;      // the setting (ui_prefs "updChk")

void loop();              // every main loop: runs the check once after a wake, when it is due
bool busy();              // a check is joining Wi-Fi or running: the device stays awake, a sync keeps Wi-Fi up

const char* firmware();   // a newer MicroSlate version seen ("3.0.6"), or ""
const char* footer();     // the main menu's footer: "Checking for updates...", "Update available: MicroSlate 3.0.6",
                          // "Up to date" / "Couldn't check for updates" (a few seconds after a check), or ""

}  // namespace updnotice
