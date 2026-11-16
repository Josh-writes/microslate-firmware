#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "sync/cloud_service.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "keyboard_layout.h"
#include "transfer/transfer_service.h"
#include "update/fw_update.h"
#include "sleep/sleep_screen.h"
#include "spell/spell_service.h"
#include "frontlight.h"
#include "settings_menu.h"

#include <HidCore.h>
#include <Keymap.h>
#include "touch_targets.h"

#include <Arduino.h>
#include <SDCardManager.h>

// External variables
extern bool autoReconnectEnabled;
extern bool cleanMode;
extern bool deleteConfirmPending;
extern WritingMode writingMode;
extern bool showWordCount;

// Layout editor (Settings > Edit keyboard layout); drawn by ui_renderer.cpp.
int layoutEdKey = -1;                 // HID key being edited, -1 = waiting for a key press
int layoutEdSlot = 0;                 // 0 normal, 1 Shift, 2 AltGr, 3 Shift+AltGr
bool layoutEdEntry = false;           // typing a new value for the slot
char layoutEdBuf[24] = "";
char layoutEdMessage[96] = "";        // what the last action did, in plain words
bool layoutEdConfirmAll = false;      // Reset all pressed once, waiting for the second press

// Esc in the editor asks first: Transfer (Enter, the highlighted choice), Exit (Esc again) or Back to the note (any
// other key, or the Back button). Drawn by ui_renderer.cpp.
bool editorExitPrompt = false;
int editorExitSel = 0;                // 0 Transfer, 1 Exit, 2 Back (to the note)
uint8_t editorSdWarn = 0;             // input_handler.h

// Settings > Editor > My words (drawn by ui_renderer.cpp).
int myWordsSel = 0;
bool myWordsRemovePending = false;    // "Remove this word?" is showing

// ---- Editor menu (Ctrl+M; input_handler.h) -----------------------------------------------------------------------
// The same settings as Settings > Editor (saved the same way, by main.cpp's prefs check), without leaving the note.
extern bool screenDirty;
uint8_t editorMenu = 0;
int editorMenuSel = 0;

static void handleEditorMenuKey(uint8_t keyCode) {
  screenDirty = true;
  if (editorMenu == 2) {   // Shortcuts page: Esc or Enter goes back to the menu, Ctrl+M closes everything
    if (keyCode == HID_KEY_ESCAPE || keyCode == HID_KEY_ENTER) editorMenu = 1;
    else if (keyCode == HID_KEY_EDITOR_MENU) editorMenu = 0;
    return;
  }
  switch (keyCode) {
    case HID_KEY_UP:    editorMenuSel = (editorMenuSel + editorMenuRows() - 1) % editorMenuRows(); return;
    case HID_KEY_DOWN:  editorMenuSel = (editorMenuSel + 1) % editorMenuRows(); return;
    case HID_KEY_ENTER: case HID_KEY_RIGHT: editorMenuChange(editorMenuSel, +1); return;
    case HID_KEY_LEFT:  if (!editorMenuOpensPage(editorMenuSel)) editorMenuChange(editorMenuSel, -1); return;
    case HID_KEY_ESCAPE: case HID_KEY_EDITOR_MENU: editorMenu = 0; return;
    default: return;   // other keys do nothing while the menu is open (nothing is typed behind it)
  }
}

void editorMenuTap(int row) {
  if (row < 0 || row >= editorMenuRows()) return;
  if (editorMenu == 2) editorMenu = 1;
  else if (row == editorMenuSel) editorMenuChange(row, +1);
  else editorMenuSel = row;
  screenDirty = true;
}

// ---- Spelling list (Ctrl+J or a long press; input_handler.h) ----------------------------------------------------
bool spellMenu = false;
int spellMenuSel = 0;
static constexpr int kSpellMaxSuggestions = 5;
static char g_spellSugg[kSpellMaxSuggestions][spell::kMaxWord];
static int g_spellSuggN = 0;
static char g_spellWord[spell::kMaxWord];
static int g_spellLo = 0, g_spellHi = 0;   // the word's bytes in the note

int spellMenuRows() { return g_spellSuggN + 1; }
const char* spellMenuWord() { return g_spellWord; }
const char* spellMenuLabel(int row) {
  if (row >= 0 && row < g_spellSuggN) return g_spellSugg[row];
  return row == g_spellSuggN ? "Add to dictionary" : "";
}

bool spellMenuOpenAt(int pos) {
  size_t s, e;
  if (pos < 0 || !spellsvc::misspelledAt(editorGetBuffer(), editorGetLength(), (size_t)pos, &s, &e)) return false;
  if (e - s >= sizeof(g_spellWord)) return false;
  memcpy(g_spellWord, editorGetBuffer() + s, e - s);
  g_spellWord[e - s] = '\0';
  g_spellLo = (int)s;
  g_spellHi = (int)e;
  g_spellSuggN = spellsvc::suggest(g_spellWord, e - s, g_spellSugg, kSpellMaxSuggestions);
  spellMenu = true;
  spellMenuSel = 0;
  screenDirty = true;
  return true;
}

// Replace the word with a suggestion (one undo step), or add it to the writer's own words.
static void spellMenuPick(int row) {
  spellMenu = false;
  screenDirty = true;
  if (row >= 0 && row < g_spellSuggN) {
    editorSelectRange(g_spellLo, g_spellHi);
    editorInsertText(g_spellSugg[row]);
  } else if (row == g_spellSuggN) {
    spellsvc::addWord(g_spellWord, strlen(g_spellWord));
  }
}

static void handleSpellMenuKey(uint8_t keyCode, bool ctrlJ) {
  screenDirty = true;
  if (ctrlJ || keyCode == HID_KEY_ESCAPE) { spellMenu = false; return; }
  const int rows = spellMenuRows();
  switch (keyCode) {
    case HID_KEY_UP:   spellMenuSel = (spellMenuSel + rows - 1) % rows; return;
    case HID_KEY_DOWN: spellMenuSel = (spellMenuSel + 1) % rows; return;
    case HID_KEY_ENTER: case HID_KEY_RIGHT: spellMenuPick(spellMenuSel); return;
    default: break;
  }
  if (keyCode >= HID_KEY_1 && keyCode < HID_KEY_1 + g_spellSuggN) spellMenuPick(keyCode - HID_KEY_1);   // 1-5 pick
}

void spellMenuTap(int row) {
  if (row >= 0 && row < spellMenuRows()) spellMenuPick(row);
}

// How the editor names a key: the character on a US keycap ("X", "-", "["), else the
// name used in layout files.
const char* layoutEdKeyName(uint8_t hid, char* out, size_t size) {
  const char* t = keymap::usText(hid, 0, false);
  if (t && t[0] > ' ' && !t[1]) {
    snprintf(out, size, "%c", (t[0] >= 'a' && t[0] <= 'z') ? char(t[0] - 32) : t[0]);
    return out;
  }
  return keymap::keyName(hid, out, size);
}

