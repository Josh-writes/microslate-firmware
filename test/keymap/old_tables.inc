// The compiled RU/DE/US tables as they were before B3 (src/keyboard_layout.cpp at b2 909b7cc),
// used as the reference the layout files must match. Extracted by a script; do not edit.
namespace old {
static bool oldIsAltGr(uint8_t mod) {
  const bool rightAlt = (mod & 0x40) != 0;
  const bool ctrlAlt = ((mod & 0x11) != 0) && ((mod & 0x44) != 0);
  return rightAlt || ctrlAlt;
}
struct LayoutKey {
  uint8_t hid;
  const char* normal;
  const char* shifted;
  const char* altGr;
  const char* shiftedAltGr;
  bool capsAffected;
};

static const LayoutKey qwertyKeys[] = {
  {0x04, "a", "A", nullptr, nullptr, true},
  {0x05, "b", "B", nullptr, nullptr, true},
  {0x06, "c", "C", nullptr, nullptr, true},
  {0x07, "d", "D", nullptr, nullptr, true},
  {0x08, "e", "E", nullptr, nullptr, true},
  {0x09, "f", "F", nullptr, nullptr, true},
  {0x0A, "g", "G", nullptr, nullptr, true},
  {0x0B, "h", "H", nullptr, nullptr, true},
  {0x0C, "i", "I", nullptr, nullptr, true},
  {0x0D, "j", "J", nullptr, nullptr, true},
  {0x0E, "k", "K", nullptr, nullptr, true},
  {0x0F, "l", "L", nullptr, nullptr, true},
  {0x10, "m", "M", nullptr, nullptr, true},
  {0x11, "n", "N", nullptr, nullptr, true},
  {0x12, "o", "O", nullptr, nullptr, true},
  {0x13, "p", "P", nullptr, nullptr, true},
  {0x14, "q", "Q", nullptr, nullptr, true},
  {0x15, "r", "R", nullptr, nullptr, true},
  {0x16, "s", "S", nullptr, nullptr, true},
  {0x17, "t", "T", nullptr, nullptr, true},
  {0x18, "u", "U", nullptr, nullptr, true},
  {0x19, "v", "V", nullptr, nullptr, true},
  {0x1A, "w", "W", nullptr, nullptr, true},
  {0x1B, "x", "X", nullptr, nullptr, true},
  {0x1C, "y", "Y", nullptr, nullptr, true},
  {0x1D, "z", "Z", nullptr, nullptr, true},
  {0x1E, "1", "!", nullptr, nullptr, false},
  {0x1F, "2", "@", nullptr, nullptr, false},
  {0x20, "3", "#", nullptr, nullptr, false},
  {0x21, "4", "$", nullptr, nullptr, false},
  {0x22, "5", "%", nullptr, nullptr, false},
  {0x23, "6", "^", nullptr, nullptr, false},
  {0x24, "7", "&", nullptr, nullptr, false},
  {0x25, "8", "*", nullptr, nullptr, false},
  {0x26, "9", "(", nullptr, nullptr, false},
  {0x27, "0", ")", nullptr, nullptr, false},
  {0x2D, "-", "_", nullptr, nullptr, false},
  {0x2E, "=", "+", nullptr, nullptr, false},
  {0x2F, "[", "{", nullptr, nullptr, false},
  {0x30, "]", "}", nullptr, nullptr, false},
  {0x31, "\\", "|", nullptr, nullptr, false},
  {0x33, ";", ":", nullptr, nullptr, false},
  {0x34, "'", "\"", nullptr, nullptr, false},
  {0x35, "`", "~", nullptr, nullptr, false},
  {0x36, ",", "<", nullptr, nullptr, false},
  {0x37, ".", ">", nullptr, nullptr, false},
  {0x38, "/", "?", nullptr, nullptr, false},
  {0x64, "\\", "|", nullptr, nullptr, false},
};

static const LayoutKey russianKeys[] = {
  {0x14, "й", "Й", nullptr, nullptr, true},
  {0x1A, "ц", "Ц", nullptr, nullptr, true},
  {0x08, "у", "У", nullptr, nullptr, true},
  {0x15, "к", "К", nullptr, nullptr, true},
  {0x17, "е", "Е", nullptr, nullptr, true},
  {0x1C, "н", "Н", nullptr, nullptr, true},
  {0x18, "г", "Г", nullptr, nullptr, true},
  {0x0C, "ш", "Ш", nullptr, nullptr, true},
  {0x12, "щ", "Щ", nullptr, nullptr, true},
  {0x13, "з", "З", nullptr, nullptr, true},
  {0x2F, "х", "Х", nullptr, nullptr, true},
  {0x30, "ъ", "Ъ", nullptr, nullptr, true},
  {0x04, "ф", "Ф", nullptr, nullptr, true},
  {0x16, "ы", "Ы", nullptr, nullptr, true},
  {0x07, "в", "В", nullptr, nullptr, true},
  {0x09, "а", "А", nullptr, nullptr, true},
  {0x0A, "п", "П", nullptr, nullptr, true},
  {0x0B, "р", "Р", nullptr, nullptr, true},
  {0x0D, "о", "О", nullptr, nullptr, true},
  {0x0E, "л", "Л", nullptr, nullptr, true},
  {0x0F, "д", "Д", nullptr, nullptr, true},
  {0x33, "ж", "Ж", nullptr, nullptr, true},
  {0x34, "э", "Э", nullptr, nullptr, true},
  {0x1D, "я", "Я", nullptr, nullptr, true},
  {0x1B, "ч", "Ч", nullptr, nullptr, true},
  {0x06, "с", "С", nullptr, nullptr, true},
  {0x19, "м", "М", nullptr, nullptr, true},
  {0x05, "и", "И", nullptr, nullptr, true},
  {0x11, "т", "Т", nullptr, nullptr, true},
  {0x10, "ь", "Ь", nullptr, nullptr, true},
  {0x36, "б", "Б", nullptr, nullptr, true},
  {0x37, "ю", "Ю", nullptr, nullptr, true},
  {0x35, "ё", "Ё", nullptr, nullptr, true},
  {0x1E, "1", "!", nullptr, nullptr, false},
  {0x1F, "2", "\"", nullptr, nullptr, false},
  {0x20, "3", "№", nullptr, nullptr, false},
  {0x21, "4", ";", nullptr, nullptr, false},
  {0x22, "5", "%", nullptr, nullptr, false},
  {0x23, "6", ":", nullptr, nullptr, false},
  {0x24, "7", "?", nullptr, nullptr, false},
  {0x25, "8", "*", nullptr, nullptr, false},
  {0x26, "9", "(", nullptr, nullptr, false},
  {0x27, "0", ")", nullptr, nullptr, false},
  {0x2D, "-", "_", nullptr, nullptr, false},
  {0x2E, "=", "+", nullptr, nullptr, false},
  {0x31, "\\", "/", nullptr, nullptr, false},
  {0x38, ".", ",", nullptr, nullptr, false},
  {0x64, "\\", "/", nullptr, nullptr, false},
};

static const LayoutKey germanKeys[] = {
  {0x04, "a", "A", nullptr, nullptr, true},
  {0x05, "b", "B", nullptr, nullptr, true},
  {0x06, "c", "C", nullptr, nullptr, true},
  {0x07, "d", "D", nullptr, nullptr, true},
  {0x08, "e", "E", "€", nullptr, true},
  {0x09, "f", "F", nullptr, nullptr, true},
  {0x0A, "g", "G", nullptr, nullptr, true},
  {0x0B, "h", "H", nullptr, nullptr, true},
  {0x0C, "i", "I", nullptr, nullptr, true},
  {0x0D, "j", "J", nullptr, nullptr, true},
  {0x0E, "k", "K", nullptr, nullptr, true},
  {0x0F, "l", "L", nullptr, nullptr, true},
  {0x10, "m", "M", "µ", nullptr, true},
  {0x11, "n", "N", nullptr, nullptr, true},
  {0x12, "o", "O", nullptr, nullptr, true},
  {0x13, "p", "P", nullptr, nullptr, true},
  {0x14, "q", "Q", "@", nullptr, true},
  {0x15, "r", "R", nullptr, nullptr, true},
  {0x16, "s", "S", nullptr, nullptr, true},
  {0x17, "t", "T", nullptr, nullptr, true},
  {0x18, "u", "U", nullptr, nullptr, true},
  {0x19, "v", "V", nullptr, nullptr, true},
  {0x1A, "w", "W", nullptr, nullptr, true},
  {0x1B, "x", "X", nullptr, nullptr, true},
  {0x1C, "z", "Z", nullptr, nullptr, true},
  {0x1D, "y", "Y", nullptr, nullptr, true},
  {0x1E, "1", "!", nullptr, nullptr, false},
  {0x1F, "2", "\"", "²", nullptr, false},
  {0x20, "3", "§", "³", nullptr, false},
  {0x21, "4", "$", nullptr, nullptr, false},
  {0x22, "5", "%", nullptr, nullptr, false},
  {0x23, "6", "&", nullptr, nullptr, false},
  {0x24, "7", "/", "{", nullptr, false},
  {0x25, "8", "(", "[", nullptr, false},
  {0x26, "9", ")", "]", nullptr, false},
  {0x27, "0", "=", "}", nullptr, false},
  {0x2D, "ß", "?", "\\", nullptr, false},
  {0x2E, "´", "`", "~", nullptr, false},
  {0x2F, "ü", "Ü", nullptr, nullptr, true},
  {0x30, "+", "*", "~", nullptr, false},
  {0x31, "#", "'", nullptr, nullptr, false},
  {0x33, "ö", "Ö", nullptr, nullptr, true},
  {0x34, "ä", "Ä", nullptr, nullptr, true},
  {0x35, "^", "°", nullptr, nullptr, false},
  {0x36, ",", ";", nullptr, nullptr, false},
  {0x37, ".", ":", nullptr, nullptr, false},
  {0x38, "-", "_", nullptr, nullptr, false},
  {0x64, "<", ">", "|", nullptr, false},
};

template <size_t N>
static const char* lookupKey(const LayoutKey (&keys)[N], uint8_t hid, uint8_t modifiers, bool capsLockOn) {
  if (hid == 0x28) return "\n";
  if (hid == 0x2B) return "\t";
  if (hid == 0x2C) return " ";

  const bool shifted = ((modifiers & 0x22) != 0);
  const bool altGr = oldIsAltGr(modifiers);
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

}  // namespace old
