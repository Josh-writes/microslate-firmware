// Host tests for the keymap engine (lib/Keymap) and the layout files in layouts/.
// Build and run (any g++ with C++17):  sh test/keymap/run-on-hinton.sh
#include "Keymap.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

#include "old_tables.inc"

using namespace keymap;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

static std::string readFile(const char* path) {
  FILE* f = fopen(path, "rb");
  if (!f) { printf("cannot open %s\n", path); exit(2); }
  std::string s;
  char buf[4096];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}

static bool load(Layout& l, const std::string& text, LoadReport& rep, GlyphCheck g = nullptr) {
  return l.loadText(text.data(), text.size(), rep, g);
}

static std::string typed(Typer& t, const Layout& l, uint8_t hid, uint8_t mods = 0, bool caps = false) {
  const char* s = t.press(l, hid, mods, caps);
  return s ? s : "<none>";
}

static bool same(const char* a, const char* b) {
  if (!a || !b) return a == b;
  return strcmp(a, b) == 0;
}

// Characters the built-in fonts can show (lib/EpdFont/builtinFonts/notosans_14_regular.h).
static bool fontHas(uint32_t c) {
  return (c >= 0x20 && c <= 0x7E) || (c >= 0xA0 && c <= 0x17F) || (c >= 0x218 && c <= 0x21B) || (c >= 0x300 && c <= 0x36F) ||
         (c >= 0x400 && c <= 0x4FF) || (c >= 0x2000 && c <= 0x2064) || (c >= 0x2066 && c <= 0x2071) ||
         (c >= 0x2074 && c <= 0x208E) || (c >= 0x2090 && c <= 0x209C) || (c >= 0x20A0 && c <= 0x20C0) ||
         c == 0x2212;
}
static bool fontShowsText(const char* s) {
  while (*s) {
    const unsigned char c = (unsigned char)*s;
    uint32_t cp = c;
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n > 1) cp = c & (0x7F >> n);
    for (int k = 1; k < n && s[k]; k++) cp = (cp << 6) | ((unsigned char)s[k] & 0x3F);
    if (!fontHas(cp)) return false;
    s += n;
  }
  return true;
}

// Every key the old table knew, with every modifier mix and Caps Lock: same text, except keys
// the file makes dead (the old tables typed accents as plain characters), characters the font
// cannot show (the generated files leave them out), and AltGr on keys where the old table had
// no AltGr character (it fell back to the plain key; the full layouts have one).
template <size_t N>
static void compareWithOld(const Layout& l, const old::LayoutKey (&keys)[N], const char* label) {
  const uint8_t mods[] = {0x00, 0x02, 0x20, 0x40, 0x42, 0x05, 0x11};
  int mismatches = 0;
  for (const auto& k : keys) {
    bool anyDead = false;
    for (int layer = 0; layer < kLayers; layer++) {
      bool d = false;
      l.text(k.hid, layer, &d);
      anyDead |= d;
    }
    if (anyDead) continue;
    for (uint8_t m : mods) {
      for (int caps = 0; caps < 2; caps++) {
        Typer t;
        const char* want = old::lookupKey(keys, k.hid, m, caps != 0);
        if (want && !fontShowsText(want)) continue;
        if (old::oldIsAltGr(m) && !k.altGr) continue;
        // Shift+AltGr: the old tables had no such layer and typed the AltGr character.
        if (old::oldIsAltGr(m) && (m & 0x22)) continue;
        // The ISO key by Left Shift on RU: the old table guessed "\ /", Linux says "/ |".
        if (k.hid == 0x64 && strcmp(label, "RU") == 0) continue;
        const char* got = t.press(l, k.hid, m, caps != 0);
        if (!same(want, got)) {
          if (mismatches++ < 5)
            printf("  %s hid=0x%02X mods=0x%02X caps=%d: want \"%s\" got \"%s\"\n", label, k.hid, m, caps,
                   want ? want : "null", got ? got : "null");
        }
      }
    }
  }
  CHECK(mismatches == 0);
}

