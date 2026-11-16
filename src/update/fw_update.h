#pragma once
// Firmware update from the GitHub release (Settings > Wi-Fi > Firmware update). The latest release's tag is read from
// github.com/<repo>/releases/latest (a redirect to .../tag/vX.Y.Z); when it is newer than this build and has this
// device's file (microslate-<device>-vX.Y.Z.bin, as release.yml names it), the writer is asked, and the file streams
// into the other app slot. A failed or interrupted download leaves the running firmware as it was. The device then
// runs from that slot, and the next update goes to the other one (as CrossPoint does); cable flashing erases otadata
// so the device starts the first slot again.
// v3 is a standalone firmware: both app slots are MicroSlate's (no dual boot).

#include <cstdint>

namespace fwupdate {

enum class Phase : uint8_t {
  Idle,         // screen closed
  NoWifi,       // no saved network, or none could be joined
  Connecting,   // joining a saved network
  Checking,     // asking GitHub
  UpToDate,     // nothing newer (or the newest release has no file for this device)
  Available,    // a newer version: Install?
  Installing,   // downloading into the other slot (pct)
  Done,         // installed: restarting
  Failed        // error text
};

struct View {
  Phase phase;
  int pct;             // Installing: 0..100
  char latest[24];     // Available / Installing / Done: "3.0.1"
  char message[96];    // UpToDate / NoWifi / Failed: the line under the title
};

const char* version();   // this build, "3.0.0"
bool supported();        // this device has release files

void open();               // the Settings row: join Wi-Fi and check
void key(uint8_t keyCode); // Enter: Install / Try again; Esc: back to Settings
void loop();               // every main loop
const View& view();
bool keepAwake();          // a check or install is running: no idle sleep

}  // namespace fwupdate
