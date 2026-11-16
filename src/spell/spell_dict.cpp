#include "spell_dict.h"

#include <cstring>

namespace spell {

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

bool open(Dict& d, const uint8_t* file, size_t len) {
  d = Dict();
  if (!file || len < 32 || memcmp(file, "MSWL2\0\0\0", 8) != 0) return false;
  Section* secs[3] = {&d.common, &d.us, &d.uk};
  for (int i = 0; i < 3; i++) {
    const uint32_t off = le32(file + 8 + 8 * i), n = le32(file + 12 + 8 * i);
    if (off > len || n > len - off) return false;
    secs[i]->data = (const char*)file + off;
    secs[i]->len = n;
  }
  d.ok = true;
  return true;
}

// Byte order, like the sort in make_wordlist.py.
static int compare(const char* a, size_t an, const char* b, size_t bn) {
  const int c = memcmp(a, b, an < bn ? an : bn);
  if (c) return c;
  return an < bn ? -1 : an > bn ? 1 : 0;
}

int rankIn(const Section& s, const char* word, size_t len) {
  if (!s.data || len == 0) return 0;
  size_t lo = 0, hi = s.len;   // both always at the start of a word (or the end)
  while (lo < hi) {
    size_t p = lo + (hi - lo) / 2;
    while (p > lo && s.data[p - 1] != '\n') p--;   // back to the start of the word holding mid
    const char* w = s.data + p;
    const char* nl = (const char*)memchr(w, '\n', s.len - p);
    const size_t ln = nl ? (size_t)(nl - w) : s.len - p;   // the word and its rank byte
    if (ln == 0) return 0;   // not a list made by make_wordlist.py
    const int c = compare(word, len, w, ln - 1);
    if (c == 0) return (unsigned char)w[ln - 1];
    if (c < 0) hi = p;
    else lo = p + ln + 1;
  }
  return 0;
}

// ---- UTF-8 and case, for English (ASCII and Latin-1 letters) ---------------------------------------------------

static uint32_t decode(const char* t, size_t to, size_t i, size_t* n) {
  const unsigned char c = (unsigned char)t[i];
  size_t k = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 1;
  if (i + k > to) k = 1;
  uint32_t cp = k == 1 ? c : c & (0x7F >> k);
  for (size_t j = 1; j < k; j++) cp = (cp << 6) | ((unsigned char)t[i + j] & 0x3F);
  *n = k;
  return cp;
}

static bool isApostrophe(uint32_t cp) { return cp == '\'' || cp == 0x2019 || cp == 0x02BC; }
static bool isDigit(uint32_t cp) { return cp >= '0' && cp <= '9'; }

static bool isLetter(uint32_t cp) {
  if ((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) return true;
  if (cp >= 0xC0 && cp <= 0x24F) return cp != 0xD7 && cp != 0xF7;   // Latin letters with accents
  return cp >= 0x370 && cp < 0x2000;                                // Greek, Cyrillic and other scripts
}

static bool isUpper(uint32_t cp) {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7);
}
static bool isLower(uint32_t cp) {
  return (cp >= 'a' && cp <= 'z') || (cp >= 0xDF && cp <= 0xFF && cp != 0xF7);
}
static uint32_t toLower(uint32_t cp) { return isUpper(cp) ? cp + 0x20 : cp; }
static uint32_t toUpper(uint32_t cp) { return isLower(cp) && cp != 0xDF && cp != 0xFF ? cp - 0x20 : cp; }

static size_t encode(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = (char)cp; return 1; }
  if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

// The letter without its accent (Latin-1 letters), or 0 when it has none.
static uint32_t baseLetter(uint32_t cp) {
  static const char kLow[] = "aaaaaaeceeeeiiiidnooooo/ouuuuyty";   // U+00E0..U+00FF
  if (cp >= 0xE0 && cp <= 0xFF && cp != 0xF7 && cp != 0xE6 && cp != 0xF0 && cp != 0xFE) return (uint32_t)kLow[cp - 0xE0];
  if (cp >= 0xC0 && cp <= 0xDD && cp != 0xD7 && cp != 0xC6 && cp != 0xD0) return (uint32_t)kLow[cp - 0xC0] - 0x20;
  return 0;
}

enum Shape { kLowerAll, kCapitalized, kUpperAll, kMixed };

// Copies the word with each letter mapped through f (and curly apostrophes made straight); false when it is too long.
template <class F>
static bool mapWord(const char* w, size_t len, char* out, size_t* outLen, F f) {
  size_t o = 0;
  for (size_t i = 0; i < len;) {
    size_t n;
    uint32_t cp = decode(w, len, i, &n);
    cp = isApostrophe(cp) ? '\'' : f(cp, i == 0);
    if (o + 4 > kMaxWord) return false;
    o += encode(cp, out + o);
    i += n;
  }
  *outLen = o;
  return true;
}

static Shape shapeOf(const char* word, size_t len) {
  int letters = 0, uppers = 0;
  bool firstUpper = false, restLower = true;
  for (size_t i = 0; i < len;) {
    size_t k;
    const uint32_t cp = decode(word, len, i, &k);
    if (isUpper(cp)) {
      uppers++;
      if (letters == 0) firstUpper = true;
      else restLower = false;
    }
    if (isUpper(cp) || isLower(cp)) letters++;
    i += k;
  }
  return uppers == 0 ? kLowerAll : uppers == letters ? kUpperAll : firstUpper && restLower ? kCapitalized : kMixed;
}

static bool listed(const Dict& d, Region r, const char* w, size_t n) {
  return inSection(d.common, w, n) || inSection(r == Region::UK ? d.uk : d.us, w, n);
}

static bool check(const Dict& d, Region r, const char* word, size_t len, bool tryAccents) {
  char buf[kMaxWord] = {};
  size_t n = 0;
  if (!mapWord(word, len, buf, &n, [](uint32_t cp, bool) { return cp; })) return true;   // too long to judge
  if (listed(d, r, buf, n)) return true;

  const Shape shape = shapeOf(word, len);

  if (shape == kCapitalized || shape == kUpperAll) {   // "The", "THE" -> "the"
    if (mapWord(word, len, buf, &n, [](uint32_t cp, bool) { return toLower(cp); }) && listed(d, r, buf, n)) return true;
  }
  if (shape == kUpperAll) {   // "PARIS" -> "Paris"
    if (mapWord(word, len, buf, &n, [](uint32_t cp, bool first) { return first ? cp : toLower(cp); }) &&
        listed(d, r, buf, n))
      return true;
  }
  if (tryAccents) {   // "naïve" -> "naive"
    bool changed = false;
    if (mapWord(word, len, buf, &n, [&](uint32_t cp, bool) {
          const uint32_t b = baseLetter(cp);
          if (b) changed = true;
          return b ? b : cp;
        }) && changed)
      return check(d, r, buf, n, false);
  }
  return false;
}

bool isCorrect(const Dict& d, Region r, const char* word, size_t len) {
  if (!d.ok || len == 0) return true;   // no list: nothing is marked
  return check(d, r, word, len, true);
}

bool nextWord(const char* text, size_t to, size_t from, size_t* start, size_t* end) {
  size_t i = from;
  while (i < to) {
    size_t n;
    uint32_t cp = decode(text, to, i, &n);
    if (!isLetter(cp) && !isDigit(cp)) { i += n; continue; }
    // A token: letters and digits, with apostrophes only between them.
    const size_t s = i;
    bool digit = false;
    size_t e = i;
    while (i < to) {
      cp = decode(text, to, i, &n);
      if (isLetter(cp) || isDigit(cp)) {
        digit = digit || isDigit(cp);
        i += n;
        e = i;
      } else if (isApostrophe(cp) && i + n < to) {
        size_t n2;
        const uint32_t next = decode(text, to, i + n, &n2);
        if (!isLetter(next)) break;
        i += n;
      } else {
        break;
      }
    }
    // Not prose: words with digits, and parts of e-mail addresses, paths and web addresses.
    const char before = s > 0 ? text[s - 1] : ' ';
    const char after = e < to ? text[e] : ' ';
    const bool code = strchr("@/\\_", before) || strchr("@/\\_", after) ||
                      (after == '.' && e + 1 < to && ((unsigned char)text[e + 1] | 0x20) >= 'a' &&
                       ((unsigned char)text[e + 1] | 0x20) <= 'z') ||
                      (before == '.' && s >= 2 && ((unsigned char)text[s - 2] | 0x20) >= 'a' &&
                       ((unsigned char)text[s - 2] | 0x20) <= 'z');
    if (!digit && !code) {
      *start = s;
      *end = e;
      return true;
    }
    i = e > s ? e : s + 1;
  }
  return false;
}

bool wordAt(const char* text, size_t to, size_t pos, size_t* start, size_t* end) {
  if (pos > to) return false;
  size_t from = pos;   // back to the start of the space-separated run holding pos
  for (int k = 0; from > 0 && k < 128 && text[from - 1] != ' ' && text[from - 1] != '\n' && text[from - 1] != '\t'; k++)
    from--;
  size_t s, e;
  while (nextWord(text, to, from, &s, &e)) {
    if (s > pos) return false;
    if (pos <= e) {
      *start = s;
      *end = e;
      return true;
    }
    from = e;
  }
  return false;
}

// ---- Suggestions ------------------------------------------------------------------------------------------------

namespace {

constexpr int kMaxLetters = 32;       // longer words get no suggestions
constexpr long kMaxLookups = 60000;   // bounds the time a long word can take
const char kAlphabet[] = "abcdefghijklmnopqrstuvwxyz'";
constexpr int kAlphabetLen = sizeof(kAlphabet) - 1;

struct Search {
  const Dict* d;
  Region r;
  Shape shape;
  char (*out)[kMaxWord];
  int score[16];
  int max, count = 0;
  long lookups = 0;
};

// The word's rank in the common and regional sections (the better one), 0 when neither lists it.
int rankListed(Search& s, const char* w, size_t n) {
  s.lookups += 2;
  const int a = rankIn(s.d->common, w, n);
  const int b = rankIn(s.r == Region::UK ? s.d->uk : s.d->us, w, n);
  return a && b ? (a < b ? a : b) : a ? a : b;
}

// One candidate (lowercase code points): kept when it is listed, in lowercase or capitalized (a name).
void tryCandidate(Search& s, const uint32_t* c, int n, int edits, bool swap) {
  if (n <= 0 || s.lookups >= kMaxLookups) return;
  char w[kMaxWord];
  size_t wn = 0;
  for (int i = 0; i < n; i++) {
    if (wn + 4 >= kMaxWord) return;
    wn += encode(c[i], w + wn);
  }
  int rank = rankListed(s, w, wn);
  if (!rank) {   // "london" -> "London"
    char t[kMaxWord];
    size_t first;
    decode(w, wn, 0, &first);
    size_t k = encode(toUpper(c[0]), t);
    memcpy(t + k, w + first, wn - first);
    k += wn - first;
    if (!(rank = rankListed(s, t, k))) return;
    memcpy(w, t, k);
    wn = k;
  }
  if (s.shape == kCapitalized || s.shape == kUpperAll) {   // the typed case carries over
    char t[kMaxWord];
    size_t k = 0;
    for (size_t i = 0; i < wn;) {
      size_t m;
      const uint32_t cp = decode(w, wn, i, &m);
      if (k + 4 >= kMaxWord) return;
      k += encode(s.shape == kUpperAll || i == 0 ? toUpper(cp) : cp, t + k);
      i += m;
    }
    memcpy(w, t, k);
    wn = k;
  }
  w[wn] = '\0';
  const int score = edits * 16 + rank * 2 + (swap ? 0 : 1);
  for (int i = 0; i < s.count; i++) {
    if (strcmp(s.out[i], w) != 0) continue;
    if (score >= s.score[i]) return;   // already there, as good or better
    for (int j = i; j + 1 < s.count; j++) {   // take the worse copy out; the better one goes in below
      memcpy(s.out[j], s.out[j + 1], kMaxWord);
      s.score[j] = s.score[j + 1];
    }
    s.count--;
    break;
  }
  int at = s.count;
  while (at > 0 && s.score[at - 1] > score) at--;
  if (at >= s.max) return;
  if (s.count < s.max) s.count++;
  for (int j = s.count - 1; j > at; j--) {
    memcpy(s.out[j], s.out[j - 1], kMaxWord);
    s.score[j] = s.score[j - 1];
  }
  memcpy(s.out[at], w, wn + 1);
  s.score[at] = score;
}

// Calls f(code points, count, swap) for each word one edit from w[0, n). `cheap` leaves out changed and added
// letters (for the second edit, where all four kinds would be too many lookups).
template <class F>
void forEachEdit(const uint32_t* w, int n, bool cheap, F f) {
  uint32_t c[kMaxLetters + 2];
  for (int i = 0; i < n; i++) {   // drop a letter
    memcpy(c, w, i * sizeof(uint32_t));
    memcpy(c + i, w + i + 1, (n - i - 1) * sizeof(uint32_t));
    f(c, n - 1, false);
  }
  for (int i = 0; i + 1 < n; i++) {   // swap two letters
    if (w[i] == w[i + 1]) continue;
    memcpy(c, w, n * sizeof(uint32_t));
    c[i] = w[i + 1];
    c[i + 1] = w[i];
    f(c, n, true);
  }
  if (cheap) return;
  for (int i = 0; i < n; i++) {   // change a letter
    memcpy(c, w, n * sizeof(uint32_t));
    for (int a = 0; a < kAlphabetLen; a++) {
      if ((uint32_t)kAlphabet[a] == w[i]) continue;
      c[i] = (uint32_t)kAlphabet[a];
      f(c, n, false);
    }
  }
  if (n >= kMaxLetters) return;
  for (int i = 0; i <= n; i++) {   // add a letter
    memcpy(c, w, i * sizeof(uint32_t));
    memcpy(c + i + 1, w + i, (n - i) * sizeof(uint32_t));
    for (int a = 0; a < kAlphabetLen; a++) {
      c[i] = (uint32_t)kAlphabet[a];
      f(c, n + 1, false);
    }
  }
}

}  // namespace

int suggest(const Dict& d, Region r, const char* word, size_t len, char (*out)[kMaxWord], int max, long* lookups,
            int maxEdits, int* scores) {
  if (lookups) *lookups = 0;
  if (!d.ok || !word || len == 0 || max <= 0) return 0;
  if (max > 16) max = 16;
  Search s;
  s.d = &d;
  s.r = r;
  s.shape = shapeOf(word, len);
  s.out = out;
  s.max = max;
  uint32_t w[kMaxLetters];
  int n = 0;
  for (size_t i = 0; i < len;) {
    size_t k;
    const uint32_t cp = decode(word, len, i, &k);
    if (n >= kMaxLetters) return 0;
    w[n++] = isApostrophe(cp) ? '\'' : toLower(cp);
    i += k;
  }
  forEachEdit(w, n, false, [&](const uint32_t* c, int cn, bool swap) { tryCandidate(s, c, cn, 1, swap); });
  if (s.count < 3 && maxEdits >= 2) {   // few one-edit words: try two edits (the second one only drops or swaps a letter)
    forEachEdit(w, n, false, [&](const uint32_t* c, int cn, bool) {
      forEachEdit(c, cn, true, [&](const uint32_t* c2, int cn2, bool) { tryCandidate(s, c2, cn2, 2, false); });
    });
  }
  if (lookups) *lookups = s.lookups;
  if (scores)
    for (int i = 0; i < s.count; i++) scores[i] = s.score[i];
  return s.count;
}

// ---- Autocorrect ------------------------------------------------------------------------------------------------

namespace {

// Common typos with one right answer (our own list). Each typo is checked by the tests not to be a listed word.
struct Typo { const char* from; const char* to; };
const Typo kTypos[] = {
    {"i", "I"},
    {"teh", "the"},          {"hte", "the"},          {"adn", "and"},          {"nad", "and"},
    {"taht", "that"},        {"thta", "that"},        {"waht", "what"},        {"wiht", "with"},
    {"thier", "their"},      {"recieve", "receive"},  {"recieved", "received"},
    {"beleive", "believe"},  {"becuase", "because"},  {"becasue", "because"},  {"definately", "definitely"},
    {"seperate", "separate"}, {"occured", "occurred"}, {"untill", "until"},    {"wierd", "weird"},
    {"freind", "friend"},    {"freinds", "friends"},  {"wich", "which"},       {"writting", "writing"},
    {"tommorow", "tomorrow"}, {"tomorow", "tomorrow"}, {"accomodate", "accommodate"},
    {"goverment", "government"}, {"enviroment", "environment"}, {"existance", "existence"},
    {"truely", "truly"},     {"arguement", "argument"}, {"begining", "beginning"}, {"neccessary", "necessary"},
    {"occurence", "occurrence"}, {"alot", "a lot"},   {"thats", "that's"},     {"dont", "don't"},
    {"doesnt", "doesn't"},   {"didnt", "didn't"},     {"isnt", "isn't"},       {"wasnt", "wasn't"},
    {"arent", "aren't"},     {"werent", "weren't"},   {"couldnt", "couldn't"},
    {"shouldnt", "shouldn't"}, {"wouldnt", "wouldn't"}, {"havent", "haven't"}, {"hasnt", "hasn't"},
    {"im", "I'm"},           {"ive", "I've"},         {"youre", "you're"},     {"theyre", "they're"},
    {"theres", "there's"},   {"knwo", "know"},
    {"konw", "know"},        {"jsut", "just"},        {"yuo", "you"},          {"becuse", "because"},
};

// The fix in the typed case: "Teh" -> "The" (a fix that starts with a capital, like "I'm", keeps it).
uint32_t decodeFirst(const char* s) {
  size_t k;
  return decode(s, strlen(s), 0, &k);
}

void withCase(const char* fix, bool capital, char* out) {
  size_t n = strlen(fix);
  if (n >= kMaxWord) n = kMaxWord - 1;
  memcpy(out, fix, n);
  out[n] = '\0';
  if (capital && out[0] >= 'a' && out[0] <= 'z') out[0] = (char)(out[0] - 32);
}

}  // namespace

const char* typoFix(const char* lower) {
  for (const Typo& t : kTypos)
    if (strcmp(t.from, lower) == 0) return t.to;
  return nullptr;
}

const char* typoListEntry(int i) {
  return i >= 0 && i < (int)(sizeof(kTypos) / sizeof(kTypos[0])) ? kTypos[i].from : nullptr;
}

bool autocorrect(const Dict& d, Region r, const char* word, size_t len, bool sentenceStart, char* out) {
  if (!d.ok || !word || len == 0 || len >= kMaxWord) return false;
  const Shape shape = shapeOf(word, len);
  if (shape == kMixed) return false;                        // "iPhone", "McDonald": the writer meant it
  if (shape == kUpperAll && len > 1) return false;          // "NASA", shouting
  if (shape == kCapitalized && !sentenceStart && len > 1) return false;   // a name mid-sentence
  char lower[kMaxWord];
  size_t ln = 0;
  if (!mapWord(word, len, lower, &ln, [](uint32_t cp, bool) { return toLower(cp); })) return false;
  lower[ln] = '\0';
  const bool capital = shape != kLowerAll;

  if (const char* fix = typoFix(lower)) {
    withCase(fix, capital, out);
    return strncmp(out, word, len) != 0 || out[len] != '\0';
  }
  if (isCorrect(d, r, word, len)) return false;
  int letters = 0;
  for (size_t i = 0; i < len;) {
    size_t k;
    decode(word, len, i, &k);
    letters++;
    i += k;
  }
  if (letters < 4) return false;   // short words have too many near neighbours

  // A clear winner one edit away: the only one, or well ahead of the next (by commonness).
  char sug[3][kMaxWord];
  int scores[3] = {};
  const int n = suggest(d, r, word, len, sug, 3, nullptr, 1, scores);
  if (n == 0) return false;
  if (n > 1 && scores[1] - scores[0] < 4) return false;
  if (shape == kLowerAll && isUpper(decodeFirst(sug[0]))) return false;   // "londn": not turned into a name
  if (strchr(sug[0], ' ')) return false;
  // People rarely get the first letter wrong: "tounge" is not "lounge", "rember" is not "member".
  size_t k0;
  if (toLower(decodeFirst(sug[0])) != toLower(decode(word, len, 0, &k0))) return false;
  withCase(sug[0], false, out);
  return true;
}

}  // namespace spell
