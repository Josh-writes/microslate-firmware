#include "keyboard_layout.h"

#include <cstddef>

struct LayoutKey {
  uint8_t hid;
  const char* normal;
  const char* shifted;
  const char* altGr;
  const char* shiftedAltGr;
  bool capsAffected;
};

static const LayoutKey qwertyKeys[] = {
  {HID_KEY_A, "a", "A", nullptr, nullptr, true},
  {HID_KEY_B, "b", "B", nullptr, nullptr, true},
  {HID_KEY_C, "c", "C", nullptr, nullptr, true},
  {HID_KEY_D, "d", "D", nullptr, nullptr, true},
  {HID_KEY_E, "e", "E", nullptr, nullptr, true},
  {HID_KEY_F, "f", "F", nullptr, nullptr, true},
  {HID_KEY_G, "g", "G", nullptr, nullptr, true},
  {HID_KEY_H, "h", "H", nullptr, nullptr, true},
  {HID_KEY_I, "i", "I", nullptr, nullptr, true},
  {HID_KEY_J, "j", "J", nullptr, nullptr, true},
  {HID_KEY_K, "k", "K", nullptr, nullptr, true},
  {HID_KEY_L, "l", "L", nullptr, nullptr, true},
  {HID_KEY_M, "m", "M", nullptr, nullptr, true},
  {HID_KEY_N, "n", "N", nullptr, nullptr, true},
  {HID_KEY_O, "o", "O", nullptr, nullptr, true},
  {HID_KEY_P, "p", "P", nullptr, nullptr, true},
  {HID_KEY_Q, "q", "Q", nullptr, nullptr, true},
  {HID_KEY_R, "r", "R", nullptr, nullptr, true},
  {HID_KEY_S, "s", "S", nullptr, nullptr, true},
  {HID_KEY_T, "t", "T", nullptr, nullptr, true},
  {HID_KEY_U, "u", "U", nullptr, nullptr, true},
  {HID_KEY_V, "v", "V", nullptr, nullptr, true},
  {HID_KEY_W, "w", "W", nullptr, nullptr, true},
  {HID_KEY_X, "x", "X", nullptr, nullptr, true},
  {HID_KEY_Y, "y", "Y", nullptr, nullptr, true},
  {HID_KEY_Z, "z", "Z", nullptr, nullptr, true},
  {HID_KEY_1, "1", "!", nullptr, nullptr, false},
  {HID_KEY_2, "2", "@", nullptr, nullptr, false},
  {HID_KEY_3, "3", "#", nullptr, nullptr, false},
  {HID_KEY_4, "4", "$", nullptr, nullptr, false},
  {HID_KEY_5, "5", "%", nullptr, nullptr, false},
  {HID_KEY_6, "6", "^", nullptr, nullptr, false},
  {HID_KEY_7, "7", "&", nullptr, nullptr, false},
  {HID_KEY_8, "8", "*", nullptr, nullptr, false},
  {HID_KEY_9, "9", "(", nullptr, nullptr, false},
  {HID_KEY_0, "0", ")", nullptr, nullptr, false},
  {HID_KEY_MINUS, "-", "_", nullptr, nullptr, false},
  {HID_KEY_EQUAL, "=", "+", nullptr, nullptr, false},
  {HID_KEY_LEFTBRACE, "[", "{", nullptr, nullptr, false},
  {HID_KEY_RIGHTBRACE, "]", "}", nullptr, nullptr, false},
  {HID_KEY_BACKSLASH, "\\", "|", nullptr, nullptr, false},
  {HID_KEY_SEMICOLON, ";", ":", nullptr, nullptr, false},
  {HID_KEY_APOSTROPHE, "'", "\"", nullptr, nullptr, false},
  {HID_KEY_GRAVE, "`", "~", nullptr, nullptr, false},
  {HID_KEY_COMMA, ",", "<", nullptr, nullptr, false},
  {HID_KEY_DOT, ".", ">", nullptr, nullptr, false},
  {HID_KEY_SLASH, "/", "?", nullptr, nullptr, false},
  {HID_KEY_NON_US_BACKSLASH, "\\", "|", nullptr, nullptr, false},
};

