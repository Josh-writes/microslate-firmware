#include "ui_renderer.h"
#include "frontlight.h"
#include "config.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "sync/cloud_service.h"
#include "qr/qrcodegen.h"
#include "editor_prof.h"
#include "keyboard_layout.h"
#include "transfer/transfer_rules.h"
#include "transfer/transfer_service.h"
#include "update/fw_update.h"
#include "sleep/sleep_screen.h"
#include "input_handler.h"   // the editor menu and SD warning state
#include "settings_menu.h"
#include "spell/spell_dict.h"
#include "spell/spell_service.h"

#include <algorithm>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <Keymap.h>
#include <HalPowerManager.h>
#include "touch_targets.h"
#include <HalDisplay.h>
#include <EpdFont.h>
#include <EpdFontFamily.h>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern bool deleteConfirmPending;
extern WritingMode writingMode;
extern FontSize fontSize;
extern bool showWordCount;
extern int layoutEdKey;
extern bool editorExitPrompt;
extern int editorExitSel;
extern uint8_t editorSdWarn;
extern bool newNotesMd;
extern char renameExt[5];
extern int layoutEdSlot;
extern bool layoutEdEntry;
extern char layoutEdBuf[24];
extern char layoutEdMessage[96];
extern bool layoutEdConfirmAll;
void layoutEdRowName(uint8_t hid, int slot, char* out, size_t size);
const char* layoutEdKeyName(uint8_t hid, char* out, size_t size);
int layoutEdRows();

// Font data includes
#include <builtinFonts/notosans_16_regular.h>
#include <builtinFonts/notosans_16_bold.h>
#include <builtinFonts/notosans_14_regular.h>
#include <builtinFonts/notosans_14_bold.h>
#include <builtinFonts/notosans_12_regular.h>
#include <builtinFonts/notosans_12_bold.h>
#include <builtinFonts/ubuntu_10_regular.h>
#include <builtinFonts/ubuntu_10_bold.h>

// Font objects (file-scoped)
static EpdFont ns16Regular(&notosans_16_regular);
static EpdFont ns16Bold(&notosans_16_bold);
static EpdFontFamily ns16Family(&ns16Regular, &ns16Bold);

static EpdFont ns14Regular(&notosans_14_regular);
static EpdFont ns14Bold(&notosans_14_bold);
static EpdFontFamily ns14Family(&ns14Regular, &ns14Bold);

// Layout files are checked against the editor font (keyboard_layout.cpp).
bool uiFontHasGlyph(uint32_t cp) { return ns14Family.getGlyph(cp) != nullptr; }

static EpdFont ns12Regular(&notosans_12_regular);
static EpdFont ns12Bold(&notosans_12_bold);
static EpdFontFamily ns12Family(&ns12Regular, &ns12Bold);

static EpdFont u10Regular(&ubuntu_10_regular);
static EpdFont u10Bold(&ubuntu_10_bold);
static EpdFontFamily u10Family(&u10Regular, &u10Bold);

// OTA app detection (defined in main.cpp)

// Extern shared state (defined in main.cpp)
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int settingsTab;
extern bool settingsForgetPending;
extern int sleepPicSel;
extern bool sleepPicPreview;
extern bool sleepPicDeletePending;
extern int bluetoothDeviceSelection;
extern bool btShowAllDevices;
extern int uiBarFocus;
extern int pairedKeyboardSelection;
extern bool pairedForgetPending;
extern int myWordsSel;
extern bool myWordsRemovePending;
extern Orientation currentOrientation;
extern int charsPerLine;
extern char renameBuffer[];
extern int renameBufferLen;
extern bool editorFindMode;
extern char findBuffer[];

void rendererSetup(GfxRenderer& renderer) {
  renderer.insertFont(FONT_LARGE, ns16Family);
  renderer.insertFont(FONT_BODY, ns14Family);
  renderer.insertFont(FONT_UI, ns12Family);
  renderer.insertFont(FONT_SMALL, u10Family);
}

// ---------------------------------------------------------------------------
// Clipped draw helpers — use renderer.truncatedText() so NO pixel ever
// exceeds screen width.  This is how crosspoint-reader prevents GFX errors.
// ---------------------------------------------------------------------------

// Draw text that is guaranteed not to overflow the screen width.
// maxW = available pixel width from x to right edge (caller computes).
// Falls back to sw - x - 5 if maxW <= 0.
static void drawClippedText(GfxRenderer& r, int font, int x, int y,
                            const char* text, int maxW = 0,
                            bool black = true,
                            EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0 || x >= sw || y < 0 || y >= sh) return;

  if (maxW <= 0) maxW = sw - x - 5;   // 5px right margin
  if (maxW <= 0) return;

  auto clipped = r.truncatedText(font, text, maxW, style);
  if (!clipped.empty()) {
    r.drawText(font, x, y, clipped.c_str(), black, style);
  }
}

// Draw right-aligned text (e.g. battery %, RSSI, settings values).
// Computes its own X from the measured text width.
static void drawRightText(GfxRenderer& r, int font, int rightEdge, int y,
                          const char* text, bool black = true,
                          EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  if (!text || !text[0]) return;
  // Use getTextWidth (bounding box) — same measurement truncatedText uses —
  // so the allocated space always matches what the truncation check expects.
  int tw = r.getTextWidth(font, text, style);
  if (tw <= 0) tw = 30;                    // safe fallback
  int x = rightEdge - tw;
  if (x < 5) x = 5;                        // don't go off left edge
  drawClippedText(r, font, x, y, text, rightEdge - x, black, style);
}

// Safe line — just clamp to screen
static void clippedLine(GfxRenderer& r, int x1, int y1, int x2, int y2,
                        bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  // Clamp rather than reject
  auto clamp = [](int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); };
  x1 = clamp(x1, 0, sw - 1);
  x2 = clamp(x2, 0, sw - 1);
  y1 = clamp(y1, 0, sh - 1);
  y2 = clamp(y2, 0, sh - 1);
  r.drawLine(x1, y1, x2, y2, state);
}

// Safe fillRect — clamp dimensions to screen
static void clippedFillRect(GfxRenderer& r, int x, int y, int w, int h,
                            bool state = true) {
  int sw = r.getScreenWidth();
  int sh = r.getScreenHeight();
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > sw) w = sw - x;
  if (y + h > sh) h = sh - y;
  if (w > 0 && h > 0) r.fillRect(x, y, w, h, state);
}

// ---------------------------------------------------------------------------
// Helper: draw battery percentage in top-right
// ---------------------------------------------------------------------------
static void drawBattery(GfxRenderer& renderer, HalGPIO& gpio, int y = 5) {
  int pct = powerManager.getBatteryPercentage();
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  drawRightText(renderer, FONT_SMALL, renderer.getScreenWidth() - 12, y, buf, !darkMode);
}

// Helper: draw BLE status
static void drawBleStatus(GfxRenderer& renderer, int x, int y) {
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "KB Connected"; break;
    case BLEState::SCANNING:     status = "Scanning..."; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "KB Disconnected"; break;
  }
  drawClippedText(renderer, FONT_SMALL, x, y, status, 0, !darkMode);
}

// ---------------------------------------------------------------------------
// Touch-first layout shared by every screen: a large title header, tall list
// cards that scroll in a window, and a four-button bar along the bottom. On the
// X4 the same bar labels sit above its physical Back / Confirm / Left / Right
// buttons, so one layout serves both devices. Bar buttons send HID_KEY_BAR0 + i;
// the same code is produced by Enter on the focused button, so touch and the
// physical buttons share one path through the input handler.
// ---------------------------------------------------------------------------
struct BarButton {
  const char* label;   // nullptr/empty = nothing drawn
  bool enabled;        // disabled buttons are left blank and not tappable
  bool dim = false;    // a disabled button that is still drawn (thin outline): the bar keeps its shape
  int arrow = 0;       // -1 / +1: drawn as a left / right triangle instead of the label (the fonts have no arrows)
};

// The Prev / Next pair of a value row or the tab strip.
static constexpr BarButton kBarLeft = {"Prev", true, false, -1};
static constexpr BarButton kBarRight = {"Next", true, false, 1};

// kClassicUi (E1001) swaps in the original v2.1 proportions: a slim header, plain highlighted
// rows and a compact button strip. Every other device keeps the large touch-friendly layout.
static constexpr bool CLASSIC       = kClassicUi;
static constexpr int HDR_H          = CLASSIC ? 36 : 60;   // title band (rule sits 4 px above its bottom)
static constexpr int LIST_ROW_H     = CLASSIC ? 29 : 60;   // drawn card height
static constexpr int LIST_ROW_PITCH = CLASSIC ? 30 : 68;   // card + gap; also the swipe row height
static constexpr int MENU_ROW_H     = CLASSIC ? 35 : 72;   // main-menu cards are a little taller
static constexpr int MENU_ROW_PITCH = CLASSIC ? 45 : 80;
static constexpr int BAR_H          = CLASSIC ? 34 : 64;
static constexpr int BAR_MARGIN     = CLASSIC ? 4 : 6;
// Row and title fonts: the old design used the 12 pt UI font throughout.
static constexpr int LIST_FONT_L    = CLASSIC ? FONT_UI : FONT_LARGE;
static constexpr int LIST_FONT_B    = CLASSIC ? FONT_UI : FONT_BODY;

static int barTopY(GfxRenderer& r) { return r.getScreenHeight() - BAR_H - BAR_MARGIN; }

// Title on the left, battery on the right, a heavy rule underneath.
static void drawScreenHeader(GfxRenderer& r, HalGPIO& gpio, const char* title, bool tc) {
  const int sw = r.getScreenWidth();
  if (CLASSIC) {
    drawClippedText(r, FONT_SMALL, 10, 5, title, sw - 90, tc, EpdFontFamily::BOLD);
    drawBattery(r, gpio, 5);
    clippedLine(r, 5, HDR_H - 4, sw - 5, HDR_H - 4, tc);
    return;
  }
  const int lh = r.getLineHeight(FONT_LARGE);
  const int ty = (HDR_H - 6 - lh) / 2;
  drawClippedText(r, FONT_LARGE, 12, ty < 2 ? 2 : ty, title, sw - 110, tc, EpdFontFamily::BOLD);
  drawBattery(r, gpio, (HDR_H - 6 - r.getLineHeight(FONT_SMALL)) / 2);
  clippedFillRect(r, 8, HDR_H - 4, sw - 16, 2, tc);
}

struct ListGeom {
  int top;      // y of the first card
  int bottom;   // lowest y a card may reach
  int rows;     // whole cards that fit
  int inset;    // right-hand gutter reserved for the slider (0 when the list fits)
};

static constexpr int SLIDER_W = CLASSIC ? 14 : 36;   // touch zone of the slider on the right edge

// `count` = total rows; when they don't all fit, a gutter is reserved on the right for
// the slider. `reserveBottom` keeps room for a status line.
static ListGeom listGeom(GfxRenderer& r, bool hasBar, int pitch, int count = 0, int reserveBottom = 0,
                         int extraTop = 0) {
  ListGeom g;
  g.top = HDR_H + 6 + extraTop;
  g.bottom = (hasBar ? barTopY(r) - 6 : r.getScreenHeight() - 8) - reserveBottom;
  // The last card needs only its own height, not the gap below it (8 px on the large layout, 1 on the
  // classic one): without this a Sticky in landscape showed one note fewer than fits.
  g.rows = (g.bottom - g.top + (CLASSIC ? 1 : 8)) / pitch;
  if (g.rows < 1) g.rows = 1;
  g.inset = (count > g.rows) ? SLIDER_W : 0;
  return g;
}

// Keep `sel` inside the window without jumping: the window only moves when the
// selection leaves it.
static void keepInView(int& start, int sel, int count, int rows) {
  if (sel < start) start = sel;
  if (sel >= start + rows) start = sel - rows + 1;
  if (start > count - rows) start = count - rows;
  if (start < 0) start = 0;
}

// A list's scroll window. It follows the highlighted row when that changes (keys, a tap),
// and otherwise obeys the touch layer's scroll requests (swipe, slider), so scrolling
// by touch leaves the highlight where it is.
struct ListView {
  int start = 0;
  int lastSel = -1;
};

static void viewWindow(ListView& v, int sel, int count, int rows) {
  if (sel != v.lastSel) {
    keepInView(v.start, sel, count, rows);
    v.lastSel = sel;
  }
  int to = 0;
  if (touchScrollTakeTo(&to)) v.start = to;
  v.start += touchScrollTakeRows();
  const int maxStart = count > rows ? count - rows : 0;
  if (v.start > maxStart) v.start = maxStart;
  if (v.start < 0) v.start = 0;
}

// Slider on the right edge of a list that scrolls: a track, a thumb sized to the visible
// fraction, and (registered with the touch layer) a wide touch zone. Drag the thumb, or
// tap the track, to jump; swiping the list itself scrolls it like a phone.
static void drawScrollbar(GfxRenderer& r, bool tc, const ListGeom& g, int pitch, int start, int count) {
  if (count <= g.rows) return;
  const int sw = r.getScreenWidth();
  const int trackH = g.rows * pitch - 8;
  int thumbH = (trackH * g.rows) / count;
  if (thumbH < 44) thumbH = 44;
  if (thumbH > trackH) thumbH = trackH;
  const int travel = trackH - thumbH;
  const int maxStart = count - g.rows;
  const int thumbY = g.top + (maxStart > 0 ? (travel * start) / maxStart : 0);
  const int cx = sw - SLIDER_W / 2 - 4;
  clippedFillRect(r, cx - 1, g.top, 2, trackH, tc);          // track
  if (CLASSIC) {
    r.fillRect(cx - 3, thumbY, 6, thumbH, tc);               // slim thumb
  } else {
    r.fillRect(cx - 9, thumbY, 18, thumbH, tc);              // thumb
    r.fillRect(cx - 5, thumbY + thumbH / 2 - 1, 10, 2, !tc); // grip mark
  }
  touchSliderSet(sw - SLIDER_W, g.top, SLIDER_W, trackH, thumbH, count, g.rows);
}

// One list card. Selected = solid while the list has focus; a heavy outline while the
// bottom bar has focus (so you can still see which row it applies to). Returns the
// text colour to use on it.
static bool drawCard(GfxRenderer& r, bool tc, int y, int h, bool selected, bool barHasFocus, int inset = 0) {
  const int cardW = r.getScreenWidth() - 16 - inset;
  const bool filled = selected && !barHasFocus;
  if (CLASSIC) {
    // Plain list: the highlighted row is a solid bar, the rest have no frame.
    if (filled) clippedFillRect(r, 5, y, cardW + 6, h, tc);
    else if (selected) r.drawRect(5, y, cardW + 6, h, 2, tc);
    return filled ? !tc : tc;
  }
  if (filled) clippedFillRect(r, 8, y, cardW, h, tc);
  else if (selected) r.drawRect(8, y, cardW, h, 3, tc);
  else r.drawRect(8, y, cardW, h, 1, tc);
  return filled ? !tc : tc;
}

// Tap zone for a card: the whole pitch across the full width, so neighbouring rows
// tile with no dead strip between them.
static void addCardRow(int index, int y, int sw, int pitch, int inset = 0) {
  touchTargetsAddRow(index, 0, y - 4, sw - inset, pitch);
}

