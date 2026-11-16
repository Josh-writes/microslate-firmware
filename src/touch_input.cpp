#include "touch_input.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <cmath>
#include <cstring>

#include "config.h"
#include "file_manager.h"
#include "input_handler.h"
#include "text_editor.h"
#include "touch_targets.h"
#include "ui_renderer.h"

extern HalGPIO gpio;
extern GfxRenderer renderer;
extern UIState currentState;
extern bool screenDirty;
extern bool deleteConfirmPending;
extern bool editorFindMode;
extern bool editorExitPrompt;
extern int editorExitSel;
extern WritingMode writingMode;
extern int uiBarFocus;
void registerActivity();  // main.cpp: resets the idle-sleep timer

namespace {

// Edge bands, as fractions of the logical screen (same values as CrossPoint's
// FreeInkUI): a swipe counts as an edge gesture when it STARTS this close to the
// edge and travels away from it with its own axis dominant. Side edges are wider
// because a thumb coming in from the bezel lands further from the edge.
constexpr float EDGE_SIDE_FRAC = 0.25f;
constexpr float EDGE_TOP_BOTTOM_FRAC = 0.14f;
// In the editor the Home swipe must start much closer to the bezel, so scrolling
// the text with a swipe up from low on the page can't exit by accident.
constexpr float EDITOR_HOME_BAND = 0.05f;

// Mid-screen vertical swipe (menu navigation).
constexpr float SWIPE_MIN_MAIN_AXIS = 0.12f;  // travel along the dominant axis
constexpr float SWIPE_AXIS_RATIO = 1.5f;      // dominant axis must beat the other by this
constexpr int MAX_STEPS = 30;                 // cap on Up/Down keys one gesture may queue

struct Point {
  float x;
  float y;
};

// FreeInk reports touch normalized (0..1) in the panel's native landscape frame.
// Invert GfxRenderer::rotateCoordinates() to get the position on the logical
// screen the user is looking at. With a panel inset (X4 Pro) the drawing sits inside it, so the touch is first made
// relative to that area.
Point toLogical(float nx, float ny) {
  const int ix = GfxRenderer::panelInsetX(), iy = GfxRenderer::panelInsetY();
  if (ix || iy) {
    nx = (nx * HalDisplay::DISPLAY_WIDTH - ix) / (HalDisplay::DISPLAY_WIDTH - 2 * ix);
    ny = (ny * HalDisplay::DISPLAY_HEIGHT - iy) / (HalDisplay::DISPLAY_HEIGHT - 2 * iy);
  }
  switch (renderer.getOrientation()) {
    case GfxRenderer::Portrait:
      return {1.0f - ny, nx};
    case GfxRenderer::LandscapeClockwise:
      return {1.0f - nx, 1.0f - ny};
    case GfxRenderer::PortraitInverted:
      return {ny, 1.0f - nx};
    case GfxRenderer::LandscapeCounterClockwise:
    default:
      return {nx, ny};
  }
}

void sendKey(uint8_t key) {
  uiBarFocus = -1;   // touch always acts on the list / the tapped button, never on bar focus
  enqueueKeyEvent(key, 0, true);
  enqueueKeyEvent(key, 0, false);
}

void sendKeyTimes(uint8_t key, int times) {
  if (times > MAX_STEPS) times = MAX_STEPS;
  for (int i = 0; i < times; i++) sendKey(key);
}

// Same behavior as a short press of the power button: save, then main menu.
void goHome() {
  if (currentState == UIState::MAIN_MENU) return;
  if (currentState == UIState::TEXT_EDITOR && editorHasUnsavedChanges()) {
    saveCurrentFile();
  }
  currentState = UIState::MAIN_MENU;
  screenDirty = true;
  registerActivity();
}

// The editor and the text-entry screens are keyboard-only: a stray palm or
// finger must never confirm or move anything there. Only edge swipes get through.
bool freeTouchInState(UIState s) {
  return s != UIState::TEXT_EDITOR && s != UIState::RENAME_FILE && s != UIState::NEW_FILE;
}

// Byte offset in the editor buffer under a touch at logical pixel (px, py), or -1
// when the touch isn't over the text. Picks the nearest character boundary by
// measuring the real (proportional-font) width of each prefix of the line.
int editorOffsetAt(int px, int py) {
  int line = 0;
  if (!touchEditorViewLineAt(py, &line)) return -1;

  const char* buf = editorGetBuffer();
  const int len = static_cast<int>(editorGetLength());
  const int start = editorGetLinePosition(line);
  int end = (line + 1 < editorGetLineCount()) ? editorGetLinePosition(line + 1) : len;
  if (end > start && buf[end - 1] == '\n') end--;  // the newline isn't drawn
  if (end <= start) return start;

  char lineBuf[512];
  int n = end - start;
  if (n > static_cast<int>(sizeof(lineBuf)) - 1) n = static_cast<int>(sizeof(lineBuf)) - 1;
  memcpy(lineBuf, buf + start, n);
  lineBuf[n] = '\0';

  const int fontId = touchEditorViewFontId();
  const int target = px - touchEditorViewTextX();
  if (target <= 0) return start;

  int prevW = 0;
  int pos = 0;
  while (pos < n) {
    int next = pos + 1;
    while (next < n && (static_cast<unsigned char>(lineBuf[next]) & 0xC0) == 0x80) next++;  // whole UTF-8 char
    const int w = editorTextAdvance(renderer, fontId, lineBuf, next);
    if (target < (prevW + w) / 2) return start + pos;  // nearer this character's left edge
    prevW = w;
    pos = next;
  }
  return start + n;  // past the end of the line
}

// Drag-to-scroll in the editor: the text follows the finger. Normal mode scrolls
// the viewport; Pagination turns a page; Typewriter steps the cursor line.
void editorSwipe(bool fingerUp, float dragFrac) {
  const int lineH = touchEditorViewLineHeight() > 0 ? touchEditorViewLineHeight() : 20;
  int lines = static_cast<int>(lroundf(dragFrac * renderer.getScreenHeight() / lineH));
  if (lines < 1) lines = 1;

  switch (writingMode) {
    case WritingMode::NORMAL:
      editorScrollViewport(fingerUp ? lines : -lines);
      break;
    case WritingMode::PAGINATION: {
      const int page = editorGetStoredVisibleLines() > 0 ? editorGetStoredVisibleLines() : 1;
      for (int i = 0; i < page; i++) {
        if (fingerUp) editorMoveCursorDown(false);
        else editorMoveCursorUp(false);
      }
      break;
    }
    case WritingMode::TYPEWRITER:
      for (int i = 0; i < lines && i < MAX_STEPS; i++) {
        if (fingerUp) editorMoveCursorDown(false);
        else editorMoveCursorUp(false);
      }
      break;
  }
  screenDirty = true;
  registerActivity();
}

// Press-and-hold on a word selects it; if the finger keeps going, the selection
// grows or shrinks by whole words under the finger, with the first word as the
// anchor (the iOS/Android behavior).
bool selectDragActive = false;
int dragAnchorLo = 0;
int dragAnchorHi = 0;
int dragLastOffset = -1;

// A finger that lands on text that is ALREADY selected is "armed": if it then
// moves past the slop it becomes a selection drag (extend the highlight); if it
// lifts without moving it stays an ordinary tap.
constexpr int DRAG_SLOP_PX = 14;

// Double-tap on a word selects it. The first tap places the cursor as usual; a
// second tap close by in time and place selects the word. The window is generous
// because an e-ink redraw sits between the taps.
constexpr unsigned long DOUBLE_TAP_MS = 450;
constexpr int DOUBLE_TAP_SLOP_PX = 40;
// A tap leaves a redraw queued for a moment; the line geometry hasn't changed, so
// a follow-up touch inside this window may still use the registered lines.
constexpr unsigned long SELF_DIRTY_MS = 1200;
unsigned long lastTapMs = 0;
int lastTapX = 0;
int lastTapY = 0;
unsigned long lastEditorTouchMs = 0;
bool dragArmed = false;
int armedX = 0;
int armedY = 0;

int logicalX(float nx, float ny) {
  return static_cast<int>(lroundf(toLogical(nx, ny).x * renderer.getScreenWidth()));
}
int logicalY(float nx, float ny) {
  return static_cast<int>(lroundf(toLogical(nx, ny).y * renderer.getScreenHeight()));
}

void extendSelectionTo(float nx, float ny) {
  const Point p = toLogical(nx, ny);
  const int off = editorOffsetAt(static_cast<int>(lroundf(p.x * renderer.getScreenWidth())),
                                 static_cast<int>(lroundf(p.y * renderer.getScreenHeight())));
  if (off < 0 || off == dragLastOffset) return;  // off the text, or nothing new to draw
  dragLastOffset = off;

  int wlo = 0, whi = 0;
  const bool onWord = editorGetWordBounds(off, &wlo, &whi);
  int anchor = dragAnchorLo;
  int cursor = dragAnchorHi;
  if (off <= dragAnchorLo) {  // dragging back before the first word
    anchor = dragAnchorHi;
    cursor = onWord ? wlo : off;
  } else if (off >= dragAnchorHi) {  // dragging forward past it
    anchor = dragAnchorLo;
    cursor = onWord ? whi : off;
  }
  editorSelectRange(anchor, cursor);
  screenDirty = true;
}

// Last finger position seen while a hold/drag was active (logical pixels): where the
// context menu anchors when the finger lifts.
int heldX = 0;
int heldY = 0;

void closeEditorMenu() {
  if (!touchEditorMenuIsOpen()) return;
  touchEditorMenuClose();
  screenDirty = true;
}

void applyMenuAction(uint8_t action) {
  switch (action) {
    case MENU_CUT: editorCut(); break;
    case MENU_COPY: editorCopy(); break;
    case MENU_PASTE: editorPaste(); break;
    case MENU_SELECT_ALL: editorSelectAll(); break;
    default: break;
  }
}

// A finger on a list's slider scrolls it live until it lifts.
bool sliderDragActive = false;
int sliderLastTarget = -1;

// A press and hold that opened the menu (on a space or line end) still has the finger down. Its lift would otherwise
// arrive as a tap outside the menu and close it at once, while the screen still showed it.
bool menuHoldActive = false;

}  // namespace

