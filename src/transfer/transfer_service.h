#pragma once
// Transfer: the device serves a small page so a phone or computer can copy or download notes. No account, no
// internet, nothing to install. It uses a saved Wi-Fi network when one is in range (the one cloud sync uses); when
// none is, the writer can start the device's own hotspot. It runs only while the Transfer screen is open.
// Spec: MicroSlate-workspace/notes/wifi-transfer-v3-spec.md (decisions of 2026-10-29).

#include <cstdint>
#include "config.h"

namespace transfer {

enum class Phase : uint8_t {
  Off,
  Looking,    // scanning for / joining a saved network
  NoWifi,     // none in range: offer the hotspot
  Ready,      // serving (home network or hotspot)
  Failed      // the hotspot or the server could not start
};

// Opens the Transfer screen. noteFile = one note ("Transfer note", from the editor or the notes list), nullptr = all
// notes. Leaving the screen goes to backTo.
void open(const char* noteFile, UIState backTo);
// Picture mode: the same screen and server, but the page takes a sleep picture from a phone (sleep_page.h) and nothing
// else is shared.
void openPicture(UIState backTo);
void close();      // stop and go back to where Transfer was opened from
void stop();       // stop without changing the screen (sleep, or the screen was left another way)
void loop();       // every loop(); cheap when off
void key(uint8_t key);   // Esc / Enter (bar buttons arrive as Esc / Enter)

struct View {
  Phase phase;
  bool hotspot;          // serving on the device's own network
  const char* what;      // the note's title, or "All notes"
  const char* ssid;      // hotspot: its name; home: the network joined
  const char* password;  // hotspot only
  const char* code;      // the access code in the address
  char ip[16];
  int sent;              // notes sent (picture mode: pictures received) since the screen opened
  bool picture;          // picture mode
  const char* error;
};
const View& view();

}  // namespace transfer