// `focus` = index of the keyboard-focused button (-1 none): drawn filled.
uint8_t uiBarMask();
static void drawButtonBar(GfxRenderer& r, const BarButton (&btns)[4], bool tc, int focus = -1, int slots = 4) {
  const int sw = r.getScreenWidth();
  const int top = barTopY(r);
  if (CLASSIC) {
    // Button-only device: no touch buttons. Screens driven by the list navigation get one
    // "Back" row at the bottom (Down past the last row reaches it, Select presses it);
    // confirmation screens get a one-line hint instead.
    const uint8_t m = uiBarMask();
    if (m & 1) {
      // Back on the left; the screen's one extra action (Forget / Scan), when it has one, on the right.
      const bool extra = (m & 8) && btns[3].label && btns[3].label[0];
      const int lh = r.getLineHeight(LIST_FONT_B);
      clippedLine(r, 5, top - 3, sw - 5, top - 3, tc);
      for (int slot = 0; slot < (extra ? 2 : 1); slot++) {
        const int idx = slot == 0 ? 0 : 3;
        const bool sel = (focus == idx);
        const int x = 5 + slot * ((sw - 10) / 2);
        const int w = extra ? (sw - 10) / 2 - 3 : sw - 10;
        if (sel) clippedFillRect(r, x, top, w, BAR_H, tc);
        else if (extra) r.drawRect(x, top, w, BAR_H, 1, tc);
        char lab[24];
        if (idx == 0) snprintf(lab, sizeof(lab), "< %s", (btns[0].label && btns[0].label[0]) ? btns[0].label : "Back");
        else snprintf(lab, sizeof(lab), "%s", btns[3].label);
        drawClippedText(r, LIST_FONT_B, x + 10, top + (BAR_H - lh) / 2, lab, w - 20, sel ? !tc : tc);
      }
    } else if ((deleteConfirmPending || pairedForgetPending) && btns[0].label && btns[1].label) {
      char hint[64];
      snprintf(hint, sizeof(hint), "Select: %s    Any other key: %s", btns[1].label, btns[0].label);
      clippedLine(r, 5, top - 3, sw - 5, top - 3, tc);
      const int lh = r.getLineHeight(FONT_SMALL);
      drawClippedText(r, FONT_SMALL, 10, top + (BAR_H - lh) / 2, hint, sw - 20, tc);
    }
    return;
  }
  clippedLine(r, 5, top - 3, sw - 5, top - 3, tc);
  const int gap = CLASSIC ? 4 : 6;
  const int w = (sw - 2 * 8 - (slots - 1) * gap) / slots;
  for (int i = 0; i < slots; i++) {
    if (!btns[i].label || !btns[i].label[0]) continue;
    const int x = 8 + i * (w + gap);
    if (!btns[i].enabled) {
      if (!btns[i].dim) continue;
      r.drawRect(x, top, w, BAR_H, 1, tc);
      const int dlh = r.getLineHeight(FONT_UI);
      const int dtw = r.getTextWidth(FONT_UI, btns[i].label, EpdFontFamily::REGULAR);
      drawClippedText(r, FONT_UI, x + (w - dtw) / 2, top + (BAR_H - dlh) / 2, btns[i].label, w - 8, tc);
      continue;   // not tappable, not reachable with the keys
    }
    const bool focused = (i == focus);
    if (focused) clippedFillRect(r, x, top, w, BAR_H, tc);
    else         r.drawRect(x, top, w, BAR_H, CLASSIC ? 1 : 2, tc);
    if (btns[i].arrow) {
      const int h = BAR_H * 2 / 5;   // triangle height; its width is a little less
      const int cx = x + w / 2, cy = top + BAR_H / 2, half = h * 2 / 5;
      const int tip = cx + btns[i].arrow * half, base = cx - btns[i].arrow * half;
      const int xs[3] = {tip, base, base};
      const int ys[3] = {cy, cy - h / 2, cy + h / 2};
      r.fillPolygon(xs, ys, 3, focused ? !tc : tc);
      touchTargetsAddButton(static_cast<uint8_t>(HID_KEY_BAR0 + i), x, top, w, r.getScreenHeight() - top);
      continue;
    }
    int font = LIST_FONT_B;
    int tw = r.getTextWidth(font, btns[i].label, EpdFontFamily::BOLD);
    if (tw > w - 10) {
      font = FONT_UI;
      tw = r.getTextWidth(font, btns[i].label, EpdFontFamily::BOLD);
    }
    if (tw > w - 10) {
      font = FONT_SMALL;
      tw = r.getTextWidth(font, btns[i].label, EpdFontFamily::BOLD);
    }
    const int lh = r.getLineHeight(font);
    int tx = x + (w - tw) / 2;
    if (tx < x + 4) tx = x + 4;
    drawClippedText(r, font, tx, top + (BAR_H - lh) / 2, btns[i].label, w - 8,
                    focused ? !tc : tc, EpdFontFamily::BOLD);
    touchTargetsAddButton(static_cast<uint8_t>(HID_KEY_BAR0 + i), x, top, w, r.getScreenHeight() - top);
  }
}

// "Delete this note?" and the like: the question, what it is about, an optional warning, and Cancel / the action.
// Takes over the list area of a list screen and presents the screen.
static void drawConfirmPrompt(GfxRenderer& r, bool tc, const ListGeom& g, const char* question, const char* subject,
                              const char* warning, const char* action) {
  const int sw = r.getScreenWidth();
  const int y = g.top + 16;
  const int subjectY = y + r.getLineHeight(LIST_FONT_L) + 14;
  drawClippedText(r, LIST_FONT_L, 20, y, question, sw - 40, tc, EpdFontFamily::BOLD);
  drawClippedText(r, LIST_FONT_B, 20, subjectY, subject, sw - 40, tc);
  if (warning) drawClippedText(r, FONT_UI, 20, subjectY + r.getLineHeight(LIST_FONT_B) + 14, warning, sw - 40, tc);
  const BarButton bar[4] = {{"Cancel", true}, {action, true}, {nullptr, false}, {nullptr, false}};
  drawButtonBar(r, bar, tc, -1);
  r.beginRefresh(HalDisplay::FAST_REFRESH);
}

// An empty list: a bold line and a hint saying how to fill it.
static void drawEmptyList(GfxRenderer& r, bool tc, const ListGeom& g, const char* title, const char* hint) {
  const int sw = r.getScreenWidth();
  drawClippedText(r, LIST_FONT_L, 20, g.top + 16, title, sw - 40, tc, EpdFontFamily::BOLD);
  drawClippedText(r, FONT_UI, 20, g.top + 16 + r.getLineHeight(LIST_FONT_L) + 10, hint, sw - 40, tc);
}

// Which Bluetooth-bar buttons are usable right now.
static uint8_t btBarMask() {
  int visIdx[32];
  int total = 0, kbd = 0;
  const int visCount = getVisibleDeviceIndices(btShowAllDevices, visIdx, 32, &total, &kbd);
  const int itemCount = visCount + ((kbd > 0 && total > kbd) ? 1 : 0);
  const bool scanning = isDeviceScanning();
  const bool pairing = getCurrentPasskey() > 0;
  uint8_t m = 1;                                               // Back
  if (itemCount > 0 && !scanning && !pairing) m |= 2;          // Connect / toggle
  if (isKeyboardConnected() && !pairing) m |= 4;               // Disconnect
  if (!scanning && !pairing) m |= 8;                           // Scan
  return m;
}

// Sleep screen list: the highlighted row can be chosen (Use): Stock, Random with two or more pictures, a picture that
// is not already in use. Shared with the input handler.
bool sleepPicCanUse() {
  const int n = sleepscr::count();
  if (sleepPicSel == 0) return sleepscr::choice() != sleepscr::Choice::Stock;
  if (sleepPicSel == 1) return n >= 2 && sleepscr::choice() != sleepscr::Choice::Random;
  if (sleepPicSel >= 2 && sleepPicSel < 2 + n) return !sleepscr::isChosen(sleepPicSel - 2);
  return false;
}

// Which bottom-bar buttons are usable on the current screen (bit i = button i; 0 = the
// screen has no bar). Shared by the drawing code and the input handler so keyboard
// focus never lands on a blank button.
uint8_t uiBarMask() {
  if (CLASSIC) {
    // Only a "Back" row, on the screens that have list navigation.
    switch (currentState) {
      case UIState::FILE_BROWSER:       return deleteConfirmPending ? 0 : (selectedFileIndex >= 1 ? 9 : 1);   // Back / Transfer
      case UIState::PAIRED_KEYBOARDS:   return pairedForgetPending ? 0 : (getPairedKeyboardCount() > 0 ? 9 : 1);  // Back / Forget
      case UIState::BLUETOOTH_SETTINGS: return 1 | (btBarMask() & 8);                                            // Back / Scan
      case UIState::SETTINGS:           // Back (/ Forget on a saved Wi-Fi network)
        return settingKind(settingsTab, settingsSelection - 1) == SettingKind::SavedWifi ? 9 : 1;
      case UIState::SLEEP_PICTURES:     return sleepPicDeletePending ? 0 : 9;                   // Back / Add (Delete on a picture and in preview)
      case UIState::MY_WORDS:           return myWordsRemovePending ? 0 : 1;                    // Back
      default:                          return 0;
    }
  }
  switch (currentState) {
    case UIState::FILE_BROWSER:
      if (deleteConfirmPending) return 0x3;                    // Cancel / Delete
      return selectedFileIndex >= 1 ? 0xF : 0x1;               // Back / Rename / Delete / Transfer
    case UIState::SETTINGS: {
      if (settingsSelection <= 0) return 0xD;                  // tab strip: Back / Prev / Next (tab)
      switch (settingKind(settingsTab, settingsSelection - 1)) {
        case SettingKind::SavedWifi: return settingsForgetPending ? 0x9 : 0xB;   // Back / Prefer / Forget (Cancel / Forget)
        case SettingKind::Value: return 0xD;                   // Back / Prev / Next (Enter also steps forward)
        default: return 0x3;                                   // Back / Open
      }
    }
    case UIState::SLEEP_PICTURES: {
      if (sleepPicDeletePending) return 0x3;                   // Cancel / Delete
      const int n = sleepscr::count();
      const bool onPic = sleepPicSel >= 2 && sleepPicSel < 2 + n;
      if (sleepPicPreview) return 0x5 | (sleepPicCanUse() ? 0x2 : 0);   // Back / Use / Delete
      return 0x9 | (sleepPicCanUse() ? 0x2 : 0) | (onPic ? 0x4 : 0);    // Back / Use / Delete / Add
    }
    case UIState::PAIRED_KEYBOARDS: {
      if (pairedForgetPending) return 0x3;                     // Cancel / Forget
      uint8_t m = 1;
      if (getPairedKeyboardCount() > 0) {
        m |= 2 | 8;                                            // Connect, Forget
        if (isKeyboardConnected()) m |= 4;                     // Disconnect
      }
      return m;
    }
    case UIState::MY_WORDS:
      return myWordsRemovePending || spellsvc::ownCount() > 0 ? 0x3 : 0x1;   // Back / Remove (Cancel / Remove)
    case UIState::BLUETOOTH_SETTINGS:
      return btBarMask();
    case UIState::LAYOUT_EDITOR:
      if (layoutEdEntry) return 0x3;                           // Cancel / Save
      if (layoutEdKey < 0) return keyboardLayoutChangedCount() > 0 ? 0x3 : 0x1;   // Back (/ Reset all)
      return 0x7;                                              // Back / Change / Reset
    default:
      return 0;
  }
}

// ===========================================================================
// Screen drawing functions
// ===========================================================================

void drawMainMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  if (CLASSIC) {
    renderer.drawCenteredText(FONT_BODY, 30, "MicroSlate", tc, EpdFontFamily::BOLD);
    drawBattery(renderer, gpio, 5);
  } else {
    drawScreenHeader(renderer, gpio, "MicroSlate", tc);
  }

  static const char* menuItems[] = {"Notes", "Settings", "Transfer", "Sync"};
  const int menuCount = 4;

  // The keyboard status line lives under the cards; the cards scroll if they don't fit.
  const int statusH = 34;
  const ListGeom g = listGeom(renderer, false, MENU_ROW_PITCH, menuCount, statusH, CLASSIC ? 48 : 0);

  static ListView lv;
  viewWindow(lv, mainMenuSelection, menuCount, g.rows);
  const int start = lv.start;
  const int cw = sw - g.inset;

  touchTargetsBegin(mainMenuSelection, MENU_ROW_PITCH);
  const int lhL = renderer.getLineHeight(LIST_FONT_L);
  for (int i = 0; i < g.rows && (start + i) < menuCount; i++) {
    const int pos = start + i;
    const int y = g.top + i * MENU_ROW_PITCH;
    addCardRow(pos, y, sw, MENU_ROW_PITCH, g.inset);
    const bool ink = drawCard(renderer, tc, y, MENU_ROW_H, pos == mainMenuSelection, false, g.inset);
    const char* label = menuItems[pos];
    drawClippedText(renderer, LIST_FONT_L, 22, y + (MENU_ROW_H - lhL) / 2, label, cw - 50, ink);
  }
  drawScrollbar(renderer, tc, g, MENU_ROW_PITCH, start, menuCount);

  drawBleStatus(renderer, 14, sh - statusH + (statusH - renderer.getLineHeight(FONT_SMALL)) / 2);
  if (cloud::glyph() == cloud::Glyph::Syncing) {
    char syncing[40];
    snprintf(syncing, sizeof(syncing), "%s syncing", cloud::providerName());
    drawRightText(renderer, FONT_SMALL, sw - 14, sh - statusH + (statusH - renderer.getLineHeight(FONT_SMALL)) / 2, syncing, !darkMode);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawFileBrowser(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawScreenHeader(renderer, gpio, "Notes", tc);
  // Background sync: "OneDrive syncing", then "OneDrive synced", in the header beside the battery (the bottom
  // corner, where the main menu shows it, is the button bar here).
  const char* cs = cloud::listStatus();
  if (cs[0]) {
    const int fy = CLASSIC ? 5 : (HDR_H - 6 - renderer.getLineHeight(FONT_SMALL)) / 2;
    drawRightText(renderer, FONT_SMALL, sw - (CLASSIC ? 64 : 76), fy, cs, tc);
  }

  const int fc = getFileCount();
  FileInfo* files = getFileList();
  const ListGeom g = listGeom(renderer, true, LIST_ROW_PITCH, getFileCount() + 1);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  const int lhU = renderer.getLineHeight(FONT_UI);
  const int barFocus = (uiBarFocus >= 0 && ((uiBarMask() >> uiBarFocus) & 1)) ? uiBarFocus : -1;

  touchTargetsBegin(selectedFileIndex, LIST_ROW_PITCH);

  // Rows: 0 = "+ New note", then one per file (file = row - 1).
  const int rows = fc + 1;
  const bool onFile = (selectedFileIndex >= 1 && selectedFileIndex <= fc);

  if (deleteConfirmPending && onFile) {
    drawConfirmPrompt(renderer, tc, g, "Delete this note?", files[selectedFileIndex - 1].title,
                      "This cannot be undone.", "Delete");
    return;
  }

  static ListView lv;
  viewWindow(lv, selectedFileIndex, rows, g.rows);
  const int start = lv.start;
  const int cw = sw - g.inset;
  for (int i = 0; i < g.rows && (start + i) < rows; i++) {
    const int pos = start + i;
    const int y = g.top + i * LIST_ROW_PITCH;
    addCardRow(pos, y, sw, LIST_ROW_PITCH, g.inset);
    const bool ink = drawCard(renderer, tc, y, LIST_ROW_H, pos == selectedFileIndex, barFocus >= 0, g.inset);
    if (pos == 0) {
      drawClippedText(renderer, LIST_FONT_B, 20, y + (LIST_ROW_H - lhB) / 2, "+  New note", cw - 40, ink,
                      EpdFontFamily::BOLD);
    } else {
      // An .md note says so at the right of its row (a .txt note and an .md note can share a title).
      int titleW = cw - 40;
      if (strcmp(noteExtOf(files[pos - 1].filename), ".md") == 0) {
        const int tagW = renderer.getTextWidth(FONT_SMALL, "md", EpdFontFamily::BOLD);
        const int lhS = renderer.getLineHeight(FONT_SMALL);
        drawClippedText(renderer, FONT_SMALL, cw - 20 - tagW, y + (LIST_ROW_H - lhS) / 2, "md", tagW + 4, ink,
                        EpdFontFamily::BOLD);
        titleW -= tagW + 12;
      }
      drawClippedText(renderer, LIST_FONT_B, 20, y + (LIST_ROW_H - lhB) / 2, files[pos - 1].title, titleW, ink);
    }
  }
  drawScrollbar(renderer, tc, g, LIST_ROW_PITCH, start, rows);
  if (sdCardMissing()) {
    drawClippedText(renderer, FONT_UI, 20, g.top + LIST_ROW_PITCH + 14, "No SD card found. Insert one to see your notes.",
                    sw - 40, tc);
  } else if (fc == 0) {
    drawClippedText(renderer, FONT_UI, 20, g.top + LIST_ROW_PITCH + 14, "No notes yet. Tap New note to start writing.",
                    sw - 40, tc);
  }
  (void)lhU;
  (void)sh;

  // Large layout: Back, then three buttons for the highlighted note; a note opens with a second tap (or Enter).
  // Classic (E1001, buttons only): the Back row, with Transfer on the right.
  if (CLASSIC) {
    const BarButton bar[4] = {{"Back", true}, {nullptr, false}, {nullptr, false}, {"Transfer", onFile}};
    drawButtonBar(renderer, bar, tc, barFocus);
  } else {
    // On "+ New note" the three stay in place, dimmed, so the bar never comes and goes.
    const BarButton bar[4] = {{"Back", true}, {"Rename", onFile, true}, {"Delete", onFile, true}, {"Transfer", onFile, true}};
    drawButtonBar(renderer, bar, tc, barFocus);
  }

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

int editorTabWidth(GfxRenderer& renderer, int fontId) {
  return kEditorTabSpaces * renderer.getTextAdvanceX(fontId, " ");
}

int editorTextAdvance(GfxRenderer& renderer, int fontId, const char* text, int len) {
  char seg[512];
  int w = 0;
  int i = 0;
  while (i < len) {
    if (text[i] == '\t') {
      w += editorTabWidth(renderer, fontId);
      i++;
      continue;
    }
    int n = 0;
    while (i + n < len && text[i + n] != '\t' && n < (int)sizeof(seg) - 1) n++;
    memcpy(seg, text + i, n);
    seg[n] = '\0';
    w += renderer.getTextAdvanceX(fontId, seg);
    i += n;
  }
  return w;
}

// Draws text[0, len) from x with each tab as a gap of editorTabWidth; nothing past x + maxW.
static void drawEditorText(GfxRenderer& r, int fontId, int x, int y, const char* text, int len, int maxW, bool black) {
  char seg[512];
  const int right = x + maxW;
  int i = 0;
  while (i < len && x < right) {
    if (text[i] == '\t') {
      x += editorTabWidth(r, fontId);
      i++;
      continue;
    }
    int n = 0;
    while (i + n < len && text[i + n] != '\t' && n < (int)sizeof(seg) - 1) n++;
    memcpy(seg, text + i, n);
    seg[n] = '\0';
    drawClippedText(r, fontId, x, y, seg, right - x, black);
    x += r.getTextAdvanceX(fontId, seg);
    i += n;
  }
}

// Helper: draw a single editor line from the buffer
static void drawEditorLine(GfxRenderer& renderer, int lineIdx, int x, int yPos,
                           int lineHeight, int maxW, bool tc) {
  char* buf = editorGetBuffer();
  size_t bufLen = editorGetLength();
  int totalLines = editorGetLineCount();

  int lineStart = editorGetLinePosition(lineIdx);
  int lineEnd = (lineIdx + 1 < totalLines) ? editorGetLinePosition(lineIdx + 1) : (int)bufLen;
  int dispEnd = lineEnd;
  if (dispEnd > lineStart && buf[dispEnd - 1] == '\n') dispEnd--;

  int len = dispEnd - lineStart;
  if (len <= 0) return;

  char lineBuf[512];
  int copyLen = (len < (int)sizeof(lineBuf) - 1) ? len : (int)sizeof(lineBuf) - 1;
  strncpy(lineBuf, buf + lineStart, copyLen);
  lineBuf[copyLen] = '\0';

  int fontId = editorFontId(fontSize);

  // Draw the line normally first.
  drawEditorText(renderer, fontId, x, yPos, lineBuf, copyLen, maxW, tc);

  // Spellcheck: a zigzag under each word not in the list (work order 1.8). The word the cursor sits at the end of is
  // still being typed, so it is left alone until the next space or punctuation.
  if (spellsvc::on()) {
    MS_PROF("d-spell");
    const int cursorOff = editorGetCursorPosition() - lineStart;
    const int zy = yPos + lineHeight - 4;   // just under the descenders
    size_t s = 0, e = 0, from = 0;
    while (spell::nextWord(lineBuf, (size_t)copyLen, from, &s, &e)) {
      from = e;
      if ((int)e == cursorOff) continue;
      if (spellsvc::wordOk(lineBuf + s, e - s)) continue;
      const int x0 = x + editorTextAdvance(renderer, fontId, lineBuf, (int)s);
      const int x1 = std::min(x + maxW, x0 + editorTextAdvance(renderer, fontId, lineBuf + s, (int)(e - s)));
      // Pixel by pixel: GfxRenderer::drawLine only does straight lines (a slanted one logs an error and draws nothing).
      static const int kZig[4] = {2, 1, 0, 1};
      const int sw = renderer.getScreenWidth(), sh = renderer.getScreenHeight();
      for (int zx = std::max(0, x0); zx < x1 && zx < sw; zx++) {
        const int py = zy + kZig[(zx - x0) & 3];
        if (py >= 0 && py < sh) renderer.drawPixel(zx, py, tc);
      }
    }
  }

  // Overlay the selection on this line as inverse video, if any overlaps it.
  int selLo, selHi;
  if (editorGetSelectionRange(&selLo, &selHi)) {
    int a = (selLo > lineStart) ? selLo : lineStart;       // clamp to line
    int b = (selHi < dispEnd)  ? selHi : dispEnd;
    int aOff = a - lineStart;
    int bOff = b - lineStart;
    if (aOff < 0) aOff = 0;
    if (bOff > copyLen) bOff = copyLen;
    if (bOff > aOff) {
      int selLen = bOff - aOff;
      int xA = x + editorTextAdvance(renderer, fontId, lineBuf, aOff);
      int selW = editorTextAdvance(renderer, fontId, lineBuf + aOff, selLen);
      // Solid block in the text colour, then the selected text in the background
      // colour on top → inverse video.
      clippedFillRect(renderer, xA, yPos, selW, lineHeight, tc);
      drawEditorText(renderer, fontId, xA, yPos, lineBuf + aOff, selLen, maxW - (xA - x), !tc);
    }
  }
}

// Helper: draw cursor at the given screen position
static void drawEditorCursor(GfxRenderer& renderer, int cursorY, int lineHeight,
                             int sw, bool tc) {
  int curLine = editorGetCursorLine();
  int curPos = editorGetCursorPosition();
  char* buf = editorGetBuffer();

  int lineStart = editorGetLinePosition(curLine);
  char prefix[512];
  int prefixLen = curPos - lineStart;
  if (prefixLen < 0) prefixLen = 0;
  if (prefixLen > (int)sizeof(prefix) - 1) prefixLen = (int)sizeof(prefix) - 1;
  while (prefixLen > 0 && ((static_cast<unsigned char>(buf[lineStart + prefixLen]) & 0xC0) == 0x80)) {
    prefixLen--;
  }
  strncpy(prefix, buf + lineStart, prefixLen);
  prefix[prefixLen] = '\0';

  int cursorX = 10 + editorTextAdvance(renderer, editorFontId(fontSize), prefix, prefixLen);
  int cursorW = renderer.getSpaceWidth(editorFontId(fontSize));
  if (cursorW < 2) cursorW = 8;

  if (cursorX >= 0 && cursorX + cursorW <= sw && cursorY >= 0 && cursorY + lineHeight <= renderer.getScreenHeight()) {
    renderer.fillRect(cursorX, cursorY, cursorW, lineHeight, tc);
  }
}

// Get the mode indicator string for the current writing mode
static const char* getModeIndicator() {
  switch (writingMode) {
    case WritingMode::TYPEWRITER: return "[T]";
    case WritingMode::PAGINATION: return "[P]";
    default:                      return "[S]";
  }
}

// Helper: draw the standard editor header, returns textAreaTop
// centerText is optional text drawn centered in the header (e.g. "Page 1/3")
static int drawEditorHeader(GfxRenderer& renderer, HalGPIO& gpio, int sw, bool tc,
                            const char* centerText = nullptr) {
  if (cleanMode && !sdLost()) {   // with the card gone the header comes back, so "No SD card" is seen
    // No header in clean mode: a refused key still says why, in a small box in the top corner.
    if (editorNoteFullShowing()) {
      const int w = renderer.getTextAdvanceX(FONT_SMALL, "Note full") + 12;
      const int h = renderer.getLineHeight(FONT_SMALL) + 6;
      clippedFillRect(renderer, sw - w - 4, 2, w, h, tc);
      drawClippedText(renderer, FONT_SMALL, sw - w + 2, 5, "Note full", w, !tc, EpdFontFamily::BOLD);
    }
    return 8;
  }

  const char* title = editorGetCurrentTitle();
  char headerBuf[64];
  if (editorHasUnsavedChanges()) {
    snprintf(headerBuf, sizeof(headerBuf), "%s *", title);
  } else {
    strncpy(headerBuf, title, sizeof(headerBuf) - 1);
    headerBuf[sizeof(headerBuf) - 1] = '\0';
  }
  // Card pulled: "No SD card" next to the battery (where the notes list says "OneDrive syncing"), so it always has
  // room; the text is safe in memory and saves itself when the card is back.
  int rightEdge = sw - 70;
  if (sdLost()) {
    const int nw = renderer.getTextWidth(FONT_SMALL, "No SD card", EpdFontFamily::BOLD);
    drawClippedText(renderer, FONT_SMALL, rightEdge - nw, 5, "No SD card", nw + 5, tc, EpdFontFamily::BOLD);
    rightEdge -= nw + 12;
  }
  // Mode indicator — right-anchored before the battery (and "No SD card")
  const char* modeInd = getModeIndicator();
  int modeW = renderer.getTextAdvanceX(FONT_SMALL, modeInd);
  int modeX = rightEdge - modeW;
  drawClippedText(renderer, FONT_SMALL, modeX, 5, modeInd, modeW + 5, tc);

  // Layout indicator — shown to the left of the mode indicator when not US
  // so the user always knows which layout Ctrl+K will toggle away from.
  int layoutIndX = modeX - 8;
  if (!keyboardLayoutIsUs()) {
    const char* layoutInd = keyboardLayoutShortName();
    int layoutW = renderer.getTextAdvanceX(FONT_SMALL, layoutInd);
    layoutIndX = modeX - 8 - layoutW;
    if (layoutIndX > 10)
      drawClippedText(renderer, FONT_SMALL, layoutIndX, 5, layoutInd, layoutW + 5, tc);
  }

  // Word count — drawn to the left of the layout indicator (or mode indicator). For a few seconds after a key or paste
  // did not fit, "Note full" takes its place (shown even when the word count is off).
  int titleMaxW = layoutIndX - 10;
  const bool noteFull = editorNoteFullShowing();
  if (showWordCount || noteFull) {
    const EpdFontFamily::Style st = noteFull ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    char wcBuf[24];
    if (noteFull)     snprintf(wcBuf, sizeof(wcBuf), "Note full");
    else {
      int wc = editorGetWordCount();
      if (wc == 1) snprintf(wcBuf, sizeof(wcBuf), "1 word");
      else         snprintf(wcBuf, sizeof(wcBuf), "%d words", wc);
    }
    int wcW = renderer.getTextWidth(FONT_SMALL, wcBuf, st);
    int wcX = layoutIndX - 8 - wcW;
    if (wcX > 10) {
      drawClippedText(renderer, FONT_SMALL, wcX, 5, wcBuf, wcW + 5, tc, st);
      titleMaxW = wcX - 10;
    }
  }

  // Menu icon (three bars) before the title on touch boards: a tap opens the editor menu, like Ctrl+M.
  int titleX = 10;
  if (!CLASSIC) {
    for (int i = 0; i < 3; i++) renderer.fillRect(12, 9 + i * 6, 18, 2, tc);
    touchTargetsAddButton(HID_KEY_EDITOR_MENU, 0, 0, 44, 32);
    titleX = 40;
    titleMaxW -= 30;
  }

  // Title — stops before word count (or mode indicator if word count hidden)
  drawClippedText(renderer, FONT_SMALL, titleX, 5, headerBuf, titleMaxW, tc, EpdFontFamily::BOLD);

  // Cloud mark right after the title, where the * sits: only when it has something to say (the * covers unsaved
  // text). An outline while saved changes wait for OneDrive, with a "!" when the last sync failed.
  const int cloudMark = editorHasUnsavedChanges() ? 0 : cloud::noteMark();
  if (cloudMark && !sdLost()) {
    const int cx = titleX + renderer.getTextWidth(FONT_SMALL, headerBuf, EpdFontFamily::BOLD) + 8, cy = 7;
    if (cx + 22 <= titleX + titleMaxW) {
      renderer.drawRoundedRect(cx, cy + 5, 22, 10, 2, 5, tc);
      renderer.drawRoundedRect(cx + 5, cy, 12, 12, 2, 6, tc);
      renderer.fillRect(cx + 4, cy + 7, 14, 6, !tc);   // hide the overlap so only the outline remains
      if (cloudMark == 2) {
        renderer.fillRect(cx + 10, cy + 5, 2, 5, tc);
        renderer.fillRect(cx + 10, cy + 11, 2, 2, tc);
      }
    }
  }

  // Centered text (e.g. page indicator)
  if (centerText) {
    int ctW = renderer.getTextAdvanceX(FONT_SMALL, centerText);
    drawClippedText(renderer, FONT_SMALL, (sw - ctW) / 2, 5, centerText, ctW + 5, tc);
  }

  drawBattery(renderer, gpio);
  clippedLine(renderer, 5, 32, sw - 5, 32, tc);
  return 38;
}

// Bottom overlay bar showing the current find query. Drawn over the editor while
// find mode is active (the matched text itself highlights via the selection).
static void drawFindBar(GfxRenderer& renderer, int sw, int sh, bool tc) {
  int barH = renderer.getLineHeight(FONT_UI) + 10;
  int barY = sh - barH;
  clippedFillRect(renderer, 0, barY, sw, barH, !tc);   // clear to background
  clippedLine(renderer, 0, barY, sw, barY, tc);        // top divider
  char line[72];
  snprintf(line, sizeof(line), "Find: %s", findBuffer);
  drawClippedText(renderer, FONT_UI, 8, barY + 5, line, sw - 100, tc);
  drawRightText(renderer, FONT_SMALL, sw - 8, barY + 7, "Enter:Next Esc:Close", tc);
}

// A pop-up list over the note (the editor menu, the spelling list): a framed box sized to fit the screen, a bold
// title, an optional line under it, touch rows with the selected one filled, and a key hint at the bottom.
// drawRow(i, boxX, boxW, textY, ink, selected) draws row i's text; the box and the selection are drawn here.
template <typename DrawRow>
static void drawOverlayList(GfxRenderer& r, bool tc, int maxW, const char* title, const char* note, int rows, int sel,
                            const char* hint, DrawRow drawRow) {
  const int sw = r.getScreenWidth();
  const int sh = r.getScreenHeight();
  const int lh = r.getLineHeight(FONT_UI);
  const int lhB = r.getLineHeight(LIST_FONT_B);
  // Fits the screen: rows get shorter on a short screen (the Sticky is 480 px high in landscape).
  const int frame = 16 + lhB + 12 + (note ? lh + 8 : 0) + 10 + lh + 12;
  const int rowH = std::max(lh + 6, std::min(lh + 22, (sh - 8 - frame) / rows));
  const int w = std::min(sw - 24, maxW);
  const int h = std::min(sh - 8, frame + rows * rowH);
  const int x = (sw - w) / 2;
  const int y = std::max(4, (sh - h) / 2);
  clippedFillRect(r, x, y, w, h, !tc);
  r.drawRect(x, y, w, h, 3, tc);
  drawClippedText(r, LIST_FONT_B, x + 16, y + 16, title, w - 32, tc, EpdFontFamily::BOLD);
  int top = y + 16 + lhB + 12;
  if (note) {
    drawClippedText(r, FONT_UI, x + 18, top, note, w - 36, tc);
    top += lh + 8;
  }
  touchTargetsBegin(sel, rowH);
  for (int i = 0; i < rows; i++) {
    const int ry = top + i * rowH;
    const bool selected = i == sel;
    if (selected) clippedFillRect(r, x + 8, ry, w - 16, rowH - 4, tc);
    drawRow(i, x, w, ry + (rowH - 4 - lh) / 2, selected ? !tc : tc, selected);
    if (!CLASSIC) touchTargetsAddRow(i, x + 8, ry, w - 16, rowH - 4);
  }
  drawClippedText(r, FONT_SMALL, x + 16, y + h - lh - 8, hint, w - 32, tc);
}

// The editor menu (Ctrl+M or the menu icon; input_handler.h): a box over the note with the writing settings, or the
// Shortcuts page. Rows are touch rows (the first tap highlights, a tap on the highlighted row changes it).
static void drawEditorMenu(GfxRenderer& r, bool tc) {
  if (!editorMenu) return;
  const int sw = r.getScreenWidth();
  const int sh = r.getScreenHeight();
  const int lh = r.getLineHeight(FONT_UI);
  const int lhB = r.getLineHeight(LIST_FONT_B);

  if (editorMenu == 2) {   // Shortcuts
    static const char* kKeys[][2] = {
        {"Ctrl+M", "This menu"},            {"Ctrl+S", "Save"},
        {"Ctrl+Z", "Undo"},                 {"Ctrl+Y", "Redo (or Ctrl+Shift+Z)"},
        {"Ctrl+A", "Select all"},           {"Ctrl+C, X, V", "Copy, cut, paste"},
        {"Ctrl+F", "Find"},                 {"Ctrl+N", "Rename the note"},
        {"Ctrl+T", "Typewriter mode"},      {"Ctrl+P", "Pagination mode"},
        {"Ctrl+H", "Focus mode"},           {"Ctrl+W", "Word count"},
        {"Ctrl+K", "Next keyboard layout"}, {"Shift+arrows", "Select text"},
        {"Tab", "Indent"},                  {"Esc", "Transfer or exit"},
        {"Ctrl+J", "Spelling suggestions"},   // last: only builds with spellcheck show it
    };
    const int kN = sizeof(kKeys) / sizeof(kKeys[0]) - (spellsvc::available() ? 0 : 1);
    const bool twoCols = sw >= 700;
    const int rows = twoCols ? (kN + 1) / 2 : kN;
    // Fits the screen: the line pitch shrinks on a short screen (a box past the edge costs a log line per pixel).
    const int frame = 16 + lhB + 14 + 10 + lh + 14;
    const int pitch = std::max(lh + 1, std::min(lh + 8, (sh - 8 - frame) / rows));
    const int w = sw - 24;
    const int h = std::min(sh - 8, frame + rows * pitch);
    const int x = 12;
    const int y = std::max(4, (sh - h) / 2);
    clippedFillRect(r, x, y, w, h, !tc);
    r.drawRect(x, y, w, h, 3, tc);
    drawClippedText(r, LIST_FONT_B, x + 16, y + 16, "Shortcuts", w - 32, tc, EpdFontFamily::BOLD);
    const int colW = twoCols ? (w - 32) / 2 : w - 32;
    const int keyW = std::min(colW / 2, 170);
    for (int i = 0; i < kN; i++) {
      const int col = twoCols ? i % 2 : 0;
      const int row = twoCols ? i / 2 : i;
      const int cx = x + 16 + col * colW;
      const int cy = y + 16 + lhB + 14 + row * pitch;
      drawClippedText(r, FONT_UI, cx, cy, kKeys[i][0], keyW - 8, tc, EpdFontFamily::BOLD);
      drawClippedText(r, FONT_UI, cx + keyW, cy, kKeys[i][1], colW - keyW - 8, tc);
    }
    drawClippedText(r, FONT_SMALL, x + 16, y + h - lh - 10,
                    CLASSIC ? "Back returns to the menu" : "Esc or a tap returns to the menu", w - 32, tc);
    return;
  }

  drawOverlayList(r, tc, 560, "Editor", nullptr, editorMenuRows(), editorMenuSel,
                  CLASSIC ? "Up/Down pick, Select changes, Back closes" : "Up/Down pick, Enter changes, Esc closes",
                  [&](int i, int x, int w, int ty, bool ink, bool sel) {
                    drawClippedText(r, FONT_UI, x + 18, ty, editorMenuLabel(i), w / 2 - 20, ink,
                                    sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
                    char val[40];
                    editorMenuValue(i, val, sizeof(val));
                    if (editorMenuOpensPage(i)) snprintf(val, sizeof(val), ">");
                    drawRightText(r, FONT_UI, x + w - 18, ty, val, ink);
                  });
}

// The spelling list (Ctrl+J or a long press on a marked word; input_handler.h): the word, its suggestions, then
// "Add to dictionary". A tap on a row picks it.
static void drawSpellMenu(GfxRenderer& r, bool tc) {
  if (!spellMenu) return;
  const int rows = spellMenuRows();
  // With no suggestions a line says so above "Add to dictionary", the last row.
  drawOverlayList(r, tc, 480, spellMenuWord(), rows == 1 ? "No suggestions" : nullptr, rows, spellMenuSel,
                  CLASSIC ? "Up/Down pick, Select picks, Back closes" : "Up/Down pick, Enter picks, Esc closes",
                  [&](int i, int x, int w, int ty, bool ink, bool sel) {
                    char label[spell::kMaxWord + 4];
                    if (i == rows - 1) snprintf(label, sizeof(label), "%s", spellMenuLabel(i));
                    else snprintf(label, sizeof(label), "%d  %s", i + 1, spellMenuLabel(i));   // 1-5 from the keys
                    drawClippedText(r, FONT_UI, x + 18, ty, label, w - 36, ink,
                                    sel ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
                  });
}

// Long-press context menu (touch): Cut/Copy when there is a selection, Paste when
// the clipboard has text, Select all always. Sits above the finger, or below it
// when there is no room. Only items that can act are shown, so none is a dead tap.
static void drawEditorContextMenu(GfxRenderer& r, int lineHeight, bool tc) {
  if (!touchEditorMenuIsOpen()) return;
  struct Item { uint8_t action; const char* label; };
  Item items[4];
  int n = 0;
  if (editorHasSelection()) {
    items[n++] = {MENU_CUT, "Cut"};
    items[n++] = {MENU_COPY, "Copy"};
  }
  if (editorHasClipboard()) items[n++] = {MENU_PASTE, "Paste"};
  items[n++] = {MENU_SELECT_ALL, "Select all"};

  const int sw = r.getScreenWidth();
  const int sh = r.getScreenHeight();
  const int gap = 4;
  const int h = 56;
  int w = (sw - 16 - (n - 1) * gap) / n;
  if (w > 140) w = 140;
  const int total = n * w + (n - 1) * gap;

  int x = touchEditorMenuAnchorX() - total / 2;
  if (x < 8) x = 8;
  if (x > sw - 8 - total) x = sw - 8 - total;
  const int ay = touchEditorMenuAnchorY();
  int y = ay - lineHeight - 12 - h;
  if (y < 4) y = ay + lineHeight + 40;
  if (y > sh - h - 4) y = sh - h - 4;

  for (int i = 0; i < n; i++) {
    const int ix = x + i * (w + gap);
    clippedFillRect(r, ix, y, w, h, !tc);
    r.drawRect(ix, y, w, h, 2, tc);
    int font = FONT_UI;
    int tw = r.getTextWidth(font, items[i].label, EpdFontFamily::BOLD);
    if (tw > w - 10) {
      font = FONT_SMALL;
      tw = r.getTextWidth(font, items[i].label, EpdFontFamily::BOLD);
    }
    const int lh = r.getLineHeight(font);
    int tx = ix + (w - tw) / 2;
    if (tx < ix + 4) tx = ix + 4;
    drawClippedText(r, font, tx, y + (h - lh) / 2, items[i].label, w - 8, tc, EpdFontFamily::BOLD);
    touchEditorMenuAddItem(items[i].action, ix, y, w, h);
  }
}


// Esc in the editor: Transfer (highlighted, Enter) or Exit (Esc again).
static void drawExitPrompt(GfxRenderer& r, bool tc) {
  if (!editorExitPrompt) return;
  const int sw = r.getScreenWidth();
  const int sh = r.getScreenHeight();
  const int lhB = r.getLineHeight(LIST_FONT_B);
  const int lhS = r.getLineHeight(FONT_SMALL);
  // Three buttons: Transfer, Exit, Back (to the note); narrower on a narrow screen.
  const int bw = std::min(CLASSIC ? 150 : 170, (sw - 24 - 4 * 16) / 3);
  const int bh = CLASSIC ? 40 : 60;
  const int w = 3 * bw + 4 * 16;
  const int h = 16 + bh + 12 + lhS + 16;
  const int x = (sw - w) / 2;
  const int y = (sh - h) / 2;
  clippedFillRect(r, x, y, w, h, !tc);
  r.drawRect(x, y, w, h, 3, tc);
  static const char* kLabels[3] = {"Transfer", "Exit", "Back"};
  for (int i = 0; i < 3; i++) {
    const int bx = x + 16 + i * (bw + 16);
    const int by = y + 16;
    const bool sel = editorExitSel == i;
    if (sel) clippedFillRect(r, bx, by, bw, bh, tc);
    else r.drawRect(bx, by, bw, bh, 2, tc);
    // A tap: Transfer sends Enter (touch_input.cpp picks Transfer first), Exit sends Esc, Back any other key.
    if (!CLASSIC)
      touchTargetsAddButton(i == 0 ? HID_KEY_ENTER : i == 1 ? HID_KEY_ESCAPE : HID_KEY_BACKSPACE, bx, by, bw, bh);
    const int tw = r.getTextWidth(LIST_FONT_B, kLabels[i], EpdFontFamily::BOLD);
    drawClippedText(r, LIST_FONT_B, bx + (bw - tw) / 2, by + (bh - lhB) / 2, kLabels[i], bw - 8, sel ? !tc : tc,
                    EpdFontFamily::BOLD);
  }
  const char* hint = CLASSIC ? "Select picks, Back exits" : "Enter picks, Esc exits, any other key goes back";
  const int hw = r.getTextWidth(FONT_SMALL, hint, EpdFontFamily::REGULAR);
  drawClippedText(r, FONT_SMALL, x + (w - hw) / 2, y + 16 + bh + 12, hint, w - 16, tc);
}

// The SD card is gone and the note has unsaved text (input_handler.h, editorSdWarn): full screen, any key goes back.
static void drawSdWarning(GfxRenderer& r, HalGPIO& gpio) {
  const int sw = r.getScreenWidth();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(r, 0, 0, sw, r.getScreenHeight(), true);
  drawScreenHeader(r, gpio, "SD card removed", tc);
  const int lhL = r.getLineHeight(LIST_FONT_L);
  const int lh = r.getLineHeight(FONT_UI) + 6;
  int y = HDR_H + 30;
  drawClippedText(r, LIST_FONT_L, 20, y, "Put the SD card back", sw - 40, tc, EpdFontFamily::BOLD);
  y += lhL + 18;
  static const char* kBody[] = {"Your note is kept in memory for now and",
                                "saves as soon as the card is back.",
                                "",
                                "Everything written since the card was",
                                "removed is lost if the device turns off."};
  for (const char* line : kBody) {
    drawClippedText(r, FONT_UI, 20, y, line, sw - 40, tc);
    y += lh;
  }
  if (editorSdWarn == 2 || editorSdWarn == 3) {
    y += lh / 2;
    drawClippedText(r, FONT_UI, 20, y,
                    editorSdWarn == 2 ? "Hold the power button again to turn off" : "Hold Back again to restart",
                    sw - 40, tc, EpdFontFamily::BOLD);
    y += lh;
    drawClippedText(r, FONT_UI, 20, y, "anyway. That text will be lost.", sw - 40, tc, EpdFontFamily::BOLD);
    y += lh;
  }
  y += lh;
  drawClippedText(r, FONT_UI, 20, y, CLASSIC ? "Press any button to go back to writing." : "Press any key to go back to writing.",
                  sw - 40, tc);
}

void drawTextEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  MS_PROF("draw");
  if (editorSdWarn) {
    renderer.clearScreen();
    drawSdWarning(renderer, gpio);
    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }
  {
    MS_PROF("d-clear");
    renderer.clearScreen();
  }
  int sw = renderer.getScreenWidth();
  int sh = renderer.getScreenHeight();
  bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  int lineHeight = renderer.getLineHeight(editorFontId(fontSize));
  if (lineHeight <= 0) lineHeight = 20;
  int totalLines = editorGetLineCount();
  int curLine = editorGetCursorLine();

  // --- TYPEWRITER MODE ---
  if (writingMode == WritingMode::TYPEWRITER) {
    // In clean mode (Ctrl+H): just text on blank screen, no header
    int textAreaTop = (cleanMode && !sdLost()) ? 0 : drawEditorHeader(renderer, gpio, sw, tc);

    // Center the current line vertically
    int textAreaHeight = sh - textAreaTop;
    int centerY = textAreaTop + (textAreaHeight / 2) - (lineHeight / 2);

    // Draw only the current line
    touchEditorViewBegin(editorFontId(fontSize), 10, lineHeight);
    if (curLine < totalLines) {
      touchEditorViewAddLine(curLine, centerY);
      drawEditorLine(renderer, curLine, 10, centerY, lineHeight, sw - 20, tc);
    }

    // Draw cursor
    drawEditorCursor(renderer, centerY, lineHeight, sw, tc);

    editorSetVisibleLines(1);

    if (editorFindMode) drawFindBar(renderer, sw, sh, tc);
    drawEditorContextMenu(renderer, lineHeight, tc);
    drawExitPrompt(renderer, tc);
    drawEditorMenu(renderer, tc);
    drawSpellMenu(renderer, tc);
    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- PAGINATION MODE ---
  if (writingMode == WritingMode::PAGINATION) {
    // Pre-compute page info for the header
    // Use a temporary linesPerPage estimate (will be exact since header height is fixed)
    int tempTextTop = (cleanMode && !sdLost()) ? 8 : 38;
    int tempLinesPerPage = (sh - 5 - tempTextTop) / lineHeight;
    if (tempLinesPerPage < 1) tempLinesPerPage = 1;
    int currentPage = curLine / tempLinesPerPage;
    int totalPages = (totalLines + tempLinesPerPage - 1) / tempLinesPerPage;
    if (totalPages < 1) totalPages = 1;

    char pageStr[16];
    snprintf(pageStr, sizeof(pageStr), "Pg %d/%d", currentPage + 1, totalPages);
    int textAreaTop = drawEditorHeader(renderer, gpio, sw, tc, pageStr);

    int textAreaBottom = sh - 5;
    int textAreaHeight = textAreaBottom - textAreaTop;
    int linesPerPage = textAreaHeight / lineHeight;
    if (linesPerPage < 1) linesPerPage = 1;

    // Recompute with actual linesPerPage if it differs
    currentPage = curLine / linesPerPage;
    int pageStart = currentPage * linesPerPage;

    editorSetVisibleLines(linesPerPage);

    // Draw lines for this page
    touchEditorViewBegin(editorFontId(fontSize), 10, lineHeight);
    for (int i = 0; i < linesPerPage && (pageStart + i) < totalLines; i++) {
      int yPos = textAreaTop + (i * lineHeight);
      touchEditorViewAddLine(pageStart + i, yPos);
      drawEditorLine(renderer, pageStart + i, 10, yPos, lineHeight, sw - 20, tc);
    }

    // Draw cursor if on this page
    if (curLine >= pageStart && curLine < pageStart + linesPerPage) {
      int cursorY = textAreaTop + ((curLine - pageStart) * lineHeight);
      drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
    }

    if (editorFindMode) drawFindBar(renderer, sw, sh, tc);
    drawEditorContextMenu(renderer, lineHeight, tc);
    drawExitPrompt(renderer, tc);
    drawEditorMenu(renderer, tc);
    drawSpellMenu(renderer, tc);
    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  // --- NORMAL MODE ---
  int textAreaTop;
  {
    MS_PROF("d-header");
    textAreaTop = drawEditorHeader(renderer, gpio, sw, tc);
  }

  int textAreaBottom = sh - 5;
  int textAreaHeight = textAreaBottom - textAreaTop;
  int visibleLines = textAreaHeight / lineHeight;

  editorSetVisibleLines(visibleLines);

  int vpStart = editorGetViewportStart();

  // Draw visible lines
  {
    MS_PROF("d-lines");
    touchEditorViewBegin(editorFontId(fontSize), 10, lineHeight);
    for (int i = 0; i < visibleLines && (vpStart + i) < totalLines; i++) {
      int yPos = textAreaTop + (i * lineHeight);
      touchEditorViewAddLine(vpStart + i, yPos);
      drawEditorLine(renderer, vpStart + i, 10, yPos, lineHeight, sw - 20, tc);
    }
  }

  // Draw cursor
  {
    MS_PROF("d-cursor");
    if (curLine >= vpStart && curLine < vpStart + visibleLines) {
      int cursorY = textAreaTop + ((curLine - vpStart) * lineHeight);
      drawEditorCursor(renderer, cursorY, lineHeight, sw, tc);
    }
  }

  if (editorFindMode) drawFindBar(renderer, sw, sh, tc);
  {
    MS_PROF("d-menu");
    drawEditorContextMenu(renderer, lineHeight, tc);
    drawExitPrompt(renderer, tc);
    drawEditorMenu(renderer, tc);
    drawSpellMenu(renderer, tc);
  }
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawRenameScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawScreenHeader(renderer, gpio, "Note title", tc);

  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  const int boxY = HDR_H + 18;
  const int boxH = lhB + 24;
  renderer.drawRect(12, boxY, sw - 24, boxH, 2, tc);
  drawClippedText(renderer, LIST_FONT_B, 22, boxY + 12, renameBuffer, sw - 50, tc);

  // Caret, same height as the text line.
  const int cursorX = 22 + renderer.getTextAdvanceX(LIST_FONT_B, renameBuffer);
  if (cursorX + 2 < sw - 14) renderer.fillRect(cursorX, boxY + 12, 2, lhB, tc);

  drawClippedText(renderer, FONT_UI, 14, boxY + boxH + 14,
                  "Type the title on a Bluetooth keyboard.", sw - 28, tc);
  // The note's format (work order 1.10): Enter saves as renameExt; the two Save buttons pick either.
  drawClippedText(renderer, FONT_UI, 14, boxY + boxH + 14 + renderer.getLineHeight(FONT_UI) + 8,
                  strcmp(renameExt, ".md") == 0 ? "Enter saves as .md." : "Enter saves as .txt.", sw - 28, tc);

  const BarButton bar[4] = {{"Cancel", true}, {"Save .txt", true}, {"Save .md", true}, {nullptr, false}};
  drawButtonBar(renderer, bar, tc, -1);
  (void)sh;

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// ---- Sleep screen (Settings > Display) ---------------------------------------------------------------------------
// One choice on one screen. Rows: 0 Stock, 1 Random, then one per picture, last "+ Add a picture". The checked row
// is what sleep shows; Use moves the check. Enter on a picture shows it full screen first. Pictures are only added from
// a phone (Add) and only managed here.

// A check mark drawn with lines (the fonts may not have one), about the height of a list line.
static void drawCheck(GfxRenderer& r, int x, int y, int h, bool black) {
  const int s = h * 2 / 3;
  for (int t = 0; t < 3; t++) {
    clippedLine(r, x, y + s / 2 + t, x + s / 3, y + s + t, black);
    clippedLine(r, x + s / 3, y + s + t, x + s, y + t, black);
  }
}

void drawSleepPictures(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;
  const int n = sleepscr::count();
  const int rows = n + 3;
  if (sleepPicSel >= rows) sleepPicSel = rows - 1;
  const int pic = (sleepPicSel >= 2 && sleepPicSel < 2 + n) ? sleepPicSel - 2 : -1;   // highlighted picture
  const int barFocus = (uiBarFocus >= 0 && ((uiBarMask() >> uiBarFocus) & 1)) ? uiBarFocus : -1;

  // Preview: the picture exactly as the sleep screen shows it, with a band on top and the bar below.
  if (sleepPicPreview && pic >= 0) {
    const bool ok = sleepscr::drawPicture(renderer, pic);
    const int bandH = renderer.getLineHeight(FONT_UI) + 14;
    clippedFillRect(renderer, 0, 0, sw, bandH, false);
    clippedLine(renderer, 0, bandH, sw, bandH, true);
    const std::string lbl = sleepscr::label(pic);
    const bool chosen = sleepscr::isChosen(pic);
    char top[96];
    if (sleepPicDeletePending)
      snprintf(top, sizeof(top), CLASSIC ? "Delete %s?  Select: Delete  Back: Cancel" : "Delete %s?", lbl.c_str());
    else if (CLASSIC)
      snprintf(top, sizeof(top), chosen ? "%s, in use" : "%s  Select: Use", lbl.c_str());
    else
      snprintf(top, sizeof(top), "%s%s   (%d of %d)", lbl.c_str(), chosen ? ", in use" : "", pic + 1, n);
    drawClippedText(renderer, FONT_UI, 12, 7, top, sw - 24, true, EpdFontFamily::BOLD);
    if (!ok) drawClippedText(renderer, FONT_UI, 20, sh / 2, "This picture could not be read.", sw - 40, true);
    if (CLASSIC) {
      const BarButton bar[4] = {{"Back", true}, {nullptr, false}, {nullptr, false}, {"Delete", true}};
      drawButtonBar(renderer, bar, true, barFocus);
    } else if (sleepPicDeletePending) {
      const BarButton bar[4] = {{"Cancel", true}, {"Delete", true}, {nullptr, false}, {nullptr, false}};
      drawButtonBar(renderer, bar, true, -1);
    } else {
      const BarButton bar[4] = {{"Back", true}, {"Use", !chosen, true}, {"Delete", true}, {nullptr, false}};
      drawButtonBar(renderer, bar, true, barFocus);
    }
    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);
  drawScreenHeader(renderer, gpio, "Sleep screen", tc);
  const int lhU = renderer.getLineHeight(FONT_UI);
  drawClippedText(renderer, FONT_UI, 20, HDR_H + 4, "The checked one shows while the MicroSlate sleeps.", sw - 40, tc);

  const ListGeom g = listGeom(renderer, true, LIST_ROW_PITCH, rows, 0, lhU + 10);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);

  if (sleepPicDeletePending && pic >= 0) {
    drawConfirmPrompt(renderer, tc, g, "Delete this picture?", sleepscr::label(pic).c_str(), nullptr, "Delete");
    return;
  }

  const sleepscr::Choice c = sleepscr::choice();
  static ListView lv;
  viewWindow(lv, sleepPicSel, rows, g.rows);
  const int start = lv.start;
  const int cw = sw - g.inset;
  const int textX = CLASSIC ? 30 : 56;   // room for the check mark
  touchTargetsBegin(sleepPicSel, LIST_ROW_PITCH);
  for (int i = 0; i < g.rows && (start + i) < rows; i++) {
    const int pos = start + i;
    const int y = g.top + i * LIST_ROW_PITCH;
    addCardRow(pos, y, sw, LIST_ROW_PITCH, g.inset);
    const bool ink = drawCard(renderer, tc, y, LIST_ROW_H, pos == sleepPicSel, barFocus >= 0, g.inset);
    const int ty = y + (LIST_ROW_H - lhB) / 2;
    std::string text;
    bool checked = false, grey = false;
    if (pos == 0) {
      text = "Stock";
      checked = c == sleepscr::Choice::Stock;
    } else if (pos == 1) {
      text = "Random (from my pictures)";
      checked = c == sleepscr::Choice::Random;
      grey = n < 2;
    } else if (pos < 2 + n) {
      text = sleepscr::label(pos - 2);
      checked = sleepscr::isChosen(pos - 2);
    } else {
      drawClippedText(renderer, LIST_FONT_B, 20, ty, "+  Add a picture", cw - 40, ink, EpdFontFamily::BOLD);
      continue;
    }
    if (checked) drawCheck(renderer, CLASSIC ? 10 : 22, ty + 2, lhB, ink);
    drawClippedText(renderer, LIST_FONT_B, textX, ty, text.c_str(), cw - textX - 24, ink,
                    checked ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (grey) drawRightText(renderer, FONT_SMALL, cw - 24, y + (LIST_ROW_H - renderer.getLineHeight(FONT_SMALL)) / 2,
                            "needs 2 pictures", ink);
  }
  drawScrollbar(renderer, tc, g, LIST_ROW_PITCH, start, rows);

  if (CLASSIC) {
    // The right button follows the row: Delete on a picture, Add elsewhere (Select on "+ Add a picture" adds too).
    const BarButton bar[4] = {{"Back", true}, {nullptr, false}, {nullptr, false}, {pic >= 0 ? "Delete" : "Add", true}};
    drawButtonBar(renderer, bar, tc, barFocus);
  } else {
    const bool canUse = sleepPicCanUse();
    const BarButton bar[4] = {{"Back", true}, {"Use", canUse, true}, {"Delete", pic >= 0, true}, {"Add", true}};
    drawButtonBar(renderer, bar, tc, barFocus);
  }
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// ---- Settings tabs --------------------------------------------------------------------------------------------

static constexpr int TAB_H = CLASSIC ? 26 : 46;

// The tab strip under the header: the open tab is filled; a heavier frame round the strip when the keys are on it.
// A tap on a tab sends HID_KEY_TAB0 + n.
static void drawSettingsTabs(GfxRenderer& r, bool tc, int y, bool focused) {
  const int sw = r.getScreenWidth();
  const int x0 = CLASSIC ? 5 : 8;
  const int w = (sw - 2 * x0) / kSettingsTabCount;
  const int font = CLASSIC ? FONT_SMALL : FONT_UI;
  const int lh = r.getLineHeight(font);
  for (int t = 0; t < kSettingsTabCount; t++) {
    const int x = x0 + t * w;
    const bool on = (t == settingsTab);
    if (on) clippedFillRect(r, x, y, w, TAB_H, tc);
    const char* name = settingsTabName(t);
    const auto style = on ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
    int tw = r.getTextWidth(font, name, style);
    if (tw > w - 6) tw = w - 6;
    drawClippedText(r, font, x + (w - tw) / 2, y + (TAB_H - lh) / 2, name, w - 6, on ? !tc : tc, style);
    if (!CLASSIC) touchTargetsAddButton(static_cast<uint8_t>(HID_KEY_TAB0 + t), x, y, w, TAB_H);
  }
  r.drawRect(x0, y, w * kSettingsTabCount, TAB_H, focused ? 3 : 1, tc);
}

void drawSettingsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawScreenHeader(renderer, gpio, "Settings", tc);

  const int rows = settingsTabRows(settingsTab);
  if (settingsSelection > rows) settingsSelection = rows;
  const bool onTabs = settingsSelection <= 0;
  const int sel = settingsSelection - 1;   // highlighted row in the tab, -1 = the tab strip

  const int barFocus = (uiBarFocus >= 0 && ((uiBarMask() >> uiBarFocus) & 1)) ? uiBarFocus : -1;
  drawSettingsTabs(renderer, tc, HDR_H + 2, onTabs && barFocus < 0);

  const ListGeom g = listGeom(renderer, true, LIST_ROW_PITCH, rows, 0, TAB_H + 6);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);

  // One scroll window per tab, so switching tabs never leaves a list scrolled off its top.
  static ListView lvs[kSettingsTabCount];
  ListView& lv = lvs[settingsTab];
  viewWindow(lv, sel < 0 ? 0 : sel, rows, g.rows);
  const int start = lv.start;
  const int cw = sw - g.inset;

  // Touch rows are numbered like settingsSelection (row n is n + 1), so a tap walks there with Up/Down.
  touchTargetsBegin(settingsSelection, LIST_ROW_PITCH);
  for (int i = 0; i < g.rows && (start + i) < rows; i++) {
    const int pos = start + i;
    const int y = g.top + i * LIST_ROW_PITCH;
    addCardRow(pos + 1, y, sw, LIST_ROW_PITCH, g.inset);
    const bool ink = drawCard(renderer, tc, y, LIST_ROW_H, pos == sel, barFocus >= 0, g.inset);

    // Label on the left of the card, value on the right.
    const SettingKind kind = settingKind(settingsTab, pos);
    const char* label = settingLabel(settingsTab, pos);
    char val[40];
    settingValue(settingsTab, pos, val, sizeof(val));
    if (kind == SettingKind::SavedWifi && pos == sel && settingsForgetPending) snprintf(val, sizeof(val), "Forget it?");

    const int ty = y + (LIST_ROW_H - lhB) / 2;
    const int valW = val[0] ? renderer.getTextWidth(LIST_FONT_B, val, EpdFontFamily::BOLD) : 0;
    drawClippedText(renderer, LIST_FONT_B, 20, ty, label, cw - 50 - valW - 12, ink,
                    kind == SettingKind::AddWifi ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    if (val[0]) drawRightText(renderer, LIST_FONT_B, cw - 24, ty, val, ink, EpdFontFamily::BOLD);
  }
  drawScrollbar(renderer, tc, g, LIST_ROW_PITCH, start, rows);
  if (settingsTab == kSettingsTabWifi && wifiSavedCount() == 0 && g.rows > 2) {
    drawClippedText(renderer, FONT_UI, 20, g.top + 2 * LIST_ROW_PITCH + 14,
                    "No saved networks yet. Sync and Transfer use the ones added here.", sw - 40, tc);
  }

  const SettingKind selKind = settingKind(settingsTab, sel);
  BarButton bar[4] = {{"Back", true}, {nullptr, false}, {nullptr, false}, {nullptr, false}};
  if (onTabs) {
    bar[2] = kBarLeft;
    bar[3] = kBarRight;
  } else if (selKind == SettingKind::SavedWifi) {
    if (settingsForgetPending) bar[0].label = "Cancel";
    else bar[1] = {"Prefer", true};
    bar[3] = {"Forget", true};
  } else if (selKind == SettingKind::Value) {
    bar[2] = kBarLeft;
    bar[3] = kBarRight;
  } else {
    bar[1] = {"Open", true};
  }
  drawButtonBar(renderer, bar, tc, barFocus);
  (void)sh;

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawBluetoothSettings(GfxRenderer& renderer, HalGPIO& gpio) {
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();

  renderer.clearScreen();
  const bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  // Header
  // Bottom-bar focus (-1 = the device list has it); ignore a stale index whose
  // button has since gone blank.
  int barFocus = uiBarFocus;
  if (barFocus >= 0 && !((uiBarMask() >> barFocus) & 1)) barFocus = -1;

  drawScreenHeader(renderer, gpio, "Bluetooth", tc);

  const int lhUi = renderer.getLineHeight(FONT_UI);
  const int lhBody = renderer.getLineHeight(LIST_FONT_B);
  const int lhSm = renderer.getLineHeight(FONT_SMALL);
  const bool scanning = isDeviceScanning();

  // Status block: one clear line, then a smaller detail line.
  const char* status = "";
  switch (getConnectionState()) {
    case BLEState::CONNECTED:    status = "Connected to keyboard"; break;
    case BLEState::SCANNING:     status = "Scanning for devices"; break;
    case BLEState::CONNECTING:   status = "Connecting..."; break;
    case BLEState::DISCONNECTED: status = "Not connected"; break;
  }
  const int statusY = HDR_H + 4;
  const int detailY = statusY + lhUi + 2;
  drawClippedText(renderer, FONT_UI, 10, statusY, status, sw - 20, tc, EpdFontFamily::BOLD);

  // Device list data — keyboards first; non-keyboards hidden behind a toggle row.
  int visIdx[32];
  int total = 0, kbd = 0;
  int visCount = getVisibleDeviceIndices(btShowAllDevices, visIdx, 32, &total, &kbd);
  const bool toggleShown = (kbd > 0 && total > kbd);
  const int itemCount = visCount + (toggleShown ? 1 : 0);

  int sel = bluetoothDeviceSelection;
  if (sel >= itemCount) sel = itemCount - 1;
  if (sel < 0) sel = 0;
  const bool onToggle = (toggleShown && sel == visCount);

  const int listTop = detailY + lhSm + 10;
  // Same window / arrows as every other list; the pairing-code view has no list.
  ListGeom lg;
  lg.top = listTop;
  lg.bottom = barTopY(renderer) - 4;
  lg.rows = (lg.bottom - lg.top) / LIST_ROW_PITCH;
  if (lg.rows < 1) lg.rows = 1;
  lg.inset = (itemCount > lg.rows) ? SLIDER_W : 0;
  const int maxRows = lg.rows;
  const int cw = sw - lg.inset;

  const uint32_t passkey = getCurrentPasskey();
  std::string storedAddr, storedName;
  const bool hasStored = getStoredDevice(storedAddr, storedName);

  touchTargetsBegin(sel, LIST_ROW_PITCH);

  if (passkey > 0) {
    // Pairing code takes over the list area.
    char passkeyStr[32];
    snprintf(passkeyStr, sizeof(passkeyStr), "%06lu", passkey);
    drawClippedText(renderer, FONT_UI, 20, listTop + 6, "PAIRING CODE", 0, tc, EpdFontFamily::BOLD);
    drawClippedText(renderer, LIST_FONT_L, 20, listTop + 10 + lhUi, passkeyStr, 0, tc, EpdFontFamily::BOLD);
    drawClippedText(renderer, FONT_SMALL, 20, listTop + 24 + lhUi + renderer.getLineHeight(LIST_FONT_L),
                    "Type this code on your keyboard, then press Enter on it", sw - 40, tc);
  } else {
    // Detail line: left = paired keyboard / search progress, right = counts.
    if (scanning) {
      static uint8_t dotPhase = 0;
      static uint32_t lastAnimMs = 0;
      if (millis() - lastAnimMs > 900) {
        dotPhase = (dotPhase + 1) % 4;
        lastAnimMs = millis();
      }
      std::string dots(dotPhase, '.');
      char scanningStr[48];
      snprintf(scanningStr, sizeof(scanningStr), "Searching%s", dots.c_str());
      drawClippedText(renderer, FONT_SMALL, 10, detailY, scanningStr, sw / 2 - 10, tc);
      char foundStr[32];
      snprintf(foundStr, sizeof(foundStr), "Found: %d", getDiscoveredDeviceCount());
      drawRightText(renderer, FONT_SMALL, sw - 10, detailY, foundStr, tc);
    } else {
      if (hasStored) {
        char pairedStr[64];
        snprintf(pairedStr, sizeof(pairedStr), "Paired: %s", storedName.c_str());
        drawClippedText(renderer, FONT_SMALL, 10, detailY, pairedStr, sw / 2 - 10, tc);
      }
      if (total > 0) {
        char countStr[40];
        if (btShowAllDevices || kbd == 0)
          snprintf(countStr, sizeof(countStr), "Devices: %d", total);
        else
          snprintf(countStr, sizeof(countStr), "Keyboards: %d", visCount);
        drawRightText(renderer, FONT_SMALL, sw - 10, detailY, countStr, tc);
      }
    }

    if (total > 0) {
      BleDeviceInfo* devices = getDiscoveredDevices();
      static ListView btView;
      viewWindow(btView, sel, itemCount, maxRows);
      const int startIndex = btView.start;
      const int itemsToShow = (itemCount - startIndex < maxRows) ? itemCount - startIndex : maxRows;

      for (int i = 0; i < itemsToShow; i++) {
        const int pos = startIndex + i;
        const int y = listTop + i * LIST_ROW_PITCH;
        const bool isSel = (sel == pos);
        // The tap zone covers the whole pitch (card + gap) across the full width, so
        // neighbouring rows tile with no dead strip between them.
        touchTargetsAddRow(pos, 0, y - 4, cw, LIST_ROW_PITCH);

        // Selected card: solid fill while the list has focus; a heavy outline while
        // the bottom bar has focus (so you can still see which device it applies to).
        const bool filled = isSel && barFocus < 0;
        if (filled) {
          clippedFillRect(renderer, 8, y, cw - 16, LIST_ROW_H, tc);
        } else if (isSel) {
          renderer.drawRect(8, y, cw - 16, LIST_ROW_H, 3, tc);
        } else {
          renderer.drawRect(8, y, cw - 16, LIST_ROW_H, 1, tc);
        }
        const bool ink = filled ? !tc : tc;   // text colour on this card
        const int nameY = y + (LIST_ROW_H - lhBody) / 2;
        const int smY = y + (LIST_ROW_H - lhSm) / 2;

        if (pos < visCount) {
          const int d = visIdx[pos];
          const bool isConnected = (getCurrentDeviceAddress() == devices[d].address);
          const char* displayName = devices[d].name.empty()
                                    ? devices[d].address.c_str()
                                    : devices[d].name.c_str();
          char rightStr[24];
          if (isConnected) snprintf(rightStr, sizeof(rightStr), "Connected");
          else             snprintf(rightStr, sizeof(rightStr), "%ddBm", devices[d].rssi);
          const int rightW = renderer.getTextWidth(FONT_SMALL, rightStr, EpdFontFamily::REGULAR);
          drawClippedText(renderer, LIST_FONT_B, 20, nameY, displayName, cw - 40 - rightW - 16, ink,
                          isConnected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
          drawRightText(renderer, FONT_SMALL, cw - 20, smY, rightStr, ink,
                        isConnected ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
        } else {
          // The "Show other devices" / "Show keyboards only" toggle row
          char toggleStr[40];
          if (btShowAllDevices) snprintf(toggleStr, sizeof(toggleStr), "Show keyboards only");
          else                  snprintf(toggleStr, sizeof(toggleStr), "Show other devices (%d)", total - kbd);
          drawClippedText(renderer, LIST_FONT_B, 20, nameY, toggleStr, cw - 40, ink);
        }
      }
      drawScrollbar(renderer, tc, lg, LIST_ROW_PITCH, startIndex, itemCount);
    } else {
      // Nothing found yet.
      const int cy = listTop + 20;
      if (scanning) {
        drawClippedText(renderer, FONT_UI, 20, cy, "Looking for keyboards...", sw - 40, tc);
      } else {
        drawClippedText(renderer, FONT_UI, 20, cy, "No devices found", sw - 40, tc, EpdFontFamily::BOLD);
        drawClippedText(renderer, FONT_SMALL, 20, cy + lhUi + 6,
                        "Put your keyboard in pairing mode, then Scan", sw - 40, tc);
      }
    }
  }

  // Button bar: Back / Connect / Disconnect / Scan (physical X4 order).
  const bool pairing = (passkey > 0);
  const BarButton bar[4] = {
    {"Back", true},
    {onToggle ? (btShowAllDevices ? "Keyboards" : "Show all") : "Connect",
     itemCount > 0 && !scanning && !pairing},
    {"Disconnect", isKeyboardConnected() && !pairing},
    {"Scan", !scanning && !pairing},
  };
  drawButtonBar(renderer, bar, tc, barFocus);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Settings > Editor > My words: the words added with "Add to dictionary" (/microslate/dictionary.txt), each one
// removable. The file can also be edited on a computer.
void drawMyWords(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, renderer.getScreenHeight(), true);
  drawScreenHeader(renderer, gpio, "My words", tc);

  const int count = spellsvc::ownCount();
  const ListGeom g = listGeom(renderer, true, LIST_ROW_PITCH, count);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  const int barFocus = (uiBarFocus >= 0 && ((uiBarMask() >> uiBarFocus) & 1)) ? uiBarFocus : -1;
  touchTargetsBegin(myWordsSel, LIST_ROW_PITCH);

  if (myWordsRemovePending && count > 0) {
    drawConfirmPrompt(renderer, tc, g, "Remove this word?", spellsvc::ownWord(myWordsSel),
                      "Spellcheck will mark it again.", "Remove");
    return;
  }

  if (count == 0) {
    drawEmptyList(renderer, tc, g, "No words yet", "In a note, Ctrl+J or a long press on a marked word adds it.");
  } else {
    static ListView lv;
    viewWindow(lv, myWordsSel, count, g.rows);
    const int start = lv.start;
    const int cw = sw - g.inset;
    for (int i = 0; i < g.rows && (start + i) < count; i++) {
      const int pos = start + i;
      const int y = g.top + i * LIST_ROW_PITCH;
      addCardRow(pos, y, sw, LIST_ROW_PITCH, g.inset);
      const bool ink = drawCard(renderer, tc, y, LIST_ROW_H, pos == myWordsSel, barFocus >= 0, g.inset);
      drawClippedText(renderer, LIST_FONT_B, 20, y + (LIST_ROW_H - lhB) / 2, spellsvc::ownWord(pos), cw - 50, ink);
    }
    drawScrollbar(renderer, tc, g, LIST_ROW_PITCH, start, count);
  }

  const BarButton bar[4] = {{"Back", true}, {"Remove", count > 0}, {nullptr, false}, {nullptr, false}};
  drawButtonBar(renderer, bar, tc, barFocus);
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

void drawPairedKeyboardsMenu(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, renderer.getScreenHeight(), true);

  drawScreenHeader(renderer, gpio, "Paired keyboards", tc);

  const int count = getPairedKeyboardCount();
  const ListGeom g = listGeom(renderer, true, LIST_ROW_PITCH, count);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  const int barFocus = (uiBarFocus >= 0 && ((uiBarMask() >> uiBarFocus) & 1)) ? uiBarFocus : -1;

  touchTargetsBegin(pairedKeyboardSelection, LIST_ROW_PITCH);

  if (pairedForgetPending && count > 0) {
    std::string addr, name; uint8_t addrType;
    getPairedKeyboard(pairedKeyboardSelection, addr, name, addrType);
    drawConfirmPrompt(renderer, tc, g, "Forget this keyboard?", name.c_str(), nullptr, "Forget");
    return;
  }

  if (count == 0) {
    drawEmptyList(renderer, tc, g, "No paired keyboards", "Open Bluetooth in Settings to scan and connect.");
  } else {
    const std::string currentAddr = getCurrentDeviceAddress();
    static ListView lv;
    viewWindow(lv, pairedKeyboardSelection, count, g.rows);
    const int start = lv.start;
    const int cw = sw - g.inset;
    for (int i = 0; i < g.rows && (start + i) < count; i++) {
      const int pos = start + i;
      const int y = g.top + i * LIST_ROW_PITCH;
      std::string addr, name; uint8_t addrType;
      getPairedKeyboard(pos, addr, name, addrType);
      addCardRow(pos, y, sw, LIST_ROW_PITCH, g.inset);
      const bool ink = drawCard(renderer, tc, y, LIST_ROW_H, pos == pairedKeyboardSelection, barFocus >= 0, g.inset);

      const bool active = (!currentAddr.empty() && currentAddr == addr);
      const char* tag = active ? "Active"
                        : pairedKeyboardNeedsRepair(pos) ? "Re-pair"
                        : (pos == getLastUsedKeyboardIndex() && currentAddr.empty()) ? "Last used" : "";
      const int tagW = tag[0] ? renderer.getTextWidth(FONT_UI, tag, EpdFontFamily::BOLD) : 0;
      drawClippedText(renderer, LIST_FONT_B, 20, y + (LIST_ROW_H - lhB) / 2, name.c_str(), cw - 50 - tagW - 12, ink,
                      active ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
      if (tag[0]) {
        drawRightText(renderer, FONT_UI, cw - 24, y + (LIST_ROW_H - renderer.getLineHeight(FONT_UI)) / 2, tag, ink,
                      EpdFontFamily::BOLD);
      }
    }
    drawScrollbar(renderer, tc, g, LIST_ROW_PITCH, start, count);
  }

  const BarButton bar[4] = {
    {"Back", true},
    {"Connect", count > 0},
    {"Disconnect", count > 0 && isKeyboardConnected()},
    {"Forget", count > 0},
  };
  drawButtonBar(renderer, bar, tc, barFocus);

  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// What a layout slot types, for the editor: "z", "space", "´  accent", "empty", or the code
// when the font cannot show it.
static void layoutSlotLabel(const char* text, bool dead, char* out, size_t size) {
  if (!text || !text[0]) { snprintf(out, size, "nothing"); return; }
  char shown[24];
  if (strcmp(text, " ") == 0) {
    snprintf(shown, sizeof(shown), "space");
  } else if (!keyboardLayoutSlotShown(text)) {
    // First character as a code; the font has no glyph for it.
    const uint8_t c = (uint8_t)text[0];
    uint32_t cp = c;
    int n = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
    if (n > 1) cp = c & (0x7F >> n);
    for (int k = 1; k < n && text[k]; k++) cp = (cp << 6) | ((uint8_t)text[k] & 0x3F);
    snprintf(shown, sizeof(shown), "U+%04X", (unsigned)cp);
  } else {
    snprintf(shown, sizeof(shown), "%s", text);
  }
  snprintf(out, size, dead ? "%s  accent" : "%s", shown);
}

void drawLayoutEditor(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, renderer.getScreenHeight(), true);

  drawScreenHeader(renderer, gpio, "Edit keyboard layout", tc);
  const int lhUi = renderer.getLineHeight(FONT_UI);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  const int lhL = renderer.getLineHeight(LIST_FONT_L);

  // Which layout, and how many keys the writer changed on it.
  char sub[96];
  const int changed = keyboardLayoutChangedCount();
  if (changed > 0)
    snprintf(sub, sizeof(sub), "%s  -  %d key%s changed by you", keyboardLayoutName(), changed, changed == 1 ? "" : "s");
  else
    snprintf(sub, sizeof(sub), "%s", keyboardLayoutName());
  drawClippedText(renderer, FONT_UI, 14, HDR_H + 6, sub, sw - 28, tc);

  if (layoutEdKey < 0) {
    const int y = HDR_H + 6 + lhUi + 24;
    drawClippedText(renderer, LIST_FONT_L, 20, y, "Press the key you want to change", sw - 40, tc, EpdFontFamily::BOLD);
    if (layoutEdMessage[0])
      drawClippedText(renderer, FONT_UI, 20, y + lhL + 16, layoutEdMessage, sw - 40, tc, EpdFontFamily::BOLD);
    const BarButton bar[4] = {{"Back", true}, {"Reset all", changed > 0}, {nullptr, false}, {nullptr, false}};
    const int focus = (uiBarFocus >= 0 && ((uiBarMask() >> uiBarFocus) & 1)) ? uiBarFocus : -1;
    drawButtonBar(renderer, bar, tc, focus);
    renderer.beginRefresh(HalDisplay::FAST_REFRESH);
    return;
  }

  const uint8_t hid = (uint8_t)layoutEdKey;
  char kn[16];
  char keyLine[64];
  snprintf(keyLine, sizeof(keyLine), "Key %s%s", layoutEdKeyName(hid, kn, sizeof(kn)),
           keyboardLayoutKeyChanged(hid) ? "  (changed by you)" : "");
  const int keyY = HDR_H + 6 + lhUi + 10;
  drawClippedText(renderer, LIST_FONT_B, 20, keyY, keyLine, sw - 40, tc, EpdFontFamily::BOLD);

  const int nRows = layoutEdRows();
  const ListGeom g = listGeom(renderer, true, LIST_ROW_PITCH, nRows, 2 * (lhUi + 6) + 8, lhUi + lhB + 18);
  const int barFocus = (uiBarFocus >= 0 && ((uiBarMask() >> uiBarFocus) & 1)) ? uiBarFocus : -1;

  touchTargetsBegin(layoutEdSlot, LIST_ROW_PITCH);
  const int rows = g.rows < nRows ? g.rows : nRows;
  const int start = layoutEdSlot >= rows ? layoutEdSlot - rows + 1 : 0;
  for (int i = 0; i < rows; i++) {
    const int slot = start + i;
    const int y = g.top + i * LIST_ROW_PITCH;
    addCardRow(slot, y, sw, LIST_ROW_PITCH, 0);
    const bool sel = slot == layoutEdSlot;
    const bool ink = drawCard(renderer, tc, y, LIST_ROW_H, sel, barFocus >= 0 || (sel && layoutEdEntry));
    const int ty = y + (LIST_ROW_H - lhB) / 2;
    char name[40];
    layoutEdRowName(hid, slot, name, sizeof(name));
    drawClippedText(renderer, LIST_FONT_B, 20, ty, name, sw / 2 - 24, ink);
    if (sel && layoutEdEntry) {
      // The value being typed, with a caret.
      const int bx = sw / 2;
      renderer.drawRect(bx - 8, y + 6, sw - bx - 10, LIST_ROW_H - 12, 2, tc);
      drawClippedText(renderer, LIST_FONT_B, bx, ty, layoutEdBuf, sw - bx - 30, tc);
      const int cx = bx + renderer.getTextAdvanceX(LIST_FONT_B, layoutEdBuf);
      if (cx + 2 < sw - 22) renderer.fillRect(cx, ty, 2, lhB, tc);
    } else {
      bool dead = false;
      const char* t = keyboardLayoutSlot(hid, slot, &dead);
      char val[40];
      layoutSlotLabel(t, dead, val, sizeof(val));
      drawRightText(renderer, LIST_FONT_B, sw - 30, ty, val, ink, EpdFontFamily::BOLD);
    }
  }

  // One line: what to do while typing, else what the last action did.
  const int hy = g.top + rows * LIST_ROW_PITCH + 4;
  if (layoutEdEntry) {
    char name[40], hint[80];
    layoutEdRowName(hid, layoutEdSlot, name, sizeof(name));
    snprintf(hint, sizeof(hint), "Type what %s should type, then Save.", name);
    drawClippedText(renderer, FONT_UI, 14, hy, hint, sw - 28, tc);
  } else if (layoutEdMessage[0]) {
    drawClippedText(renderer, FONT_UI, 14, hy, layoutEdMessage, sw - 28, tc, EpdFontFamily::BOLD);
  }

  if (layoutEdEntry) {
    const BarButton bar[4] = {{"Cancel", true}, {"Save", true}, {nullptr, false}, {nullptr, false}};
    drawButtonBar(renderer, bar, tc, -1);
  } else {
    const BarButton bar[4] = {{"Back", true}, {"Change", true}, {"Reset", true}, {nullptr, false}};
    drawButtonBar(renderer, bar, tc, barFocus);
  }
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Helper: draw signal strength indicator (1-4 bars)
static void drawSignalBars(GfxRenderer& r, int x, int y, int rssi, bool color) {
  // RSSI to bars: > -50 = 4, > -65 = 3, > -75 = 2, else 1
  int bars = (rssi > -50) ? 4 : (rssi > -65) ? 3 : (rssi > -75) ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 3;  // bar heights: 4, 7, 10, 13
    int by = y + 13 - bh;
    if (i < bars) {
      clippedFillRect(r, x + i * 5, by, 3, bh, color);
    } else {
      clippedFillRect(r, x + i * 5, by + bh - 2, 3, 2, color);
    }
  }
}

// ---------------------------------------------------------------------------
// Code entry sign-in (Dropbox): a QR code for the authorization page on the left, the steps and the typed code on the
// right. The QR code is made once per address and reused for every redraw.
// ---------------------------------------------------------------------------
static char s_qrText[260] = "";
static uint8_t s_qr[qrcodegen_BUFFER_LEN_FOR_VERSION(12)];
static bool s_qrOk = false;

static void drawCodeEntry(GfxRenderer& renderer, const cloud::ScreenModel& m, int y, int sw, bool tc) {
  const int lhB = renderer.getLineHeight(FONT_BODY);
  const int lhU = renderer.getLineHeight(FONT_UI);
  const int areaH = barTopY(renderer) - 8 - y;

  if (strcmp(s_qrText, m.qrText) != 0) {
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(12)];
    s_qrOk = qrcodegen_encodeText(m.qrText, tmp, s_qr, qrcodegen_Ecc_LOW, 1, 12, qrcodegen_Mask_AUTO, true);
    snprintf(s_qrText, sizeof(s_qrText), "%s", m.qrText);
  }
  int qrPx = 0;
  if (s_qrOk) {
    const int n = qrcodegen_getSize(s_qr);
    const int quiet = 2;   // modules of white around the code, needed by scanners (also in dark mode)
    int scale = areaH / (n + 2 * quiet);
    if (scale > 5) scale = 5;
    if (scale < 1) scale = 1;
    const int total = (n + 2 * quiet) * scale;
    clippedFillRect(renderer, 20, y, total, total, false);
    for (int yy = 0; yy < n; yy++)
      for (int xx = 0; xx < n; xx++)
        if (qrcodegen_getModule(s_qr, xx, yy)) clippedFillRect(renderer, 20 + (xx + quiet) * scale, y + (yy + quiet) * scale, scale, scale, true);
    qrPx = total;
  }

  const int tx = qrPx ? 20 + qrPx + 24 : 20;
  const int tw = sw - tx - 20;
  int ty = y;
  for (int i = 0; i < m.lineCount; i++) {
    const bool last = i == m.lineCount - 1;   // the countdown; a message about the typed code takes its place
    drawClippedText(renderer, last ? FONT_UI : FONT_BODY, tx, ty, last && m.codeNote[0] ? m.codeNote : m.lines[i], tw, tc);
    ty += (last ? lhU : lhB) + 4;
  }

  if (!m.codeBox) return;

  // The typed code, in a box, wrapped by measured width over up to two rows so every character shows.
  const int rowH = lhU + 4;
  const int boxH = 2 * rowH + 10;
  ty += 2;
  clippedFillRect(renderer, tx, ty, tw, boxH, tc);
  clippedFillRect(renderer, tx + 2, ty + 2, tw - 4, boxH - 4, !tc);
  char shown[100];
  snprintf(shown, sizeof(shown), "%s%s", m.codeInput, strncmp(m.codeNote, "Checking", 8) == 0 ? "" : "_");
  const int len = (int)strlen(shown);
  int pos = 0;
  for (int row = 0; row < 2 && pos < len; row++) {
    char seg[100];
    int take = 0;
    while (pos + take < len && take < (int)sizeof(seg) - 1) {
      seg[take] = shown[pos + take];
      seg[take + 1] = '\0';
      if (renderer.getTextWidth(FONT_UI, seg, EpdFontFamily::REGULAR) > tw - 24) { seg[take] = '\0'; break; }
      take++;
    }
    if (take == 0) break;
    seg[take] = '\0';
    drawClippedText(renderer, FONT_UI, tx + 10, ty + 6 + row * rowH, seg, tw - 14, tc);
    pos += take;
  }
}

void drawSyncScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;

  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);

  drawScreenHeader(renderer, gpio, wifiAdding() ? "Add Wi-Fi" : "Sync", tc);

  const SyncState state = getSyncState();
  const int top = HDR_H + 16;
  const int lhL = renderer.getLineHeight(LIST_FONT_L);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  BarButton bar[4] = {{"Back", true}, {nullptr, false}, {nullptr, false}, {nullptr, false}};

  switch (state) {
    case SyncState::SCANNING: {
      drawClippedText(renderer, LIST_FONT_L, 20, top, "Scanning for networks...", sw - 40, tc, EpdFontFamily::BOLD);
      bar[0].label = "Cancel";
      break;
    }

    case SyncState::NETWORK_LIST: {
      const int nc = getNetworkCount();
      const int sel = getSelectedNetwork();

      if (nc == 0) {
        const char* st = getSyncStatusText();
        drawClippedText(renderer, LIST_FONT_L, 20, top, st[0] ? st : "No networks found", sw - 40, tc,
                        EpdFontFamily::BOLD);
        bar[1] = {"Rescan", true};
      } else {
        const ListGeom g = listGeom(renderer, true, LIST_ROW_PITCH, nc);
        static ListView lv;
        viewWindow(lv, sel, nc, g.rows);
        const int start = lv.start;
        const int cw = sw - g.inset;

        touchTargetsBegin(sel, LIST_ROW_PITCH);
        for (int i = 0; i < g.rows && (start + i) < nc; i++) {
          const int pos = start + i;
          const int y = g.top + i * LIST_ROW_PITCH;
          addCardRow(pos, y, sw, LIST_ROW_PITCH, g.inset);
          const bool ink = drawCard(renderer, tc, y, LIST_ROW_H, pos == sel, false, g.inset);

          char label[48];
          snprintf(label, sizeof(label), "%s%s", getNetworkSSID(pos), isNetworkSaved(pos) ? "  (saved)" : "");
          drawClippedText(renderer, LIST_FONT_B, 20, y + (LIST_ROW_H - lhB) / 2, label, cw - 130, ink);
          // Lock marker + signal strength on the right.
          if (isNetworkEncrypted(pos)) {
            drawRightText(renderer, FONT_SMALL, cw - 62, y + (LIST_ROW_H - renderer.getLineHeight(FONT_SMALL)) / 2,
                          "Lock", ink);
          }
          drawSignalBars(renderer, cw - 46, y + (LIST_ROW_H - 14) / 2, getNetworkRSSI(pos), ink);
        }
        drawScrollbar(renderer, tc, g, LIST_ROW_PITCH, start, nc);
        bar[1] = {"Select", true};
      }
      break;
    }

    case SyncState::PASSWORD_ENTRY: {
      const int sel = getSelectedNetwork();
      char heading[64];
      snprintf(heading, sizeof(heading), "Password for %s", getNetworkSSID(sel));
      drawClippedText(renderer, LIST_FONT_B, 14, top, heading, sw - 28, tc, EpdFontFamily::BOLD);

      const int boxY = top + lhB + 14;
      const int boxH = lhB + 24;
      renderer.drawRect(12, boxY, sw - 24, boxH, 2, tc);

      // Dots, not the password itself (privacy).
      const int pLen = getPasswordLen();
      char dots[64];
      for (int i = 0; i < pLen && i < 63; i++) dots[i] = '*';
      dots[pLen < 63 ? pLen : 63] = '\0';
      drawClippedText(renderer, LIST_FONT_B, 22, boxY + 12, dots, sw - 50, tc);
      int cursorW = renderer.getSpaceWidth(LIST_FONT_B);
      if (cursorW < 2) cursorW = 8;
      const int cursorX = 22 + renderer.getTextAdvanceX(LIST_FONT_B, dots);
      if (cursorX + cursorW < sw - 14) renderer.fillRect(cursorX, boxY + 12, cursorW, lhB, tc);

      drawClippedText(renderer, FONT_UI, 14, boxY + boxH + 14, "Type the password on a Bluetooth keyboard.",
                      sw - 28, tc);
      bar[0].label = "Cancel";
      bar[1] = {"Connect", true};
      break;
    }

    case SyncState::CONNECTING: {
      drawClippedText(renderer, LIST_FONT_L, 20, top, "Connecting...", sw - 40, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12, getSyncStatusText(), sw - 40, tc);
      bar[0].label = wifiAutoConnecting() ? "Other network" : "Cancel";
      break;
    }

    case SyncState::CLOUD: {
      // The sync service decides what the OneDrive screen says; this only draws its model.
      const cloud::ScreenModel& m = cloud::screenModel();
      drawClippedText(renderer, LIST_FONT_L, 20, top, m.title, sw - 40, tc, EpdFontFamily::BOLD);
      int y = top + lhL + 14;
      if (m.codeEntry) drawCodeEntry(renderer, m, y, sw, tc);
      if (m.rowCount > 0) {
        // The cloud list: main-menu style cards, one per cloud. Tap or Up/Down + Enter chooses one.
        const ListGeom g = listGeom(renderer, true, MENU_ROW_PITCH, m.rowCount, 0, lhL + 10);
        touchTargetsBegin(m.rowSel, MENU_ROW_PITCH);
        const int cw = sw - g.inset;
        for (int i = 0; i < g.rows && i < m.rowCount; i++) {
          const int ry = g.top + i * MENU_ROW_PITCH;
          addCardRow(i, ry, sw, MENU_ROW_PITCH, g.inset);
          const bool ink = drawCard(renderer, tc, ry, MENU_ROW_H, i == m.rowSel, false, g.inset);
          drawClippedText(renderer, FONT_LARGE, 22, ry + (MENU_ROW_H - lhL) / 2, m.rowName[i], cw - 90, ink);
          if (m.rowSigned[i]) {   // a check mark, drawn as lines so it never depends on a font glyph
            const int cx = cw - 50, cy = ry + MENU_ROW_H / 2;
            for (int t = -1; t <= 1; t++) {
              clippedLine(renderer, cx, cy + t, cx + 8, cy + 9 + t, ink);
              clippedLine(renderer, cx + 8, cy + 9 + t, cx + 26, cy - 11 + t, ink);
            }
          }
        }
      }
      for (int i = 0; i < (m.codeEntry ? 0 : m.lineCount); i++) {
        if (m.bigLine && i == m.bigIndex) {
          drawClippedText(renderer, LIST_FONT_L, 20, y, m.lines[i], sw - 40, tc, EpdFontFamily::BOLD);
          y += lhL + 14;
        } else {
          drawClippedText(renderer, LIST_FONT_B, 20, y, m.lines[i], sw - 40, tc);
          y += lhB + 10;
        }
      }
      for (int i = 0; i < 4; i++) bar[i] = {m.bar[i], m.bar[i] != nullptr};
      break;
    }

    case SyncState::CONNECT_FAILED: {
      drawClippedText(renderer, LIST_FONT_L, 20, top, "Connection failed", sw - 40, tc, EpdFontFamily::BOLD);
      bar[1] = {"Retry", true};
      break;
    }

    case SyncState::SAVE_PROMPT: {
      drawClippedText(renderer, LIST_FONT_L, 20, top, "Connected", sw - 40, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12, getSyncStatusText(), sw - 40, tc);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12 + lhB + 20, "Save this password?", sw - 40, tc,
                      EpdFontFamily::BOLD);
      bar[0].label = "No";
      bar[1] = {"Yes", true};
      break;
    }

    case SyncState::FORGET_PROMPT: {
      drawClippedText(renderer, LIST_FONT_L, 20, top, "Saved password failed", sw - 40, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 16, "Forget the saved password?", sw - 40, tc,
                      EpdFontFamily::BOLD);
      bar[0].label = "No";
      bar[1] = {"Yes", true};
      break;
    }
  }

  drawButtonBar(renderer, bar, tc, -1);
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// ---------------------------------------------------------------------------
// Transfer: QR codes and addresses for a phone or computer. Everything a QR code carries is also printed, because a
// computer usually has no camera.
// ---------------------------------------------------------------------------
struct QrCache {
  char text[128];
  uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
  bool ok;
};
static QrCache s_xferQr[2];

// Draws text as a QR code (white quiet zone, also in dark mode) no larger than maxPx; returns its side, 0 if none.
static int drawQrAt(GfxRenderer& r, QrCache& c, const char* text, int x, int y, int maxPx) {
  if (strcmp(c.text, text) != 0) {
    uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(8)];
    c.ok = qrcodegen_encodeText(text, tmp, c.qr, qrcodegen_Ecc_MEDIUM, 1, 8, qrcodegen_Mask_AUTO, true);
    snprintf(c.text, sizeof(c.text), "%s", text);
  }
  if (!c.ok) return 0;
  const int n = qrcodegen_getSize(c.qr);
  const int quiet = 2;
  int scale = maxPx / (n + 2 * quiet);
  if (scale > 7) scale = 7;
  if (scale < 1) scale = 1;
  const int total = (n + 2 * quiet) * scale;
  clippedFillRect(r, x, y, total, total, false);
  for (int yy = 0; yy < n; yy++)
    for (int xx = 0; xx < n; xx++)
      if (qrcodegen_getModule(c.qr, xx, yy)) clippedFillRect(r, x + (xx + quiet) * scale, y + (yy + quiet) * scale, scale, scale, true);
  return total;
}

struct XferLine {
  const char* text;
  int font;
  bool bold;
};

// One QR code with its lines of text: beside it (side = true) or under it.
static void drawXferBlock(GfxRenderer& r, QrCache& c, const char* qr, int x, int y, int w, int h, const XferLine* lines,
                          int n, bool side, bool tc) {
  int textH = 0;
  for (int i = 0; i < n; i++) textH += r.getLineHeight(lines[i].font) + 4;
  const int maxQr = side ? (h < w / 2 ? h : w / 2) : (h - textH - 8 < w ? h - textH - 8 : w);
  const int q = drawQrAt(r, c, qr, side ? x : x + (w - (maxQr > 0 ? maxQr : 0)) / 2, y, maxQr);
  int tx = side ? x + q + 16 : x;
  int ty = side ? y + (q > textH ? (q - textH) / 2 : 0) : y + q + 8;
  const int tw = side ? w - q - 16 : w;
  for (int i = 0; i < n; i++) {
    if (!lines[i].text || !lines[i].text[0]) continue;
    drawClippedText(r, lines[i].font, tx, ty, lines[i].text, tw, tc,
                    lines[i].bold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);
    ty += r.getLineHeight(lines[i].font) + 4;
  }
}

void drawTransferScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);
  const transfer::View& v = transfer::view();
  drawScreenHeader(renderer, gpio, v.picture ? "Add a picture" : "Transfer", tc);

  const int top = HDR_H + (CLASSIC ? 8 : 16);
  const int lhL = renderer.getLineHeight(LIST_FONT_L);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  const int bottom = barTopY(renderer) - 8;
  BarButton bar[4] = {{"Back", true}, {nullptr, false}, {nullptr, false}, {nullptr, false}};
  char what[64];
  if (v.picture) snprintf(what, sizeof(what), "Send a sleep picture from your phone");
  else snprintf(what, sizeof(what), "Sharing: %s", v.what);
  char sent[32] = "";
  if (v.sent > 0) snprintf(sent, sizeof(sent), v.picture ? "%d received" : "%d sent", v.sent);

  switch (v.phase) {
    case transfer::Phase::Looking:
      drawClippedText(renderer, LIST_FONT_L, 20, top, "Looking for Wi-Fi...", sw - 40, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12, what, sw - 40, tc);
      break;

    case transfer::Phase::NoWifi:
      drawClippedText(renderer, LIST_FONT_L, 20, top, "No Wi-Fi", sw - 40, tc, EpdFontFamily::BOLD);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12, "No saved Wi-Fi network is in range.", sw - 40, tc);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12 + lhB + 10,
                      "Start a hotspot: the MicroSlate makes its own Wi-Fi", sw - 40, tc);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12 + 2 * (lhB + 10), "for your phone or computer.", sw - 40, tc);
      drawClippedText(renderer, LIST_FONT_B, 20, top + lhL + 12 + 3 * (lhB + 10) + 6,
                      "Or add your network in Wi-Fi settings.", sw - 40, tc);
      bar[1] = {"Start hotspot", true};
      bar[2] = {"Wi-Fi settings", true};
      break;

    case transfer::Phase::Failed:
      drawClippedText(renderer, LIST_FONT_L, 20, top, v.error, sw - 40, tc, EpdFontFamily::BOLD);
      bar[1] = {"Try again", true};
      break;

    case transfer::Phase::Ready: {
      drawClippedText(renderer, LIST_FONT_B, 20, top, what, sw - 40, tc, EpdFontFamily::BOLD);
      const int y0 = top + lhB + 12;
      const int areaH = bottom - y0;
      const bool portrait = sw < sh;
      char url[64], name[48], ipText[40];
      snprintf(url, sizeof(url), "http://%s/%s/", v.ip, v.code);
      snprintf(name, sizeof(name), "microslate.local/%s", v.code);
      snprintf(ipText, sizeof(ipText), "or %s/%s", v.ip, v.code);
      if (!v.hotspot) {
        char net[48];
        snprintf(net, sizeof(net), "Wi-Fi: %s", v.ssid);
        const XferLine lines[] = {
            {"Phone: scan the code", FONT_UI, false},
            {"Computer: open", FONT_UI, false},
            {name, LIST_FONT_L, true},
            {ipText, FONT_UI, false},
            {net, FONT_SMALL, false},
            {sent, FONT_UI, true},
        };
        drawXferBlock(renderer, s_xferQr[0], url, 20, y0, sw - 40, areaH, lines, 6, !portrait, tc);
        bar[1] = {"Use hotspot", true};
      } else {
        char wifiQr[128], netName[48], pass[40];
        snprintf(wifiQr, sizeof(wifiQr), "%s", transfer::wifiQrText(v.ssid, v.password).c_str());
        snprintf(netName, sizeof(netName), "Name: %s", v.ssid);
        snprintf(pass, sizeof(pass), "Password: %s", v.password);
        const XferLine join[] = {
            {"1. Join this Wi-Fi", LIST_FONT_B, true},
            {netName, FONT_UI, false},
            {pass, FONT_UI, false},
        };
        const XferLine page[] = {
            {"2. Open", LIST_FONT_B, true},
            {name, FONT_UI, true},
            {ipText, FONT_SMALL, false},
            {sent, FONT_UI, true},
        };
        if (portrait) {   // one above the other, each code beside its text
          const int h = (areaH - 12) / 2;
          drawXferBlock(renderer, s_xferQr[1], wifiQr, 20, y0, sw - 40, h, join, 3, true, tc);
          drawXferBlock(renderer, s_xferQr[0], url, 20, y0 + h + 12, sw - 40, h, page, 4, true, tc);
        } else {          // side by side, each code above its text
          const int w = (sw - 60) / 2;
          drawXferBlock(renderer, s_xferQr[1], wifiQr, 20, y0, w, areaH, join, 3, false, tc);
          drawXferBlock(renderer, s_xferQr[0], url, 40 + w, y0, w, areaH, page, 4, false, tc);
        }
      }
      break;
    }

    default:
      break;
  }

  if (CLASSIC) {
    // Button-only device: say what the two buttons do.
    char hint[64];
    if (bar[1].label && bar[2].label)
      snprintf(hint, sizeof(hint), "Select: %s   Down: %s   Back: Exit", bar[1].label, bar[2].label);
    else if (bar[1].label) snprintf(hint, sizeof(hint), "Select: %s    Back: Exit", bar[1].label);
    else snprintf(hint, sizeof(hint), "Back: Exit");
    const int top2 = barTopY(renderer);
    clippedLine(renderer, 5, top2 - 3, sw - 5, top2 - 3, tc);
    drawClippedText(renderer, FONT_SMALL, 10, top2 + (BAR_H - renderer.getLineHeight(FONT_SMALL)) / 2, hint, sw - 20, tc);
  } else {
    drawButtonBar(renderer, bar, tc, -1);
  }
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Settings > Wi-Fi > Firmware update (src/update/fw_update): check, ask, progress, restart.
void drawFwUpdateScreen(GfxRenderer& renderer, HalGPIO& gpio) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);
  drawScreenHeader(renderer, gpio, "Firmware update", tc);

  const fwupdate::View& v = fwupdate::view();
  const int top = HDR_H + (CLASSIC ? 8 : 16);
  const int lhL = renderer.getLineHeight(LIST_FONT_L);
  const int lhB = renderer.getLineHeight(LIST_FONT_B);
  const int y2 = top + lhL + 12;
  const int y3 = y2 + lhB + 10;
  BarButton bar[4] = {{"Back", true}, {nullptr, false}, {nullptr, false}, {nullptr, false}};
  char title[64], line[96];
  title[0] = line[0] = '\0';
  snprintf(line, sizeof(line), "This MicroSlate has version %s.", fwupdate::version());

  switch (v.phase) {
    case fwupdate::Phase::Connecting: snprintf(title, sizeof(title), "Connecting to Wi-Fi..."); break;
    case fwupdate::Phase::Checking: snprintf(title, sizeof(title), "Checking for updates..."); break;
    case fwupdate::Phase::NoWifi:
      snprintf(title, sizeof(title), "No Wi-Fi");
      snprintf(line, sizeof(line), "%s", v.message);
      bar[1] = {"Try again", true};
      break;
    case fwupdate::Phase::UpToDate:
      snprintf(title, sizeof(title), "No update");
      drawClippedText(renderer, LIST_FONT_B, 20, y3, v.message, sw - 40, tc);
      break;
    case fwupdate::Phase::Available:
      snprintf(title, sizeof(title), "Version %s is available", v.latest);
      drawClippedText(renderer, LIST_FONT_B, 20, y3, "Notes and settings stay as they are.", sw - 40, tc);
      drawClippedText(renderer, LIST_FONT_B, 20, y3 + lhB + 10, "It takes a minute or two, then the MicroSlate restarts.",
                      sw - 40, tc);
      bar[1] = {"Install", true};
      break;
    case fwupdate::Phase::Installing:
      snprintf(title, sizeof(title), "Installing %s...  %d%%", v.latest, v.pct);
      snprintf(line, sizeof(line), "Keep the MicroSlate on until it restarts.");
      break;
    case fwupdate::Phase::Done:
      snprintf(title, sizeof(title), "Version %s installed", v.latest);
      snprintf(line, sizeof(line), "Restarting...");
      bar[0] = {nullptr, false};
      break;
    case fwupdate::Phase::Failed:
      snprintf(title, sizeof(title), "The update did not work");
      snprintf(line, sizeof(line), "%s", v.message);
      drawClippedText(renderer, LIST_FONT_B, 20, y3, "The MicroSlate still runs the version it had.", sw - 40, tc);
      bar[1] = {"Try again", true};
      break;
    default:
      break;
  }
  if (v.phase == fwupdate::Phase::Installing) bar[0] = {nullptr, false};   // no way out mid-install
  drawClippedText(renderer, LIST_FONT_L, 20, top, title, sw - 40, tc, EpdFontFamily::BOLD);
  drawClippedText(renderer, LIST_FONT_B, 20, y2, line, sw - 40, tc);

  if (v.phase == fwupdate::Phase::Installing) {   // progress bar
    const int bx = 20, bw = sw - 40, bh = 18;
    const int by = y3 + 10;
    clippedLine(renderer, bx, by, bx + bw, by, tc);
    clippedLine(renderer, bx, by + bh, bx + bw, by + bh, tc);
    clippedLine(renderer, bx, by, bx, by + bh, tc);
    clippedLine(renderer, bx + bw, by, bx + bw, by + bh, tc);
    clippedFillRect(renderer, bx, by, bw * v.pct / 100, bh, tc);
  }

  if (CLASSIC) {
    // Button-only device: say what the buttons do.
    char hint[64] = "";
    if (bar[1].label) snprintf(hint, sizeof(hint), "Select: %s    Back: Exit", bar[1].label);
    else if (bar[0].label) snprintf(hint, sizeof(hint), "Back: Exit");
    const int top2 = barTopY(renderer);
    clippedLine(renderer, 5, top2 - 3, sw - 5, top2 - 3, tc);
    drawClippedText(renderer, FONT_SMALL, 10, top2 + (BAR_H - renderer.getLineHeight(FONT_SMALL)) / 2, hint, sw - 20, tc);
  } else {
    drawButtonBar(renderer, bar, tc, -1);
  }
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}

