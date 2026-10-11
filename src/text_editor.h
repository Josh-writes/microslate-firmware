#pragma once

#include "config.h"

void editorInit();
void editorClear();
void editorLoadBuffer(size_t length);  // After filling buffer externally, set length + reset cursor

// Buffer access
char* editorGetBuffer();
size_t editorGetLength();
int editorGetCursorPosition();

// Editing operations
void editorInsertChar(char c);
void editorInsertText(const char* text);
void editorDeleteChar();     // Backspace
void editorDeleteForward();  // Delete key

// Cursor movement. extend=true keeps an anchor so the range between the anchor
// and the cursor becomes the selection (Shift+arrow). extend=false collapses an
// existing selection: Left/Right jump to its edge and stop; the others drop the
// selection and move from the caret.
void editorMoveCursorLeft(bool extend = false);
void editorMoveCursorRight(bool extend = false);
void editorMoveCursorUp(bool extend = false);
void editorMoveCursorDown(bool extend = false);
void editorMoveCursorHome(bool extend = false);
void editorMoveCursorEnd(bool extend = false);

// Selection + clipboard. Selection is empty when anchor == cursor; the range is a
// half-open byte interval [lo, hi) on character boundaries. The clipboard is
// internal to the device and survives opening/creating another note.
void editorSelectAll();
bool editorHasSelection();
bool editorGetSelectionRange(int* lo, int* hi);
void editorCopy();
void editorCut();
void editorPaste();

// Line/viewport management
void editorSetCharsPerLine(int cpl);
void editorSetVisibleLines(int n);   // Tell editor how many lines are visible on screen
int editorGetStoredVisibleLines();   // Get the last set visible lines count
void editorRecalculateLines();
int editorGetVisibleLines(int lineHeight, int textAreaHeight);
int editorGetViewportStart();
int editorGetCursorLine();
int editorGetCursorCol();
int editorGetLineCount();
int editorGetLinePosition(int lineIndex);

// File metadata
void editorSetCurrentFile(const char* filename);
void editorSetCurrentTitle(const char* title);
const char* editorGetCurrentFile();
const char* editorGetCurrentTitle();
bool editorHasUnsavedChanges();
void editorSetUnsavedChanges(bool v);
int editorGetWordCount();
