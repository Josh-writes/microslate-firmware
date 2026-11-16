#include "touch_targets.h"

#include <Arduino.h>

#include "config.h"

namespace {

// A full list drops the target, so the tap does nothing: say so in debug builds (the Settings bar was lost that
// way until the button list grew to 12).
bool full(int count, int max, const char* what) {
  if (count < max) return false;
  DBG_PRINTF("[TOUCH] %s list full (%d), a target was dropped\n", what, max);
  (void)what;   // unused in release builds
  return true;
}

struct Row {
  int index;
  int x, y, w, h;
};

constexpr int MAX_ROWS = 24;  // the busiest screen draws ~10 rows
Row rows[MAX_ROWS];
int rowCount = 0;
int selected = 0;
int pitch = 0;

struct Button {
  uint8_t keyCode;
  int x, y, w, h;
};
constexpr int MAX_BUTTONS = 12;   // Settings has the most: 5 tabs + the 4-button bar
Button buttons[MAX_BUTTONS];
int buttonCount = 0;

// Editor view.
struct EditorLine {
  int lineIndex;
  int y;
};
constexpr int MAX_EDITOR_LINES = 64;
EditorLine editorLines[MAX_EDITOR_LINES];
int editorLineCount = 0;
int editorFont = 0;
int editorTextX = 0;
int editorLineH = 0;

// Scrolling-list slider + pending scroll requests.
struct Slider {
  bool active;
  int x, y, w, h;
  int thumbH;
  int count, rows;
};
Slider slider = {};
int pendingRows = 0;
int pendingTo = -1;

struct MenuItem {
  uint8_t action;
  int x, y, w, h;
};
constexpr int MAX_MENU_ITEMS = 4;
MenuItem menuItems[MAX_MENU_ITEMS];
int menuItemCount = 0;
bool menuOpen = false;
int menuAnchorX = 0;
int menuAnchorY = 0;

}  // namespace

void touchEditorMenuOpen(int anchorX, int anchorY) {
  menuOpen = true;
  menuAnchorX = anchorX;
  menuAnchorY = anchorY;
}
void touchEditorMenuClose() { menuOpen = false; menuItemCount = 0; }
bool touchEditorMenuIsOpen() { return menuOpen; }
int touchEditorMenuAnchorX() { return menuAnchorX; }
int touchEditorMenuAnchorY() { return menuAnchorY; }

void touchEditorMenuAddItem(uint8_t action, int x, int y, int w, int h) {
  if (full(menuItemCount, MAX_MENU_ITEMS, "menu")) return;
  menuItems[menuItemCount++] = {action, x, y, w, h};
}

bool touchEditorMenuHit(int px, int py, uint8_t* action) {
  for (int i = 0; i < menuItemCount; i++) {
    const MenuItem& m = menuItems[i];
    if (px >= m.x && px < m.x + m.w && py >= m.y && py < m.y + m.h) {
      *action = m.action;
      return true;
    }
  }
  return false;
}

void touchSliderSet(int x, int y, int w, int h, int thumbH, int count, int rows) {
  slider = {true, x, y, w, h, thumbH, count, rows};
}
bool touchSliderActive() { return slider.active; }
bool touchSliderHit(int px, int py) {
  return slider.active && px >= slider.x && px < slider.x + slider.w && py >= slider.y && py < slider.y + slider.h;
}
int touchSliderStartForY(int py) {
  if (!slider.active || slider.count <= slider.rows) return 0;
  const int travel = slider.h - slider.thumbH;
  if (travel <= 0) return 0;
  int off = py - slider.y - slider.thumbH / 2;
  if (off < 0) off = 0;
  if (off > travel) off = travel;
  const int maxStart = slider.count - slider.rows;
  return (off * maxStart + travel / 2) / travel;
}
void touchScrollRequestRows(int deltaRows) { pendingRows += deltaRows; }
void touchScrollRequestTo(int startRow) { pendingTo = startRow; pendingRows = 0; }
int touchScrollTakeRows() { const int r = pendingRows; pendingRows = 0; return r; }
bool touchScrollTakeTo(int* startRow) {
  if (pendingTo < 0) return false;
  *startRow = pendingTo;
  pendingTo = -1;
  return true;
}
void touchScrollDiscard() { pendingRows = 0; pendingTo = -1; }

void touchTargetsClear() {
  slider.active = false;
  menuItemCount = 0;
  rowCount = 0;
  buttonCount = 0;
  selected = 0;
  pitch = 0;
  editorLineCount = 0;
}

void touchTargetsAddButton(uint8_t keyCode, int x, int y, int w, int h) {
  if (full(buttonCount, MAX_BUTTONS, "button")) return;
  buttons[buttonCount++] = {keyCode, x, y, w, h};
}

int touchTargetsButtonCount() { return buttonCount; }

bool touchTargetsButtonHit(int px, int py, uint8_t* keyCode) {
  for (int i = 0; i < buttonCount; i++) {
    const Button& b = buttons[i];
    if (px >= b.x && px < b.x + b.w && py >= b.y && py < b.y + b.h) {
      *keyCode = b.keyCode;
      return true;
    }
  }
  return false;
}

void touchTargetsBegin(int selectedIndex, int rowPitchPx) {
  selected = selectedIndex;
  pitch = rowPitchPx;
}

void touchTargetsAddRow(int index, int x, int y, int w, int h) {
  if (full(rowCount, MAX_ROWS, "row")) return;
  rows[rowCount++] = {index, x, y, w, h};
}

int touchTargetsCount() { return rowCount; }
int touchTargetsSelected() { return selected; }
int touchTargetsRowPitch() { return pitch; }

bool touchTargetsHit(int px, int py, int* index) {
  for (int i = 0; i < rowCount; i++) {
    const Row& r = rows[i];
    if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) {
      *index = r.index;
      return true;
    }
  }
  return false;
}

void touchEditorViewBegin(int fontId, int textX, int lineHeight) {
  editorFont = fontId;
  editorTextX = textX;
  editorLineH = lineHeight;
  editorLineCount = 0;
}

void touchEditorViewAddLine(int lineIndex, int y) {
  if (full(editorLineCount, MAX_EDITOR_LINES, "editor line")) return;
  editorLines[editorLineCount++] = {lineIndex, y};
}

int touchEditorViewFontId() { return editorFont; }
int touchEditorViewTextX() { return editorTextX; }
int touchEditorViewLineHeight() { return editorLineH; }

bool touchEditorViewLineAt(int py, int* lineIndex) {
  if (editorLineCount == 0 || py < editorLines[0].y) return false;
  for (int i = 0; i < editorLineCount; i++) {
    if (py < editorLines[i].y + editorLineH) {
      *lineIndex = editorLines[i].lineIndex;
      return true;
    }
  }
  *lineIndex = editorLines[editorLineCount - 1].lineIndex;  // margin below the last line
  return true;
}
