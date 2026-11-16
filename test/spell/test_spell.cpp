// Host tests for the spellcheck lookup (src/spell/spell_dict.h). Run with: sh test/spell/run-on-hinton.sh
// Uses the real built-in list, data/spell/en_words.dat: every word in it must be found, regions must stay apart,
// and the case, apostrophe and accent rules must hold.
#include "spell/spell_dict.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace spell;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                      \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }       \
  } while (0)

static bool ok(const Dict& d, Region r, const char* w) { return isCorrect(d, r, w, strlen(w)); }

static std::vector<std::string> wordsOf(const Section& s) {
  std::vector<std::string> out;
  std::string cur;
  for (size_t i = 0; i < s.len; i++) {
    if (s.data[i] == '\n') { cur.pop_back(); out.push_back(cur); cur.clear(); }   // less the rank byte
    else cur += s.data[i];
  }
  return out;
}

static void testSmallSection() {
  const char text[] = "app\x03\napple\x01\nbanana\x02\nzoo\x05\n";
  Section s{text, sizeof(text) - 1};
  CHECK(inSection(s, "app", 3));
  CHECK(inSection(s, "apple", 5));
  CHECK(inSection(s, "banana", 6));
  CHECK(inSection(s, "zoo", 3));
  CHECK(!inSection(s, "ap", 2));
  CHECK(!inSection(s, "apples", 6));
  CHECK(!inSection(s, "a", 1));
  CHECK(!inSection(s, "zzz", 3));
  CHECK(rankIn(s, "apple", 5) == 1 && rankIn(s, "zoo", 3) == 5 && rankIn(s, "app", 3) == 3);
  CHECK(!inSection(s, "", 0));
  Section empty;
  CHECK(!inSection(empty, "a", 1));
  Dict d;
  const uint8_t junk[40] = {'n', 'o', 'p', 'e'};
  CHECK(!open(d, junk, sizeof(junk)) && !d.ok);
  CHECK(isCorrect(d, Region::US, "anything", 8));   // no list: nothing is marked
}

static void testSuggest(const Dict& d);
static void testAutocorrect(const Dict& d);

static void testRealList(const std::vector<uint8_t>& file) {
  Dict d;
  CHECK(open(d, file.data(), file.size()) && d.ok);
  if (!d.ok) return;

  // Every listed word is found where it belongs, and a regional word only in its own region.
  const auto common = wordsOf(d.common), us = wordsOf(d.us), uk = wordsOf(d.uk);
  CHECK(common.size() > 100000 && us.size() > 3000 && uk.size() > 3000);
  int missing = 0;
  for (const auto& w : common) missing += !inSection(d.common, w.data(), w.size());
  for (const auto& w : us) missing += !inSection(d.us, w.data(), w.size());
  for (const auto& w : uk) missing += !inSection(d.uk, w.data(), w.size());
  CHECK(missing == 0);
  printf("%zu common, %zu US, %zu UK words: %d not found\n", common.size(), us.size(), uk.size(), missing);

  // Regions.
  CHECK(ok(d, Region::US, "color") && !ok(d, Region::UK, "color"));
  CHECK(ok(d, Region::UK, "colour") && !ok(d, Region::US, "colour"));
  CHECK(ok(d, Region::US, "theater") && ok(d, Region::UK, "theatre") && !ok(d, Region::US, "theatre"));
  CHECK(ok(d, Region::US, "organize") && ok(d, Region::UK, "organize") && ok(d, Region::UK, "organise") &&
        !ok(d, Region::US, "organise"));

  // Case.
  CHECK(ok(d, Region::US, "the") && ok(d, Region::US, "The") && ok(d, Region::US, "THE"));
  CHECK(!ok(d, Region::US, "tHe"));
  CHECK(ok(d, Region::US, "Paris") && ok(d, Region::US, "PARIS") && !ok(d, Region::US, "paris"));
  CHECK(ok(d, Region::US, "I") && ok(d, Region::US, "a"));

  // Apostrophes and accents.
  CHECK(ok(d, Region::US, "don't") && ok(d, Region::US, "don\xE2\x80\x99t"));
  CHECK(ok(d, Region::US, "it's") && ok(d, Region::US, "dog's"));
  CHECK(ok(d, Region::US, "caf\xC3\xA9"));          // café is listed
  CHECK(ok(d, Region::US, "na\xC3\xAFve"));         // naïve passes as naive
  CHECK(ok(d, Region::US, "CAF\xC3\x89"));          // CAFÉ

  // Misspellings.
  CHECK(!ok(d, Region::US, "teh") && !ok(d, Region::US, "recieve") && !ok(d, Region::US, "definately"));
  CHECK(!ok(d, Region::UK, "seperate") && ok(d, Region::UK, "separate"));

  testSuggest(d);
  testAutocorrect(d);
}

