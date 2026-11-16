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
void editorInsertText(const char* text);
void editorDeleteChar();     // Backspace
void editorDeleteForward();  // Delete key
// Undo/redo (edit_history.h): typing goes back a word at a time; paste, cut and typing over a selection one step each.
// False when there is nothing to undo or redo.
bool editorUndo();
bool editorRedo();

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

// Touch/pointer positioning. SetCursorPosition puts the cursor at a byte offset
// (snapped to a UTF-8 boundary); extend=true keeps/arms the selection anchor.
// SelectWordAt selects the word under the offset (false if there is none).
// ScrollViewport scrolls the visible lines without moving the cursor.
void editorSetCursorPosition(int pos, bool extend = false);
bool editorSelectWordAt(int pos);
// GetWordBounds: the [lo, hi) byte range of the word under the offset (or just
// before it); false when there is none. SelectRange sets an exact selection with
// the cursor at `cursor` (anchor and cursor may be in either order).
bool editorGetWordBounds(int pos, int* lo, int* hi);
void editorSelectRange(int anchor, int cursor);
void editorScrollViewport(int deltaLines);

// Selection + clipboard. Selection is empty when anchor == cursor; the range is a
// half-open byte interval [lo, hi) on character boundaries. The clipboard is
// internal to the device and survives opening/creating another note.
void editorSelectAll();
bool editorHasSelection();
bool editorGetSelectionRange(int* lo, int* hi);
bool editorHasClipboard();
void editorCopy();
void editorCut();
void editorPaste();

// Find the next occurrence of `query` (plain byte match, UTF-8 safe) starting
// from the cursor, wrapping to the top. On a hit, selects the match and scrolls
// to it; returns true. No match (or empty query) returns false and changes nothing.
bool editorFindNext(const char* query);

// Set when a newline is inserted (a paragraph break) — used by the autosave to
// flush at natural boundaries. Peek with Has…, clear when a save consumes it.
bool editorHasParagraphBreakPending();
void editorClearParagraphBreak();

// True for a few seconds after a key or paste was refused because the note is full (the header says "Note full").
bool editorNoteFullShowing();

// Line/viewport management
void editorSetCharsPerLine(int cpl);
// Wrap by pixel width (per-ASCII advance table); maxPx 0 falls back to charsPerLine
void editorSetPixelWrap(int maxPx, const uint16_t* w128, int fallback);
void editorSetVisibleLines(int n);   // Tell editor how many lines are visible on screen
int editorGetStoredVisibleLines();   // Get the last set visible lines count
void editorRecalculateLines();
int editorGetViewportStart();
int editorGetCursorLine();
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
