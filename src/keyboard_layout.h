#pragma once

#include "config.h"

static constexpr int KEYBOARD_LAYOUT_COUNT = 3;

KeyboardLayout keyboardLayoutFromIndex(int index);
KeyboardLayout keyboardLayoutNext(KeyboardLayout layout);
KeyboardLayout keyboardLayoutPrev(KeyboardLayout layout);
const char* keyboardLayoutName(KeyboardLayout layout);
const char* keyboardLayoutShortName(KeyboardLayout layout);

// Returns a UTF-8 string literal for printable keys, or nullptr for non-printable keys.
const char* keyboardLayoutText(KeyboardLayout layout, uint8_t hid, uint8_t modifiers, bool capsLockOn);

// US QWERTY is used in fields where ASCII input is expected, regardless of editor layout.
const char* keyboardLayoutQwertyText(uint8_t hid, uint8_t modifiers, bool capsLockOn);