// One row of the key: "X", "Shift+X", "AltGr+X", "Shift+AltGr+X".
void layoutEdRowName(uint8_t hid, int slot, char* out, size_t size) {
  static const char* kPrefix[4] = {"", "Shift+", "AltGr+", "Shift+AltGr+"};
  char kn[16];
  snprintf(out, size, "%s%s", kPrefix[slot & 3], layoutEdKeyName(hid, kn, sizeof(kn)));
}

int layoutEdRows() { return keyboardLayoutHasAltGr() ? 4 : 2; }

// --- Input Queue ---
static KeyEvent inputQueue[INPUT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;
static volatile bool queueFull = false;

// --- CapsLock state ---
static bool capsLockOn = false;

// Where to return after title edit is confirmed or cancelled
static UIState renameReturnState = UIState::FILE_BROWSER;

// Forward declaration
static void openTitleEdit(const char* currentTitle, UIState returnTo);

// OTA app detection (defined in main.cpp)

// --- Shared UI state (defined in main.cpp) ---
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int settingsTab;
extern bool settingsForgetPending;
extern int sleepPicSel;
extern bool sleepPicPreview;
extern bool sleepPicDeletePending;
bool sleepPicCanUse();   // ui_renderer.cpp
extern int bluetoothDeviceSelection;
extern bool btShowAllDevices;
extern int uiBarFocus;
extern int pairedKeyboardSelection;
extern bool pairedForgetPending;
extern int charsPerLine;
extern bool screenDirty;
extern char renameBuffer[];
extern int renameBufferLen;
extern char renameExt[5];   // ".txt" or ".md" on the title screen
extern bool newNotesMd;    // Settings > Editor > New notes
extern bool editorFindMode;
extern char findBuffer[];
extern int findBufferLen;

void inputSetup() {
  queueHead = 0;
  queueTail = 0;
  queueFull = false;
  capsLockOn = false;
}

static bool isQueueEmpty() {
  return (queueHead == queueTail) && !queueFull;
}

void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed) {
  noInterrupts();
  if (!queueFull) {
    inputQueue[queueHead].keyCode = keyCode;
    inputQueue[queueHead].modifiers = modifiers;
    inputQueue[queueHead].pressed = pressed;
    queueHead = (queueHead + 1) % INPUT_QUEUE_SIZE;
    if (queueHead == queueTail) queueFull = true;
  }
  interrupts();
}

static KeyEvent dequeueKeyEvent() {
  KeyEvent event = {0, 0, false};
  noInterrupts();
  if (!isQueueEmpty()) {
    event = inputQueue[queueTail];
    queueTail = (queueTail + 1) % INPUT_QUEUE_SIZE;
    queueFull = false;
  }
  interrupts();
  return event;
}

char hidToAscii(uint8_t hid, uint8_t modifiers) {
  const char* text = keyboardLayoutQwertyText(hid, modifiers, capsLockOn);
  if (!text || text[0] == '\0' || text[1] != '\0') return 0;
  return text[0];
}

// Handle the in-document find overlay. Enter searches / finds next, Esc closes,
// Backspace edits the query, printable keys extend it.
static void handleFindKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ESCAPE) {
    editorFindMode = false;
    screenDirty = true;
    return;
  }
  if (keyCode == HID_KEY_ENTER) {
    if (findBufferLen > 0) editorFindNext(findBuffer);
    screenDirty = true;
    return;
  }
  if (keyCode == HID_KEY_BACKSPACE) {
    if (findBufferLen > 0) {
      findBufferLen--;
      findBuffer[findBufferLen] = '\0';
    }
    screenDirty = true;
    return;
  }
  const char* text = keyboardLayoutText(keyCode, modifiers, capsLockOn);
  if (text != nullptr && text[0]) {
    int tlen = (int)strlen(text);
    if (findBufferLen + tlen < MAX_FIND_LEN) {
      strcpy(findBuffer + findBufferLen, text);
      findBufferLen += tlen;
    }
    screenDirty = true;
  }
}

// Handle text editor input
// The card is gone with unsaved text: stay in the editor and say why (the text lives only in memory until the card
// is back). True when the warning was raised.
static bool warnIfSdGone() {
  if (!sdLost() || !editorHasUnsavedChanges()) return false;
  editorSdWarn = 1;
  screenDirty = true;
  return true;
}

static void leaveEditor() {
  if (warnIfSdGone()) return;
  if (editorHasUnsavedChanges() && !saveCurrentFile() && warnIfSdGone()) return;   // the save found the card gone
  currentState = UIState::FILE_BROWSER;
  screenDirty = true;
}

// Transfer the open note: saved first, then the Transfer screen; leaving it returns to the notes list on this note.
static void transferOpenNote() {
  if (warnIfSdGone()) return;
  if (editorHasUnsavedChanges() && !saveCurrentFile()) { warnIfSdGone(); return; }   // never share an unsaved copy
  transfer::open(editorGetCurrentFile(), UIState::FILE_BROWSER);
  screenDirty = true;
}

static void handleExitPrompt(uint8_t keyCode) {
  editorExitPrompt = false;
  screenDirty = true;
  switch (keyCode) {
    case HID_KEY_ESCAPE: leaveEditor(); return;
    case HID_KEY_ENTER:
      if (editorExitSel == 0) transferOpenNote();
      else if (editorExitSel == 1) leaveEditor();
      return;   // 2 = Back: to the note
    case HID_KEY_RIGHT: case HID_KEY_DOWN: case HID_KEY_TAB:
      editorExitSel = (editorExitSel + 1) % 3;
      editorExitPrompt = true;   // still choosing
      return;
    case HID_KEY_LEFT: case HID_KEY_UP:
      editorExitSel = (editorExitSel + 2) % 3;
      editorExitPrompt = true;
      return;
    default:
      return;   // any other key: back to writing (the key itself is not typed)
  }
}

// Autocorrect (work order 1.9): the last fix, which Backspace takes back while it is the very next key.
static bool g_acArmed = false;
static int g_acStart = 0, g_acAfter = 0;   // where the fix starts; the cursor after the key that ended the word
static char g_acOrig[spell::kMaxWord], g_acFix[spell::kMaxWord];

// A key that ends a word: a space, a new line, a tab or punctuation.
static bool endsWord(const char* t) {
  return t && t[0] && !t[1] && strchr(" \n\t.,;:!?)]}\"", t[0]) != nullptr;
}

// Types text; when it ends a word that autocorrect fixes, the word is fixed first (silently).
static void typeText(const char* text) {
  bool fixed = false;
  if (endsWord(text) && !editorHasSelection()) {
    const int cur = editorGetCursorPosition();
    size_t s = 0;
    char fix[spell::kMaxWord];
    if (spellsvc::autocorrectWord(editorGetBuffer(), editorGetLength(), (size_t)cur, &s, fix)) {
      const int n = cur - (int)s;
      memcpy(g_acOrig, editorGetBuffer() + s, (size_t)n);
      g_acOrig[n] = '\0';
      snprintf(g_acFix, sizeof(g_acFix), "%s", fix);
      editorSelectRange((int)s, cur);
      editorInsertText(fix);   // one undo step; Ctrl+Z takes it back too
      fixed = editorGetCursorPosition() == (int)s + (int)strlen(fix);
      if (!fixed && editorHasSelection()) editorSetCursorPosition(cur, false);   // did not fit: leave it
      g_acStart = (int)s;
    }
  }
  editorInsertText(text);
  if (fixed) {
    g_acAfter = editorGetCursorPosition();
    g_acArmed = true;
  }
}

