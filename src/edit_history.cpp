#include "edit_history.h"

#include <cstring>

namespace edhist {

// The buffer holds steps one after another, oldest first. A step is a header, the removed bytes, the inserted bytes
// and a trailer holding the step's total size (so the steps can be walked backwards from the end).
// [0, cur) are steps that can be undone (the newest is just before cur); [cur, used) can be redone.
struct Header {
  int32_t at;
  int32_t removedLen;
  int32_t insertedLen;
  int32_t cursorBefore;
  uint8_t kind;
  uint8_t pad[3];
};
static constexpr size_t kHeader = sizeof(Header);
static constexpr size_t kTrailer = sizeof(uint32_t);

static uint8_t* g_buf = nullptr;
static size_t g_cap = 0;
static size_t g_used = 0;
static size_t g_cur = 0;
static bool g_groupOpen = false;   // the newest step may still take the next typed character (or removed one)

static size_t stepSize(const Header& h) { return kHeader + (size_t)h.removedLen + (size_t)h.insertedLen + kTrailer; }

static Header headerAt(size_t off) {
  Header h;
  memcpy(&h, g_buf + off, kHeader);
  return h;
}

static void writeTrailer(size_t stepStart, size_t size) {
  const uint32_t s = (uint32_t)size;
  memcpy(g_buf + stepStart + size - kTrailer, &s, kTrailer);
}

// Offset of the newest step before end (end > 0).
static size_t stepBefore(size_t end) {
  uint32_t s;
  memcpy(&s, g_buf + end - kTrailer, kTrailer);
  return end - s;
}

void init(uint8_t* buf, size_t cap) {
  g_buf = buf;
  g_cap = buf ? cap : 0;
  clear();
}

void clear() {
  g_used = g_cur = 0;
  g_groupOpen = false;
}

void breakGroup() { g_groupOpen = false; }

static bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }

// Drops the oldest steps until `need` more bytes fit (at least a quarter of the buffer at a time, so a full history
// does not move its bytes on every key). Only steps that can be undone are dropped; the redo steps are already gone.
static bool makeRoom(size_t need) {
  if (need > g_cap) return false;
  if (g_used + need <= g_cap) return true;
  const size_t want = g_used + need - g_cap > g_cap / 4 ? g_used + need - g_cap : g_cap / 4;
  size_t off = 0;
  while (off < g_used && off < want) off += stepSize(headerAt(off));
  if (g_used + need - off > g_cap) return false;   // cannot happen: dropping everything frees all of it
  memmove(g_buf, g_buf + off, g_used - off);
  g_used -= off;
  g_cur -= off;
  return true;
}

// Can this one-character change join the newest step? Typing: right after what that step inserted, and not the first
// letter of a new word (a letter after a space starts a new step). Backspace and Delete: right next to what it removed,
// stopping the same way at a word's edge.
static bool joins(int at, const char* removed, int removedLen, const char* inserted, int insertedLen, Kind kind) {
  if (!g_groupOpen || g_cur != g_used || g_cur == 0) return false;
  const size_t last = stepBefore(g_cur);
  const Header h = headerAt(last);
  if (h.kind != (uint8_t)kind) return false;
  const char* hRemoved = (const char*)g_buf + last + kHeader;
  const char* hInserted = hRemoved + h.removedLen;
  switch (kind) {
    case Kind::Typing:
      if (at != h.at + h.insertedLen || removedLen != 0 || insertedLen <= 0) return false;
      // "the " + "c": the new word is a new step. Spaces after a word stay with it.
      return !(h.insertedLen > 0 && isSpace(hInserted[h.insertedLen - 1]) && !isSpace(inserted[0]));
    case Kind::Backspace:
      // Removing backwards: the new piece sits just before the removed run. Spaces first, then the word before them,
      // then stop: a space removed after letters starts a new step.
      if (at + removedLen != h.at || insertedLen != 0) return false;
      return !(h.removedLen > 0 && !isSpace(hRemoved[0]) && isSpace(removed[removedLen - 1]));
    case Kind::DeleteFwd:
      if (at != h.at || insertedLen != 0) return false;
      return !(h.removedLen > 0 && !isSpace(hRemoved[h.removedLen - 1]) && isSpace(removed[0]));
    default:
      return false;
  }
}