static void testUs() {
  Layout us;
  CHECK(strcmp(us.shortName(), "US") == 0);
  CHECK(us.latin());
  compareWithOld(us, old::qwertyKeys, "US");
  Typer t;
  CHECK(typed(t, us, 0x04) == "a");
  CHECK(typed(t, us, 0x04, 0x02) == "A");
  CHECK(typed(t, us, 0x04, 0, true) == "A");          // Caps Lock
  CHECK(typed(t, us, 0x04, 0x02, true) == "a");       // Caps + Shift
  CHECK(typed(t, us, 0x1E, 0, true) == "1");          // Caps does not shift digits
  CHECK(typed(t, us, 0x28) == "\n");
  CHECK(typed(t, us, 0x2C) == " ");
  CHECK(typed(t, us, 0x59) == "1");                   // keypad 1
  CHECK(typed(t, us, 0x52) == "<none>");              // Up arrow types nothing
  // usText (ASCII fields) agrees with the built-in US layout
  for (int k = 0; k < 0x90; k++) {
    for (uint8_t m : {uint8_t(0), uint8_t(2)}) {
      if (k >= 0x54 && k <= 0x63) continue;           // usText has no keypad
      Typer t2;
      CHECK(same(usText(uint8_t(k), m, false), t2.press(us, uint8_t(k), m, false)));
    }
  }
  CHECK(shortcutLetter(us, 0x16) == 's');
  CHECK(shortcutLetter(us, 0x1E) == 0);
}

static void testGerman() {
  Layout de;
  LoadReport rep;
  CHECK(load(de, readFile("layouts/de.txt"), rep));
  CHECK(rep.problems == 0);
  for (int i = 0; i < rep.listed; i++) printf("  de.txt line %d: %s\n", rep.list[i].line, rep.list[i].text);
  CHECK(strcmp(de.shortName(), "DE") == 0);
  CHECK(de.latin());
  compareWithOld(de, old::germanKeys, "DE");
  Typer t;
  CHECK(typed(t, de, 0x1C) == "z");                   // US Y position types z
  CHECK(typed(t, de, 0x14, 0x40) == "@");             // AltGr+Q
  CHECK(typed(t, de, 0x2F, 0, true) == "Ü");          // Caps on ü
  CHECK(typed(t, de, 0x2D, 0, true) == "ß");          // ß has no capital here: Caps leaves it
  // Dead keys: ´ then e = é, ^ then a = â, ` then space = `, ´ then x = ´x
  CHECK(typed(t, de, 0x2E) == "");
  CHECK(typed(t, de, 0x08) == "é");
  CHECK(typed(t, de, 0x35) == "");
  CHECK(typed(t, de, 0x04) == "â");
  CHECK(typed(t, de, 0x2E, 0x02) == "");
  CHECK(typed(t, de, 0x2C) == "`");
  CHECK(typed(t, de, 0x2E) == "");
  CHECK(typed(t, de, 0x1B) == "´x");
  CHECK(typed(t, de, 0x2E) == "");                    // dead then dead: first accent typed
  CHECK(typed(t, de, 0x35) == "´");
  CHECK(t.pending());
  t.cancel();
  CHECK(!t.pending());
  // Ctrl shortcuts follow the letter on the key: the key labelled Z (US Y position) is 'z'
  CHECK(shortcutLetter(de, 0x1C) == 'z');
  CHECK(shortcutLetter(de, 0x1D) == 'y');
  CHECK(shortcutLetter(de, 0x33) == 0);               // ö: no shortcut letter
}

static void testRussian() {
  Layout ru;
  LoadReport rep;
  CHECK(load(ru, readFile("layouts/ru.txt"), rep));
  CHECK(rep.problems == 0);
  for (int i = 0; i < rep.listed; i++) printf("  ru.txt line %d: %s\n", rep.list[i].line, rep.list[i].text);
  CHECK(!ru.latin());
  compareWithOld(ru, old::russianKeys, "RU");
  Typer t;
  CHECK(typed(t, ru, 0x14) == "й");
  CHECK(typed(t, ru, 0x35, 0, true) == "Ё");
  CHECK(shortcutLetter(ru, 0x16) == 's');             // Ctrl+S still saves on Cyrillic
}

