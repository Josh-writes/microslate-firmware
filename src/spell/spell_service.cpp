#include "spell_service.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "config.h"
#include "../sd_safe.h"

uint8_t spellLang = 0;
bool autocorrectOn = false;

#if defined(MS_SPELLCHECK)
// board_build.embed_files = data/spell/en_words.dat (platformio.ini): the linker names it after its path.
extern const uint8_t kEnWordsStart[] asm("_binary_data_spell_en_words_dat_start");
extern const uint8_t kEnWordsEnd[] asm("_binary_data_spell_en_words_dat_end");
#endif

namespace spellsvc {

static spell::Dict g_dict;
static bool g_opened = false;

static const spell::Dict& dict() {
  if (!g_opened) {
    g_opened = true;
#if defined(MS_SPELLCHECK)
    spell::open(g_dict, kEnWordsStart, (size_t)(kEnWordsEnd - kEnWordsStart));
#endif
  }
  return g_dict;
}

bool available() { return dict().ok; }
bool on() { return spellLang != 0 && available(); }

const char* langLabel(uint8_t lang) {
  switch (lang) {
    case 1: return "US English";
    case 2: return "UK English";
    default: return "Off";
  }
}

static spell::Region region() { return spellLang == 2 ? spell::Region::UK : spell::Region::US; }

// ---- The writer's own words -------------------------------------------------------------------------------------

constexpr char kPersonalPath[] = "/microslate/dictionary.txt";
constexpr size_t kPersonalMaxBytes = 256 * 1024;

static std::vector<std::string> g_own;   // sorted, compared without regard to ASCII case
static bool g_ownLoaded = false;

static int compareNoCase(const char* a, size_t an, const char* b, size_t bn) {
  const size_t n = an < bn ? an : bn;
  for (size_t i = 0; i < n; i++) {
    const int ca = tolower((unsigned char)a[i]), cb = tolower((unsigned char)b[i]);
    if (ca != cb) return ca - cb;
  }
  return an < bn ? -1 : an > bn ? 1 : 0;
}

static bool ownLess(const std::string& a, const std::string& b) {
  return compareNoCase(a.data(), a.size(), b.data(), b.size()) < 0;
}

// Read once, the first time a word is looked up with spellcheck on (lines starting with # are notes).
static void loadOwn() {
  if (g_ownLoaded) return;
  g_ownLoaded = true;
  if (!SdMan.exists(kPersonalPath)) return;
  auto f = SdMan.open(kPersonalPath, O_RDONLY);
  if (!f.isOpen()) return;
  const size_t size = std::min((size_t)f.size(), kPersonalMaxBytes);
  std::string text(size, '\0');
  const int got = f.read(&text[0], (int)size);
  f.close();
  if (got <= 0) return;
  text.resize((size_t)got);
  size_t i = 0;
  while (i < text.size()) {
    size_t e = text.find('\n', i);
    if (e == std::string::npos) e = text.size();
    size_t a = i, b = e;
    while (a < b && (text[a] == ' ' || text[a] == '\t')) a++;
    while (b > a && (text[b - 1] == ' ' || text[b - 1] == '\t' || text[b - 1] == '\r')) b--;
    if (b > a && text[a] != '#' && b - a < spell::kMaxWord) g_own.emplace_back(text, a, b - a);
    i = e + 1;
  }
  std::sort(g_own.begin(), g_own.end(), ownLess);
  DBG_PRINTF("[SPELL] %u word(s) of your own from %s\n", (unsigned)g_own.size(), kPersonalPath);
}

static bool isOwn(const char* word, size_t len) {
  loadOwn();
  size_t lo = 0, hi = g_own.size();
  while (lo < hi) {
    const size_t mid = (lo + hi) / 2;
    const int c = compareNoCase(word, len, g_own[mid].data(), g_own[mid].size());
    if (c == 0) return true;
    if (c < 0) hi = mid;
    else lo = mid + 1;
  }
  return false;
}

bool wordOk(const char* word, size_t len) {
  if (!on()) return true;
  return spell::isCorrect(dict(), region(), word, len) || isOwn(word, len);
}

bool misspelledAt(const char* text, size_t len, size_t pos, size_t* start, size_t* end) {
  if (!on() || !text) return false;
  size_t s, e;
  if (!spell::wordAt(text, len, pos, &s, &e) || wordOk(text + s, e - s)) return false;
  *start = s;
  *end = e;
  return true;
}

int suggest(const char* word, size_t len, char (*out)[spell::kMaxWord], int max) {
  if (!on()) return 0;
  const unsigned long t0 = millis();
  long lookups = 0;
  const int n = spell::suggest(dict(), region(), word, len, out, max, &lookups);
  DBG_PRINTF("[SPELL] %d suggestion(s) for \"%.*s\" in %lu ms (%ld lookups)\n", n, (int)len, word, millis() - t0,
             lookups);
  return n;
}

bool addWord(const char* word, size_t len) {
  if (!word || len == 0 || len >= spell::kMaxWord) return false;
  if (!isOwn(word, len)) {
    std::string w(word, len);
    g_own.insert(std::upper_bound(g_own.begin(), g_own.end(), w, ownLess), w);
  }
  SdMan.mkdir("/microslate");
  auto f = SdMan.open(kPersonalPath, O_WRONLY | O_CREAT | O_AT_END);
  if (!f.isOpen()) {
    DBG_PRINTLN("[SPELL] could not open the dictionary file to add a word");
    return false;
  }
  bool ok = f.write((const uint8_t*)word, len) == len && f.write((const uint8_t*)"\n", 1) == 1;
  f.close();
  DBG_PRINTF("[SPELL] added \"%.*s\" to %s%s\n", (int)len, word, kPersonalPath, ok ? "" : " (write failed)");
  return ok;
}

// ---- Autocorrect --------------------------------------------------------------------------------------------------

static std::vector<std::string> g_takenBack;   // words whose fix the writer took back since the device started

void autocorrectTakenBack(const char* word, size_t len) {
  if (!word || len == 0) return;
  if (g_takenBack.size() >= 64) g_takenBack.erase(g_takenBack.begin());
  g_takenBack.emplace_back(word, len);
}

bool autocorrectWord(const char* text, size_t len, size_t end, size_t* start, char* fix) {
  if (!autocorrectOn || !on() || !text || end == 0) return false;
  size_t s, e;
  if (!spell::wordAt(text, len, end, &s, &e) || e != end || e - s >= spell::kMaxWord) return false;
  if (isOwn(text + s, e - s)) return false;
  for (const std::string& w : g_takenBack)
    if (compareNoCase(w.data(), w.size(), text + s, e - s) == 0) return false;
  // The start of a sentence: nothing before the word but spaces, quotes or brackets, back to . ! ? or a new line.
  size_t p = s;
  while (p > 0 && (text[p - 1] == ' ' || text[p - 1] == '\t' || text[p - 1] == '"' || text[p - 1] == '(' ||
                   text[p - 1] == '\'' || (unsigned char)text[p - 1] >= 0x80))
    p--;
  const bool sentenceStart = p == 0 || text[p - 1] == '.' || text[p - 1] == '!' || text[p - 1] == '?' ||
                             text[p - 1] == '\n';
  if (!spell::autocorrect(dict(), region(), text + s, e - s, sentenceStart, fix)) return false;
  *start = s;
  DBG_PRINTF("[SPELL] autocorrect \"%.*s\" -> \"%s\"\n", (int)(e - s), text + s, fix);
  return true;
}

int ownCount() {
  loadOwn();
  return (int)g_own.size();
}

const char* ownWord(int i) {
  loadOwn();
  return i >= 0 && i < (int)g_own.size() ? g_own[i].c_str() : "";
}

bool removeOwn(int i) {
  loadOwn();
  if (i < 0 || i >= (int)g_own.size()) return false;
  const std::string word = g_own[i];
  g_own.erase(g_own.begin() + i);
  // The file again without the word's lines; other lines (notes, other words) stay as the writer left them.
  auto f = SdMan.open(kPersonalPath, O_RDONLY);
  if (!f.isOpen()) return false;
  const size_t size = std::min((size_t)f.size(), kPersonalMaxBytes);
  std::string text(size, '\0');
  const int got = f.read(&text[0], (int)size);
  f.close();
  if (got < 0) return false;
  text.resize((size_t)got);
  std::string kept;
  kept.reserve(text.size());
  size_t at = 0;
  while (at < text.size()) {
    size_t e = text.find('\n', at);
    const size_t next = e == std::string::npos ? text.size() : e + 1;
    if (e == std::string::npos) e = text.size();
    size_t a = at, z = e;
    while (a < z && (text[a] == ' ' || text[a] == '\t')) a++;
    while (z > a && (text[z - 1] == ' ' || text[z - 1] == '\t' || text[z - 1] == '\r')) z--;
    if (compareNoCase(text.data() + a, z - a, word.data(), word.size()) != 0) kept.append(text, at, next - at);
    at = next;
  }
  const bool ok = sdWriteSafely(kPersonalPath, kept.data(), kept.size());
  DBG_PRINTF("[SPELL] removed \"%s\" from %s%s\n", word.c_str(), kPersonalPath, ok ? "" : " (write failed)");
  return ok;
}

}  // namespace spellsvc