// Backspace right after a fix: the word goes back to what was typed (the key after it stays), and is left alone
// for the rest of the session. False when the last key was not a fix.
static bool takeBackFix() {
  const int fixLen = (int)strlen(g_acFix), origLen = (int)strlen(g_acOrig);
  if (editorHasSelection() || editorGetCursorPosition() != g_acAfter ||
      g_acStart + fixLen > (int)editorGetLength() || memcmp(editorGetBuffer() + g_acStart, g_acFix, fixLen) != 0)
    return false;
  const int tail = g_acAfter - (g_acStart + fixLen);   // the key that ended the word
  editorSelectRange(g_acStart, g_acStart + fixLen);
  editorInsertText(g_acOrig);
  editorSetCursorPosition(g_acStart + origLen + tail, false);
  spellsvc::autocorrectTakenBack(g_acOrig, (size_t)origLen);
  DBG_PRINTF("[SPELL] autocorrect taken back: \"%s\"\n", g_acOrig);
  return true;
}

static void handleEditorKey(uint8_t keyCode, uint8_t modifiers) {
  touchEditorMenuClose();  // any key dismisses the touch context menu
  const bool acArmed = g_acArmed;   // only the very next key can take a fix back
  g_acArmed = false;
  if (editorSdWarn) {      // the SD card warning: any key goes back to writing (the key is not typed)
    editorSdWarn = 0;
    screenDirty = true;
    return;
  }
  if (editorMenu) {        // the editor menu takes the keys while it is open (Ctrl+M again closes it)
    const bool ctrlM = isCtrl(modifiers) && keyboardLayoutShortcutLetter(keyCode) == 'm';
    handleEditorMenuKey(ctrlM ? HID_KEY_EDITOR_MENU : keyCode);
    return;
  }
  if (spellMenu) {         // the spelling list takes the keys while it is open (Ctrl+J again closes it)
    handleSpellMenuKey(keyCode, isCtrl(modifiers) && keyboardLayoutShortcutLetter(keyCode) == 'j');
    return;
  }
  if (keyCode == HID_KEY_EDITOR_MENU) {   // a tap on the menu icon in the header
    editorMenu = 1;
    editorMenuSel = 0;
    screenDirty = true;
    return;
  }
  if (editorExitPrompt) {
    handleExitPrompt(keyCode);
    return;
  }
  // Find overlay takes over input while active.
  if (editorFindMode) {
    handleFindKey(keyCode, modifiers);
    return;
  }

  // A waiting accent (dead) key only combines with the next typed character.
  const bool ctrl = isCtrl(modifiers) && !isAltGr(modifiers);
  if (ctrl || !keyboardLayoutIsTextKey(keyCode)) keyboardLayoutCancelDead();

  // Ctrl shortcuts, by the letter the key has in the active layout (DE: the key labelled Z).
  if (ctrl) {
    const char sc = keyboardLayoutShortcutLetter(keyCode);
    if (sc == 's') {
      saveCurrentFile();
      screenDirty = true;
      return;
    }
    // Undo Ctrl+Z; redo Ctrl+Y or Ctrl+Shift+Z.
    if (sc == 'z' || sc == 'y') {
      const bool redo = sc == 'y' || isShift(modifiers);
      if (redo ? editorRedo() : editorUndo()) screenDirty = true;
      return;
    }
    // Editor menu: Ctrl+M (the writing settings, and the Shortcuts page).
    if (sc == 'm') {
      editorMenu = 1;
      editorMenuSel = 0;
      screenDirty = true;
      return;
    }
    // Spelling list for the marked word at the cursor: Ctrl+J.
    if (sc == 'j') {
      spellMenuOpenAt(editorGetCursorPosition());
      return;
    }
    // Focus (clean) mode: Ctrl+H (it was Ctrl+Z until undo took that key).
    if (sc == 'h') {
      cleanMode = !cleanMode;
      screenDirty = true;
      return;
    }
    if (sc == 'n') {
      openTitleEdit(editorGetCurrentTitle(), UIState::TEXT_EDITOR);
      return;
    }
    if (sc == 't') {
      writingMode = (writingMode == WritingMode::TYPEWRITER) ? WritingMode::NORMAL : WritingMode::TYPEWRITER;
      screenDirty = true;
      return;
    }
    if (sc == 'f') {
      // Enter find mode. (Font size moved here historically; it stays available
      // in Settings → Font Size.)
      editorFindMode = true;
      findBuffer[0] = '\0';
      findBufferLen = 0;
      screenDirty = true;
      return;
    }
    if (sc == 'w') {
      showWordCount = !showWordCount;
      screenDirty = true;
      return;
    }
    if (sc == 'k') {
      keyboardLayoutNext();
      screenDirty = true;
      return;
    }
    if (sc == 'p') {
      writingMode = (writingMode == WritingMode::PAGINATION) ? WritingMode::NORMAL : WritingMode::PAGINATION;
      screenDirty = true;
      return;
    }
    // Clipboard: Ctrl+A select all, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste
    if (sc == 'a') {
      editorSelectAll();
      screenDirty = true;
      return;
    }
    if (sc == 'c') {
      editorCopy();
      return;
    }
    if (sc == 'x') {
      editorCut();
      screenDirty = true;
      return;
    }
    if (sc == 'v') {
      editorPaste();
      screenDirty = true;
      return;
    }
    // Ctrl+Left/Right: jump pages in pagination mode
    if (writingMode == WritingMode::PAGINATION) {
      int pageSize = editorGetStoredVisibleLines();
      if (keyCode == HID_KEY_LEFT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorUp();
        screenDirty = true;
        return;
      }
      if (keyCode == HID_KEY_RIGHT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorDown();
        screenDirty = true;
        return;
      }
    }
    return;
  }

  // Esc: ask Transfer or Exit (Esc again exits, Enter transfers).
  if (keyCode == HID_KEY_ESCAPE) {
    keyboardLayoutCancelDead();
    if (warnIfSdGone()) return;   // card gone with unsaved text: neither choice can save
    editorExitPrompt = true;
    editorExitSel = 0;
    screenDirty = true;
    return;
  }

  // Tab indents: a real tab character in the note (drawn as a fixed indent). Writing modes are on Ctrl+T, Ctrl+P and
  // Settings. Shift+Tab does nothing for now (kept free for an outdent).
  if (keyCode == HID_KEY_TAB) {
    if (!isShift(modifiers)) {
      typeText("\t");
      screenDirty = true;
    }
    return;
  }

  // Navigation keys. Shift+arrow/Home/End extends the selection (Ctrl already
  // returned above, so it keeps its own meaning). Backspace/Delete handle an
  // active selection internally.
  const bool extend = isShift(modifiers);
  switch (keyCode) {
    case HID_KEY_LEFT:      editorMoveCursorLeft(extend);  screenDirty = true; return;
    case HID_KEY_RIGHT:     editorMoveCursorRight(extend); screenDirty = true; return;
    case HID_KEY_UP:        editorMoveCursorUp(extend);    screenDirty = true; return;
    case HID_KEY_DOWN:      editorMoveCursorDown(extend);  screenDirty = true; return;
    case HID_KEY_HOME:      editorMoveCursorHome(extend);  screenDirty = true; return;
    case HID_KEY_END:       editorMoveCursorEnd(extend);   screenDirty = true; return;
    case HID_KEY_BACKSPACE: if (!(acArmed && takeBackFix())) editorDeleteChar(); screenDirty = true; return;
    case HID_KEY_DELETE:    editorDeleteForward();   screenDirty = true; return;
  }

  // CapsLock toggle
  if (keyCode == HID_KEY_CAPSLOCK) {
    capsLockOn = !capsLockOn;
    return;
  }

  // Printable character
  const char* text = keyboardLayoutText(keyCode, modifiers, capsLockOn);
  if (text != nullptr && text[0]) {
    typeText(text);
    screenDirty = true;
  }
}