// The suggestions for a word, best first; prints them and the lookups it took.
static std::vector<std::string> sugg(const Dict& d, Region r, const char* w, long* lookupsOut = nullptr) {
  char out[5][kMaxWord];
  long lookups = 0;
  const int n = suggest(d, r, w, strlen(w), out, 5, &lookups);
  std::vector<std::string> v(out, out + n);
  printf("  %-12s (%6ld lookups):", w, lookups);
  for (const auto& s : v) printf(" %s", s.c_str());
  printf("\n");
  if (lookupsOut) *lookupsOut = lookups;
  return v;
}

static bool first(const std::vector<std::string>& v, const char* w) { return !v.empty() && v[0] == w; }
static bool has(const std::vector<std::string>& v, const char* w) {
  for (const auto& s : v)
    if (s == w) return true;
  return false;
}

static void testSuggest(const Dict& d) {
  CHECK(first(sugg(d, Region::US, "teh"), "the"));
  CHECK(first(sugg(d, Region::US, "recieve"), "receive"));
  CHECK(first(sugg(d, Region::US, "Teh"), "The"));
  CHECK(first(sugg(d, Region::US, "TEH"), "THE"));
  CHECK(has(sugg(d, Region::US, "seperate"), "separate"));
  CHECK(has(sugg(d, Region::US, "definately"), "definitely"));   // two edits
  CHECK(has(sugg(d, Region::US, "becuase"), "because"));
  CHECK(has(sugg(d, Region::US, "wierd"), "weird"));
  CHECK(has(sugg(d, Region::US, "londn"), "London"));            // a name keeps its capital
  CHECK(has(sugg(d, Region::US, "colr"), "color"));
  CHECK(has(sugg(d, Region::UK, "colur"), "colour"));
  CHECK(has(sugg(d, Region::US, "dont"), "don't"));
  // Every suggestion is spelled right, and none repeats.
  for (const char* w : {"teh", "recieve", "acommodate", "occured", "thier", "untill", "xqzv"}) {
    const auto v = sugg(d, Region::US, w);
    for (size_t i = 0; i < v.size(); i++) {
      CHECK(isCorrect(d, Region::US, v[i].data(), v[i].size()));
      for (size_t j = i + 1; j < v.size(); j++) CHECK(v[i] != v[j]);
    }
  }
  // The work is bounded: a long word stops at the lookup limit.
  long lookups = 0;
  sugg(d, Region::US, "abcdefghijklmnopqrstuvwxyzab", &lookups);
  CHECK(lookups <= 60000 + 8);
  CHECK(sugg(d, Region::US, "").empty());
}

// What autocorrect does with a word ("" when it leaves it alone); prints it.
static std::string ac(const Dict& d, const char* w, bool sentenceStart = false) {
  char out[kMaxWord];
  const bool fixed = autocorrect(d, Region::US, w, strlen(w), sentenceStart, out);
  printf("  ac %-12s -> %s\n", w, fixed ? out : "(left)");
  return fixed ? std::string(out) : std::string();
}

