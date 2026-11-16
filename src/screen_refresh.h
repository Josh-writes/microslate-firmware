#pragma once

// Screen refresh (E1001): Settings > Display > Refresh chooses the waveform for screen updates.
// Clear (the default) is the panel's own; Fast is about 0.1 s quicker per update but can leave faint traces.
// Neither adds a flashing full refresh. The choice is kept in NVS. On other devices every call does nothing
// and present() is false, so the Settings row stays hidden.
namespace refresh {

bool present();
void begin();           // at boot: restores the saved choice
void cycle(int dir);    // Clear <-> Fast; saves the choice
const char* label();    // "Clear" or "Fast"

}  // namespace refresh