// Centered notice, used while the notes list waits for OneDrive.
void drawCloudNotice(GfxRenderer& renderer, HalGPIO& gpio, const char* text, const char* sub) {
  renderer.clearScreen();
  const int sw = renderer.getScreenWidth();
  const int sh = renderer.getScreenHeight();
  const bool tc = !darkMode;
  if (darkMode) clippedFillRect(renderer, 0, 0, sw, sh, true);
  drawScreenHeader(renderer, gpio, "Notes", tc);
  const int lh = renderer.getLineHeight(LIST_FONT_L);
  drawClippedText(renderer, LIST_FONT_L, 20, sh / 2 - lh, text, sw - 40, tc, EpdFontFamily::BOLD);
  // sub may hold several lines separated by '\n'.
  const int lhSub = renderer.getLineHeight(FONT_UI);
  int y = sh / 2 + 8;
  for (const char* s = sub; s && *s;) {
    const char* nl = strchr(s, '\n');
    const size_t n = nl ? (size_t)(nl - s) : strlen(s);
    char line[128];
    snprintf(line, sizeof(line), "%.*s", (int)n, s);
    drawClippedText(renderer, FONT_UI, 20, y, line, sw - 40, tc);
    y += lhSub + 4;
    s = nl ? nl + 1 : s + n;
  }
  renderer.beginRefresh(HalDisplay::FAST_REFRESH);
}