void record(int at, const char* removed, int removedLen, const char* inserted, int insertedLen, Kind kind,
            int cursorBefore) {
  if (!g_buf || (removedLen <= 0 && insertedLen <= 0)) return;
  if (joins(at, removed, removedLen, inserted, insertedLen, kind)) {
    const size_t grow = (size_t)removedLen + (size_t)insertedLen;
    size_t last = stepBefore(g_cur);
    if (g_used + grow > g_cap) {
      if (!makeRoom(grow)) { clear(); return; }
      if (g_cur == 0) {   // the open step itself was dropped: start a fresh one
        g_groupOpen = false;
        record(at, removed, removedLen, inserted, insertedLen, kind, cursorBefore);
        return;
      }
      last = stepBefore(g_cur);
    }
    Header h = headerAt(last);
    uint8_t* body = g_buf + last + kHeader;
    if (kind == Kind::Typing) {
      memcpy(body + h.removedLen + h.insertedLen, inserted, (size_t)insertedLen);
      h.insertedLen += insertedLen;
    } else if (kind == Kind::Backspace) {
      memmove(body + removedLen, body, (size_t)h.removedLen);   // the new piece goes in front
      memcpy(body, removed, (size_t)removedLen);
      h.removedLen += removedLen;
      h.at = at;
    } else {   // DeleteFwd: the new piece goes after
      memcpy(body + h.removedLen, removed, (size_t)removedLen);
      h.removedLen += removedLen;
    }
    memcpy(g_buf + last, &h, kHeader);
    const size_t size = stepSize(h);
    writeTrailer(last, size);
    g_used = g_cur = last + size;
    return;
  }

  g_used = g_cur;   // a new change drops whatever could have been redone
  Header h{};
  h.at = at;
  h.removedLen = removedLen;
  h.insertedLen = insertedLen;
  h.cursorBefore = cursorBefore;
  h.kind = (uint8_t)kind;
  const size_t size = stepSize(h);
  if (!makeRoom(size)) {   // bigger than the whole history: it cannot be undone, and older steps would be wrong
    clear();
    return;
  }
  const size_t off = g_used;
  memcpy(g_buf + off, &h, kHeader);
  if (removedLen > 0) memcpy(g_buf + off + kHeader, removed, (size_t)removedLen);
  if (insertedLen > 0) memcpy(g_buf + off + kHeader + removedLen, inserted, (size_t)insertedLen);
  writeTrailer(off, size);
  g_used = g_cur = off + size;
  g_groupOpen = kind != Kind::Other;
}

bool canUndo() { return g_buf && g_cur > 0; }
bool canRedo() { return g_buf && g_cur < g_used; }

bool undo(Change* out) {
  if (!canUndo()) return false;
  const size_t off = stepBefore(g_cur);
  const Header h = headerAt(off);
  out->at = h.at;
  out->oldLen = h.insertedLen;
  out->newText = (const char*)g_buf + off + kHeader;
  out->newLen = h.removedLen;
  out->cursor = h.cursorBefore;
  g_cur = off;
  g_groupOpen = false;
  return true;
}

bool redo(Change* out) {
  if (!canRedo()) return false;
  const Header h = headerAt(g_cur);
  out->at = h.at;
  out->oldLen = h.removedLen;
  out->newText = (const char*)g_buf + g_cur + kHeader + h.removedLen;
  out->newLen = h.insertedLen;
  out->cursor = h.at + h.insertedLen;
  g_cur += stepSize(h);
  g_groupOpen = false;
  return true;
}

size_t usedBytes() { return g_used; }

int stepCount() {
  int n = 0;
  for (size_t off = 0; off < g_used; off += stepSize(headerAt(off))) n++;
  return n;
}

}  // namespace edhist