static void testFileFeatures() {
  Layout l;
  LoadReport rep;
  const char* text =
      "\xEF\xBB\xBF# test layout\r\n"
      "name = Test Intl\r\n"
      "altgr = any-alt\r\n"
      "APOSTROPHE = dead:' dead:\"\n"
      "GRAVE = dead:` dead:~\n"
      "6 = 6 dead:^\n"
      "compose ' = c ç C Ç\n"
      "S = s S ß \\u00A7\n"
      "0x87 = / ?\n"
      "KP_DOT = ,\n"
      "Q = q Q none -caps\n"
      "1 = \\s !\n";
  CHECK(load(l, text, rep));
  CHECK(rep.problems == 0);
  for (int i = 0; i < rep.listed; i++) printf("  line %d: %s\n", rep.list[i].line, rep.list[i].text);
  CHECK(strcmp(l.name(), "Test Intl") == 0);
  CHECK(strcmp(l.shortName(), "TES") == 0);           // made from the name when not given
  CHECK(rep.keys == 8 && rep.composes == 2);
  Typer t;
  CHECK(typed(t, l, 0x34) == "");
  CHECK(typed(t, l, 0x06) == "ç");                    // file's own compose pair
  CHECK(typed(t, l, 0x34) == "");
  CHECK(typed(t, l, 0x08, 0x02) == "É");              // built-in pair, upper case
  CHECK(typed(t, l, 0x34, 0x02) == "");
  CHECK(typed(t, l, 0x18) == "ü");                    // " then u
  CHECK(typed(t, l, 0x23, 0x02) == "");
  CHECK(typed(t, l, 0x08) == "ê");
  CHECK(typed(t, l, 0x35, 0x02) == "");
  CHECK(typed(t, l, 0x11) == "ñ");
  CHECK(typed(t, l, 0x16, 0x04) == "ß");              // left Alt is AltGr (any-alt)
  CHECK(typed(t, l, 0x16, 0x06) == "§");
  CHECK(typed(t, l, 0x87, 0x02) == "?");
  CHECK(typed(t, l, 0x63) == ",");
  CHECK(typed(t, l, 0x14, 0, true) == "q");           // -caps
  CHECK(typed(t, l, 0x1E) == " ");                    // \s
  CHECK(typed(t, l, 0x05) == "b");                    // base us still there

  // Cyrillic built-in pair: и + ˘ = й
  Layout c;
  CHECK(load(c, "base = us\nB = и И\nGRAVE = dead:\\u02D8\n", rep));
  Typer t2;
  CHECK(typed(t2, c, 0x35) == "");
  CHECK(typed(t2, c, 0x05) == "й");
}

static bool fontAsciiOnly(uint32_t cp) { return cp < 0x80; }

static void testProblems() {
  Layout l;
  LoadReport rep;
  const char* text =
      "name = Broken\n"
      "F = f F\n"                // 2 fine
      "NOSUCHKEY = a\n"          // 3 unknown key
      "A\n"                      // 3 no '='
      "B = 1 2 3 4 5\n"          // 4 too many values
      "C = \\q\n"                // 5 bad escape
      "D = \xFF\n"               // 6 bad UTF-8
      "base = none\n"            // 7 too late
      "altgr = left\n"           // 8 bad value
      "compose ´ = a\n"          // 9 not a pair
      "E = é É\n";               // 10 fine, but the font cannot show it
  CHECK(load(l, text, rep, fontAsciiOnly));
  CHECK(rep.problems == 8);
  CHECK(rep.unshown == 2);
  CHECK(rep.listed == kMaxProblems);
  CHECK(rep.list[0].line == 3);
  Typer t;
  CHECK(typed(t, l, 0x08) == "é");                    // kept despite the font note
  CHECK(typed(t, l, 0x04) == "a");                    // bad lines left US in place

  // The editor's new my-layout.txt: a name and base us, no keys yet. Usable: US with a name.
  CHECK(load(l, "# My keyboard layout.\nname = My layout\nshort = MY\nbase = us\n", rep));
  CHECK(strcmp(l.shortName(), "MY") == 0 && strcmp(l.name(), "My layout") == 0);
  CHECK(typed(t, l, 0x04) == "a");

  // Nothing usable: falls back to US
  CHECK(!load(l, "# empty\n\n", rep));
  CHECK(strcmp(l.shortName(), "US") == 0);

  // base = none really empties the layout
  CHECK(load(l, "base = none\nA = x\n", rep));
  CHECK(typed(t, l, 0x05) == "<none>");
  CHECK(typed(t, l, 0x04) == "x");

  // Pool overflow is reported, never overrun (run under ASan)
  std::string big = "base = none\n";
  for (int i = 0; i < 400; i++) big += "A = aaaaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbbbbbbbbbbb\n";
  CHECK(load(l, big, rep));
  CHECK(rep.problems > 0);

  // Random input never crashes
  srand(7);
  for (int n = 0; n < 5000; n++) {
    std::string s;
    int len = rand() % 400;
    for (int i = 0; i < len; i++) {
      int r = rand() % 10;
      s += r < 2 ? '\n' : r < 3 ? '=' : r < 4 ? ' ' : r < 5 ? '\\' : char(rand() % 256);
    }
    l.loadText(s.data(), s.size(), rep, fontAsciiOnly);
    Typer tt;
    for (int k = 0; k < 0x90; k++) tt.press(l, uint8_t(k), uint8_t(rand()), rand() & 1);
  }
}

