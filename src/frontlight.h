#pragma once

#include <stdint.h>

// Frontlight (X4 Pro): Off and five brightness steps, warm and cool lit together (as Draftling does; no warmth
// setting). Settings > Display > Frontlight and Ctrl+B change it; the step is kept in NVS. On devices without a
// frontlight every call does nothing and present() is false, so the Settings row stays hidden.
namespace light {

bool present();
void begin();                 // at boot: restores the saved step
void cycle(int dir);          // +1 brighter (Off after the brightest), -1 dimmer; saves the step
void off();                   // before deep sleep (the saved step is kept for the next boot)
const char* label();          // "Off", "20%" ... "100%"

}  // namespace light
