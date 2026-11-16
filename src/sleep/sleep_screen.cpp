#include "sleep_screen.h"

#include "../config.h"
#include "../file_manager.h"

#include <Arduino.h>
#include <Bitmap.h>
#include <GfxRenderer.h>
#include <Preferences.h>
#include <SDCardManager.h>
#include <esp_random.h>

#include <algorithm>
#include <vector>

namespace sleepscr {
namespace {

constexpr const char* kDir = "/sleep";
constexpr const char* kNs = "sleepscr";
constexpr int kMaxPictures = 999;

Choice g_choice = Choice::Stock;
std::string g_picture;   // the chosen picture's file name (Choice::Picture)
std::string g_last;      // shown at the last sleep (Random never repeats it)
bool g_everAdded = false;   // a picture was added once: later ones no longer become the sleep screen by themselves
std::vector<std::string> g_names;
bool g_loaded = false;

void save() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putUChar("choice", (uint8_t)g_choice);
  p.putString("pic", g_picture.c_str());
  p.putString("last", g_last.c_str());
  p.putBool("added", g_everAdded);
  p.end();
}

void path(const std::string& name, char* out, size_t n) { snprintf(out, n, "%s/%s", kDir, name.c_str()); }

// sleep-2 before sleep-10: shorter names first, then by name.
bool byNumber(const std::string& a, const std::string& b) {
  return a.size() != b.size() ? a.size() < b.size() : a < b;
}

int indexOf(const std::string& name) {
  for (size_t i = 0; i < g_names.size(); i++)
    if (g_names[i] == name) return (int)i;
  return -1;
}

// The pictures changed: keep the choice if it still fits, otherwise fall back (sleep_rules: settleChoice).
void settle() {
  const bool there = !g_picture.empty() && indexOf(g_picture) >= 0;
  const Choice c = settleChoice(g_choice, there, (int)g_names.size());
  bool changed = c != g_choice;
  if (c == Choice::Picture && !there) {
    g_picture = g_names.empty() ? std::string() : g_names[0];
    changed = true;
  }
  if (changed) {
    g_choice = c;
    save();
    DBG_PRINTF("[SLEEP] choice now %s\n", choiceLabel().c_str());
  }
}

}  // namespace

void init() {
  Preferences p;
  if (p.begin(kNs, true)) {
    const uint8_t c = p.getUChar("choice", 0);
    g_choice = c == 1 ? Choice::Random : c == 2 ? Choice::Picture : Choice::Stock;
    g_picture = p.getString("pic", "").c_str();
    g_last = p.getString("last", "").c_str();
    g_everAdded = p.getBool("added", false);
    p.end();
  }
  g_loaded = false;
}

void refresh() {
  g_loaded = true;
  g_names.clear();
  if (sdCardMissing()) return;   // no card: nothing to settle against; the stock screen shows meanwhile
  auto dir = SdMan.open(kDir);
  if (dir && dir.isDirectory()) {
    char n[64];
    for (auto f = dir.openNextFile(); f && (int)g_names.size() < kMaxPictures; f = dir.openNextFile()) {
      f.getName(n, sizeof(n));
      const size_t len = strlen(n);
      if (!f.isDirectory() && n[0] != '.' && len > 4 && strcasecmp(n + len - 4, ".bmp") == 0) g_names.push_back(n);
      f.close();
    }
  }
  if (dir) dir.close();
  std::sort(g_names.begin(), g_names.end(), byNumber);
  settle();
}

int count() {
  if (!g_loaded) refresh();
  return (int)g_names.size();
}

const char* name(int i) { return (i >= 0 && i < count()) ? g_names[i].c_str() : ""; }
std::string label(int i) { return (i >= 0 && i < count()) ? pictureLabel(g_names[i]) : std::string(); }

Choice choice() {
  count();   // settled against the card
  return g_choice;
}

bool isChosen(int i) { return choice() == Choice::Picture && i >= 0 && i < count() && g_names[i] == g_picture; }

std::string choiceLabel() {
  switch (g_choice) {
    case Choice::Random: return "Random";
    case Choice::Picture: return pictureLabel(g_picture);
    default: return "Stock";
  }
}

void chooseStock() {
  g_choice = Choice::Stock;
  save();
}

void chooseRandom() {
  if (count() < 2) return;
  g_choice = Choice::Random;
  save();
}

void choosePicture(int i) {
  if (i < 0 || i >= count()) return;
  g_choice = Choice::Picture;
  g_picture = g_names[i];
  save();
}

bool remove(int i) {
  if (i < 0 || i >= count()) return false;
  char p[96];
  path(g_names[i], p, sizeof(p));
  const bool ok = SdMan.remove(p);
  DBG_PRINTF("[SLEEP] removed %s%s\n", g_names[i].c_str(), ok ? "" : " (FAILED)");
  if (g_names[i] == g_last) {
    g_last.clear();
    save();
  }
  refresh();   // the chosen one gone: falls back (settle)
  return ok;
}