static const LayoutKey russianKeys[] = {
  {HID_KEY_Q, "й", "Й", nullptr, nullptr, true},
  {HID_KEY_W, "ц", "Ц", nullptr, nullptr, true},
  {HID_KEY_E, "у", "У", nullptr, nullptr, true},
  {HID_KEY_R, "к", "К", nullptr, nullptr, true},
  {HID_KEY_T, "е", "Е", nullptr, nullptr, true},
  {HID_KEY_Y, "н", "Н", nullptr, nullptr, true},
  {HID_KEY_U, "г", "Г", nullptr, nullptr, true},
  {HID_KEY_I, "ш", "Ш", nullptr, nullptr, true},
  {HID_KEY_O, "щ", "Щ", nullptr, nullptr, true},
  {HID_KEY_P, "з", "З", nullptr, nullptr, true},
  {HID_KEY_LEFTBRACE, "х", "Х", nullptr, nullptr, true},
  {HID_KEY_RIGHTBRACE, "ъ", "Ъ", nullptr, nullptr, true},
  {HID_KEY_A, "ф", "Ф", nullptr, nullptr, true},
  {HID_KEY_S, "ы", "Ы", nullptr, nullptr, true},
  {HID_KEY_D, "в", "В", nullptr, nullptr, true},
  {HID_KEY_F, "а", "А", nullptr, nullptr, true},
  {HID_KEY_G, "п", "П", nullptr, nullptr, true},
  {HID_KEY_H, "р", "Р", nullptr, nullptr, true},
  {HID_KEY_J, "о", "О", nullptr, nullptr, true},
  {HID_KEY_K, "л", "Л", nullptr, nullptr, true},
  {HID_KEY_L, "д", "Д", nullptr, nullptr, true},
  {HID_KEY_SEMICOLON, "ж", "Ж", nullptr, nullptr, true},
  {HID_KEY_APOSTROPHE, "э", "Э", nullptr, nullptr, true},
  {HID_KEY_Z, "я", "Я", nullptr, nullptr, true},
  {HID_KEY_X, "ч", "Ч", nullptr, nullptr, true},
  {HID_KEY_C, "с", "С", nullptr, nullptr, true},
  {HID_KEY_V, "м", "М", nullptr, nullptr, true},
  {HID_KEY_B, "и", "И", nullptr, nullptr, true},
  {HID_KEY_N, "т", "Т", nullptr, nullptr, true},
  {HID_KEY_M, "ь", "Ь", nullptr, nullptr, true},
  {HID_KEY_COMMA, "б", "Б", nullptr, nullptr, true},
  {HID_KEY_DOT, "ю", "Ю", nullptr, nullptr, true},
  {HID_KEY_GRAVE, "ё", "Ё", nullptr, nullptr, true},
  {HID_KEY_1, "1", "!", nullptr, nullptr, false},
  {HID_KEY_2, "2", "\"", nullptr, nullptr, false},
  {HID_KEY_3, "3", "№", nullptr, nullptr, false},
  {HID_KEY_4, "4", ";", nullptr, nullptr, false},
  {HID_KEY_5, "5", "%", nullptr, nullptr, false},
  {HID_KEY_6, "6", ":", nullptr, nullptr, false},
  {HID_KEY_7, "7", "?", nullptr, nullptr, false},
  {HID_KEY_8, "8", "*", nullptr, nullptr, false},
  {HID_KEY_9, "9", "(", nullptr, nullptr, false},
  {HID_KEY_0, "0", ")", nullptr, nullptr, false},
  {HID_KEY_MINUS, "-", "_", nullptr, nullptr, false},
  {HID_KEY_EQUAL, "=", "+", nullptr, nullptr, false},
  {HID_KEY_BACKSLASH, "\\", "/", nullptr, nullptr, false},
  {HID_KEY_SLASH, ".", ",", nullptr, nullptr, false},
  {HID_KEY_NON_US_BACKSLASH, "\\", "/", nullptr, nullptr, false},
};

