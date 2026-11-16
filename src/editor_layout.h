#pragma once
// Editor line layout with no device code, so it runs in host tests (test/editor).
// Wrapping is greedy and depends only on where a line starts: after a change, only the lines from just before the
// change are wrapped again, until a new line start meets an old one past the change; the rest of the table is the old
// one shifted by the size change. Typing costs the same in a 512 KB note as in a short one.
// Plan: MicroSlate-workspace/notes/v3.0-work-order.md, revision 2026-10-29.

#include <cstddef>
#include <cstdint>

namespace edlayout {

struct WrapParams {
  int pxMax = 0;                    // > 0: wrap by pixel width (advance per ASCII byte in pxW, pxFallback otherwise)
  const uint16_t* pxW = nullptr;    // 128 entries
  int pxFallback = 0;
  int charsPerLine = 40;            // used when pxMax == 0
};

// Line starts: pos[0] == 0, strictly increasing, count >= 1. grow(t, need) makes cap >= need and returns false when
// it cannot; the table then stops short (lines past cap are not laid out), as the fixed table did before.
struct LineTable {
  int* pos = nullptr;
  int count = 0;
  int cap = 0;
  bool truncated = false;   // the text has more lines than the table could hold
  bool (*grow)(LineTable& t, int need) = nullptr;
};

// Lays out the whole text.
void rebuild(LineTable& t, const char* text, int len, const WrapParams& p);

// After text[at, at + oldLen) was replaced by newLen bytes (the text is already changed, len is the new length),
// brings the table up to date. scratch (scratchCap ints) holds the new line starts of the changed region; when a change
// produces more lines than that, the whole text is laid out instead. Returns false when it had to lay out everything.
bool update(LineTable& t, const char* text, int len, int at, int oldLen, int newLen, const WrapParams& p,
            int* scratch, int scratchCap);

// The line holding byte offset pos (the last line whose start is <= pos).
int lineOf(const LineTable& t, int pos);

// True when replacing replaceLen bytes (<= textLen) of a textLen-byte text with insertLen bytes still fits a buffer of
// cap bytes, which keeps one byte for the terminating zero. Checked before anything is deleted, so a paste or key that
// does not fit leaves the text (and the selection) as it was.
inline bool replacementFits(size_t textLen, size_t replaceLen, size_t insertLen, size_t cap) {
  return textLen - replaceLen + insertLen < cap;
}

}  // namespace edlayout
