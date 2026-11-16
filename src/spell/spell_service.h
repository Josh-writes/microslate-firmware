#pragma once
// Spellcheck on the device: the built-in English list (embedded in the firmware on the S3 boards, MS_SPELLCHECK),
// the writer's own words (/microslate/dictionary.txt on the card) and the language setting. The lookup itself is
// spell_dict.h. Work order 1.8.

#include <cstddef>
#include <cstdint>

#include "spell_dict.h"

// Settings > Editor > Spellcheck (and the editor menu): 0 off, 1 US English, 2 UK English. Saved with the UI prefs.
extern uint8_t spellLang;
// Settings > Editor > Autocorrect (and the editor menu); works while spellcheck is on. Saved with the UI prefs.
extern bool autocorrectOn;

namespace spellsvc {

bool available();   // this build has a word list (the S3 boards)
bool on();          // available and switched on
const char* langLabel(uint8_t lang);   // "Off", "US English", "UK English"
constexpr uint8_t kLangCount = 3;

// True when the word (UTF-8, as typed) is spelled right in the chosen language or is one of the writer's own
// words, or spellcheck is off.
bool wordOk(const char* word, size_t len);

// The misspelled word at byte `pos` of text[0, len) (the one the squiggle marks), as [*start, *end).
bool misspelledAt(const char* text, size_t len, size_t pos, size_t* start, size_t* end);

// Spelling suggestions, best first (spell::suggest); returns how many.
int suggest(const char* word, size_t len, char (*out)[spell::kMaxWord], int max);

// Adds the word to the writer's own words: from now on it passes, and it is added as a line to
// /microslate/dictionary.txt (one word per line; the writer can edit the file on a computer). False when the card
// could not be written (the word still passes until the device restarts).
bool addWord(const char* word, size_t len);

// Autocorrect (work order 1.9): the word that ends at byte `end` of text[0, len) (the writer just typed a space or
// punctuation after it), when it should be fixed: true, its start in *start and the fix in fix (kMaxWord bytes).
// Never the writer's own words, nor a word whose fix was taken back this session (autocorrectTakenBack).
bool autocorrectWord(const char* text, size_t len, size_t end, size_t* start, char* fix);
void autocorrectTakenBack(const char* word, size_t len);   // Backspace right after a fix: leave this word alone

// The writer's own words, for Settings > Editor > My words: how many, the i-th in alphabetical order, and removing
// one (from the list and from every line of the file that holds it). False when the card could not be written.
int ownCount();
const char* ownWord(int i);
bool removeOwn(int i);

}  // namespace spellsvc
