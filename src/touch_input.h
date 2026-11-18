#pragma once

// Touch input for devices with a touch panel (reTerminal Sticky). Touch feeds the
// same key-event queue the physical buttons use. Gestures follow CrossPoint so
// both firmwares feel the same:
//   tap a list row                                   -> highlight it; tap it again to open
//   swipe up / down (mid-screen)                     -> highlight follows the finger,
//                                                       one row per row-height dragged
//   swipe left->right from the left edge (left 25%)  -> Back (Esc)
//   swipe up from the bottom edge (bottom 14%)       -> Home (main menu)
// Screens that publish no rows (status, pairing code) treat a tap as Confirm.
// The text editor and text-entry screens are keyboard-only: there only the two
// edge swipes (Back, Home) are honored, never taps or mid-screen swipes. Taps
// are also ignored while a delete confirmation is pending.
// A no-op on devices without touch.
void processTouch();

// main.cpp: what a short press of the power button does (save, close the screen, main menu). The home swipe uses it.
void goToMainMenu();
