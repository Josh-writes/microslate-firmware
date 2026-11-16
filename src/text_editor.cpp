#include "text_editor.h"
#include "editor_layout.h"
#include "edit_history.h"
#include "editor_prof.h"
#include <Arduino.h>
#include <cstring>
#include <algorithm>
#include <esp_heap_caps.h>

// --- Text buffer ---
// Allocated in editorInit(): from PSRAM when the board has it, otherwise from the heap.
static char* textBuffer = nullptr;
static size_t textLength = 0;
static int cursorPosition = 0;

// --- Selection + clipboard ---
// selectionAnchor < 0 means no selection. When >= 0, the selection is the range
// between it and cursorPosition (empty if they coincide). The clipboard is a
// separate buffer that persists across notes (never cleared on load/clear).
static int selectionAnchor = -1;
static char* clipboard = nullptr;
static size_t clipboardLen = 0;

// Set when a newline is inserted; consumed by the autosave (paragraph-break flush).
static bool paragraphBreakPending = false;

// millis() of the last key or paste refused because the note is full (0: none); the header shows "Note full".
static uint32_t noteFullAtMs = 0;
static constexpr uint32_t kNoteFullShowMs = 3000;

// --- File metadata ---
static char currentFile[MAX_FILENAME_LEN] = "";
static char currentTitle[MAX_TITLE_LEN] = "Untitled";
static bool unsavedChanges = false;

// --- Line management ---
// Line starts (editor_layout.h). The table grows as the note gets longer, up to MAX_LINES. After an edit only the lines
// around it are wrapped again; lineBreaksDirty asks for a full layout (a note loaded, the font or width changed).
static edlayout::LineTable lines;
static int layoutScratch[256];   // new line starts of one edit; more than this and the whole note is laid out
static uint32_t editGen = 0;     // bumped by every change to the text (the word count cache)
static int cursorLine = 0;
static int cursorCol = 0;
static int viewportStartLine = 0;
static int charsPerLine = 40;
static int storedVisibleLines = 20;  // Updated by renderer each frame
static uint16_t pxW[128];
static int pxFallback = 0;
static int pxMax = 0;  // >0: wrap by pixel width instead of character count
static bool lineBreaksDirty = true;  // Only recompute line breaks when buffer/charsPerLine changes

// Forward declaration
static void ensureCursorVisible(int visibleLines);

static bool isUtf8ContinuationByte(unsigned char c) {
  return (c & 0xC0) == 0x80;
}

static int utf8CharLenAt(int pos) {
  if (pos < 0 || pos >= (int)textLength) return 0;
  unsigned char c = static_cast<unsigned char>(textBuffer[pos]);
  int len = 1;
  if (c < 0x80) len = 1;
  else if ((c >> 5) == 0x6) len = 2;
  else if ((c >> 4) == 0xE) len = 3;
  else if ((c >> 3) == 0x1E) len = 4;
  if (pos + len > (int)textLength) return 1;
  return len;
}

static int utf8NextPos(int pos) {
  if (pos >= (int)textLength) return (int)textLength;
  int next = pos + utf8CharLenAt(pos);
  return std::min(next, (int)textLength);
}

static int utf8PrevPos(int pos) {
  if (pos <= 0) return 0;
  pos--;
  while (pos > 0 && isUtf8ContinuationByte(static_cast<unsigned char>(textBuffer[pos]))) {
    pos--;
  }
  return pos;
}

static int utf8CountChars(int start, int end) {
  start = std::max(0, start);
  end = std::min(end, (int)textLength);
  int count = 0;
  for (int i = start; i < end; i = utf8NextPos(i)) {
    count++;
  }
  return count;
}

static int lineContentEnd(int lineIndex) {
  int lineStart = lines.pos[lineIndex];
  int lineEnd = (lineIndex + 1 < lines.count) ? lines.pos[lineIndex + 1] : (int)textLength;
  if (lineEnd > lineStart && textBuffer[lineEnd - 1] == '\n') lineEnd--;
  return lineEnd;
}

static int byteOffsetForColumn(int lineStart, int lineEnd, int col) {
  int pos = lineStart;
  for (int i = 0; i < col && pos < lineEnd; i++) {
    pos = utf8NextPos(pos);
  }
  return std::min(pos, lineEnd);
}