static const LayoutKey germanKeys[] = {
  {HID_KEY_A, "a", "A", nullptr, nullptr, true},
  {HID_KEY_B, "b", "B", nullptr, nullptr, true},
  {HID_KEY_C, "c", "C", nullptr, nullptr, true},
  {HID_KEY_D, "d", "D", nullptr, nullptr, true},
  {HID_KEY_E, "e", "E", "€", nullptr, true},
  {HID_KEY_F, "f", "F", nullptr, nullptr, true},
  {HID_KEY_G, "g", "G", nullptr, nullptr, true},
  {HID_KEY_H, "h", "H", nullptr, nullptr, true},
  {HID_KEY_I, "i", "I", nullptr, nullptr, true},
  {HID_KEY_J, "j", "J", nullptr, nullptr, true},
  {HID_KEY_K, "k", "K", nullptr, nullptr, true},
  {HID_KEY_L, "l", "L", nullptr, nullptr, true},
  {HID_KEY_M, "m", "M", "µ", nullptr, true},
  {HID_KEY_N, "n", "N", nullptr, nullptr, true},
  {HID_KEY_O, "o", "O", nullptr, nullptr, true},
  {HID_KEY_P, "p", "P", nullptr, nullptr, true},
  {HID_KEY_Q, "q", "Q", "@", nullptr, true},
  {HID_KEY_R, "r", "R", nullptr, nullptr, true},
  {HID_KEY_S, "s", "S", nullptr, nullptr, true},
  {HID_KEY_T, "t", "T", nullptr, nullptr, true},
  {HID_KEY_U, "u", "U", nullptr, nullptr, true},
  {HID_KEY_V, "v", "V", nullptr, nullptr, true},
  {HID_KEY_W, "w", "W", nullptr, nullptr, true},
  {HID_KEY_X, "x", "X", nullptr, nullptr, true},
  {HID_KEY_Y, "z", "Z", nullptr, nullptr, true},
  {HID_KEY_Z, "y", "Y", nullptr, nullptr, true},
  {HID_KEY_1, "1", "!", nullptr, nullptr, false},
  {HID_KEY_2, "2", "\"", "²", nullptr, false},
  {HID_KEY_3, "3", "§", "³", nullptr, false},
  {HID_KEY_4, "4", "$", nullptr, nullptr, false},
  {HID_KEY_5, "5", "%", nullptr, nullptr, false},
  {HID_KEY_6, "6", "&", nullptr, nullptr, false},
  {HID_KEY_7, "7", "/", "{", nullptr, false},
  {HID_KEY_8, "8", "(", "[", nullptr, false},
  {HID_KEY_9, "9", ")", "]", nullptr, false},
  {HID_KEY_0, "0", "=", "}", nullptr, false},
  {HID_KEY_MINUS, "ß", "?", "\\", nullptr, false},
  {HID_KEY_EQUAL, "´", "`", "~", nullptr, false},
  {HID_KEY_LEFTBRACE, "ü", "Ü", nullptr, nullptr, true},
  {HID_KEY_RIGHTBRACE, "+", "*", "~", nullptr, false},
  {HID_KEY_BACKSLASH, "#", "'", nullptr, nullptr, false},
  {HID_KEY_SEMICOLON, "ö", "Ö", nullptr, nullptr, true},
  {HID_KEY_APOSTROPHE, "ä", "Ä", nullptr, nullptr, true},
  {HID_KEY_GRAVE, "^", "°", nullptr, nullptr, false},
  {HID_KEY_COMMA, ",", ";", nullptr, nullptr, false},
  {HID_KEY_DOT, ".", ":", nullptr, nullptr, false},
  {HID_KEY_SLASH, "-", "_", nullptr, nullptr, false},
  {HID_KEY_NON_US_BACKSLASH, "<", ">", "|", nullptr, false},
};

template <size_t N>
static const char* lookupKey(const LayoutKey (&keys)[N], uint8_t hid, uint8_t modifiers, bool capsLockOn) {
  if (hid == HID_KEY_ENTER) return "\n";
  if (hid == HID_KEY_TAB) return "\t";
  if (hid == HID_KEY_SPACE) return " ";

  const bool shifted = isShift(modifiers);
  const bool altGr = isAltGr(modifiers);
  for (const LayoutKey& key : keys) {
    if (key.hid != hid) continue;

    if (altGr && key.altGr) {
      return (shifted && key.shiftedAltGr) ? key.shiftedAltGr : key.altGr;
    }

    const bool useShifted = key.capsAffected ? (shifted ^ capsLockOn) : shifted;
    return useShifted ? key.shifted : key.normal;
  }

  return nullptr;
}

KeyboardLayout keyboardLayoutFromIndex(int index) {
  if (index < 0 || index >= KEYBOARD_LAYOUT_COUNT) return KeyboardLayout::QWERTY;
  return static_cast<KeyboardLayout>(index);
}

KeyboardLayout keyboardLayoutNext(KeyboardLayout layout) {
  return keyboardLayoutFromIndex((static_cast<int>(layout) + 1) % KEYBOARD_LAYOUT_COUNT);
}

KeyboardLayout keyboardLayoutPrev(KeyboardLayout layout) {
  return keyboardLayoutFromIndex((static_cast<int>(layout) - 1 + KEYBOARD_LAYOUT_COUNT) % KEYBOARD_LAYOUT_COUNT);
}

const char* keyboardLayoutName(KeyboardLayout layout) {
  switch (layout) {
    case KeyboardLayout::RUSSIAN_JCUKEN: return "RU ЙЦУКЕН";
    case KeyboardLayout::GERMAN_QWERTZ:  return "DE QWERTZ";
    case KeyboardLayout::QWERTY:
    default:                             return "US QWERTY";
  }
}

const char* keyboardLayoutShortName(KeyboardLayout layout) {
  switch (layout) {
    case KeyboardLayout::RUSSIAN_JCUKEN: return "RU";
    case KeyboardLayout::GERMAN_QWERTZ:  return "DE";
    case KeyboardLayout::QWERTY:
    default:                             return "US";
  }
}

const char* keyboardLayoutQwertyText(uint8_t hid, uint8_t modifiers, bool capsLockOn) {
  return lookupKey(qwertyKeys, hid, modifiers, capsLockOn);
}

const char* keyboardLayoutText(KeyboardLayout layout, uint8_t hid, uint8_t modifiers, bool capsLockOn) {
  switch (layout) {
    case KeyboardLayout::RUSSIAN_JCUKEN:
      return lookupKey(russianKeys, hid, modifiers, capsLockOn);
    case KeyboardLayout::GERMAN_QWERTZ:
      return lookupKey(germanKeys, hid, modifiers, capsLockOn);
    case KeyboardLayout::QWERTY:
    default:
      return keyboardLayoutQwertyText(hid, modifiers, capsLockOn);
  }
}
