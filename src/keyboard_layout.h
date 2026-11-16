#pragma once

#include <cstdint>

// Keyboard layouts (lib/Keymap): US QWERTY is built in; every *.txt in /microslate/layouts/
// on the SD card is another layout. Index 0 is US, then the files in name order.
// Loading reads the SD card, so these run on the main task only.

// Finds the layout files and selects the saved one (file name, "" = US). legacyIndex is
// the old built-in choice (1 = RU, 2 = DE), used once when no file name was saved.
void keyboardLayoutsInit(const char* savedFile, int legacyIndex);

void keyboardLayoutNext();
void keyboardLayoutPrev();

const char* keyboardLayoutName();        // "US QWERTY", "Deutsch (QWERTZ)"
const char* keyboardLayoutShortName();   // "US", "DE"
const char* keyboardLayoutFile();        // "de.txt", "" for US
bool keyboardLayoutIsUs();
int keyboardLayoutProblems();            // problems found in the active file (see layout-problems.txt)

// UTF-8 text a key types with the active layout, "" when an accent (dead) key is waiting
// for the next key, or nullptr when the key types nothing.
const char* keyboardLayoutText(uint8_t hid, uint8_t modifiers, bool capsLockOn);
void keyboardLayoutCancelDead();
bool keyboardLayoutDeadPending();   // an accent key is waiting for the next key
bool keyboardLayoutIsTextKey(uint8_t hid);

// Letter a Ctrl shortcut matches for this key (a-z), or 0: the letter on the key in Latin
// layouts (DE: the key labelled Z), the US position in other scripts.
char keyboardLayoutShortcutLetter(uint8_t hid);

// --- Layout editor ---
// Changes are kept apart from the layout files, one small file per layout in
// /microslate/layouts/changes/ (the layout file itself is never edited here).
// What a key gives on a layer (0 normal, 1 Shift, 2 AltGr, 3 Shift+AltGr), nullptr if
// nothing; *dead set for an accent key. Includes the user's changes.
const char* keyboardLayoutSlot(uint8_t hid, int layer, bool* dead);
bool keyboardLayoutKeyChanged(uint8_t hid);   // the user changed this key
int keyboardLayoutChangedCount();
bool keyboardLayoutHasAltGr();                // any key types something with AltGr
// Sets one slot (value nullptr or "" = types nothing) as a change and reloads.
bool keyboardLayoutSetSlot(uint8_t hid, int layer, const char* value);
// Removes the user's change of a key: 1 done, 0 the key was not changed, -1 failed.
int keyboardLayoutResetKey(uint8_t hid);
// Removes every change made to the active layout.
bool keyboardLayoutResetAll();
// False when the editor font cannot show a character of this text.
bool keyboardLayoutSlotShown(const char* text);

// US QWERTY is used in fields where ASCII input is expected, regardless of editor layout.
const char* keyboardLayoutQwertyText(uint8_t hid, uint8_t modifiers, bool capsLockOn);
