#pragma once
// Spellcheck word lookup, with no device code, so it runs in host tests (test/spell).
// The built-in English list (data/spell/en_words.dat, made by scripts/make_wordlist.py from SCOWL) is three sorted
// sections: words common to US and UK English, US-only and UK-only, each word with a rank (how common it is). A word is looked up in the common section and
// the chosen region's, straight from the data (no copy, no index): a binary search over '\n'-separated words.
// Plan: MicroSlate-workspace/notes/v3.0-work-order.md (1.8).

#include <cstddef>
#include <cstdint>

namespace spell {

enum class Region : uint8_t { US = 0, UK = 1 };

constexpr size_t kMaxWord = 64;   // bytes; longer words are not checked

struct Section {
  const char* data = nullptr;   // words sorted by their bytes, each followed by a rank byte (1-5) and '\n'
  size_t len = 0;
};

struct Dict {
  Section common, us, uk;
  bool ok = false;
};

// Reads the header of a word list file held in memory (flash or RAM); false when it is not one.
bool open(Dict& d, const uint8_t* file, size_t len);

// The rank of the exact bytes word[0, len) in the section: 1 (most common) to 5, or 0 when it is not listed.
int rankIn(const Section& s, const char* word, size_t len);
inline bool inSection(const Section& s, const char* word, size_t len) { return rankIn(s, word, len) != 0; }

// The spelling check for one word as typed (UTF-8): case is forgiven the way a writer expects ("the", "The" and
// "THE" pass when "the" is listed; "Paris" and "PARIS" pass when "Paris" is listed, "paris" does not), a curly
// apostrophe counts as a straight one, and a word whose accent-free form is listed passes ("naïve" -> "naive").
bool isCorrect(const Dict& d, Region r, const char* word, size_t len);

// Words in text[from, to): the next word at or after `from` as [*start, *end), false when there is none.
// A word is letters (any UTF-8 letter beyond ASCII counts) with apostrophes inside it ("don't"); hyphens split
// words ("well-known" is two). Words holding a digit are skipped (they are not checked: "2nd", "mp3").
bool nextWord(const char* text, size_t to, size_t from, size_t* start, size_t* end);

// The word (as nextWord finds it) that holds byte `pos` of text[0, to), or ends right at it; false when none does.
bool wordAt(const char* text, size_t to, size_t pos, size_t* start, size_t* end);

// Spelling suggestions for a misspelled word: listed words one edit away (a letter dropped, added, changed, or two
// letters swapped), then two edits away when one edit finds few. Best first: fewer edits, then the more common word,
// then a swap. The typed case carries over ("Teh" -> "The", "TEH" -> "THE"). Up to `max` (at most 16) words, each
// a NUL-terminated string in out[i]; returns how many. `lookups` (optional) counts the list searches, for the tests.
// maxEdits 1 skips the two-edit search; `scores` (optional, `max` ints) gets each one's score (lower is better:
// 16 per edit, 2 per rank step, 1 when not a swap).
int suggest(const Dict& d, Region r, const char* word, size_t len, char (*out)[kMaxWord], int max,
            long* lookups = nullptr, int maxEdits = 2, int* scores = nullptr);

// Autocorrect (work order 1.9) for a word just finished: true and the fix in out (kMaxWord bytes) when the word is a
// common typo from our own list ("teh", "dont", a lone "i"), or is misspelled with one clear answer a single edit
// away. Never for words in capitals, mixed case, a capitalized word mid-sentence (sentenceStart false: a name),
// words under 4 letters outside the list, or a fix that would be a name or two words. The caller skips the
// writer's own words.
bool autocorrect(const Dict& d, Region r, const char* word, size_t len, bool sentenceStart, char* out);
const char* typoFix(const char* lower);   // the list's fix for a lowercase typo, or nullptr
const char* typoListEntry(int i);         // the i-th typo in the list (nullptr past the end), for the tests

}  // namespace spell