// Open the title edit screen, returning to `returnTo` on confirm/cancel
static void openTitleEdit(const char* currentTitle, UIState returnTo) {
  strncpy(renameBuffer, currentTitle, MAX_TITLE_LEN - 1);
  renameBuffer[MAX_TITLE_LEN - 1] = '\0';
  renameBufferLen = strlen(renameBuffer);
  renameReturnState = returnTo;
  // What Enter saves as: the note's own format for an existing note, the Settings choice (Editor > New notes, .txt
  // unless changed) for a new one. The Save .txt / Save .md buttons pick either; changing it is only a rename.
  const char* file = "";
  if (returnTo == UIState::TEXT_EDITOR) file = editorGetCurrentFile();
  else if (selectedFileIndex >= 1 && selectedFileIndex <= getFileCount()) file = getFileList()[selectedFileIndex - 1].filename;
  snprintf(renameExt, sizeof(renameExt), "%s", file[0] ? noteExtOf(file) : (newNotesMd ? ".md" : ".txt"));
  currentState = UIState::RENAME_FILE;
  screenDirty = true;
}

// Handle title edit input
static void handleRenameKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ENTER) {
    if (renameBufferLen > 0) {
      if (renameReturnState == UIState::TEXT_EDITOR) {
        editorSetCurrentTitle(renameBuffer);
        if (editorGetCurrentFile()[0] == '\0') {
          // New file — derive filename from title
          char filename[MAX_FILENAME_LEN];
          deriveUniqueFilename(renameBuffer, renameExt, filename, MAX_FILENAME_LEN);
          editorSetCurrentFile(filename);
        } else {
          // Existing file — rename on disk to match new title (and format)
          updateFileTitle(editorGetCurrentFile(), renameBuffer, renameExt);
        }
        editorSetUnsavedChanges(true);
        saveCurrentFile();
      } else {
        // Updating title of a file selected in the browser
        FileInfo* files = getFileList();
        updateFileTitle(files[selectedFileIndex - 1].filename, renameBuffer, renameExt);   // row 0 is "+ New note"
      }
    }
    currentState = renameReturnState;
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_ESCAPE) {
    if (renameReturnState == UIState::TEXT_EDITOR && editorGetCurrentFile()[0] == '\0') {
      // Cancelling the title of a brand-new note: nothing was saved yet, so drop it and
      // go back to the Notes list instead of opening an untitled note.
      editorClear();
      editorSetUnsavedChanges(false);
      refreshFileList();
      currentState = UIState::FILE_BROWSER;
    } else {
      currentState = renameReturnState;
    }
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_BACKSPACE) {
    if (renameBufferLen > 0) {
      renameBufferLen--;
      renameBuffer[renameBufferLen] = '\0';
      screenDirty = true;
    }
    return;
  }

  // Allow all printable characters in a title (including spaces)
  char c = hidToAscii(keyCode, modifiers);
  if (c != 0 && c >= ' ' && renameBufferLen < MAX_TITLE_LEN - 1) {
    renameBuffer[renameBufferLen++] = c;
    renameBuffer[renameBufferLen] = '\0';
    screenDirty = true;
  }
}

// --- Bottom-bar navigation shared by every list screen -----------------------
// The four bar buttons are reachable by focus: Down past the last row enters the bar,
// Left/Right move along it, Enter presses the focused button, Up from its first button
// returns to the list. A touch on a bar button arrives as HID_KEY_BAR0 + i, the same
// code Enter on the focused button produces, so both end up in one place.
//
// Returns true when the key was consumed. `activate` is set to the pressed bar button
// (0-3) or -1 for plain navigation, which the caller treats as "just redraw".
uint8_t uiBarMask();   // ui_renderer.cpp: which bar buttons exist on this screen

static bool barNav(const KeyEvent& e, int& sel, int count, int& activate) {
  activate = -1;
  const uint8_t mask = uiBarMask();
  if (uiBarFocus >= 0 && !((mask >> uiBarFocus) & 1)) uiBarFocus = -1;   // button went away

  const uint8_t k = e.keyCode;
  if (k >= HID_KEY_BAR0 && k < HID_KEY_BAR0 + 4) {
    activate = k - HID_KEY_BAR0;
    uiBarFocus = -1;
    return true;
  }

  auto nextBar = [&](int from, int dir) -> int {
    for (int i = from + dir; i >= 0 && i < 4; i += dir)
      if ((mask >> i) & 1) return i;
    return -1;
  };
  const bool inBar = (uiBarFocus >= 0);

  if (k == HID_KEY_DOWN) {
    if (inBar) {
      const int n = nextBar(uiBarFocus, +1);
      if (n >= 0) uiBarFocus = n;
      else { uiBarFocus = -1; if (count > 0) sel = 0; }               // wrap to the list top
    } else if (count > 0 && sel < count - 1) {
      sel++;
    } else {
      const int n = nextBar(-1, +1);                                    // past the last row
      if (n >= 0) uiBarFocus = n;
      else if (count > 0) sel = 0;
    }
    screenDirty = true;
    return true;
  }
  if (k == HID_KEY_UP) {
    if (inBar) {
      const int n = nextBar(uiBarFocus, -1);
      if (n >= 0) uiBarFocus = n;
      else { uiBarFocus = -1; if (count > 0) sel = count - 1; }
    } else if (count > 0 && sel > 0) {
      sel--;
    } else {
      const int n = nextBar(4, -1);                                     // above the first row
      if (n >= 0) uiBarFocus = n;
      else if (count > 0) sel = count - 1;
    }
    screenDirty = true;
    return true;
  }
  if (inBar && (k == HID_KEY_LEFT || k == HID_KEY_RIGHT)) {
    const int n = nextBar(uiBarFocus, k == HID_KEY_RIGHT ? +1 : -1);
    if (n >= 0) uiBarFocus = n;
    screenDirty = true;
    return true;
  }
  if (inBar && k == HID_KEY_ENTER) {
    activate = uiBarFocus;
    uiBarFocus = -1;
    return true;
  }
  return false;
}