static edlayout::WrapParams wrapParams() {
  edlayout::WrapParams p;
  p.pxMax = pxMax;
  p.pxW = pxW;
  p.pxFallback = pxFallback;
  p.charsPerLine = charsPerLine;
  return p;
}

// The line table grows in steps up to MAX_LINES (PSRAM when the board has it).
static bool growLines(edlayout::LineTable& t, int need) {
  if (need > MAX_LINES) return false;
  int cap = t.cap ? t.cap : 1024;
  while (cap < need) cap *= 2;
  if (cap > MAX_LINES) cap = MAX_LINES;
  const size_t bytes = (size_t)cap * sizeof(int);
  void* p = heap_caps_realloc(t.pos, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  const bool inPsram = p != nullptr;
  if (!p) p = heap_caps_realloc(t.pos, bytes, MALLOC_CAP_8BIT);
  DBG_PRINTF("[EDIT] line table to %d lines (%u bytes) in %s\n", cap, (unsigned)bytes,
             inPsram ? "PSRAM" : (p ? "INTERNAL RAM" : "nothing: no memory"));
  if (!p) return false;
  t.pos = (int*)p;
  t.cap = cap;
  return true;
}

// text[at, at + oldLen) became newLen bytes: lay out the lines around the change (or everything later, when a full
// layout is already due).
static void textChanged(int at, int oldLen, int newLen) {
  unsavedChanges = true;
  editGen++;
  if (!lineBreaksDirty) {
    MS_PROF("wrap");
    edlayout::update(lines, textBuffer, (int)textLength, at, oldLen, newLen, wrapParams(), layoutScratch,
                     (int)(sizeof(layoutScratch) / sizeof(layoutScratch[0])));
  }
}

// The undo history keeps what a change removes and inserts (edit_history.h). Called just before the change.
static void recordChange(int at, int removedLen, const char* inserted, int insertedLen, edhist::Kind kind) {
  edhist::record(at, textBuffer + at, removedLen, inserted, insertedLen, kind, cursorPosition);
}

// True when text is exactly one UTF-8 character (a typed key), which undo groups into words.
static bool isOneChar(const char* text, size_t len) {
  const unsigned char c = (unsigned char)text[0];
  const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
  return n == len;
}

// Undo and redo: replace text[at, at + oldLen) by newLen bytes, with no history of its own.
static void replaceRaw(int at, int oldLen, const char* text, int newLen) {
  memmove(textBuffer + at + newLen, textBuffer + at + oldLen, textLength - at - oldLen);
  if (newLen > 0) memcpy(textBuffer + at, text, (size_t)newLen);
  textLength = textLength - oldLen + newLen;
  textBuffer[textLength] = '\0';
  textChanged(at, oldLen, newLen);
}

static bool applyHistory(bool (*step)(edhist::Change*)) {
  edhist::Change ch;
  if (!step(&ch)) return false;
  selectionAnchor = -1;
  replaceRaw(ch.at, ch.oldLen, ch.newText, ch.newLen);
  cursorPosition = std::max(0, std::min(ch.cursor, (int)textLength));
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
  return true;
}

bool editorUndo() { return applyHistory(edhist::undo); }
bool editorRedo() { return applyHistory(edhist::redo); }

static void deleteByteRange(int start, int end) {
  if (start < 0 || end <= start || start >= (int)textLength) return;
  end = std::min(end, (int)textLength);

  memmove(textBuffer + start, textBuffer + end, textLength - end);
  textLength -= (end - start);
  cursorPosition = start;
  textBuffer[textLength] = '\0';
  textChanged(start, end - start, 0);

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// Lay out the whole note when it is due (lineBreaksDirty), then find the cursor's line and column.
void editorRecalculateLines() {
  if (lineBreaksDirty) {
    MS_PROF("layout");
    edlayout::rebuild(lines, textBuffer, (int)textLength, wrapParams());
    lineBreaksDirty = false;
  }
  if (lines.count <= 0) {   // no memory for even one line: behave as one empty line
    cursorLine = 0;
    cursorCol = 0;
    return;
  }
  cursorLine = edlayout::lineOf(lines, cursorPosition);
  cursorCol = utf8CountChars(lines.pos[cursorLine], cursorPosition);
}

// Ensure cursor is visible by adjusting viewport
static void ensureCursorVisible(int visibleLines) {
  if (visibleLines <= 0) visibleLines = 20; // fallback

  if (cursorLine < viewportStartLine) {
    viewportStartLine = cursorLine;
  } else if (cursorLine >= viewportStartLine + visibleLines) {
    viewportStartLine = cursorLine - visibleLines + 1;
  }

  if (viewportStartLine < 0) viewportStartLine = 0;
  if (viewportStartLine >= lines.count) viewportStartLine = std::max(0, lines.count - 1);
}

static void* editorAlloc(size_t bytes) {
  void* p = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(1, bytes, MALLOC_CAP_8BIT);
  return p;
}

void editorInit() {
  if (!textBuffer) {
    textBuffer = (char*)editorAlloc(TEXT_BUFFER_SIZE);
    clipboard = (char*)editorAlloc(TEXT_BUFFER_SIZE);
    lines.grow = growLines;
    growLines(lines, 1);
    uint8_t* hist = (uint8_t*)editorAlloc(UNDO_HISTORY_SIZE);   // no memory: undo is off, nothing else changes
    edhist::init(hist, hist ? UNDO_HISTORY_SIZE : 0);
  }
  edhist::clear();
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  selectionAnchor = -1;
  currentFile[0] = '\0';
  strncpy(currentTitle, "Untitled", MAX_TITLE_LEN - 1);
  unsavedChanges = false;
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editGen++;
  editorRecalculateLines();
}

void editorClear() {
  edhist::clear();   // a new note: nothing to undo
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  selectionAnchor = -1;
  unsavedChanges = false;
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editGen++;
  editorRecalculateLines();
}

void editorLoadBuffer(size_t length) {
#if defined(MS_EDITOR_PROF)
  // In-place speed check on the real note buffer, right after load. It must move all `length` bytes both ways: moving
  // length - 1 (as it did until 2026-11-01) left the last byte a copy of the one before it, and a save then wrote
  // that changed last character to the card. There is always room for one more byte (the terminator's place).
  if (length > 2 && length < TEXT_BUFFER_SIZE) {
    const uint32_t t0 = micros();
    memmove(textBuffer + 1, textBuffer, length);
    const uint32_t t1 = micros();
    memmove(textBuffer, textBuffer + 1, length);
    Serial.printf("[BENCH] load %u bytes: memmove in the note buffer = %u us\n", (unsigned)length, (unsigned)(t1 - t0));
  }
#endif
  edhist::clear();   // another note opened: nothing to undo
  textLength = length;
  textBuffer[textLength] = '\0';
  cursorPosition = (int)textLength;  // Start at end
  selectionAnchor = -1;
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editGen++;
  editorRecalculateLines();
  // Scroll to show cursor
  ensureCursorVisible(storedVisibleLines);
}

char* editorGetBuffer() { return textBuffer; }
size_t editorGetLength() { return textLength; }
int editorGetCursorPosition() { return cursorPosition; }

// Counted again only after the text changed (a 512 KB note takes a while to count, and the header asks every redraw).
int editorGetWordCount() {
  static uint32_t countedGen = 0xFFFFFFFF;
  static int cached = 0;
  if (countedGen == editGen) return cached;
  MS_PROF("words");
  int count = 0;
  bool inWord = false;
  for (size_t i = 0; i < textLength; i++) {
    char c = textBuffer[i];
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
      inWord = false;
    } else {
      if (!inWord) { count++; inWord = true; }
    }
  }
  countedGen = editGen;
  cached = count;
  return count;
}

void editorInsertText(const char* text) {
  MS_PROF("insert");
  if (!text || text[0] == '\0') return;
  size_t insertLen = strlen(text);
  int lo, hi;
  const bool replacing = editorGetSelectionRange(&lo, &hi);
  // Too big for the note: change nothing (not even the selection) and say so in the header.
  if (!edlayout::replacementFits(textLength, replacing ? (size_t)(hi - lo) : 0, insertLen, TEXT_BUFFER_SIZE)) {
    noteFullAtMs = millis() | 1;
    DBG_PRINTF("[EDIT] note full: %u bytes do not fit (%u of %u used)\n", (unsigned)insertLen, (unsigned)textLength,
               (unsigned)(TEXT_BUFFER_SIZE - 1));
    return;
  }
  if (strchr(text, '\n')) paragraphBreakPending = true;  // flag paragraph breaks for autosave
  if (replacing) recordChange(lo, hi - lo, text, (int)insertLen, edhist::Kind::Other);
  else recordChange(cursorPosition, 0, text, (int)insertLen,
                    isOneChar(text, insertLen) ? edhist::Kind::Typing : edhist::Kind::Other);
  // Typing over a selection replaces it.
  if (replacing) {
    selectionAnchor = -1;
    deleteByteRange(lo, hi);  // cursor now at lo
  }

  // Shift text right
  MS_PROF("memmove");
  memmove(textBuffer + cursorPosition + insertLen,
          textBuffer + cursorPosition,
          textLength - cursorPosition);
  memcpy(textBuffer + cursorPosition, text, insertLen);
  const int at = cursorPosition;
  cursorPosition += insertLen;
  textLength += insertLen;
  textBuffer[textLength] = '\0';
  textChanged(at, 0, (int)insertLen);

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorDeleteChar() {
  if (editorHasSelection()) {
    int lo, hi;
    editorGetSelectionRange(&lo, &hi);
    selectionAnchor = -1;
    recordChange(lo, hi - lo, nullptr, 0, edhist::Kind::Other);
    deleteByteRange(lo, hi);
    return;
  }
  if (cursorPosition <= 0 || textLength == 0) return;

  int start = utf8PrevPos(cursorPosition);
  recordChange(start, cursorPosition - start, nullptr, 0, edhist::Kind::Backspace);
  deleteByteRange(start, cursorPosition);
}

void editorDeleteForward() {
  if (editorHasSelection()) {
    int lo, hi;
    editorGetSelectionRange(&lo, &hi);
    selectionAnchor = -1;
    recordChange(lo, hi - lo, nullptr, 0, edhist::Kind::Other);
    deleteByteRange(lo, hi);
    return;
  }
  if (cursorPosition >= (int)textLength) return;

  int end = utf8NextPos(cursorPosition);
  recordChange(cursorPosition, end - cursorPosition, nullptr, 0, edhist::Kind::DeleteFwd);
  deleteByteRange(cursorPosition, end);
}

// Called at the start of a cursor move: extend=true arms/keeps the selection
// anchor; extend=false drops any selection.
static void updateSelectionAnchor(bool extend) {
  if (extend) {
    if (selectionAnchor < 0) selectionAnchor = cursorPosition;
  } else {
    selectionAnchor = -1;
  }
}

void editorMoveCursorLeft(bool extend) {
  // Plain Left with a selection collapses to its left edge, without moving further.
  if (!extend && editorHasSelection()) {
    int lo, hi;
    editorGetSelectionRange(&lo, &hi);
    selectionAnchor = -1;
    cursorPosition = lo;
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
    return;
  }
  updateSelectionAnchor(extend);
  if (cursorPosition > 0) {
    cursorPosition = utf8PrevPos(cursorPosition);
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorRight(bool extend) {
  if (!extend && editorHasSelection()) {
    int lo, hi;
    editorGetSelectionRange(&lo, &hi);
    selectionAnchor = -1;
    cursorPosition = hi;
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
    return;
  }
  updateSelectionAnchor(extend);
  if (cursorPosition < (int)textLength) {
    cursorPosition = utf8NextPos(cursorPosition);
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorUp(bool extend) {
  updateSelectionAnchor(extend);
  // cursorLine/cursorCol are already valid from the previous operation
  if (cursorLine <= 0) return;

  int targetLine = cursorLine - 1;
  int lineStart = lines.pos[targetLine];
  int lineEnd = lineContentEnd(targetLine);

  cursorPosition = byteOffsetForColumn(lineStart, lineEnd, cursorCol);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorDown(bool extend) {
  updateSelectionAnchor(extend);
  if (cursorLine >= lines.count - 1) return;

  int targetLine = cursorLine + 1;
  int lineStart = lines.pos[targetLine];
  int lineEnd = lineContentEnd(targetLine);

  cursorPosition = byteOffsetForColumn(lineStart, lineEnd, cursorCol);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorHome(bool extend) {
  updateSelectionAnchor(extend);
  cursorPosition = lines.pos[cursorLine];
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorEnd(bool extend) {
  updateSelectionAnchor(extend);
  int lineEnd;
  if (cursorLine + 1 < lines.count) {
    lineEnd = lines.pos[cursorLine + 1];
    // Step back over newline if present
    if (lineEnd > 0 && textBuffer[lineEnd - 1] == '\n') lineEnd--;
  } else {
    lineEnd = (int)textLength;
  }
  cursorPosition = lineEnd;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// --- Touch / pointer positioning ---

// Pull a byte offset into [0, textLength] and onto a UTF-8 character boundary.
static int snapToBoundary(int pos) {
  if (pos < 0) return 0;
  if (pos > (int)textLength) return (int)textLength;
  while (pos > 0 && pos < (int)textLength && isUtf8ContinuationByte(static_cast<unsigned char>(textBuffer[pos]))) {
    pos--;
  }
  return pos;
}

void editorSetCursorPosition(int pos, bool extend) {
  updateSelectionAnchor(extend);
  cursorPosition = snapToBoundary(pos);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// Letters, digits and every multi-byte UTF-8 byte count as word bytes, so words in
// any script stay whole. An apostrophe is word-internal ("don't") but is trimmed
// from the ends of the selection.
static bool isWordByte(unsigned char c) {
  if (c >= 0x80) return true;
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '\'';
}

bool editorGetWordBounds(int pos, int* lo, int* hi) {
  const int len = (int)textLength;
  if (len == 0) return false;
  int p = snapToBoundary(pos);
  bool onWord = p < len && isWordByte(static_cast<unsigned char>(textBuffer[p]));
  // A press just past the end of a word still means that word.
  if (!onWord && p > 0 && isWordByte(static_cast<unsigned char>(textBuffer[p - 1]))) {
    p--;
    onWord = true;
  }
  if (!onWord) return false;

  int a = p;
  while (a > 0 && isWordByte(static_cast<unsigned char>(textBuffer[a - 1]))) a--;
  int b = p;
  while (b < len && isWordByte(static_cast<unsigned char>(textBuffer[b]))) b++;
  while (a < b && textBuffer[a] == '\'') a++;
  while (b > a && textBuffer[b - 1] == '\'') b--;
  if (b <= a) return false;
  if (lo) *lo = a;
  if (hi) *hi = b;
  return true;
}

bool editorSelectWordAt(int pos) {
  int lo, hi;
  if (!editorGetWordBounds(pos, &lo, &hi)) return false;
  selectionAnchor = lo;
  cursorPosition = hi;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
  return true;
}

void editorSelectRange(int anchor, int cursor) {
  selectionAnchor = snapToBoundary(anchor);
  cursorPosition = snapToBoundary(cursor);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// Scroll the viewport without touching the cursor (drag-to-scroll). The next
// cursor move or keystroke brings the cursor back into view, as on a phone.
void editorScrollViewport(int deltaLines) {
  int maxStart = lines.count - storedVisibleLines;
  if (maxStart < 0) maxStart = 0;
  viewportStartLine += deltaLines;
  if (viewportStartLine > maxStart) viewportStartLine = maxStart;
  if (viewportStartLine < 0) viewportStartLine = 0;
}

// --- Selection + clipboard ---

bool editorHasSelection() {
  return selectionAnchor >= 0 && selectionAnchor != cursorPosition;
}

bool editorGetSelectionRange(int* lo, int* hi) {
  if (!editorHasSelection()) return false;
  int a = selectionAnchor;
  int b = cursorPosition;
  if (a > b) { int t = a; a = b; b = t; }
  if (lo) *lo = a;
  if (hi) *hi = b;
  return true;
}

void editorSelectAll() {
  if (textLength == 0) return;
  selectionAnchor = 0;
  cursorPosition = (int)textLength;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

bool editorHasClipboard() { return clipboardLen > 0; }

void editorCopy() {
  int lo, hi;
  if (!editorGetSelectionRange(&lo, &hi)) return;
  size_t n = (size_t)(hi - lo);
  if (n >= TEXT_BUFFER_SIZE) n = TEXT_BUFFER_SIZE - 1;
  memcpy(clipboard, textBuffer + lo, n);
  clipboard[n] = '\0';
  clipboardLen = n;
}

void editorCut() {
  int lo, hi;
  if (!editorGetSelectionRange(&lo, &hi)) return;
  editorCopy();
  selectionAnchor = -1;
  recordChange(lo, hi - lo, nullptr, 0, edhist::Kind::Other);
  deleteByteRange(lo, hi);  // sets cursor = lo, recalculates
}

void editorPaste() {
  if (clipboardLen == 0) return;
  DBG_PRINTF("[EDIT] paste %u bytes at %d of %u\n", (unsigned)clipboardLen, cursorPosition, (unsigned)textLength);
  editorInsertText(clipboard);  // replaces an active selection; changes nothing if the result would not fit
}

bool editorFindNext(const char* query) {
  if (!query || query[0] == '\0' || textLength == 0) return false;
  int qlen = (int)strlen(query);
  if (qlen > (int)textLength) return false;

  // Search forward from the caret, then wrap to the start. Plain byte compare is
  // UTF-8 safe: the query's bytes only match a whole character's bytes in the buffer.
  int from = cursorPosition;
  int match = -1;
  for (int i = from; i + qlen <= (int)textLength; i++) {
    if (memcmp(textBuffer + i, query, qlen) == 0) { match = i; break; }
  }
  if (match < 0) {
    for (int i = 0; i + qlen <= from && i + qlen <= (int)textLength; i++) {
      if (memcmp(textBuffer + i, query, qlen) == 0) { match = i; break; }
    }
  }
  if (match < 0) return false;

  selectionAnchor = match;             // highlight the hit via the selection renderer
  cursorPosition = match + qlen;       // caret past the match so Enter finds the next
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
  return true;
}

bool editorNoteFullShowing() {
  if (noteFullAtMs && millis() - noteFullAtMs >= kNoteFullShowMs) noteFullAtMs = 0;
  return noteFullAtMs != 0;
}

bool editorHasParagraphBreakPending() { return paragraphBreakPending; }
void editorClearParagraphBreak() { paragraphBreakPending = false; }

void editorSetCharsPerLine(int cpl) {
  if (cpl != charsPerLine) {
    charsPerLine = cpl;
    lineBreaksDirty = true;
  }
  editorRecalculateLines();
}

void editorSetPixelWrap(int maxPx, const uint16_t* w128, int fallback) {
  bool changed = maxPx != pxMax || fallback != pxFallback ||
                 (w128 && memcmp(pxW, w128, sizeof(pxW)) != 0);
  pxMax = maxPx;
  pxFallback = fallback;
  if (w128) memcpy(pxW, w128, sizeof(pxW));
  if (changed) lineBreaksDirty = true;
}

void editorSetVisibleLines(int n) {
  if (n > 0) storedVisibleLines = n;
}

int editorGetStoredVisibleLines() {
  return storedVisibleLines;
}

int editorGetViewportStart() { return viewportStartLine; }
int editorGetCursorLine() { return cursorLine; }
int editorGetLineCount() { return lines.count; }

int editorGetLinePosition(int lineIndex) {
  if (lineIndex < 0 || lineIndex >= lines.count) return 0;
  return lines.pos[lineIndex];
}

void editorSetCurrentFile(const char* filename) {
  strncpy(currentFile, filename, MAX_FILENAME_LEN - 1);
  currentFile[MAX_FILENAME_LEN - 1] = '\0';
}

void editorSetCurrentTitle(const char* title) {
  strncpy(currentTitle, title, MAX_TITLE_LEN - 1);
  currentTitle[MAX_TITLE_LEN - 1] = '\0';
}

const char* editorGetCurrentFile() { return currentFile; }
const char* editorGetCurrentTitle() { return currentTitle; }
bool editorHasUnsavedChanges() { return unsavedChanges; }
void editorSetUnsavedChanges(bool v) { unsavedChanges = v; }
