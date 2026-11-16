#pragma once

#include <cstdint>

// Tap targets published by the list screens. Each draw function registers the
// rows it actually drew (in logical pixels) plus the currently selected index;
// the touch handler hit-tests taps against them. The registry is cleared before
// every redraw, so it always matches what is on the glass.

void touchTargetsClear();

// Call once when a list is drawn: the selected index and the pixel distance
// between consecutive rows (used to turn a swipe's length into a row count).
void touchTargetsBegin(int selectedIndex, int rowPitchPx);

// Register one row: `index` is the position the screen's Up/Down keys move
// through; the rectangle is where the row is drawn (logical pixels).
void touchTargetsAddRow(int index, int x, int y, int w, int h);

int touchTargetsCount();
int touchTargetsSelected();
int touchTargetsRowPitch();

// True and sets *index when (px, py) lands on a registered row.
bool touchTargetsHit(int px, int py, int* index);

// On-screen action buttons (the bottom bar). A tap on one sends `keyCode` at once
// (no highlight-first step): the same key the matching physical button sends, so
// touch and hardware buttons share one code path.
void touchTargetsAddButton(uint8_t keyCode, int x, int y, int w, int h);
bool touchTargetsButtonHit(int px, int py, uint8_t* keyCode);
int touchTargetsButtonCount();

// --- Scrolling lists ---
// A list that doesn't fit draws a slider on its right edge and registers it here. The
// touch layer turns swipes and slider drags into scroll requests; the next draw takes
// them, moves the window and redraws. The highlighted row never changes by scrolling.
void touchSliderSet(int x, int y, int w, int h, int thumbH, int count, int rows);
bool touchSliderActive();
bool touchSliderHit(int px, int py);
int touchSliderStartForY(int py);   // list window start for a finger at py (thumb centred on it)
void touchScrollRequestRows(int deltaRows);   // move the window; + = later rows
void touchScrollRequestTo(int startRow);      // absolute window start
int touchScrollTakeRows();                    // consumed by the draw
bool touchScrollTakeTo(int* startRow);
void touchScrollDiscard();                    // drop requests no list consumed

// --- Editor context menu (long-press) ---
// Open/closed state survives redraws (it is the user's, not the draw's); the item
// rectangles are re-registered by every draw while it is open.
enum EditorMenuAction : uint8_t { MENU_CUT = 1, MENU_COPY, MENU_PASTE, MENU_SELECT_ALL };
void touchEditorMenuOpen(int anchorX, int anchorY);
void touchEditorMenuClose();
bool touchEditorMenuIsOpen();
int touchEditorMenuAnchorX();
int touchEditorMenuAnchorY();
void touchEditorMenuAddItem(uint8_t action, int x, int y, int w, int h);
bool touchEditorMenuHit(int px, int py, uint8_t* action);

// --- Text editor view ---------------------------------------------------------
// The editor draws wrapped lines at known heights. It registers each drawn line
// so a touch point can be turned into a line, and then (by measuring text widths)
// into a byte offset.

// Call once per editor draw: the font used for the text, the x where text starts,
// and the height of one line.
void touchEditorViewBegin(int fontId, int textX, int lineHeight);
// Register one drawn line: its index in the editor's line table and its top y.
void touchEditorViewAddLine(int lineIndex, int y);

int touchEditorViewFontId();
int touchEditorViewTextX();
int touchEditorViewLineHeight();

// Maps a touch y to a drawn line. Above the first line returns false; below the
// last drawn line it clamps to that last line (a tap in the margin below the text).
bool touchEditorViewLineAt(int py, int* lineIndex);
