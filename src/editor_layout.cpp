#include "editor_layout.h"

#include <cstring>

namespace edlayout {

static int charEndAt(const char* t, int len, int i) {
  const unsigned char c = (unsigned char)t[i];
  int n = 1;
  if ((c >> 5) == 0x6) n = 2;
  else if ((c >> 4) == 0xE) n = 3;
  else if ((c >> 3) == 0x1E) n = 4;
  return i + n > len ? i + 1 : i + n;
}

// Lays out lines from `start` (a line start) and calls emit(s) for each following line start until emit returns false
// or the text ends. Every line is wrapped from a fresh state (after a word wrap the scan goes back to the new line's
// start), so a line's breaks depend only on where it starts and the text from there on. That is what lets update()
// stop as soon as a new line start meets an old one.
template <class Emit>
static void wrapFrom(const char* t, int len, int start, const WrapParams& p, Emit emit) {
  int lineStart = start;
  int col = 0, px = 0, lastSpace = -1;
  for (int i = start; i < len;) {
    if (t[i] == '\n') {   // hard line break
      if (!emit(i + 1)) return;
      lineStart = i + 1;
      col = px = 0;
      lastSpace = -1;
      i++;
      continue;
    }
    const int charEnd = charEndAt(t, len, i);
    if (t[i] == ' ') lastSpace = i;
    col++;
    bool wrapNow;
    if (p.pxMax > 0) {
      const unsigned char c = (unsigned char)t[i];
      const int w = c < 128 ? p.pxW[c] : p.pxFallback;
      px += w;
      wrapNow = c != ' ' && px > p.pxMax && i > lineStart;
      if (wrapNow && lastSpace <= lineStart) {
        // No space on this line: break before this character (a word wider than the line).
        if (!emit(i)) return;
        lineStart = i;
        col = 1;
        px = w;
        lastSpace = -1;
        wrapNow = false;
      }
    } else {
      wrapNow = col >= p.charsPerLine;
    }
    if (wrapNow) {
      // After the last space on the line, or (character mode, no space) after this character.
      const int breakPos = lastSpace > lineStart ? lastSpace + 1 : charEnd;
      if (!emit(breakPos)) return;
      lineStart = breakPos;
      col = px = 0;
      lastSpace = -1;
      i = breakPos;   // lay the new line out from its start
      continue;
    }
    i = charEnd;
  }
}

static bool reserve(LineTable& t, int need) {
  if (need <= t.cap) return true;
  return t.grow && t.grow(t, need) && t.cap >= need;
}

void rebuild(LineTable& t, const char* text, int len, const WrapParams& p) {
  t.count = 0;
  t.truncated = false;
  if (!reserve(t, 1)) return;
  t.pos[0] = 0;
  t.count = 1;
  wrapFrom(text, len, 0, p, [&](int s) {
    if (!reserve(t, t.count + 1)) {   // out of room: the rest is not laid out
      t.truncated = true;
      return false;
    }
    t.pos[t.count++] = s;
    return true;
  });
}

int lineOf(const LineTable& t, int pos) {
  int lo = 0, hi = t.count - 1;
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (t.pos[mid] <= pos) lo = mid;
    else hi = mid - 1;
  }
  return lo < 0 ? 0 : lo;
}

bool update(LineTable& t, const char* text, int len, int at, int oldLen, int newLen, const WrapParams& p,
            int* scratch, int scratchCap) {
  if (t.count <= 0 || t.truncated) {   // a cut-short table has no true tail to keep
    rebuild(t, text, len, p);
    return false;
  }
  const int d = newLen - oldLen;
  const int oldEnd = at + oldLen;
  // Line starts up to two lines before the change are still right: the decision that ended the line before line L-1
  // looked at text no further than line L-1's own start (see notes in the host test).
  const int L = lineOf(t, at);
  const int R = L >= 2 ? L - 2 : 0;

  int n = 0;
  int j = R + 1;     // old lines, scanned forward to find where the new layout meets the old one
  int meet = -1;
  bool full = false;
  wrapFrom(text, len, t.pos[R], p, [&](int s) {
    while (j < t.count && (t.pos[j] < oldEnd || t.pos[j] + d < s)) j++;
    if (j < t.count && t.pos[j] + d == s) {   // same start, same text from here on: the rest is unchanged
      meet = j;
      return false;
    }
    if (n >= scratchCap) {
      full = true;
      return false;
    }
    scratch[n++] = s;
    return true;
  });
  if (full) {
    rebuild(t, text, len, p);
    return false;
  }

  const int tail = meet >= 0 ? t.count - meet : 0;
  const int need = R + 1 + n + tail;
  if (!reserve(t, need)) {
    rebuild(t, text, len, p);
    return false;
  }
  if (tail) {
    if (meet != R + 1 + n) memmove(&t.pos[R + 1 + n], &t.pos[meet], (size_t)tail * sizeof(int));
    if (d) {
      int* q = &t.pos[R + 1 + n];
      for (int k = 0; k < tail; k++) q[k] += d;
    }
  }
  if (n) memcpy(&t.pos[R + 1], scratch, (size_t)n * sizeof(int));
  t.count = need;
  return true;
}

}  // namespace edlayout