// Sets one key slot the way the on-device editor does, and reloads the result.
static std::string editKey(const std::string& file, uint8_t hid, int layer, const char* value, bool dead) {
  Layout l;
  LoadReport rep;
  load(l, file, rep);
  const char* vals[kLayers];
  bool deads[kLayers];
  for (int i = 0; i < kLayers; i++) vals[i] = l.text(hid, i, &deads[i]);
  vals[layer] = value;
  deads[layer] = dead;
  char line[160];
  CHECK(formatKeyLine(hid, vals, deads, l.capsFlag(hid), line, sizeof(line)));
  static char out[16384];
  const size_t n = withKeyLine(file.data(), file.size(), hid, line, out, sizeof(out));
  CHECK(n > 0);
  return std::string(out, n);
}

static void testEditing() {
  // A small fixed sample (not a pack file, so the pack can change without breaking this).
  const std::string de =
      "# MicroSlate keyboard layout: German (test sample)\n"
      "name = Deutsch\n"
      "short = DE\n"
      "base = us\n"
      "\n"
      "Y = z Z\n"
      "Z = y Y\n"
      "EQUAL = dead:\xC2\xB4 dead:`\n"
      "SEMICOLON = \xC3\xB6 \xC3\x96\n";
  // Change an existing key: only that line changes, comments stay.
  std::string e = editKey(de, 0x1C, kNormal, "x", false);   // US Y position
  CHECK(e.find("Y = x Z\n") != std::string::npos);
  CHECK(e.find("# MicroSlate keyboard layout: German") == 0);
  CHECK(e.size() == de.size());                            // "z" -> "x", same length
  Layout l;
  LoadReport rep;
  CHECK(load(l, e, rep) && rep.problems == 0);
  Typer t;
  CHECK(typed(t, l, 0x1C) == "x");
  CHECK(typed(t, l, 0x1C, 0x02) == "Z");
  CHECK(typed(t, l, 0x1D) == "y");                         // other keys untouched

  // A key the file does not list is added at the end; base us keeps the rest.
  e = editKey(de, 0x04, kAltGr, "æ", false);
  CHECK(e.substr(e.size() - 11) == "A = a A æ\n");
  CHECK(load(l, e, rep) && rep.problems == 0);
  CHECK(typed(t, l, 0x04, 0x40) == "æ");

  // Make a slot an accent key, then clear it.
  e = editKey(de, 0x1E, kShift, "~", true);
  CHECK(load(l, e, rep) && rep.problems == 0);
  CHECK(typed(t, l, 0x1E, 0x02) == "");
  CHECK(typed(t, l, 0x11) == "ñ");
  e = editKey(e, 0x1E, kShift, nullptr, false);
  CHECK(e.find("1 = 1\n") != std::string::npos);
  CHECK(load(l, e, rep) && rep.problems == 0);
  CHECK(typed(t, l, 0x1E, 0x02) == "<none>");

  // Values that need escaping read back exactly.
  const char* tricky[] = {" ", "\\", "none", "NONE", "dead:x", "+caps", "-caps", "=", "#", "a b"};
  for (const char* v : tricky) {
    e = editKey("base = us\n", 0x05, kNormal, v, false);
    CHECK(load(l, e, rep) && rep.problems == 0);
    Typer t2;
    if (typed(t2, l, 0x05) != v) printf("  escape round trip failed for \"%s\": %s", v, e.c_str());
    CHECK(typed(t2, l, 0x05) == v);
  }
  // A middle layer left empty is written as none.
  e = editKey("base = us\n", 0x05, kAltGr, "þ", false);
  e = editKey(e, 0x05, kShift, nullptr, false);
  CHECK(e.find("B = b none þ\n") != std::string::npos);

  // CRLF files stay CRLF; the last of two lines for a key is the one replaced.
  e = editKey("base = us\r\nQ = 1\r\nQ = 2\r\n", 0x14, kNormal, "3", false);
  CHECK(e == "base = us\r\nQ = 1\r\nQ = 3\r\n");
  e = editKey("base = us\r\n# no newline at the end", 0x14, kNormal, "3", false);
  CHECK(e == "base = us\r\n# no newline at the end\r\nQ = 3 Q\r\n");   // US keeps its Shift letter
  // +caps / -caps survive an edit.
  e = editKey("Q = q Q -caps\n", 0x14, kAltGr, "@", false);
  CHECK(e == "Q = q Q @ -caps\n");

  // Default: put a key back as the original file had it, or drop its line(s).
  {
    const std::string orig = "base = us\r\nY = z Z\r\n";
    const std::string edited = "base = us\r\nY = x Z\r\nX = \xC5\x82 X\r\n";
    char line[64];
    CHECK(keyLineIn(orig.data(), orig.size(), 0x1C, line, sizeof line) && strcmp(line, "Y = z Z") == 0);
    CHECK(!keyLineIn(orig.data(), orig.size(), 0x1B, line, sizeof line));
    char out[128];
    size_t n = withKeyLine(edited.data(), edited.size(), 0x1C, "Y = z Z", out, sizeof out);
    CHECK(std::string(out, n) == "base = us\r\nY = z Z\r\nX = \xC5\x82 X\r\n");
    bool ok = false;
    n = withoutKey(edited.data(), edited.size(), 0x1B, out, sizeof out, &ok);
    CHECK(ok && std::string(out, n) == "base = us\r\nY = x Z\r\n");
    Layout r;
    LoadReport rr;
    CHECK(load(r, std::string(out, n), rr));
    Typer tr;
    CHECK(typed(tr, r, 0x1B) == "x");                     // back to US
    n = withoutKey("Q = 1\nQ = 2", 11, 0x14, out, sizeof out, &ok);
    CHECK(ok && n == 0);                                  // every line for the key goes
    n = withoutKey("A = b\n", 6, 0x04, out, 1, &ok);
    CHECK(ok && n == 0);                                  // fits: empty result
    n = withoutKey("B = b\n", 6, 0x04, out, 3, &ok);
    CHECK(!ok);                                           // does not fit
  }

  // Too small an output buffer fails cleanly.
  char small[8];
  CHECK(withKeyLine("base = us\n", 10, 0x14, "Q = 3", small, sizeof(small)) == 0);

  // Entry values
  char v[16];
  CHECK(entryValue("\\u0142", v, sizeof v) && strcmp(v, "ł") == 0);
  CHECK(entryValue("\\u{1F600}", v, sizeof v) && strcmp(v, "\xF0\x9F\x98\x80") == 0);
  CHECK(entryValue("\\u1F600", v, sizeof v) && strcmp(v, "\\u1F600") == 0);   // 5 digits need braces
  // In a file, \uXXXX is exactly four digits, so letters after it stay letters.
  Layout lu;
  LoadReport ru;
  CHECK(load(lu, "B = \\u00E9a\n", ru) && ru.problems == 0);
  Typer tu;
  CHECK(typed(tu, lu, 0x05) == "éa");
  CHECK(entryValue("\\u12", v, sizeof v) && strcmp(v, "\\u12") == 0);   // too short: as typed
  CHECK(entryValue("ab", v, sizeof v) && strcmp(v, "ab") == 0);
  CHECK(keyByName("y") == 0x1C && keyByName("MINUS") == 0x2D && keyByName("nope") == -1);
}

