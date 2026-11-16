#pragma once
// Undo/redo history for the editor, with no device code, so it runs in host tests (test/editor).
// Each step records one replacement: at byte `at`, `removed` became `inserted`, with the cursor where it was before.
// Typing is grouped into one step per word (a word and the spaces after it); Backspace and Delete runs are grouped the
// same way; paste, cut and typing over a selection are one step each. The steps live in one fixed buffer (1 MB on the
// S3 boards); when it is full the oldest steps are dropped. Plan: MicroSlate-workspace/notes/v3.0-work-order.md (1.2).

#include <cstddef>
#include <cstdint>

namespace edhist {

enum class Kind : uint8_t {
  Typing,      // one character typed at the cursor (no selection)
  Backspace,   // one character removed before the cursor
  DeleteFwd,   // one character removed after the cursor
  Other,       // anything else: paste, cut, typing over a selection, a run of text
};

// Use buf (cap bytes) for the history; cap 0 or a null buf turns undo off.
void init(uint8_t* buf, size_t cap);
void clear();          // another note opened: nothing to undo
void breakGroup();     // the next change starts a new step (the cursor moved, a note was saved by hand, ...)

// Called just before the editor replaces text[at, at + removedLen) (the bytes in `removed`) by `inserted`.
// A step bigger than the whole history cannot be kept: the history is cleared instead.
void record(int at, const char* removed, int removedLen, const char* inserted, int insertedLen, Kind kind,
            int cursorBefore);

bool canUndo();
bool canRedo();

// The change undo or redo makes: replace text[at, at + oldLen) by newText (newLen bytes), then put the cursor at
// cursor. newText points into the history and stays valid until the next call into edhist.
struct Change {
  int at;
  int oldLen;
  const char* newText;
  int newLen;
  int cursor;
};
bool undo(Change* out);   // false when there is nothing to undo
bool redo(Change* out);   // false when there is nothing to redo

size_t usedBytes();       // for the memory check and the tests
int stepCount();

}  // namespace edhist
