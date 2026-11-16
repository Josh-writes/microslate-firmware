#pragma once
// Keymap engine: what text a key types under a keyboard layout. Pure C++ (no Arduino),
// shared by every device build and tested on a PC (test/keymap).
//
// A layout gives each key up to four layers (normal, Shift, AltGr, Shift+AltGr), each a
// short UTF-8 text or a dead key (an accent that waits for the next key). US QWERTY is
// built in; other layouts are plain text files read with Layout::loadText():
//
//   name = Deutsch (QWERTZ)
//   short = DE
//   base = us            # start from US and list only what differs (or: base = none)
//   altgr = right-alt    # or: any-alt, so Option on a Mac keyboard works as AltGr
//   Y = z Z
//   EQUAL = dead:´ dead:`
//   compose ´ = c ć      # extra or replacement accent pairs (common ones are built in)
//
// Keys are named by their place on a US keyboard (A, 1, MINUS, LBRACKET ... or 0x2D).
// Values are separated by spaces; \s is a space, \\ a backslash, \uXXXX (exactly four
// hex digits) or \u{X...} a code point,
// "none" leaves a layer empty, and +caps / -caps says whether Caps Lock acts like Shift on
// that key (by default it does for letters that have an upper case). A line starting
// with # is a comment.

#include <cstddef>
#include <cstdint>

namespace keymap {

constexpr int kKeys = 0x90;          // HID keyboard usages 0x00-0x8F
constexpr size_t kPoolSize = 3072;   // all key texts of one layout
constexpr int kMaxCompose = 128;     // compose lines in a file
constexpr int kMaxProblems = 8;      // problems kept in a load report

enum Layer : uint8_t { kNormal = 0, kShift, kAltGr, kShiftAltGr, kLayers };

struct Problem {
  int line;
  char text[72];
};

struct LoadReport {
  int keys = 0;        // key lines applied
  int composes = 0;    // compose pairs applied
  int problems = 0;    // lines (or parts) that were skipped
  int unshown = 0;     // characters the device font cannot show (kept anyway)
  Problem list[kMaxProblems];
  int listed = 0;
  void add(int line, const char* fmt, ...);
};

// Returns true when the device font has a glyph for this code point.
using GlyphCheck = bool (*)(uint32_t cp);

class Layout {
 public:
  Layout() { loadUs(); }
  void loadUs();
  // Reads a layout file. Bad lines are skipped and reported; returns false only when
  // nothing usable was found (the layout is then US).
  bool loadText(const char* text, size_t len, LoadReport& report, GlyphCheck canShow = nullptr);
  // Applies the user's changes on top of the loaded layout: a file of key lines only (the
  // on-device editor writes it). Each key it sets is marked as changed. Returns true when
  // at least one key was applied.
  bool applyChanges(const char* text, size_t len, LoadReport& report, GlyphCheck canShow = nullptr);
  bool changed(uint8_t hid) const;   // set by the changes file
  int changedCount() const;

  const char* name() const { return name_; }
  const char* shortName() const { return short_; }
  bool anyAltIsAltGr() const { return anyAlt_; }
  bool latin() const;

  // Text of a key on a layer (nullptr if none). For a dead key, *dead is set and the
  // text is its accent.
  const char* text(uint8_t hid, int layer, bool* dead = nullptr) const;
  bool capsActsAsShift(uint8_t hid) const;
  // +1 / -1 when the file says +caps / -caps for this key, 0 when Caps follows the letters.
  int capsFlag(uint8_t hid) const;
  // Layout's own compose pair (accent + base), or nullptr.
  const char* composeOwn(const char* accent, const char* base) const;

 private:
  void clearKeys();
  void parse(const char* text, size_t len, LoadReport& rep, GlyphCheck canShow, bool changes, bool* sawHeader);
  uint16_t store(const char* s, bool dead);
  bool setKey(uint8_t hid, int layer, const char* value, bool dead);

  uint16_t keys_[kKeys][kLayers];
  uint8_t caps_[kKeys / 8];       // explicit +caps
  uint8_t noCaps_[kKeys / 8];     // explicit -caps
  uint8_t changed_[kKeys / 8];    // keys set by the changes file
  char pool_[kPoolSize];
  size_t used_ = 0;
  struct Pair { uint16_t accent, base, result; };
  Pair compose_[kMaxCompose];
  int composeCount_ = 0;
  char name_[40];
  char short_[8];
  bool anyAlt_ = false;
  int latin_ = -1;                // -1 = decide from the letters
};

// Turns key presses into text with one layout, keeping the dead-key state.
class Typer {
 public:
  // Text a key press types, "" when it was consumed (a dead key waiting), or nullptr
  // when the key types nothing. mods are HID modifier bits.
  const char* press(const Layout& layout, uint8_t hid, uint8_t mods, bool capsLock);
  void cancel() { pending_[0] = '\0'; }
  bool pending() const { return pending_[0] != '\0'; }

 private:
  char pending_[8] = "";
  char out_[24];
};

// Plain US QWERTY text for a key (ASCII-only fields), nullptr if none.
const char* usText(uint8_t hid, uint8_t mods, bool capsLock);

// The letter a Ctrl shortcut should match (a-z), or 0. Latin layouts use the letter the
// key types on the normal layer (DE: the key labelled Z gives 'z'); other scripts use the
// US position, so Ctrl+S still saves on a Cyrillic layout.
char shortcutLetter(const Layout& layout, uint8_t hid);

// True for keys a layout can give text (letters, digits, punctuation, keypad, ISO keys).
bool isLayoutKey(uint8_t hid);

// Key name as used in layout files ("A", "MINUS", "0x87"), for reports.
const char* keyName(uint8_t hid, char* buf, size_t size);

// Key named in a layout file line's left side ("y", "MINUS", "0x2D"), or -1.
int keyByName(const char* name);

// --- Editing a layout file (the on-device layout editor) ---

// One key line: "NAME = v1 v2 v3 v4 [+caps|-caps]". values[i] nullptr or "" = empty
// (written as "none" between filled layers); dead[i] marks an accent key. Returns false
// when it does not fit in out.
bool formatKeyLine(uint8_t hid, const char* const values[kLayers], const bool dead[kLayers], int capsFlag,
                   char* out, size_t size);

// The file text with this key's line set to `line`: the last line for the key is
// replaced (it is the one that counts), otherwise the line is added at the end. Every
// other line, comments included, is kept as it was; CRLF files stay CRLF. Returns the
// new length, or 0 when it does not fit in out.
size_t withKeyLine(const char* text, size_t len, uint8_t hid, const char* line, char* out, size_t size);

// The file text with every line for this key removed (the key falls back to the base
// layout). Returns the new length (may be 0 for an empty result: check `ok`).
size_t withoutKey(const char* text, size_t len, uint8_t hid, char* out, size_t size, bool* ok);

// Copies the last line that sets this key (without its line ending) into line. False when
// the text has no line for the key.
bool keyLineIn(const char* text, size_t len, uint8_t hid, char* line, size_t size);

// What the editor stores for a typed entry: "ł" (4 to 6 hex digits) becomes that
// character, anything else is kept as typed. False when it is not valid.
bool entryValue(const char* typed, char* out, size_t size);

}  // namespace keymap