// The whole layout pack: every file loads cleanly and types only what the font can show.
static void testPack() {
  int files = 0;
  for (const auto& e : std::filesystem::directory_iterator("layouts")) {
    if (e.path().extension() != ".txt") continue;
    files++;
    Layout l;
    LoadReport rep;
    const bool ok = load(l, readFile(e.path().string().c_str()), rep, fontHas);
    if (!ok || rep.problems || rep.unshown) {
      printf("  %s: ok=%d problems=%d unshown=%d\n", e.path().string().c_str(), ok, rep.problems, rep.unshown);
      for (int i = 0; i < rep.listed; i++) printf("    line %d: %s\n", rep.list[i].line, rep.list[i].text);
    }
    CHECK(ok && rep.problems == 0 && rep.unshown == 0);
    CHECK(rep.keys > 20);
  }
  CHECK(files >= 30);
  // A few known keys from the real keyboards
  Layout fr, pl, br;
  LoadReport rep;
  CHECK(load(fr, readFile("layouts/fr.txt"), rep) && load(pl, readFile("layouts/pl.txt"), rep) &&
        load(br, readFile("layouts/br.txt"), rep));
  Typer t;
  CHECK(typed(t, fr, 0x14) == "a");                    // AZERTY: US Q position is a
  CHECK(typed(t, fr, 0x1E, 0x02) == "1");              // digits need Shift
  CHECK(typed(t, fr, 0x2F) == "");                     // ^ is an accent key
  CHECK(typed(t, fr, 0x08) == "ê");
  CHECK(shortcutLetter(fr, 0x33) == 'm');              // Ctrl+M is on the US semicolon key
  CHECK(typed(t, pl, 0x04, 0x40) == "ą");              // Polish programmers: AltGr+A
  CHECK(typed(t, br, 0x87) == "/");                    // ABNT2's extra key next to right Shift
  Layout ro;
  CHECK(load(ro, readFile("layouts/ro.txt"), rep));
  CHECK(typed(t, ro, 0x33) == "ș");                    // Romanian standard: comma below, not cedilla
  CHECK(typed(t, ro, 0x34, 0x02) == "Ț");
  CHECK(typed(t, ro, 0x2F, 0, true) == "Ă");           // Caps Lock capitalises ă
  CHECK(typed(t, ro, 0x35, 0x02) == "”");
  CHECK(shortcutLetter(ro, 0x33) == 0);               // ș: no shortcut letter
}

