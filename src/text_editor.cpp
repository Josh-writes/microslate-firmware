#include "text_editor.h"
#include <cstring>
#include <algorithm>

// --- Text buffer ---
static char textBuffer[TEXT_BUFFER_SIZE];
static size_t textLength = 0;
static int cursorPosition = 0;

// --- File metadata ---
static char currentFile[MAX_FILENAME_LEN] = "";
static char currentTitle[MAX_TITLE_LEN] = "Untitled";
static bool unsavedChanges = false;

// --- Line management ---
static int linePositions[MAX_LINES];  // Index into textBuffer for start of each line
static int lineCount = 0;
static int cursorLine = 0;
static int cursorCol = 0;
static int viewportStartLine = 0;
static int charsPerLine = 40;
static int storedVisibleLines = 20;  // Updated by renderer each frame
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
  int lineStart = linePositions[lineIndex];
  int lineEnd = (lineIndex + 1 < lineCount) ? linePositions[lineIndex + 1] : (int)textLength;
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

static void deleteByteRange(int start, int end) {
  if (start < 0 || end <= start || start >= (int)textLength) return;
  end = std::min(end, (int)textLength);

  memmove(textBuffer + start, textBuffer + end, textLength - end);
  textLength -= (end - start);
  cursorPosition = start;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

// Recalculate line breaks (word wrap) and cursor position.
// The O(textLength) line break loop only runs when the buffer or charsPerLine changed.
// Cursor line/col is always recomputed (cheap O(cursorLine) with early exit).
void editorRecalculateLines() {
  if (lineBreaksDirty) {
    lineCount = 0;
    linePositions[0] = 0;
    lineCount = 1;

    int col = 0;
    int lastSpace = -1;

    for (int i = 0; i < (int)textLength && lineCount < MAX_LINES;) {
      if (textBuffer[i] == '\n') {
        // Hard line break
        if (lineCount < MAX_LINES) {
          linePositions[lineCount] = i + 1;
          lineCount++;
        }
        col = 0;
        lastSpace = -1;
        i++;
        continue;
      }

      int charEnd = utf8NextPos(i);
      if (textBuffer[i] == ' ') {
        lastSpace = i;
      }

      col++;
      if (col >= charsPerLine) {
        // Word wrap
        int breakPos;
        if (lastSpace > linePositions[lineCount - 1]) {
          breakPos = lastSpace + 1; // Break after space
        } else {
          breakPos = charEnd;  // Hard break mid-word, but never inside UTF-8
        }

        if (lineCount < MAX_LINES) {
          linePositions[lineCount] = breakPos;
          lineCount++;
        }
        col = utf8CountChars(breakPos, charEnd);
        lastSpace = -1;
      }

      i = charEnd;
    }
    lineBreaksDirty = false;
  }

  // Compute cursor line and column (always — cheap O(cursorLine) with early exit)
  cursorLine = 0;
  for (int i = 1; i < lineCount; i++) {
    if (cursorPosition >= linePositions[i]) {
      cursorLine = i;
    } else {
      break;
    }
  }
  cursorCol = utf8CountChars(linePositions[cursorLine], cursorPosition);
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
  if (viewportStartLine >= lineCount) viewportStartLine = std::max(0, lineCount - 1);
}

void editorInit() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  currentFile[0] = '\0';
  strncpy(currentTitle, "Untitled", MAX_TITLE_LEN - 1);
  unsavedChanges = false;
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editorRecalculateLines();
}

void editorClear() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  unsavedChanges = false;
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editorRecalculateLines();
}

void editorLoadBuffer(size_t length) {
  textLength = length;
  textBuffer[textLength] = '\0';
  cursorPosition = (int)textLength;  // Start at end
  viewportStartLine = 0;
  lineBreaksDirty = true;
  editorRecalculateLines();
  // Scroll to show cursor
  ensureCursorVisible(storedVisibleLines);
}

char* editorGetBuffer() { return textBuffer; }
size_t editorGetLength() { return textLength; }
int editorGetCursorPosition() { return cursorPosition; }

int editorGetWordCount() {
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
  return count;
}

void editorInsertChar(char c) {
  char text[2] = {c, '\0'};
  editorInsertText(text);
}

void editorInsertText(const char* text) {
  if (!text || text[0] == '\0') return;
  size_t insertLen = strlen(text);
  if (textLength + insertLen >= TEXT_BUFFER_SIZE) return;

  // Shift text right
  memmove(textBuffer + cursorPosition + insertLen,
          textBuffer + cursorPosition,
          textLength - cursorPosition);
  memcpy(textBuffer + cursorPosition, text, insertLen);
  cursorPosition += insertLen;
  textLength += insertLen;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;
  lineBreaksDirty = true;

  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorDeleteChar() {
  if (cursorPosition <= 0 || textLength == 0) return;

  int start = utf8PrevPos(cursorPosition);
  deleteByteRange(start, cursorPosition);
}

void editorDeleteForward() {
  if (cursorPosition >= (int)textLength) return;

  int end = utf8NextPos(cursorPosition);
  deleteByteRange(cursorPosition, end);
}

void editorMoveCursorLeft() {
  if (cursorPosition > 0) {
    cursorPosition = utf8PrevPos(cursorPosition);
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorRight() {
  if (cursorPosition < (int)textLength) {
    cursorPosition = utf8NextPos(cursorPosition);
    editorRecalculateLines();
    ensureCursorVisible(storedVisibleLines);
  }
}

void editorMoveCursorUp() {
  // cursorLine/cursorCol are already valid from the previous operation
  if (cursorLine <= 0) return;

  int targetLine = cursorLine - 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = lineContentEnd(targetLine);

  cursorPosition = byteOffsetForColumn(lineStart, lineEnd, cursorCol);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorDown() {
  if (cursorLine >= lineCount - 1) return;

  int targetLine = cursorLine + 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = lineContentEnd(targetLine);

  cursorPosition = byteOffsetForColumn(lineStart, lineEnd, cursorCol);
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorHome() {
  cursorPosition = linePositions[cursorLine];
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorMoveCursorEnd() {
  int lineEnd;
  if (cursorLine + 1 < lineCount) {
    lineEnd = linePositions[cursorLine + 1];
    // Step back over newline if present
    if (lineEnd > 0 && textBuffer[lineEnd - 1] == '\n') lineEnd--;
  } else {
    lineEnd = (int)textLength;
  }
  cursorPosition = lineEnd;
  editorRecalculateLines();
  ensureCursorVisible(storedVisibleLines);
}

void editorSetCharsPerLine(int cpl) {
  if (cpl != charsPerLine) {
    charsPerLine = cpl;
    lineBreaksDirty = true;
  }
  editorRecalculateLines();
}

void editorSetVisibleLines(int n) {
  if (n > 0) storedVisibleLines = n;
}

int editorGetStoredVisibleLines() {
  return storedVisibleLines;
}

int editorGetVisibleLines(int lineHeight, int textAreaHeight) {
  if (lineHeight <= 0) return 20;
  return textAreaHeight / lineHeight;
}

int editorGetViewportStart() { return viewportStartLine; }
int editorGetCursorLine() { return cursorLine; }
int editorGetCursorCol() { return cursorCol; }
int editorGetLineCount() { return lineCount; }

int editorGetLinePosition(int lineIndex) {
  if (lineIndex < 0 || lineIndex >= lineCount) return 0;
  return linePositions[lineIndex];
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