bool drawPicture(GfxRenderer& r, int i) {
  if (i < 0 || i >= count()) return false;
  char p[96];
  path(g_names[i], p, sizeof(p));
  FsFile f = SdMan.open(p, O_RDONLY);
  if (!f) return false;
  bool ok = false;
  {
    Bitmap bmp(f, false);
    if (bmp.parseHeaders() == BmpReaderError::Ok) {
      const int sw = r.getScreenWidth(), sh = r.getScreenHeight();
      // Fit: a picture made for the other screen shape (the card moved between devices) is shrunk, centred.
      const float scale = std::min(1.0f, std::min((float)sw / bmp.getWidth(), (float)sh / bmp.getHeight()));
      const int dw = (int)(bmp.getWidth() * scale), dh = (int)(bmp.getHeight() * scale);
      r.drawBitmap(bmp, (sw - dw) / 2, (sh - dh) / 2, sw, sh);
      ok = true;
    }
  }
  f.close();
  return ok;
}

void drawStock(GfxRenderer& r) {
  // Placeholder until the logo is chosen (branding-assets.md): the name, centred, with the wake hint.
  const int sw = r.getScreenWidth();
  const int sh = r.getScreenHeight();
  const char* title = "MicroSlate";
  const int tw = r.getTextWidth(FONT_LARGE, title, EpdFontFamily::BOLD);
  const int ty = sh * 40 / 100;
  r.drawText(FONT_LARGE, (sw - tw) / 2, ty, title, true, EpdFontFamily::BOLD);
  const int lineW = tw + 40;
  r.fillRect((sw - lineW) / 2, ty + r.getLineHeight(FONT_LARGE) + 10, lineW, 2, true);
  const char* sub = "Asleep";
  r.drawText(FONT_UI, (sw - r.getTextAdvanceX(FONT_UI, sub)) / 2, ty + r.getLineHeight(FONT_LARGE) + 24, sub, true);
  const char* footer = "Hold Power to wake";
  r.drawText(FONT_SMALL, (sw - r.getTextAdvanceX(FONT_SMALL, footer)) / 2, sh * 80 / 100, footer, true);
}

void drawForSleep(GfxRenderer& r) {
  if (!sdCardMissing()) {
    refresh();
    int i = -1;
    if (g_choice == Choice::Picture) i = indexOf(g_picture);
    else if (g_choice == Choice::Random) i = pickRandom(count(), indexOf(g_last), esp_random());
    if (i >= 0 && drawPicture(r, i)) {
      DBG_PRINTF("[SLEEP] picture %s%s\n", g_names[i].c_str(), g_choice == Choice::Random ? " (random)" : "");
      if (g_names[i] != g_last) {
        g_last = g_names[i];
        save();
      }
      return;
    }
    if (i >= 0) r.clearScreen();   // a picture that could not be read: the stock screen instead, never a blank one
  }
  drawStock(r);
}

bool acceptUpload(std::string& savedName, const char*& error) {
  error = "";
  FsFile f = SdMan.open(kUploadTmp, O_RDONLY);
  if (!f) { error = "The MicroSlate could not store the picture (SD card)."; return false; }
  uint8_t head[62];
  const size_t len = (size_t)f.size();
  const int got = f.read(head, sizeof(head));
  f.close();
  int w = 0, h = 0;
  if (got != (int)sizeof(head) || !pictureLooksValid(head, sizeof(head), len, w, h)) {
    SdMan.remove(kUploadTmp);
    error = "That is not a picture this page made. Reload the page and try again.";
    return false;
  }
  refresh();
  if (count() >= kMaxPictures) {
    SdMan.remove(kUploadTmp);
    error = "The MicroSlate is full of pictures. Delete some on the device first.";
    return false;
  }
  savedName = nextPictureName(g_names);
  char p[96];
  path(savedName, p, sizeof(p));
  if (!SdMan.rename(kUploadTmp, p)) {
    SdMan.remove(kUploadTmp);
    error = "The MicroSlate could not store the picture (SD card).";
    return false;
  }
  refresh();
  if (!g_everAdded) {   // the first picture ever: that is why it was added
    g_everAdded = true;
    if (g_choice == Choice::Stock) {
      g_choice = Choice::Picture;
      g_picture = savedName;
    }
    save();
  }
  DBG_PRINTF("[SLEEP] received %s (%dx%d, %u bytes), sleep screen: %s\n", savedName.c_str(), w, h, (unsigned)len,
             choiceLabel().c_str());
  return true;
}

}  // namespace sleepscr
