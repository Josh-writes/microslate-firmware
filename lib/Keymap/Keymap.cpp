#include "Keymap.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace keymap {

namespace {

struct ComposeEntry {
  uint16_t base;
  uint8_t mark;
  uint16_t result;
};
#include "KeymapCompose.inc"

constexpr uint8_t kEnter = 0x28, kTab = 0x2B, kSpace = 0x2C;
constexpr uint8_t kModShift = 0x22, kModCtrl = 0x11, kModLeftAlt = 0x04, kModRightAlt = 0x40;

struct KeyNameEntry {
  uint8_t hid;
  const char* name;
};

// Names used in layout files: the key's place on a US keyboard. First name wins in reports.
const KeyNameEntry kNames[] = {
    {0x2D, "MINUS"}, {0x2E, "EQUAL"}, {0x2F, "LBRACKET"}, {0x30, "RBRACKET"}, {0x31, "BACKSLASH"},
    {0x32, "HASH"}, {0x33, "SEMICOLON"}, {0x34, "APOSTROPHE"}, {0x34, "QUOTE"}, {0x35, "GRAVE"},
    {0x36, "COMMA"}, {0x37, "DOT"}, {0x37, "PERIOD"}, {0x38, "SLASH"},
    {0x54, "KP_SLASH"}, {0x55, "KP_STAR"}, {0x56, "KP_MINUS"}, {0x57, "KP_PLUS"},
    {0x59, "KP_1"}, {0x5A, "KP_2"}, {0x5B, "KP_3"}, {0x5C, "KP_4"}, {0x5D, "KP_5"},
    {0x5E, "KP_6"}, {0x5F, "KP_7"}, {0x60, "KP_8"}, {0x61, "KP_9"}, {0x62, "KP_0"},
    {0x63, "KP_DOT"}, {0x64, "ISO_BACKSLASH"}, {0x67, "KP_EQUAL"}, {0x85, "KP_COMMA"},
    {0x87, "INTL1"}, {0x88, "INTL2"}, {0x89, "INTL3"},
};

// US QWERTY, keys 0x04-0x38: normal and shifted character ('\0' = none).
const char kUsNormal[] = "abcdefghijklmnopqrstuvwxyz1234567890\0\0\0\0 -=[]\\\0;'`,./";
const char kUsShift[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ!@#$%^&*()\0\0\0\0 _+{}|\0:\"~<>?";
const char kUsKeypad[] = "/*-+\n1234567890.";  // 0x54-0x63

const char* kUsName = "US QWERTY";

// Copies src into a fixed buffer, cutting a long text at a character boundary.
void copyText(char* dst, size_t size, const char* src) {
  size_t n = strlen(src);
  if (n >= size) {
    n = size - 1;
    while (n > 0 && (uint8_t(src[n]) & 0xC0) == 0x80) n--;  // do not split a UTF-8 sequence
  }
  memcpy(dst, src, n);
  dst[n] = '\0';
}

bool isLatinAsciiLetter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

// Decodes one UTF-8 code point; returns its length (0 if malformed).
int utf8Decode(const char* s, uint32_t& cp) {
  const uint8_t c = uint8_t(s[0]);
  if (c < 0x80) { cp = c; return c ? 1 : 0; }
  int n = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
  if (!n) return 0;
  cp = c & (0x7F >> n);
  for (int i = 1; i < n; i++) {
    const uint8_t d = uint8_t(s[i]);
    if ((d & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (d & 0x3F);
  }
  return n;
}

int utf8Encode(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = char(cp); return 1; }
  if (cp < 0x800) { out[0] = char(0xC0 | (cp >> 6)); out[1] = char(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = char(0xE0 | (cp >> 12)); out[1] = char(0x80 | ((cp >> 6) & 0x3F)); out[2] = char(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = char(0xF0 | (cp >> 18)); out[1] = char(0x80 | ((cp >> 12) & 0x3F));
  out[2] = char(0x80 | ((cp >> 6) & 0x3F)); out[3] = char(0x80 | (cp & 0x3F));
  return 4;
}

// The single code point of s, or 0 when s is empty or longer than one character.
uint32_t singleCp(const char* s) {
  uint32_t cp = 0;
  int n = utf8Decode(s, cp);
  return (n && s[n] == '\0') ? cp : 0;
}

// Upper case of a letter in the ranges the fonts cover, or 0.
uint32_t upperOf(uint32_t c) {
  if (c >= 'a' && c <= 'z') return c - 0x20;
  if (c >= 0xE0 && c <= 0xFE && c != 0xF7) return c - 0x20;
  if (c == 0xFF) return 0x178;
  if ((c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177)) return (c & 1) ? c - 1 : 0;
  if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) return (c & 1) ? 0 : c - 1;
  if (c >= 0x430 && c <= 0x44F) return c - 0x20;
  if (c >= 0x450 && c <= 0x45F) return c - 0x50;
  if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x4FF))
    return (c & 1) ? c - 1 : 0;
  if (c >= 0x4C1 && c <= 0x4CE) return (c & 1) ? 0 : c - 1;
  return 0;
}

// Combining mark of a spacing accent as typed on a dead key, or 0.
uint32_t markOfAccent(uint32_t a) {
  switch (a) {
    case '`': return 0x300;
    case 0xB4: case '\'': return 0x301;
    case '^': case 0x2C6: return 0x302;
    case '~': case 0x2DC: return 0x303;
    case 0xAF: return 0x304;
    case 0x2D8: return 0x306;
    case 0x2D9: return 0x307;
    case 0xA8: case '"': return 0x308;
    case 0xB0: case 0x2DA: return 0x30A;
    case 0x2DD: return 0x30B;
    case 0x2C7: return 0x30C;
    case 0xB8: return 0x327;
    case 0x2DB: return 0x328;
    default: return 0;
  }
}

void bitSet(uint8_t* bits, int i, bool on) {
  if (on) bits[i / 8] |= uint8_t(1u << (i % 8));
  else bits[i / 8] &= uint8_t(~(1u << (i % 8)));
}
bool bitGet(const uint8_t* bits, int i) { return bits[i / 8] & (1u << (i % 8)); }

int keyFromName(const char* n) {
  if ((n[0] == '0') && (n[1] == 'x' || n[1] == 'X') && n[2]) {
    unsigned v = 0;
    for (const char* p = n + 2; *p; p++) {
      char c = *p;
      int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
      if (d < 0 || v > 0xFF) return -1;
      v = v * 16 + unsigned(d);
    }
    return (v < unsigned(kKeys) && isLayoutKey(uint8_t(v))) ? int(v) : -1;
  }
  if (n[1] == '\0') {
    char c = n[0];
    if (c >= 'a' && c <= 'z') c = char(c - 32);
    if (c >= 'A' && c <= 'Z') return 0x04 + (c - 'A');
    if (c >= '1' && c <= '9') return 0x1E + (c - '1');
    if (c == '0') return 0x27;
    return -1;
  }
  for (const auto& e : kNames) {
    const char* a = e.name;
    const char* b = n;
    while (*a && *b && (*a == ((*b >= 'a' && *b <= 'z') ? char(*b - 32) : *b))) { a++; b++; }
    if (!*a && !*b) return e.hid;
  }
  return -1;
}

bool equalsNoCase(const char* a, const char* b) {
  for (; *a && *b; a++, b++) {
    char x = (*a >= 'A' && *a <= 'Z') ? char(*a + 32) : *a;
    char y = (*b >= 'A' && *b <= 'Z') ? char(*b + 32) : *b;
    if (x != y) return false;
  }
  return *a == *b;
}

// Decodes one value token in place: \s, \\, \uXXXX. Returns false on a bad escape or bad UTF-8.
bool decodeToken(const char* in, char* out, size_t size) {
  size_t o = 0;
  for (const char* p = in; *p;) {
    char tmp[4];
    int n = 0;
    if (*p == '\\') {
      if (p[1] == 's') { tmp[0] = ' '; n = 1; p += 2; }
      else if (p[1] == '\\') { tmp[0] = '\\'; n = 1; p += 2; }
      else if (p[1] == '\0') { tmp[0] = '\\'; n = 1; p += 1; }  // a lone \ at the end is itself
      else if (p[1] == 'u') {
        // \uXXXX is exactly four hex digits, so "ét" is "ét"; longer codes go in
        // braces: \u{1F600}.
        uint32_t cp = 0;
        int digits = 0;
        p += 2;
        const bool braces = *p == '{';
        if (braces) p++;
        while (digits < (braces ? 6 : 4)) {
          char c = *p;
          int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
          if (d < 0) break;
          cp = cp * 16 + uint32_t(d);
          digits++;
          p++;
        }
        if (braces) {
          if (*p != '}' || digits == 0) return false;
          p++;
        } else if (digits < 4) {
          return false;
        }
        if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        n = utf8Encode(cp, tmp);
      } else {
        return false;
      }
    } else {
      uint32_t cp;
      n = utf8Decode(p, cp);
      if (!n) return false;
      memcpy(tmp, p, size_t(n));
      p += n;
    }
    if (o + size_t(n) >= size) return false;
    memcpy(out + o, tmp, size_t(n));
    o += size_t(n);
  }
  out[o] = '\0';
  return o > 0;
}

}  // namespace

void LoadReport::add(int line, const char* fmt, ...) {
  problems++;
  if (listed >= kMaxProblems) return;
  Problem& p = list[listed++];
  p.line = line;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(p.text, sizeof(p.text), fmt, ap);
  va_end(ap);
}

// --- Layout ---

void Layout::clearKeys() {
  memset(keys_, 0, sizeof(keys_));
  memset(caps_, 0, sizeof(caps_));
  memset(noCaps_, 0, sizeof(noCaps_));
  memset(changed_, 0, sizeof(changed_));
  used_ = 1;  // offset 0 means "none"
  pool_[0] = '\0';
  composeCount_ = 0;
}

uint16_t Layout::store(const char* s, bool dead) {
  const size_t len = strlen(s);
  const size_t need = len + 1 + (dead ? 1 : 0);
  if (used_ + need > kPoolSize) return 0;
  const uint16_t at = uint16_t(used_);
  if (dead) pool_[used_++] = '\x01';
  memcpy(pool_ + used_, s, len + 1);
  used_ += len + 1;
  return at;
}

bool Layout::setKey(uint8_t hid, int layer, const char* value, bool dead) {
  if (!value) { keys_[hid][layer] = 0; return true; }
  const uint16_t at = store(value, dead);
  if (!at) return false;
  keys_[hid][layer] = at;
  return true;
}

void Layout::loadUs() {
  clearKeys();
  snprintf(name_, sizeof(name_), "%s", kUsName);
  snprintf(short_, sizeof(short_), "US");
  anyAlt_ = false;
  latin_ = -1;
  char s[2] = {0, 0};
  for (int k = 0x04; k <= 0x38; k++) {
    if (k == kSpace) continue;
    if ((s[0] = kUsNormal[k - 0x04])) setKey(uint8_t(k), kNormal, s, false);
    if ((s[0] = kUsShift[k - 0x04])) setKey(uint8_t(k), kShift, s, false);
  }
  setKey(0x64, kNormal, "\\", false);
  setKey(0x64, kShift, "|", false);
  for (int k = 0x54; k <= 0x63; k++) {
    s[0] = kUsKeypad[k - 0x54];
    if (k != 0x58) setKey(uint8_t(k), kNormal, s, false);  // keypad Enter is Enter
  }
}

const char* Layout::text(uint8_t hid, int layer, bool* dead) const {
  if (dead) *dead = false;
  if (hid >= kKeys || layer < 0 || layer >= kLayers) return nullptr;
  const uint16_t at = keys_[hid][layer];
  if (!at) return nullptr;
  const char* p = pool_ + at;
  if (*p == '\x01') {
    if (dead) *dead = true;
    p++;
  }
  return p;
}

bool Layout::capsActsAsShift(uint8_t hid) const {
  if (hid >= kKeys) return false;
  if (bitGet(caps_, hid)) return true;
  if (bitGet(noCaps_, hid)) return false;
  const char* n = text(hid, kNormal);
  const char* s = text(hid, kShift);
  if (!n || !s) return false;
  const uint32_t a = singleCp(n), b = singleCp(s);
  return a && b && upperOf(a) == b;
}

int Layout::capsFlag(uint8_t hid) const {
  if (hid >= kKeys) return 0;
  if (bitGet(caps_, hid)) return 1;
  if (bitGet(noCaps_, hid)) return -1;
  return 0;
}

bool Layout::latin() const {
  if (latin_ >= 0) return latin_ == 1;
  int letters = 0;
  for (int k = 0x04; k <= 0x1D; k++) {
    const char* t = text(uint8_t(k), kNormal);
    if (t && t[0] && !t[1] && isLatinAsciiLetter(t[0])) letters++;
  }
  return letters >= 13;
}

const char* Layout::composeOwn(const char* accent, const char* base) const {
  for (int i = 0; i < composeCount_; i++) {
    if (strcmp(pool_ + compose_[i].accent, accent) == 0 && strcmp(pool_ + compose_[i].base, base) == 0)
      return pool_ + compose_[i].result;
  }
  return nullptr;
}

bool Layout::loadText(const char* text, size_t len, LoadReport& rep, GlyphCheck canShow) {
  loadUs();
  rep = LoadReport();
  name_[0] = '\0';
  short_[0] = '\0';
  bool sawHeader = false;   // a file that only names itself (base us, no keys yet) is usable
  parse(text, len, rep, canShow, false, &sawHeader);
  if (!name_[0]) snprintf(name_, sizeof(name_), "%s", short_[0] ? short_ : "Custom");
  if (!short_[0]) {
    int i = 0;
    for (const char* c = name_; *c && i < 3; c++)
      if (isLatinAsciiLetter(*c)) short_[i++] = (*c >= 'a') ? char(*c - 32) : *c;
    short_[i] = '\0';
  }
  if (rep.keys == 0 && rep.composes == 0 && !sawHeader) {
    loadUs();
    return false;
  }
  return true;
}

bool Layout::applyChanges(const char* text, size_t len, LoadReport& rep, GlyphCheck canShow) {
  rep = LoadReport();
  bool sawHeader = false;
  parse(text, len, rep, canShow, true, &sawHeader);
  return rep.keys > 0;
}

bool Layout::changed(uint8_t hid) const { return hid < kKeys && bitGet(changed_, hid); }

int Layout::changedCount() const {
  int n = 0;
  for (int k = 0; k < kKeys; k++) n += bitGet(changed_, k) ? 1 : 0;
  return n;
}

// One pass over a layout file (changes = false) or a changes file (changes = true: key
// lines only, each marks its key as changed).
void Layout::parse(const char* text, size_t len, LoadReport& rep, GlyphCheck canShow, bool changes,
                   bool* sawHeader) {
  bool sawKey = false;
  if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) { text += 3; len -= 3; }

  char line[256];
  int lineNo = 0;
  size_t pos = 0;
  while (pos < len) {
    size_t end = pos;
    while (end < len && text[end] != '\n') end++;
    lineNo++;
    size_t n = end - pos;
    const bool tooLong = n >= sizeof(line);
    if (!tooLong) {
      memcpy(line, text + pos, n);
      line[n] = '\0';
    }
    pos = end + 1;
    if (tooLong) { rep.add(lineNo, "line too long"); continue; }
    while (n && (line[n - 1] == '\r' || line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = '\0';
    char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') continue;

    char* eq = strchr(p, '=');
    if (!eq) { rep.add(lineNo, "no '=' in line"); continue; }
    // Left side: everything before the first '=' (a compose accent cannot be '=').
    *eq = '\0';
    char* left = p;
    char* right = eq + 1;
    for (char* e = eq - 1; e >= left && (*e == ' ' || *e == '\t'); e--) *e = '\0';
    while (*right == ' ' || *right == '\t') right++;

    // Header values keep their spaces; take them before the right side is split.
    char whole[sizeof(line)];
    snprintf(whole, sizeof(whole), "%s", right);

    // Split the right side into tokens.
    char* tok[12];
    int nt = 0;
    for (char* q = right; *q && nt < 12;) {
      while (*q == ' ' || *q == '\t') q++;
      if (!*q) break;
      tok[nt++] = q;
      while (*q && *q != ' ' && *q != '\t') q++;
      if (*q) *q++ = '\0';
    }

    if (changes && (strncmp(left, "compose", 7) == 0 || equalsNoCase(left, "name") || equalsNoCase(left, "short") ||
                    equalsNoCase(left, "base") || equalsNoCase(left, "altgr") || equalsNoCase(left, "latin"))) {
      rep.add(lineNo, "a changes file holds only key lines");
      continue;
    }
    if (equalsNoCase(left, "name")) {
      copyText(name_, sizeof(name_), whole);
      *sawHeader = true;
      continue;
    }
    if (equalsNoCase(left, "short")) {
      copyText(short_, sizeof(short_), nt ? tok[0] : "");
      *sawHeader = true;
      continue;
    }
    if (equalsNoCase(left, "base")) {
      if (sawKey || composeCount_) { rep.add(lineNo, "base must come before keys and compose lines"); continue; }
      if (nt == 1 && equalsNoCase(tok[0], "none")) {
        clearKeys();
      } else if (!(nt == 1 && equalsNoCase(tok[0], "us"))) {
        rep.add(lineNo, "base must be us or none");
      }
      continue;
    }
    if (equalsNoCase(left, "altgr")) {
      if (nt == 1 && equalsNoCase(tok[0], "any-alt")) anyAlt_ = true;
      else if (nt == 1 && equalsNoCase(tok[0], "right-alt")) anyAlt_ = false;
      else rep.add(lineNo, "altgr must be right-alt or any-alt");
      continue;
    }
    if (equalsNoCase(left, "latin")) {
      if (nt == 1 && equalsNoCase(tok[0], "yes")) latin_ = 1;
      else if (nt == 1 && equalsNoCase(tok[0], "no")) latin_ = 0;
      else rep.add(lineNo, "latin must be yes or no");
      continue;
    }

    // compose <accent> = base result [base result ...]
    if (strncmp(left, "compose", 7) == 0 && (left[7] == ' ' || left[7] == '\t')) {
      char* a = left + 8;
      while (*a == ' ' || *a == '\t') a++;
      char accent[16];
      if (!decodeToken(a, accent, sizeof(accent))) { rep.add(lineNo, "bad compose accent"); continue; }
      if (nt == 0 || nt % 2) { rep.add(lineNo, "compose needs pairs: base result"); continue; }
      for (int i = 0; i + 1 < nt; i += 2) {
        char base[16], result[16];
        if (!decodeToken(tok[i], base, sizeof(base)) || !decodeToken(tok[i + 1], result, sizeof(result))) {
          rep.add(lineNo, "bad compose pair %s %s", tok[i], tok[i + 1]);
          continue;
        }
        if (composeCount_ >= kMaxCompose) { rep.add(lineNo, "too many compose pairs"); break; }
        Pair c{store(accent, false), store(base, false), store(result, false)};
        if (!c.accent || !c.base || !c.result) { rep.add(lineNo, "layout too large"); break; }
        compose_[composeCount_++] = c;
        rep.composes++;
      }
      continue;
    }

    // KEY = normal [shift [altgr [shift+altgr]]] [+caps|-caps]
    const int hid = keyFromName(left);
    if (hid < 0) { rep.add(lineNo, "unknown key %s", left); continue; }
    int caps = 0;
    if (nt && (strcmp(tok[nt - 1], "+caps") == 0 || strcmp(tok[nt - 1], "-caps") == 0)) {
      caps = tok[nt - 1][0] == '+' ? 1 : -1;
      nt--;
    }
    if (nt == 0 || nt > kLayers) { rep.add(lineNo, "%s needs 1 to 4 values", left); continue; }
    char values[kLayers][24];
    bool dead[kLayers] = {};
    bool ok = true;
    for (int i = 0; i < nt && ok; i++) {
      const char* t = tok[i];
      if (equalsNoCase(t, "none")) { values[i][0] = '\0'; continue; }
      if (strncmp(t, "dead:", 5) == 0) { dead[i] = true; t += 5; }
      if (!decodeToken(t, values[i], sizeof(values[i]))) { rep.add(lineNo, "bad value %s", tok[i]); ok = false; }
    }
    if (!ok) continue;
    sawKey = true;
    for (int i = 0; i < kLayers; i++) {
      const char* v = (i < nt && values[i][0]) ? values[i] : nullptr;
      if (!setKey(uint8_t(hid), i, v, dead[i])) { rep.add(lineNo, "layout too large"); ok = false; break; }
      // An accent key's mark (ˇ ˘ ˛) need not have a glyph: what it combines into does.
      if (v && canShow && !dead[i]) {
        const char* q = v;
        uint32_t cp;
        int m;
        while (*q && (m = utf8Decode(q, cp)) > 0) {
          if (cp >= 0x20 && !canShow(cp)) {
            rep.unshown++;
            rep.add(lineNo, "%s: U+%04X cannot be shown by the font", left, unsigned(cp));
            rep.problems--;  // a note, not a skipped line
          }
          q += m;
        }
      }
    }
    if (!ok) continue;
    bitSet(caps_, hid, caps == 1);
    bitSet(noCaps_, hid, caps == -1);
    if (changes) bitSet(changed_, hid, true);
    rep.keys++;
  }
}

// --- Typer ---

const char* Typer::press(const Layout& l, uint8_t hid, uint8_t mods, bool capsLock) {
  const char* t = nullptr;
  bool dead = false;
  if (hid == kEnter || hid == 0x58) t = "\n";
  else if (hid == kTab) t = "\t";
  else if (hid == kSpace) t = " ";
  else {
    const bool shift = mods & kModShift;
    const bool altGr = (mods & kModRightAlt) || ((mods & kModCtrl) && (mods & (kModLeftAlt | kModRightAlt))) ||
                       (l.anyAltIsAltGr() && (mods & kModLeftAlt));
    if (altGr) {
      bool d1 = false, d2 = false;
      const char* a = l.text(hid, kAltGr, &d1);
      const char* sa = shift ? l.text(hid, kShiftAltGr, &d2) : nullptr;
      if (sa) { t = sa; dead = d2; }
      else if (a) { t = a; dead = d1; }
    }
    if (!t) {
      const bool useShift = l.capsActsAsShift(hid) ? (shift != capsLock) : shift;
      t = l.text(hid, useShift ? kShift : kNormal, &dead);
    }
  }
  if (!t) return nullptr;

  if (pending_[0]) {
    char accent[sizeof(pending_)];
    memcpy(accent, pending_, sizeof(accent));
    pending_[0] = '\0';
    const char* own = l.composeOwn(accent, t);
    if (own) { snprintf(out_, sizeof(out_), "%s", own); return out_; }
    // Built-in pairs: base letter + the accent's combining mark.
    const uint32_t mark = markOfAccent(singleCp(accent));
    const uint32_t base = singleCp(t);
    if (mark && base && !dead) {
      for (const auto& e : kComposeTable) {
        if (e.base == base && kComposeMarks[e.mark] == mark) {
          out_[utf8Encode(e.result, out_)] = '\0';
          return out_;
        }
      }
    }
    if (hid == kSpace) { snprintf(out_, sizeof(out_), "%s", accent); return out_; }
    if (dead) {
      // Two dead keys: the first accent is typed, the second one waits.
      snprintf(pending_, sizeof(pending_), "%s", t);
      snprintf(out_, sizeof(out_), "%s", accent);
      return out_;
    }
    snprintf(out_, sizeof(out_), "%s%s", accent, t);
    return out_;
  }
  if (dead) {
    snprintf(pending_, sizeof(pending_), "%s", t);
    return "";
  }
  return t;
}

// --- Helpers ---

const char* usText(uint8_t hid, uint8_t mods, bool capsLock) {
  static char out[2];
  if (hid == kEnter) return "\n";
  if (hid == kTab) return "\t";
  if (hid == kSpace) return " ";
  const bool shift = mods & kModShift;
  char c = 0;
  if (hid >= 0x04 && hid <= 0x38) {
    const bool letter = hid <= 0x1D;
    const bool up = letter ? (shift != capsLock) : shift;
    c = up ? kUsShift[hid - 0x04] : kUsNormal[hid - 0x04];
  } else if (hid == 0x64) {
    c = shift ? '|' : '\\';
  }
  if (!c) return nullptr;
  out[0] = c;
  out[1] = '\0';
  return out;
}

char shortcutLetter(const Layout& l, uint8_t hid) {
  if (l.latin()) {
    const char* t = l.text(hid, kNormal);
    if (t && t[0] && !t[1] && isLatinAsciiLetter(t[0])) return char(t[0] | 0x20);
    return 0;
  }
  if (hid >= 0x04 && hid <= 0x1D) return char('a' + (hid - 0x04));
  return 0;
}

bool isLayoutKey(uint8_t hid) {
  if (hid >= 0x04 && hid <= 0x27) return true;
  for (const auto& e : kNames) if (e.hid == hid) return true;
  return false;
}

int keyByName(const char* name) { return keyFromName(name); }

namespace {

// Appends one value token, escaped so the loader reads back exactly `v`.
bool appendToken(const char* v, bool dead, char*& o, const char* end) {
  auto put = [&](const char* s, size_t n) {
    if (o + n >= end) return false;
    memcpy(o, s, n);
    o += n;
    return true;
  };
  if (!put(" ", 1)) return false;
  if (!v || !v[0]) return put("none", 4);
  if (dead && !put("dead:", 5)) return false;
  // Words the loader would read as keywords get their first letter as a code.
  const bool keyword = !dead && (equalsNoCase(v, "none") || strncmp(v, "dead:", 5) == 0 ||
                                 strcmp(v, "+caps") == 0 || strcmp(v, "-caps") == 0);
  for (const char* p = v; *p; p++) {
    if (*p == ' ') { if (!put("\\s", 2)) return false; }
    else if (*p == '\\') { if (!put("\\\\", 2)) return false; }
    else if (p == v && keyword) {
      char u[8];
      snprintf(u, sizeof(u), "\\u%04X", unsigned(uint8_t(*p)));
      if (!put(u, strlen(u))) return false;
    } else if (!put(p, 1)) return false;
  }
  return true;
}

// Key named by a file line's left side, or -1 for comments, headers and compose lines.
int lineKey(const char* s, size_t n) {
  size_t i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  if (i >= n || s[i] == '#') return -1;
  size_t eq = i;
  while (eq < n && s[eq] != '=') eq++;
  if (eq >= n) return -1;
  size_t e = eq;
  while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
  char name[24];
  if (e - i == 0 || e - i >= sizeof(name)) return -1;
  memcpy(name, s + i, e - i);
  name[e - i] = '\0';
  return keyFromName(name);
}

}  // namespace

bool formatKeyLine(uint8_t hid, const char* const values[kLayers], const bool dead[kLayers], int capsFlag,
                   char* out, size_t size) {
  if (size < 8) return false;
  char* o = out;
  const char* end = out + size;
  char name[16];
  keyName(hid, name, sizeof(name));
  const size_t nl = strlen(name);
  if (nl + 3 >= size) return false;
  memcpy(o, name, nl);
  o += nl;
  memcpy(o, " =", 2);
  o += 2;
  int last = -1;
  for (int i = 0; i < kLayers; i++) if (values[i] && values[i][0]) last = i;
  if (last < 0) last = 0;   // all empty: "NAME = none"
  for (int i = 0; i <= last; i++) {
    if (!appendToken(values[i], dead[i] && values[i] && values[i][0], o, end)) return false;
  }
  if (capsFlag) {
    const char* f = capsFlag > 0 ? " +caps" : " -caps";
    if (o + 6 >= end) return false;
    memcpy(o, f, 6);
    o += 6;
  }
  *o = '\0';
  return true;
}

size_t withKeyLine(const char* text, size_t len, uint8_t hid, const char* line, char* out, size_t size) {
  // Find the last line that sets this key.
  size_t lastStart = len, lastEnd = len;
  bool found = false, crlf = false;
  for (size_t pos = 0; pos < len;) {
    size_t end = pos;
    while (end < len && text[end] != '\n') end++;
    size_t n = end - pos;
    if (n && text[pos + n - 1] == '\r') { n--; crlf = true; }
    if (lineKey(text + pos, n) == hid) { lastStart = pos; lastEnd = pos + n; found = true; }
    pos = end + 1;
  }
  const size_t ll = strlen(line);
  size_t o = 0;
  auto put = [&](const char* s, size_t n) {
    if (o + n >= size) return false;
    memcpy(out + o, s, n);
    o += n;
    return true;
  };
  if (found) {
    if (!put(text, lastStart) || !put(line, ll) || !put(text + lastEnd, len - lastEnd)) return 0;
  } else {
    if (!put(text, len)) return 0;
    if (len && text[len - 1] != '\n' && !put(crlf ? "\r\n" : "\n", crlf ? 2 : 1)) return 0;
    if (!put(line, ll) || !put(crlf ? "\r\n" : "\n", crlf ? 2 : 1)) return 0;
  }
  out[o] = '\0';
  return o;
}

size_t withoutKey(const char* text, size_t len, uint8_t hid, char* out, size_t size, bool* ok) {
  size_t o = 0;
  *ok = false;
  for (size_t pos = 0; pos < len;) {
    size_t end = pos;
    while (end < len && text[end] != '\n') end++;
    size_t n = end - pos;
    const size_t withNl = end < len ? n + 1 : n;
    size_t body = n;
    if (body && text[pos + body - 1] == '\r') body--;
    if (lineKey(text + pos, body) != hid) {
      if (o + withNl >= size) return 0;
      memcpy(out + o, text + pos, withNl);
      o += withNl;
    }
    pos = end + 1;
  }
  if (o >= size) return 0;
  out[o] = '\0';
  *ok = true;
  return o;
}

bool keyLineIn(const char* text, size_t len, uint8_t hid, char* line, size_t size) {
  bool found = false;
  for (size_t pos = 0; pos < len;) {
    size_t end = pos;
    while (end < len && text[end] != '\n') end++;
    size_t n = end - pos;
    if (n && text[pos + n - 1] == '\r') n--;
    if (lineKey(text + pos, n) == hid && n < size) {
      memcpy(line, text + pos, n);
      line[n] = '\0';
      found = true;
    }
    pos = end + 1;
  }
  return found;
}

bool entryValue(const char* typed, char* out, size_t size) {
  // The whole entry is one code: \uXXXX or \u{X...}. Anything else is kept as typed.
  if (typed[0] == '\\' && typed[1] == 'u') {
    const char* h = typed + 2;
    const bool braces = *h == '{';
    if (braces) h++;
    size_t n = 0;
    while ((h[n] >= '0' && h[n] <= '9') || (h[n] >= 'a' && h[n] <= 'f') || (h[n] >= 'A' && h[n] <= 'F')) n++;
    const bool whole = braces ? (n >= 1 && n <= 6 && h[n] == '}' && h[n + 1] == '\0') : (n == 4 && h[n] == '\0');
    if (whole) return decodeToken(typed, out, size);
  }
  if (strlen(typed) >= size) return false;
  strcpy(out, typed);
  return true;
}

const char* keyName(uint8_t hid, char* buf, size_t size) {
  if (hid >= 0x04 && hid <= 0x1D) snprintf(buf, size, "%c", 'A' + (hid - 0x04));
  else if (hid >= 0x1E && hid <= 0x26) snprintf(buf, size, "%c", '1' + (hid - 0x1E));
  else if (hid == 0x27) snprintf(buf, size, "0");
  else {
    for (const auto& e : kNames) {
      if (e.hid == hid) { snprintf(buf, size, "%s", e.name); return buf; }
    }
    snprintf(buf, size, "0x%02X", hid);
  }
  return buf;
}

}  // namespace keymap