// Bar button -> the key the screen's older handlers already understand.
static uint8_t barKey(int idx, uint8_t third, uint8_t fourth) {
  switch (idx) {
    case 0: return HID_KEY_ESCAPE;
    case 1: return HID_KEY_ENTER;
    case 2: return third;
    default: return fourth;
  }
}

static void dispatchEvent(const KeyEvent& event) {
  if (!event.pressed) return;

  // Win/Cmd + key is a system shortcut, never text (the MX Keys Mini's F-row sends Win+H
  // and Win+Shift+S). Win+Space switches the keyboard layout in the editor, as on a PC;
  // the others are left for key mapping.
  if (event.modifiers & (MOD_GUI_LEFT | MOD_GUI_RIGHT)) {
    if (event.keyCode == HID_KEY_SPACE && currentState == UIState::TEXT_EDITOR) {
      keyboardLayoutNext();
      screenDirty = true;
    }
    return;
  }

  // Ctrl+B on any screen steps the frontlight (devices with one), as in Draftling.
  if (light::present() && isCtrl(event.modifiers) && !isAltGr(event.modifiers) &&
      keyboardLayoutShortcutLetter(event.keyCode) == 'b') {
    light::cycle(1);
    if (currentState == UIState::SETTINGS) screenDirty = true;   // the Frontlight row shows the new step
    return;
  }

  // The bar focus belongs to one screen; never carry it to the next.
  static UIState lastState = UIState::MAIN_MENU;
  if (currentState != lastState) {
    uiBarFocus = -1;
    editorExitPrompt = false;
    editorMenu = 0;
    spellMenu = false;
    lastState = currentState;
  }

  switch (currentState) {
    case UIState::MAIN_MENU: {
      const int menuCount = 4;   // Notes, Settings, Transfer, Sync
      int act = -1;
      if (barNav(event, mainMenuSelection, menuCount, act)) break;
      if (event.keyCode == HID_KEY_ENTER) {
        if (mainMenuSelection == 0) {
          refreshFileList();
          currentState = UIState::FILE_BROWSER;
          screenDirty = true;
        } else if (mainMenuSelection == 1) {
          wifiSavedRefresh();
          settingsSelection = 1;   // the open tab's first row
          settingsForgetPending = false;
          currentState = UIState::SETTINGS;
          screenDirty = true;
        } else if (mainMenuSelection == 2) {
          refreshFileList();
          transfer::open(nullptr, UIState::MAIN_MENU);   // all notes
          screenDirty = true;
        } else if (mainMenuSelection == 3) {
          wifiSyncStart();
          currentState = UIState::WIFI_SYNC;
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::FILE_BROWSER: {
      // Rows: 0 = "+ New note", then one per file (file = row - 1).
      const int fc = getFileCount();
      const int rows = fc + 1;
      const bool onFile = (selectedFileIndex >= 1);

      // A note is waiting for its sync before it opens ("Syncing with OneDrive..."): Esc (or Back) stays on the
      // list instead; other keys wait.
      if (cloud::browseHoldActive()) {
        if (event.keyCode == HID_KEY_ESCAPE || event.keyCode == HID_KEY_BAR0) cloud::browseHoldCancel();
        break;
      }

      // "This note is too large to open": any key (or a tap, which arrives as Enter) goes back to the list.
      if (loadRefusedBytes()) {
        loadRefusedClear();
        screenDirty = true;
        break;
      }

      // Delete confirmation pending: Enter / the Delete button confirm, anything else cancels.
      if (deleteConfirmPending) {
        if ((event.keyCode == HID_KEY_ENTER || event.keyCode == HID_KEY_BAR0 + 1) && onFile) {
          FileInfo* files = getFileList();
          deleteFile(files[selectedFileIndex - 1].filename);
          const int newFc = getFileCount();
          if (selectedFileIndex > newFc) selectedFileIndex = newFc;   // last row is file newFc - 1
          if (selectedFileIndex < 0) selectedFileIndex = 0;
        }
        deleteConfirmPending = false;
        screenDirty = true;
        break;
      }

      int act = -1;
      if (barNav(event, selectedFileIndex, rows, act) && act < 0) break;   // plain navigation

      // Bar buttons: Back / Rename / Delete / Transfer (E1001: Back / Transfer). Keyboard: Esc back, Enter open,
      // Ctrl+N rename, Ctrl+D delete, Ctrl+T transfer.
      const bool ctrl = isCtrl(event.modifiers) && !isAltGr(event.modifiers);
      const char sc = ctrl ? keyboardLayoutShortcutLetter(event.keyCode) : 0;
      const bool keyBack = act < 0 && event.keyCode == HID_KEY_ESCAPE;
      const bool doBack = act == 0 || keyBack;
      const bool doOpen = act < 0 && event.keyCode == HID_KEY_ENTER;
      const bool doRename = (!kClassicUi && act == 1) || (act < 0 && sc == 'n');
      const bool doDelete = (!kClassicUi && act == 2) || (act < 0 && sc == 'd');
      const bool doTransfer = act == 3 || (act < 0 && sc == 't');

      if (doOpen && !onFile) {
        createNewFile();
        openTitleEdit("Untitled", UIState::TEXT_EDITOR);
      } else if (doOpen) {
        // During a sync the note's own transfer goes first and it opens when that is done (cloud_service.h).
        FileInfo* files = getFileList();
        if (!cloud::holdNoteOpen(files[selectedFileIndex - 1].filename)) loadFile(files[selectedFileIndex - 1].filename);
        screenDirty = true;
      } else if (doRename && onFile) {
        FileInfo* files = getFileList();
        openTitleEdit(files[selectedFileIndex - 1].title, UIState::FILE_BROWSER);
      } else if (doDelete && onFile) {
        deleteConfirmPending = true;
        screenDirty = true;
      } else if (doTransfer && onFile) {
        FileInfo* files = getFileList();
        transfer::open(files[selectedFileIndex - 1].filename, UIState::FILE_BROWSER);
        screenDirty = true;
      } else if (doBack) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
      break;
    }

    case UIState::TEXT_EDITOR:
      handleEditorKey(event.keyCode, event.modifiers);
      break;

    case UIState::RENAME_FILE: {
      // On-screen Cancel is Esc; "Save .txt" and "Save .md" pick the format and save (Enter keeps the default).
      if (event.keyCode >= HID_KEY_BAR0 && event.keyCode < HID_KEY_BAR0 + 4) {
        const int b = event.keyCode - HID_KEY_BAR0;
        if (b == 1 || b == 2) snprintf(renameExt, sizeof(renameExt), "%s", b == 2 ? ".md" : ".txt");
        handleRenameKey(barKey(b, HID_KEY_ENTER, 0), 0);
      } else {
        handleRenameKey(event.keyCode, event.modifiers);
      }
      break;
    }

    case UIState::SETTINGS: {
      // Tabs (config.h): settingsSelection 0 is the tab strip, 1.. the open tab's rows. Up/Down move through the
      // strip, the rows and the bar. Tab / Shift+Tab, a tap on a tab, or Left/Right (and Enter) on the strip change
      // tab. On a value row the bar is Back / Prev / Next (Esc / Left / Right); Enter also steps forward.
      auto showTab = [](int t, bool keepOnStrip) {
        settingsTab = (t + kSettingsTabCount) % kSettingsTabCount;
        settingsSelection = keepOnStrip ? 0 : 1;
        settingsForgetPending = false;
        uiBarFocus = -1;
        if (settingsTab == kSettingsTabWifi) wifiSavedRefresh();
        screenDirty = true;
      };
      if (event.keyCode >= HID_KEY_TAB0 && event.keyCode < HID_KEY_TAB0 + kSettingsTabCount) {
        showTab(event.keyCode - HID_KEY_TAB0, true);   // a tap: the first tap on a row only highlights it
        break;
      }
      if (event.keyCode == HID_KEY_TAB) {
        showTab(settingsTab + (isShift(event.modifiers) ? -1 : 1), settingsSelection <= 0);
        break;
      }

      const int rowCount = settingsTabRows(settingsTab);
      int act = -1;
      uint8_t key = event.keyCode;
      const bool wasForget = settingsForgetPending;
      if (barNav(event, settingsSelection, rowCount + 1, act)) {
        if (act < 0) { settingsForgetPending = false; break; }   // plain navigation
      }

      // The tab strip: Prev / Next, Left / Right and Enter change tab.
      if (settingsSelection <= 0 && uiBarFocus < 0) {
        if (act == 0 || (act < 0 && key == HID_KEY_ESCAPE)) {
          currentState = UIState::MAIN_MENU;
          screenDirty = true;
        } else if (act == 2 || (act < 0 && key == HID_KEY_LEFT)) {
          showTab(settingsTab - 1, true);
        } else if (act == 3 || (act < 0 && (key == HID_KEY_RIGHT || key == HID_KEY_ENTER))) {
          showTab(settingsTab + 1, true);
        }
        break;
      }

      const int row = settingsSelection - 1;
      const SettingKind kind = settingKind(settingsTab, row);

      // A saved Wi-Fi network: Enter / Prefer makes it the one tried first; Forget (or Delete) asks, then removes it.
      if (kind == SettingKind::SavedWifi) {
        const int w = settingWifiIndex(settingsTab, row);
        const bool forget = act == 3 || (act < 0 && (key == HID_KEY_DELETE || key == HID_KEY_BACKSPACE));
        settingsForgetPending = false;
        if (forget && wasForget) {
          wifiSavedForget(w);
          wifiSavedRefresh();
          const int n = settingsTabRows(settingsTab);
          if (settingsSelection > n) settingsSelection = n;
        } else if (forget) {
          settingsForgetPending = true;
        } else if (wasForget) {
          // Cancel, or any other key: keep the network
        } else if (act == 1 || (act < 0 && key == HID_KEY_ENTER)) {
          wifiSavedPrefer(w);
        } else if (act == 0 || (act < 0 && key == HID_KEY_ESCAPE)) {
          currentState = UIState::MAIN_MENU;
        }
        screenDirty = true;
        break;
      }
      if (act >= 0) key = barKey(act, HID_KEY_LEFT, HID_KEY_RIGHT);

      // Value rows: Enter or Right steps forward, Left back. Rows that open a screen: Enter (or Right) opens it;
      // Add a network opens on Enter only.
      if (key == HID_KEY_ENTER || key == HID_KEY_RIGHT || key == HID_KEY_LEFT) {
        const int dir = key == HID_KEY_LEFT ? -1 : 1;
        if (kind == SettingKind::Value) settingStep(settingsTab, row, dir);
        else if (kind == SettingKind::Opens && dir > 0) settingOpen(settingsTab, row);
        else if (kind == SettingKind::AddWifi && key == HID_KEY_ENTER) settingOpen(settingsTab, row);
        screenDirty = true;
      } else if (key == HID_KEY_ESCAPE) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
      break;
    }

    case UIState::BLUETOOTH_SETTINGS: {
      // Build the visible list (keyboards first; others only when "show all").
      // The last selectable row may be the "Show other devices" toggle.
      int visIdx[32];
      int total = 0, kbd = 0;
      int visCount = getVisibleDeviceIndices(btShowAllDevices, visIdx, 32, &total, &kbd);
      bool toggleShown = (kbd > 0 && total > kbd);   // a real filter choice exists
      int itemCount = visCount + (toggleShown ? 1 : 0);

      // Keep selection in range
      if (itemCount == 0) bluetoothDeviceSelection = 0;
      else if (bluetoothDeviceSelection >= itemCount) bluetoothDeviceSelection = itemCount - 1;
      if (bluetoothDeviceSelection < 0) bluetoothDeviceSelection = 0;

      bool onToggle = (toggleShown && bluetoothDeviceSelection == visCount);

      // Bar: Back / Connect / Disconnect / Scan = Esc / Enter / Left / Right.
      int act = -1;
      uint8_t key = event.keyCode;
      if (barNav(event, bluetoothDeviceSelection, itemCount, act)) {
        if (act < 0) break;
        key = barKey(act, HID_KEY_LEFT, HID_KEY_RIGHT);
      }

      if (key == HID_KEY_ESCAPE) {
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (key == HID_KEY_ENTER) {
        if (onToggle) {
          btShowAllDevices = !btShowAllDevices;   // flip filter, reset to top
          bluetoothDeviceSelection = 0;
        } else if (visCount > 0 && !isDeviceScanning()) {
          connectToDevice(visIdx[bluetoothDeviceSelection]);
        } else if (!isDeviceScanning()) {
          startDeviceScan();   // nothing to connect to: scan instead
        }
        screenDirty = true;
      } else if (key == HID_KEY_RIGHT) {
        if (!isDeviceScanning()) startDeviceScan();
        screenDirty = true;
      } else if (key == HID_KEY_LEFT) {
        if (isKeyboardConnected()) {
          disconnectCurrentDevice();
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::MY_WORDS: {
      const int count = spellsvc::ownCount();
      // "Remove this word?": Enter or the Remove button removes it, anything else keeps it.
      if (myWordsRemovePending) {
        if ((event.keyCode == HID_KEY_ENTER || event.keyCode == HID_KEY_BAR0 + 1) && count > 0) {
          spellsvc::removeOwn(myWordsSel);
          const int n = spellsvc::ownCount();
          if (myWordsSel >= n) myWordsSel = n > 0 ? n - 1 : 0;
        }
        myWordsRemovePending = false;
        screenDirty = true;
        break;
      }
      // Bar: Back / Remove.
      int act = -1;
      uint8_t key = event.keyCode;
      if (barNav(event, myWordsSel, count, act)) {
        if (act < 0) break;
        key = barKey(act, 0, 0);
      }
      if (key == HID_KEY_ESCAPE) {
        currentState = UIState::SETTINGS;
      } else if ((key == HID_KEY_ENTER || key == HID_KEY_DELETE || key == HID_KEY_BACKSPACE) && count > 0) {
        myWordsRemovePending = true;
      }
      screenDirty = true;
      break;
    }

    case UIState::PAIRED_KEYBOARDS: {
      const int count = getPairedKeyboardCount();

      // Forget confirmation pending: Enter / the Forget button confirm, anything else cancels.
      if (pairedForgetPending) {
        if ((event.keyCode == HID_KEY_ENTER || event.keyCode == HID_KEY_BAR0 + 1) && count > 0) {
          removePairedKeyboard(pairedKeyboardSelection);
          const int newCount = getPairedKeyboardCount();
          if (newCount == 0) pairedKeyboardSelection = 0;
          else if (pairedKeyboardSelection >= newCount) pairedKeyboardSelection = newCount - 1;
        }
        pairedForgetPending = false;
        screenDirty = true;
        break;
      }

      // Bar: Back / Connect / Disconnect / Forget.
      int act = -1;
      uint8_t key = event.keyCode;
      if (barNav(event, pairedKeyboardSelection, count, act)) {
        if (act < 0) break;
        key = barKey(act, HID_KEY_LEFT, HID_KEY_D);
      }

      if (key == HID_KEY_ESCAPE) {
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (key == HID_KEY_ENTER) {
        if (count > 0) {
          connectToPairedKeyboard(pairedKeyboardSelection);
          currentState = UIState::SETTINGS;
          screenDirty = true;
        }
      } else if (key == HID_KEY_D) {
        // Arm the forget confirmation ('D' on a keyboard, or the Forget button)
        if (count > 0) {
          pairedForgetPending = true;
          screenDirty = true;
        }
      } else if (key == HID_KEY_LEFT) {
        // Disconnect if this keyboard is the currently connected one
        if (count > 0 && isKeyboardConnected()) {
          std::string addr, name; uint8_t addrType;
          getPairedKeyboard(pairedKeyboardSelection, addr, name, addrType);
          if (getCurrentDeviceAddress() == addr) {
            disconnectCurrentDevice();
            screenDirty = true;
          }
        }
      }
      break;
    }

    case UIState::LAYOUT_EDITOR: {
      char row[40];
      // Typing what the selected row should type: Enter saves, Esc cancels.
      if (layoutEdEntry) {
        const uint8_t k = (event.keyCode == HID_KEY_BAR0) ? HID_KEY_ESCAPE
                        : (event.keyCode == HID_KEY_BAR0 + 1) ? HID_KEY_ENTER : event.keyCode;
        if (k == HID_KEY_ESCAPE) {
          layoutEdEntry = false;
          keyboardLayoutCancelDead();
        } else if (k == HID_KEY_ENTER) {
          char v[sizeof(layoutEdBuf)];
          layoutEdRowName((uint8_t)layoutEdKey, layoutEdSlot, row, sizeof(row));
          if (!keymap::entryValue(layoutEdBuf, v, sizeof(v))) {
            snprintf(layoutEdMessage, sizeof(layoutEdMessage), "That is not a character it can type");
          } else if (!keyboardLayoutSetSlot((uint8_t)layoutEdKey, layoutEdSlot, v)) {
            snprintf(layoutEdMessage, sizeof(layoutEdMessage), "Could not save to the SD card");
          } else if (!v[0]) {
            snprintf(layoutEdMessage, sizeof(layoutEdMessage), "%s types nothing now", row);
          } else {
            snprintf(layoutEdMessage, sizeof(layoutEdMessage), "%s now types %s%s", row, v,
                     keyboardLayoutSlotShown(v) ? "" : " (the screen cannot show it)");
          }
          layoutEdEntry = false;
          keyboardLayoutCancelDead();
        } else if (k == HID_KEY_BACKSPACE) {
          int n = (int)strlen(layoutEdBuf);
          while (n > 0 && ((uint8_t)layoutEdBuf[n - 1] & 0xC0) == 0x80) n--;   // whole UTF-8 character
          if (n > 0) n--;
          layoutEdBuf[n] = '\0';
        } else {
          const char* text = keyboardLayoutText(k, event.modifiers, false);
          if (text && text[0] && strlen(layoutEdBuf) + strlen(text) < sizeof(layoutEdBuf) - 1)
            strcat(layoutEdBuf, text);
        }
        screenDirty = true;
        break;
      }

      // Waiting for a key. Bar: Back, and Reset all (asks for a second press) when there are changes.
      if (layoutEdKey < 0) {
        // Up/Down reach the bar (Back, Reset all), Left/Right move along it, Enter presses.
        int noRows = 0, act = -1;
        if (barNav(event, noRows, 0, act) && act < 0) break;
        const uint8_t k = act == 0 ? HID_KEY_BAR0 : act == 1 ? uint8_t(HID_KEY_BAR0 + 1) : event.keyCode;
        if (k == HID_KEY_ESCAPE || k == HID_KEY_BAR0) {
          layoutEdConfirmAll = false;
          currentState = UIState::SETTINGS;
        } else if (k == HID_KEY_BAR0 + 1 && keyboardLayoutChangedCount() > 0) {
          if (!layoutEdConfirmAll) {
            layoutEdConfirmAll = true;
            snprintf(layoutEdMessage, sizeof(layoutEdMessage), "Press Reset all again to undo all %d changed keys",
                     keyboardLayoutChangedCount());
          } else {
            layoutEdConfirmAll = false;
            snprintf(layoutEdMessage, sizeof(layoutEdMessage), "%s",
                     keyboardLayoutResetAll() ? "All keys are back to normal" : "Could not change the SD card");
          }
        } else if (keyboardLayoutIsTextKey(k) && k != HID_KEY_ENTER && k != HID_KEY_SPACE) {
          layoutEdKey = k;
          layoutEdSlot = 0;
          layoutEdMessage[0] = '\0';
          layoutEdConfirmAll = false;
          uiBarFocus = -1;
        }
        screenDirty = true;
        break;
      }

      // One key shown. Up/Down pick the row. Bar: Back / Change / Reset; on a keyboard Esc,
      // Enter and Delete do the same, and another key switches to that key.
      int act = -1;
      if (barNav(event, layoutEdSlot, layoutEdRows(), act) && act < 0) break;
      const uint8_t k = act == 0 ? HID_KEY_ESCAPE : act == 1 ? HID_KEY_ENTER
                      : act == 2 ? HID_KEY_DELETE : event.keyCode;
      if (k == HID_KEY_ESCAPE) {
        layoutEdKey = -1;
        layoutEdMessage[0] = '\0';
      } else if (k == HID_KEY_ENTER) {
        layoutEdEntry = true;
        layoutEdBuf[0] = '\0';
        layoutEdMessage[0] = '\0';
        keyboardLayoutCancelDead();
      } else if (k == HID_KEY_DELETE || k == HID_KEY_BACKSPACE) {
        const uint8_t hid = (uint8_t)layoutEdKey;
        layoutEdRowName(hid, 0, row, sizeof(row));
        const int r = keyboardLayoutResetKey(hid);
        if (r == 0) {
          snprintf(layoutEdMessage, sizeof(layoutEdMessage), "%s is not changed, nothing to reset", row);
        } else if (r < 0) {
          snprintf(layoutEdMessage, sizeof(layoutEdMessage), "Could not save to the SD card");
        } else {
          const char* now = keyboardLayoutSlot(hid, 0, nullptr);
          if (now && now[0]) snprintf(layoutEdMessage, sizeof(layoutEdMessage), "%s is back to normal: types %s", row, now);
          else snprintf(layoutEdMessage, sizeof(layoutEdMessage), "%s is back to normal", row);
        }
      } else if (keyboardLayoutIsTextKey(k) && k != HID_KEY_ENTER && k != HID_KEY_SPACE) {
        layoutEdKey = k;
        layoutEdSlot = 0;
        layoutEdMessage[0] = '\0';
      }
      if (layoutEdSlot >= layoutEdRows()) layoutEdSlot = 0;
      screenDirty = true;
      break;
    }

    case UIState::WIFI_SYNC: {
      // On-screen buttons: the left one is Esc (Back / Cancel / No), the next Enter
      // (Select / Connect / Retry / Yes).
      if (event.keyCode >= HID_KEY_BAR0 && event.keyCode < HID_KEY_BAR0 + 4) {
        syncHandleKey(barKey(event.keyCode - HID_KEY_BAR0, HID_KEY_BAR0 + 2, HID_KEY_BAR0 + 3), 0);
      } else {
        syncHandleKey(event.keyCode, event.modifiers);
      }
      break;
    }

    case UIState::SLEEP_PICTURES: {
      // Rows: 0 Stock, 1 Random, then one per picture, last "+ Add a picture". Use (or Enter on Stock / Random) makes
      // the row the sleep screen. Enter on a picture shows it full screen, where Enter is Use, Left / Right step
      // through the pictures and Delete asks once.
      const int n = sleepscr::count();
      const int rows = n + 3;
      if (sleepPicSel >= rows) sleepPicSel = rows - 1;
      const int pic = (sleepPicSel >= 2 && sleepPicSel < 2 + n) ? sleepPicSel - 2 : -1;
      const uint8_t k = event.keyCode;
      auto use = [&]() {
        if (!sleepPicCanUse()) return;
        if (sleepPicSel == 0) sleepscr::chooseStock();
        else if (sleepPicSel == 1) sleepscr::chooseRandom();
        else if (pic >= 0) sleepscr::choosePicture(pic);
      };

      if (sleepPicDeletePending) {
        if ((k == HID_KEY_ENTER || k == HID_KEY_BAR0 + 1) && pic >= 0) {
          sleepscr::remove(pic);   // the one in use: the choice falls back (Random, the last picture, or Stock)
          const int m = sleepscr::count();
          if (sleepPicSel >= 2 + m) sleepPicSel = m > 0 ? 1 + m : 0;
          if (m == 0) sleepPicPreview = false;
        }
        sleepPicDeletePending = false;
        screenDirty = true;
        break;
      }

      int act = -1;
      if (sleepPicPreview) {
        int none = 0;
        if (barNav(event, none, 0, act) && act < 0) break;
        if (act == 0 || (act < 0 && k == HID_KEY_ESCAPE)) sleepPicPreview = false;
        else if (act == 1 || (act < 0 && k == HID_KEY_ENTER)) use();
        else if (act == 2 || (kClassicUi && act == 3) || (act < 0 && (k == HID_KEY_DELETE || k == HID_KEY_BACKSPACE)))
          sleepPicDeletePending = true;
        else if (act < 0 && k == HID_KEY_LEFT && sleepPicSel > 2) sleepPicSel--;
        else if (act < 0 && k == HID_KEY_RIGHT && sleepPicSel < 1 + n) sleepPicSel++;
        screenDirty = true;
        break;
      }

      if (barNav(event, sleepPicSel, rows, act) && act < 0) break;   // plain navigation
      const bool onAdd = sleepPicSel == rows - 1;
      if (act == 0 || (act < 0 && k == HID_KEY_ESCAPE)) {
        currentState = UIState::SETTINGS;
      } else if ((act == 3 && !(kClassicUi && pic >= 0)) || (act < 0 && k == HID_KEY_ENTER && onAdd)) {
        transfer::openPicture(UIState::SLEEP_PICTURES);   // QR code; the phone sends, the device stores
      } else if (act < 0 && k == HID_KEY_ENTER && pic >= 0) {
        sleepPicPreview = true;
      } else if (act == 1 || (act < 0 && k == HID_KEY_ENTER)) {
        use();
      } else if (pic >= 0 && ((!kClassicUi && act == 2) || (kClassicUi && act == 3) ||
                              (act < 0 && (k == HID_KEY_DELETE || k == HID_KEY_BACKSPACE)))) {
        sleepPicDeletePending = true;
      }
      screenDirty = true;
      break;
    }

    case UIState::FW_UPDATE:
      // Bar: Back (Esc), then Install / Try again (Enter).
      if (event.keyCode >= HID_KEY_BAR0 && event.keyCode < HID_KEY_BAR0 + 4)
        fwupdate::key(barKey(event.keyCode - HID_KEY_BAR0, 0, 0));
      else
        fwupdate::key(event.keyCode);
      screenDirty = true;
      break;

    case UIState::TRANSFER:
      // Bar: Back (Esc), the main action (Enter: Start hotspot / Use hotspot / Try again) and, with no Wi-Fi,
      // Wi-Fi settings (Right).
      if (event.keyCode >= HID_KEY_BAR0 && event.keyCode < HID_KEY_BAR0 + 4)
        transfer::key(barKey(event.keyCode - HID_KEY_BAR0, HID_KEY_RIGHT, 0));
      else
        transfer::key(event.keyCode);
      break;

    default:
      break;
  }
}

// Key repeat for keyboard keys (the X4's own buttons repeat in main.cpp).
static hidcore::KeyRepeat keyRepeat;
bool displayCaughtUp();   // main.cpp
static UIState keyRepeatState = UIState::MAIN_MENU;   // the screen the held key was pressed on

int processAllInput() {
  int processedCount = 0;
  while (!isQueueEmpty()) {
    KeyEvent event = dequeueKeyEvent();
    keyRepeat.onEvent(event.keyCode, event.modifiers, event.pressed, millis());
    if (event.pressed) keyRepeatState = currentState;
    dispatchEvent(event);
    // An accent key waits for the next key and never repeats.
    if (event.pressed && keyboardLayoutDeadPending()) keyRepeat.cancel();
    processedCount++;
  }

  if (keyRepeat.key()) {
    // A dropped keyboard never sends the release; a new screen or an accent waiting stops it too.
    if (!isKeyboardConnected() || currentState != keyRepeatState || keyboardLayoutDeadPending()) {
      keyRepeat.cancel();
    } else {
      const uint8_t mods = bleHeldModifiers();
      const uint8_t k = keyRepeat.tick(millis(), mods, displayCaughtUp());
      if (k) {
        dispatchEvent(KeyEvent{k, mods, true});
        processedCount++;
      }
    }
  }
  return processedCount;
}