static void testNames() {
  char b[16];
  CHECK(strcmp(keyName(0x04, b, sizeof b), "A") == 0);
  CHECK(strcmp(keyName(0x27, b, sizeof b), "0") == 0);
  CHECK(strcmp(keyName(0x2D, b, sizeof b), "MINUS") == 0);
  CHECK(strcmp(keyName(0x64, b, sizeof b), "ISO_BACKSLASH") == 0);
  CHECK(isLayoutKey(0x04) && isLayoutKey(0x87) && !isLayoutKey(0x52) && !isLayoutKey(0x28));
}

static void testChanges() {
  // The editor's changes file sits on top of a layout; the layout file is never touched.
  const std::string de =
      "name = Deutsch\n"
      "short = DE\n"
      "Y = z Z\n"
      "Z = y Y\n";
  Layout l;
  LoadReport rep;
  CHECK(load(l, de, rep) && l.changedCount() == 0);
  const std::string changes =
      "# Keys changed on the MicroSlate\n"
      "Y = x X\n"
      "B = none\n";
  CHECK(l.applyChanges(changes.data(), changes.size(), rep) && rep.keys == 2 && rep.problems == 0);
  Typer t;
  CHECK(typed(t, l, 0x1C) == "x");           // the change wins over the layout
  CHECK(typed(t, l, 0x1D) == "y");           // keys not in the changes file keep the layout's value
  CHECK(l.text(0x05, kNormal) == nullptr);   // emptied
  CHECK(l.changed(0x1C) && l.changed(0x05) && !l.changed(0x1D) && l.changedCount() == 2);
  CHECK(strcmp(l.shortName(), "DE") == 0);   // the name stays the layout's
  // Header and compose lines do not belong in a changes file: reported, not applied.
  const std::string bad = "name = Mine\nbase = none\ncompose ` = a b\nQ = q Q\n";
  CHECK(l.applyChanges(bad.data(), bad.size(), rep) && rep.problems == 3 && rep.keys == 1);
  CHECK(strcmp(l.shortName(), "DE") == 0 && typed(t, l, 0x1C) == "x");
  // On US: an empty changes file changes nothing; loading a layout again clears the marks.
  Layout us;
  CHECK(!us.applyChanges("", 0, rep) && us.changedCount() == 0);
  const std::string x = "X = \\u0142 \\u0141\n";
  CHECK(us.applyChanges(x.data(), x.size(), rep) && us.changed(0x1B));
  CHECK(typed(t, us, 0x1B) == "\xC5\x82");
  us.loadUs();
  CHECK(us.changedCount() == 0 && typed(t, us, 0x1B) == "x");
  // Removing the key's line is what Reset does: the key goes back to the layout.
  char out[256];
  bool ok = false;
  const size_t n = withoutKey(changes.data(), changes.size(), 0x1C, out, sizeof(out), &ok);
  CHECK(ok && std::string(out, n) == "# Keys changed on the MicroSlate\nB = none\n");
  CHECK(load(l, de, rep) && l.applyChanges(out, n, rep) && !l.changed(0x1C) && typed(t, l, 0x1C) == "z");
}

int main() {
  testUs();
  testGerman();
  testRussian();
  testFileFeatures();
  testProblems();
  testEditing();
  testChanges();
  testPack();
  testNames();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