void processTouch() {
  if (!gpio.hasTouch()) return;

  if (currentState != UIState::TEXT_EDITOR && touchEditorMenuIsOpen()) touchEditorMenuClose();

  // A selection drag owns the finger until it lifts. While held, follow it; on
  // the frame it lifts, swallow that release's tap/swipe so it can't collapse
  // the selection or be read as a gesture.
  if (selectDragActive) {
    float hx = 0, hy = 0;
    if (currentState == UIState::TEXT_EDITOR && gpio.isTouchHeldAt(hx, hy)) {
      extendSelectionTo(hx, hy);
      heldX = logicalX(hx, hy);
      heldY = logicalY(hx, hy);
      registerActivity();
      return;
    }
    selectDragActive = false;
    // Finger lifted after a press-and-hold: offer the context menu at that spot.
    if (currentState == UIState::TEXT_EDITOR && editorHasSelection()) {
      touchEditorMenuOpen(heldX, heldY);
      screenDirty = true;
    }
    return;
  }

  if (menuHoldActive) {
    float hx = 0, hy = 0;
    if (currentState == UIState::TEXT_EDITOR && gpio.isTouchHeldAt(hx, hy)) return;
    menuHoldActive = false;
    float d1 = 0, d2 = 0, d3 = 0, d4 = 0;   // swallow the lift: the menu stays open for the next tap
    gpio.wasSwipe(d1, d2, d3, d4);
    gpio.wasTouchTap(d1, d2);
    return;
  }

  float dnx = 0, dny = 0;
  const bool touchedDown = gpio.wasTouchDown(dnx, dny);

  // A finger that lands on a scrolling list's slider owns the list until it lifts: the
  // window follows the thumb, and a plain tap on the track jumps there.
  if (touchedDown && currentState != UIState::TEXT_EDITOR && touchSliderActive() &&
      touchSliderHit(logicalX(dnx, dny), logicalY(dnx, dny))) {
    sliderDragActive = true;
    sliderLastTarget = touchSliderStartForY(logicalY(dnx, dny));
    touchScrollRequestTo(sliderLastTarget);
    screenDirty = true;
    registerActivity();
    return;
  }
  if (sliderDragActive) {
    float hx = 0, hy = 0;
    if (currentState != UIState::TEXT_EDITOR && gpio.isTouchHeldAt(hx, hy)) {
      const int target = touchSliderStartForY(logicalY(hx, hy));
      if (target != sliderLastTarget) {
        sliderLastTarget = target;
        touchScrollRequestTo(target);
        screenDirty = true;
      }
      registerActivity();
      return;
    }
    sliderDragActive = false;
    // Swallow this finger's own tap / swipe so the lift can't also be read as a gesture.
    float d1 = 0, d2 = 0, d3 = 0, d4 = 0;
    gpio.wasSwipe(d1, d2, d3, d4);
    gpio.wasTouchTap(d1, d2);
    return;
  }

  // Touch-down on an existing selection arms a drag-extend.
  if (touchedDown && currentState == UIState::TEXT_EDITOR && !editorFindMode && !screenDirty &&
      !touchEditorMenuIsOpen()) {
    dragArmed = false;
    int lo = 0, hi = 0;
    if (editorGetSelectionRange(&lo, &hi)) {
      armedX = logicalX(dnx, dny);
      armedY = logicalY(dnx, dny);
      const int off = editorOffsetAt(armedX, armedY);
      if (off >= lo && off <= hi) dragArmed = true;
    }
  }
  if (dragArmed) {
    float hx = 0, hy = 0;
    if (currentState == UIState::TEXT_EDITOR && gpio.isTouchHeldAt(hx, hy)) {
      const int dx = logicalX(hx, hy) - armedX;
      const int dy = logicalY(hx, hy) - armedY;
      if (abs(dx) > DRAG_SLOP_PX || abs(dy) > DRAG_SLOP_PX) {
        int lo = 0, hi = 0;
        if (editorGetSelectionRange(&lo, &hi)) {
          DBG_PRINTLN("[TOUCH] drag from selection -> extend");
          selectDragActive = true;
          dragAnchorLo = lo;
          dragAnchorHi = hi;
          dragLastOffset = -1;
          dragArmed = false;
          extendSelectionTo(hx, hy);
          registerActivity();
          return;
        }
        dragArmed = false;
      }
      // Held but not moved yet: fall through so a stationary hold can still long-press.
    } else {
      dragArmed = false;  // lifted without moving: the normal tap path below handles it
    }
  }

  // Press and hold on a word in the editor selects it, like a phone. The finger
  // may then keep dragging to extend the selection (selectDragActive, above).
  float lx = 0, ly = 0;
  if (gpio.wasTouchLongPress(lx, ly) && currentState == UIState::TEXT_EDITOR && !editorFindMode && !screenDirty &&
      !editorMenu && !spellMenu && !editorExitPrompt && !editorSdWarn) {   // no text press under an open box
    const int lpx = logicalX(lx, ly);
    const int lpy = logicalY(lx, ly);
    uint8_t ignored = 0;
    if (touchEditorMenuIsOpen() && touchEditorMenuHit(lpx, lpy, &ignored)) {
      // Holding a menu button isn't a text press.
    } else {
      const int off = editorOffsetAt(lpx, lpy);
      if (off >= 0 && spellMenuOpenAt(off)) {   // a marked word: its spelling list instead of selecting it
        DBG_PRINTF("[TOUCH] long-press -> spelling list for the word at %d\n", off);
        touchEditorMenuClose();
        menuHoldActive = true;   // swallow the lift, so it can't pick a row of the list that just opened under it
        registerActivity();
        return;
      }
      if (off >= 0 && editorSelectWordAt(off)) {
        DBG_PRINTF("[TOUCH] long-press -> select word at %d (drag to extend)\n", off);
        int lo = 0, hi = 0;
        editorGetSelectionRange(&lo, &hi);
        selectDragActive = true;
        dragAnchorLo = lo;
        dragAnchorHi = hi;
        dragLastOffset = -1;
        heldX = lpx;
        heldY = lpy;
        touchEditorMenuClose();  // reopens at the lift point
        screenDirty = true;
        registerActivity();
        return;
      }
      if (off >= 0) {
        // Not on a word (space / line end): cursor goes there and the menu offers Paste.
        DBG_PRINTF("[TOUCH] long-press -> cursor %d + menu\n", off);
        editorSetCursorPosition(off, false);
        touchEditorMenuOpen(lpx, lpy);
        menuHoldActive = true;
        screenDirty = true;
        registerActivity();
        return;
      }
    }
  }

  // Always drain this frame's gestures, even when ignoring them, so a swipe made
  // in the editor can't be replayed later as a tap.
  float sx = 0, sy = 0, ex = 0, ey = 0;
  const bool swiped = gpio.wasSwipe(sx, sy, ex, ey);
  float tx = 0, ty = 0;
  const bool tapped = !swiped && gpio.wasTouchTap(tx, ty);
  if (!swiped && !tapped) return;

  if (swiped) {
    closeEditorMenu();
    const Point a = toLogical(sx, sy);
    const Point b = toLogical(ex, ey);
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float adx = fabsf(dx);
    const float ady = fabsf(dy);

    // Edge gestures first (and in every state): deliberate, bezel-anchored.
    if (a.x <= EDGE_SIDE_FRAC && dx > 0 && adx > ady) {
      DBG_PRINTLN("[TOUCH] left-edge swipe right -> BACK");
      sendKey(HID_KEY_ESCAPE);
      return;
    }
    const float homeBand = (currentState == UIState::TEXT_EDITOR) ? EDITOR_HOME_BAND : EDGE_TOP_BOTTOM_FRAC;
    if (a.y >= 1.0f - homeBand && dy < 0 && ady > adx) {
      DBG_PRINTLN("[TOUCH] bottom-edge swipe up -> HOME");
      goHome();
      return;
    }

    if (currentState == UIState::TEXT_EDITOR) {
      if (!editorFindMode && ady >= SWIPE_MIN_MAIN_AXIS && ady >= adx * SWIPE_AXIS_RATIO) {
        DBG_PRINTF("[TOUCH] editor swipe %s\n", dy < 0 ? "up" : "down");
        editorSwipe(dy < 0, ady);
      }
      return;
    }
    if (!freeTouchInState(currentState)) return;
    if (ady >= SWIPE_MIN_MAIN_AXIS && ady >= adx * SWIPE_AXIS_RATIO) {
      const int pitch = touchTargetsRowPitch();
      if (touchSliderActive() && pitch > 0) {
        // Phone-style: the list follows the finger and the highlight stays put. Finger
        // up = later rows come into view.
        int rows = static_cast<int>(lroundf(ady * renderer.getScreenHeight() / pitch));
        if (rows < 1) rows = 1;
        DBG_PRINTF("[TOUCH] list swipe %s x%d -> scroll\n", dy < 0 ? "up" : "down", rows);
        touchScrollRequestRows(dy < 0 ? rows : -rows);
        screenDirty = true;
        registerActivity();
        return;
      }
      // A list that fits has nothing to scroll: the swipe moves the highlight instead,
      // by as many rows as the finger dragged across.
      int rows = 1;
      if (pitch > 0) {
        rows = static_cast<int>(lroundf(ady * renderer.getScreenHeight() / pitch));
        if (rows < 1) rows = 1;
      }
      DBG_PRINTF("[TOUCH] swipe %s x%d\n", dy < 0 ? "up -> UP" : "down -> DOWN", rows);
      sendKeyTimes(dy < 0 ? HID_KEY_UP : HID_KEY_DOWN, rows);
    }
    return;
  }

  // Tap in the editor puts the cursor there (and drops any selection).
  if (currentState == UIState::TEXT_EDITOR) {
    const unsigned long now = millis();
    if (editorSdWarn) {   // the SD card warning: a tap goes back to writing, like any key
      editorSdWarn = 0;
      screenDirty = true;
      registerActivity();
      return;
    }
    if (editorMenu) {   // the editor menu: a tap on a row highlights it, a tap on the highlighted row changes it
      const Point mp = toLogical(tx, ty);
      int row = -1;
      if (editorMenu == 1 && touchTargetsHit(static_cast<int>(lroundf(mp.x * renderer.getScreenWidth())),
                                             static_cast<int>(lroundf(mp.y * renderer.getScreenHeight())), &row)) {
        DBG_PRINTF("[TOUCH] editor menu row %d\n", row);
        editorMenuTap(row);
      } else {
        editorMenu = editorMenu == 2 ? 1 : 0;   // the Shortcuts page goes back to the menu; outside the menu closes it
        screenDirty = true;
      }
      registerActivity();
      return;
    }
    if (spellMenu) {   // the spelling list: a tap on a row picks it; outside the list closes it
      const Point sp = toLogical(tx, ty);
      int row = -1;
      if (touchTargetsHit(static_cast<int>(lroundf(sp.x * renderer.getScreenWidth())),
                          static_cast<int>(lroundf(sp.y * renderer.getScreenHeight())), &row)) {
        DBG_PRINTF("[TOUCH] spelling list row %d\n", row);
        spellMenuTap(row);
      } else {
        spellMenu = false;
        screenDirty = true;
      }
      registerActivity();
      return;
    }
    if (editorExitPrompt) {   // Transfer or Exit: a tap on a button picks it; anywhere else goes back to writing
      const Point bp = toLogical(tx, ty);
      uint8_t key = 0;
      if (touchTargetsButtonHit(static_cast<int>(lroundf(bp.x * renderer.getScreenWidth())),
                                static_cast<int>(lroundf(bp.y * renderer.getScreenHeight())), &key)) {
        DBG_PRINTF("[TOUCH] exit prompt button key 0x%02x\n", key);
        if (key == HID_KEY_ENTER) editorExitSel = 0;   // the Transfer button
        sendKey(key);
      } else {
        editorExitPrompt = false;
        screenDirty = true;
      }
      registerActivity();
      return;
    }
    if (editorFindMode) return;
    if (touchEditorMenuIsOpen()) {
      // A tap on a menu button runs it; a tap anywhere else just dismisses the menu.
      const Point mp = toLogical(tx, ty);
      uint8_t action = 0;
      if (touchEditorMenuHit(static_cast<int>(lroundf(mp.x * renderer.getScreenWidth())),
                             static_cast<int>(lroundf(mp.y * renderer.getScreenHeight())), &action)) {
        DBG_PRINTF("[TOUCH] context menu action %d\n", action);
        applyMenuAction(action);
      }
      touchEditorMenuClose();
      screenDirty = true;
      registerActivity();
      return;
    }
    if (screenDirty && now - lastEditorTouchMs > SELF_DIRTY_MS) return;  // stale rows
    const Point p = toLogical(tx, ty);
    const int px = static_cast<int>(lroundf(p.x * renderer.getScreenWidth()));
    const int py = static_cast<int>(lroundf(p.y * renderer.getScreenHeight()));
    {
      uint8_t key = 0;   // the menu icon in the header
      if (touchTargetsButtonHit(px, py, &key) && key == HID_KEY_EDITOR_MENU) {
        DBG_PRINTLN("[TOUCH] editor menu icon");
        sendKey(key);
        registerActivity();
        return;
      }
    }
    const int off = editorOffsetAt(px, py);
    if (off < 0) return;  // header / outside the text

    const bool doubleTap = lastTapMs != 0 && now - lastTapMs <= DOUBLE_TAP_MS &&
                           abs(px - lastTapX) <= DOUBLE_TAP_SLOP_PX && abs(py - lastTapY) <= DOUBLE_TAP_SLOP_PX;
    lastTapMs = doubleTap ? 0 : now;  // a double-tap consumes both taps
    lastTapX = px;
    lastTapY = py;
    lastEditorTouchMs = now;

    if (doubleTap && editorSelectWordAt(off)) {
      DBG_PRINTF("[TOUCH] double-tap (%d,%d) -> select word at %d\n", px, py, off);
    } else if (!editorHasSelection() && off == editorGetCursorPosition()) {
      // A tap on the cursor opens the menu there (Paste, Select all), as on a phone.
      DBG_PRINTF("[TOUCH] tap on the cursor (%d,%d) -> menu\n", px, py);
      touchEditorMenuOpen(px, py);
    } else {
      DBG_PRINTF("[TOUCH] tap (%d,%d) -> cursor %d\n", px, py, off);
      editorSetCursorPosition(off, false);
    }
    screenDirty = true;
    registerActivity();
    return;
  }
  // A redraw is queued, so the registered rows no longer match the glass.
  if (screenDirty) return;

  const Point p = toLogical(tx, ty);

  // On-screen action buttons act on a single tap, whatever rows are present. They work
  // on every screen, including the text-entry ones and the confirmation prompts (which
  // draw their own Cancel / Delete buttons).
  {
    const int bx = static_cast<int>(lroundf(p.x * renderer.getScreenWidth()));
    const int by = static_cast<int>(lroundf(p.y * renderer.getScreenHeight()));
    uint8_t key = 0;
    if (touchTargetsButtonHit(bx, by, &key)) {
      DBG_PRINTF("[TOUCH] tap (%d,%d) -> button key 0x%02x\n", bx, by, key);
      sendKey(key);
      return;
    }
  }

  // Text-entry screens take no other touch; neither does a confirmation prompt.
  if (!freeTouchInState(currentState)) return;
  if (deleteConfirmPending) return;

  // Screens with a button bar but no rows: a tap elsewhere does nothing.
  if (touchTargetsCount() == 0 && touchTargetsButtonCount() > 0) return;

  // Screens that publish no rows (status, pairing code): a tap is a plain Confirm.
  if (touchTargetsCount() == 0) {
    DBG_PRINTF("[TOUCH] tap (%.2f,%.2f) -> CONFIRM\n", p.x, p.y);
    sendKey(HID_KEY_ENTER);
    return;
  }

  // Tap on a row: the first tap only highlights it; tapping the highlighted row
  // again opens it, so a stray touch never launches something by accident.
  const int px = static_cast<int>(lroundf(p.x * renderer.getScreenWidth()));
  const int py = static_cast<int>(lroundf(p.y * renderer.getScreenHeight()));
  int hit = 0;
  if (!touchTargetsHit(px, py, &hit)) {
    DBG_PRINTF("[TOUCH] tap (%d,%d) -> no row\n", px, py);
    return;
  }
  const int delta = hit - touchTargetsSelected();
  if (abs(delta) > MAX_STEPS) return;  // too far to walk reliably; ignore rather than mis-activate
  DBG_PRINTF("[TOUCH] tap (%d,%d) -> row %d (selected %d, delta %d)\n", px, py, hit, touchTargetsSelected(), delta);
  if (delta != 0) {
    sendKeyTimes(delta < 0 ? HID_KEY_UP : HID_KEY_DOWN, abs(delta));
    return;
  }
  sendKey(HID_KEY_ENTER);
}