static void testAutocorrect(const Dict& d) {
  // Every typo in our list is a real misspelling (not a listed word), and its fix is spelled right.
  for (int i = 0; typoListEntry(i); i++) {
    const char* t = typoListEntry(i);
    if (strcmp(t, "i") == 0) continue;   // the letter i is listed; a lone i still becomes I
    const bool listedWord = isCorrect(d, Region::US, t, strlen(t));
    if (listedWord) printf("FAIL typo \"%s\" is a listed word\n", t);
    CHECK(!listedWord);
    const char* fix = typoFix(t);
    std::string f = fix;
    for (size_t a = 0, b; a <= f.size(); a = b + 1) {   // "a lot" is two words
      b = f.find(' ', a);
      if (b == std::string::npos) b = f.size();
      CHECK(isCorrect(d, Region::US, f.data() + a, b - a));
    }
  }
  // The list.
  CHECK(ac(d, "teh") == "the");
  CHECK(ac(d, "Teh", true) == "The");
  CHECK(ac(d, "dont") == "don't");
  CHECK(ac(d, "i") == "I");
  CHECK(ac(d, "im") == "I'm");
  CHECK(ac(d, "alot") == "a lot");
  // Clear single answers one edit away.
  CHECK(ac(d, "becasue") == "because");
  CHECK(ac(d, "langauge") == "language");
  CHECK(ac(d, "probaly") == "probably");
  // Left alone.
  CHECK(ac(d, "the") == "");                // right
  CHECK(ac(d, "Teh") == "");                // capitalized mid-sentence: maybe a name
  CHECK(ac(d, "TEH") == "");                // capitals
  CHECK(ac(d, "iPhone") == "");             // mixed case
  CHECK(ac(d, "xqzv") == "");               // no answer
  CHECK(ac(d, "londn") == "");              // would become a name
  CHECK(ac(d, "tha") == "");                // short and not in the list
  CHECK(ac(d, "Hinton") == "");             // a name
  // A survey of common misspellings, to see what it does (printed only).
  for (const char* w : {"wierd", "recieve", "acheive", "adress", "apparantly", "calender", "comming", "concious",
                        "embarass", "familar", "finaly", "foriegn", "gaurd", "happend", "immediatly", "independant",
                        "knowlege", "libary", "mispell", "noticable", "occassion", "posession", "realy",
                        "rember", "sentance", "suprise", "togther", "tounge", "wether", "writen"})
    ac(d, w);
}

static void testWordAt() {
  const char* t = "one tow, three a@b.com end";
  const size_t n = strlen(t);
  size_t s = 0, e = 0;
  CHECK(wordAt(t, n, 0, &s, &e) && s == 0 && e == 3);
  CHECK(wordAt(t, n, 3, &s, &e) && s == 0 && e == 3);       // just after the word
  CHECK(wordAt(t, n, 5, &s, &e) && s == 4 && e == 7);       // inside "tow"
  CHECK(wordAt(t, n, 7, &s, &e) && s == 4 && e == 7);       // before the comma
  CHECK(!wordAt(t, n, 8, &s, &e));                          // the space after the comma
  CHECK(wordAt(t, n, 9, &s, &e) && s == 9 && e == 14);
  CHECK(!wordAt(t, n, 17, &s, &e));                         // inside the e-mail address
  CHECK(wordAt(t, n, n, &s, &e) && s == n - 3 && e == n);   // at the very end
  CHECK(!wordAt(t, n, n + 1, &s, &e));
}

static std::vector<std::string> words(const char* t) {
  std::vector<std::string> out;
  const size_t n = strlen(t);
  size_t s, e, from = 0;
  while (nextWord(t, n, from, &s, &e)) {
    out.push_back(std::string(t + s, e - s));
    from = e;
  }
  return out;
}

static void testWords() {
  const auto w = words("Hello, world! don't 'quoted' stop 2nd mp3 well-known mail a@b.com path/to x");
  const std::vector<std::string> want = {"Hello", "world", "don't", "quoted", "stop", "well", "known", "mail", "x"};
  CHECK(w == want);
  if (w != want) {
    for (const auto& s : w) printf("  [%s]", s.c_str());
    printf("\n");
  }
  const auto c = words("\xE2\x80\x9CHello\xE2\x80\x9D she said \xE2\x80\x94 caf\xC3\xA9 don\xE2\x80\x99t\tend\n");
  const std::vector<std::string> wantC = {"Hello", "she", "said", "caf\xC3\xA9", "don\xE2\x80\x99t", "end"};
  CHECK(c == wantC);
  CHECK(words("").empty() && words("  ... 123 !! ").empty());
  CHECK(words("example.com www.example.org").empty());
}

int main(int argc, char** argv) {
  testSmallSection();
  testWords();
  testWordAt();
  const char* path = argc > 1 ? argv[1] : "data/spell/en_words.dat";
  FILE* f = fopen(path, "rb");
  CHECK(f != nullptr);
  if (f) {
    std::vector<uint8_t> file;
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) file.insert(file.end(), buf, buf + n);
    fclose(f);
    testRealList(file);
  }
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
