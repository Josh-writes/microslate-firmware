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

// Forward declaration
static void ensureCursorVisible(int visibleLines);

// Recalculate line breaks (word wrap)
void editorRecalculateLines() {
  lineCount = 0;
  linePositions[0] = 0;
  lineCount = 1;

  int col = 0;
  int lastSpace = -1;

  for (int i = 0; i < (int)textLength && lineCount < MAX_LINES; i++) {
    if (textBuffer[i] == '\n') {
      // Hard line break
      if (lineCount < MAX_LINES) {
        linePositions[lineCount] = i + 1;
        lineCount++;
      }
      col = 0;
      lastSpace = -1;
      continue;
    }

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
        breakPos = i + 1;  // Hard break mid-word
      }

      if (lineCount < MAX_LINES) {
        linePositions[lineCount] = breakPos;
        lineCount++;
      }
      col = i + 1 - breakPos;
      lastSpace = -1;
    }
  }

  // Compute cursor line and column
  cursorLine = 0;
  for (int i = 1; i < lineCount; i++) {
    if (cursorPosition >= linePositions[i]) {
      cursorLine = i;
    } else {
      break;
    }
  }
  cursorCol = cursorPosition - linePositions[cursorLine];
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
  editorRecalculateLines();
}

void editorClear() {
  memset(textBuffer, 0, TEXT_BUFFER_SIZE);
  textLength = 0;
  cursorPosition = 0;
  unsavedChanges = false;
  viewportStartLine = 0;
  editorRecalculateLines();
}

void editorLoadBuffer(size_t length) {
  textLength = length;
  textBuffer[textLength] = '\0';
  cursorPosition = (int)textLength;  // Start at end
  viewportStartLine = 0;
  editorRecalculateLines();
  // Scroll to show cursor
  ensureCursorVisible(20);
}

char* editorGetBuffer() { return textBuffer; }
size_t editorGetLength() { return textLength; }
int editorGetCursorPosition() { return cursorPosition; }

void editorInsertChar(char c) {
  if (textLength >= TEXT_BUFFER_SIZE - 1) return;

  // Shift text right
  for (int i = (int)textLength; i > cursorPosition; i--) {
    textBuffer[i] = textBuffer[i - 1];
  }
  textBuffer[cursorPosition] = c;
  cursorPosition++;
  textLength++;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;

  editorRecalculateLines();
  ensureCursorVisible(20);
}

void editorDeleteChar() {
  if (cursorPosition <= 0 || textLength == 0) return;

  for (int i = cursorPosition - 1; i < (int)textLength - 1; i++) {
    textBuffer[i] = textBuffer[i + 1];
  }
  cursorPosition--;
  textLength--;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;

  editorRecalculateLines();
  ensureCursorVisible(20);
}

void editorDeleteForward() {
  if (cursorPosition >= (int)textLength) return;

  for (int i = cursorPosition; i < (int)textLength - 1; i++) {
    textBuffer[i] = textBuffer[i + 1];
  }
  textLength--;
  textBuffer[textLength] = '\0';
  unsavedChanges = true;

  editorRecalculateLines();
  ensureCursorVisible(20);
}

void editorMoveCursorLeft() {
  if (cursorPosition > 0) {
    cursorPosition--;
    editorRecalculateLines();
    ensureCursorVisible(20);
  }
}

void editorMoveCursorRight() {
  if (cursorPosition < (int)textLength) {
    cursorPosition++;
    editorRecalculateLines();
    ensureCursorVisible(20);
  }
}

void editorMoveCursorUp() {
  editorRecalculateLines();
  if (cursorLine <= 0) return;

  int targetLine = cursorLine - 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  int lineLen = lineEnd - lineStart;
  // Don't count trailing newline
  if (lineLen > 0 && textBuffer[lineStart + lineLen - 1] == '\n') lineLen--;

  cursorPosition = lineStart + std::min(cursorCol, lineLen);
  editorRecalculateLines();
  ensureCursorVisible(20);
}

void editorMoveCursorDown() {
  editorRecalculateLines();
  if (cursorLine >= lineCount - 1) return;

  int targetLine = cursorLine + 1;
  int lineStart = linePositions[targetLine];
  int lineEnd = (targetLine + 1 < lineCount) ? linePositions[targetLine + 1] : (int)textLength;
  int lineLen = lineEnd - lineStart;
  if (lineLen > 0 && textBuffer[lineStart + lineLen - 1] == '\n') lineLen--;

  cursorPosition = lineStart + std::min(cursorCol, lineLen);
  editorRecalculateLines();
  ensureCursorVisible(20);
}

void editorMoveCursorHome() {
  editorRecalculateLines();
  cursorPosition = linePositions[cursorLine];
  editorRecalculateLines();
  ensureCursorVisible(20);
}

void editorMoveCursorEnd() {
  editorRecalculateLines();
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
  ensureCursorVisible(20);
}

void editorSetCharsPerLine(int cpl) {
  charsPerLine = cpl;
  editorRecalculateLines();
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
