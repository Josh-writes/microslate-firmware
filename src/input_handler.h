#pragma once

#include "config.h"

void inputSetup();
void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed);
int processAllInput();
char hidToAscii(uint8_t hid, uint8_t modifiers);

// The SD card is gone and the open note has unsaved text: the editor shows a full-screen warning instead of leaving.
// 1 = after Esc, Exit, Transfer or a short power press; 2 = after a power-button hold (a second hold turns the device
// off anyway); 3 = after the 5 s Back hold (a second hold restarts anyway).
// Any key or tap goes back to writing.
extern uint8_t editorSdWarn;

// The editor menu (Ctrl+M or the menu icon; work order 1.11): 0 closed, 1 the settings rows, 2 the Shortcuts page.
// editorMenuSel is the highlighted row. Rows: Writing mode, Font size, Word count, Focus mode, Keyboard layout,
// Spellcheck (builds with a word list), Shortcuts; named by editorMenuLabel / editorMenuValue.
extern uint8_t editorMenu;
extern int editorMenuSel;
void editorMenuTap(int row);   // touch: the first tap highlights a row, a tap on the highlighted row changes it

// The spelling list (Ctrl+J, or a long press on a marked word; work order 1.8): suggestions for the misspelled word,
// then "Add to dictionary". spellMenuSel is the highlighted row; spellMenuRows() = suggestions + 1.
extern bool spellMenu;
extern int spellMenuSel;
int spellMenuRows();
const char* spellMenuWord();           // the misspelled word, as typed
const char* spellMenuLabel(int row);   // a suggestion, or "Add to dictionary" (the last row)
bool spellMenuOpenAt(int pos);         // opens the list when the word at buffer offset pos is marked
void spellMenuTap(int row);            // touch: a tap on a row picks it
