#include "keyboard_layout.h"

#include <Arduino.h>
#include <Keymap.h>
#include <SDCardManager.h>

#include <cstdlib>
#include <cstring>

#include "config.h"
#include "sd_safe.h"

bool uiFontHasGlyph(uint32_t cp);   // ui_renderer.cpp: the editor font can show this character

namespace {

constexpr char kDir[] = "/microslate/layouts";
// The user's own key changes, one file per layout, in a folder so the layout scan skips
// them. The layout files themselves are never edited on the device.
constexpr char kChangesDir[] = "/microslate/layouts/changes";
constexpr char kUsChanges[] = "built-in-us.txt";   // changes made while on the built-in US layout
constexpr char kChangesHeader[] =
    "# Keys changed on the MicroSlate, on top of the layout of the same name. One key per line.\n";
constexpr char kProblemsPath[] = "/microslate/layout-problems.txt";
constexpr int kMaxFiles = 24;
constexpr size_t kMaxFileBytes = 16 * 1024;

keymap::Layout g_layout;   // the active layout (US when index 0) with the user's changes
keymap::Typer g_typer;
char g_files[kMaxFiles][40];
int g_fileCount = 0;
int g_active = 0;
int g_problems = 0;

void scanFiles() {
  g_fileCount = 0;
  if (!SdMan.exists(kDir)) return;
  auto dir = SdMan.open(kDir);
  if (!dir || !dir.isDirectory()) { if (dir) dir.close(); return; }
  dir.rewindDirectory();
  char name[64];
  for (auto f = dir.openNextFile(); f && g_fileCount < kMaxFiles; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const bool isDir = f.isDirectory();
    f.close();
    const size_t len = strlen(name);
    if (isDir || name[0] == '.' || len < 5 || len >= sizeof(g_files[0])) continue;
    if (strcasecmp(name + len - 4, ".txt") != 0) continue;
    strcpy(g_files[g_fileCount++], name);
  }
  dir.close();
  qsort(g_files, size_t(g_fileCount), sizeof(g_files[0]),
        [](const void* a, const void* b) { return strcasecmp((const char*)a, (const char*)b); });
}

// Whole file into a new buffer (free() it); nullptr when missing or too large.
char* readWhole(const char* path, size_t* len) {
  auto f = SdMan.open(path, O_RDONLY);
  if (!f.isOpen()) return nullptr;
  const size_t size = size_t(f.size());
  char* buf = size <= kMaxFileBytes ? (char*)malloc(size + 1) : nullptr;
  const int got = buf ? f.read(buf, int(size)) : -1;
  f.close();
  if (got < 0) { free(buf); return nullptr; }
  buf[got] = '\0';
  *len = size_t(got);
  return buf;
}

// One line per problem, for the user to fix the file; removed when the file is clean.
void writeProblems(const char* file, const keymap::LoadReport& rep) {
  if (rep.problems == 0 && rep.unshown == 0) {
    if (SdMan.exists(kProblemsPath)) SdMan.remove(kProblemsPath);
    return;
  }
  auto f = SdMan.open(kProblemsPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f.isOpen()) return;
  char line[128];
  int n = snprintf(line, sizeof(line), "%s: %d line(s) skipped, %d character(s) the font cannot show\n", file,
                   rep.problems, rep.unshown);
  f.write((const uint8_t*)line, size_t(n));
  for (int i = 0; i < rep.listed; i++) {
    n = snprintf(line, sizeof(line), "line %d: %s\n", rep.list[i].line, rep.list[i].text);
    f.write((const uint8_t*)line, size_t(n));
  }
  if (rep.problems + rep.unshown > rep.listed) {
    n = snprintf(line, sizeof(line), "(only the first %d are listed)\n", rep.listed);
    f.write((const uint8_t*)line, size_t(n));
  }
  f.close();
}

// The changes file of the active layout.
void changesPath(char* path, size_t size) {
  snprintf(path, size, "%s/%s", kChangesDir, g_active > 0 ? g_files[g_active - 1] : kUsChanges);
}

// Puts the user's changes on top of the loaded layout.
void applyChanges(bool baseClean) {
  char path[110];
  changesPath(path, sizeof(path));
  size_t len = 0;
  char* text = readWhole(path, &len);
  if (!text) return;
  keymap::LoadReport rep;
  g_layout.applyChanges(text, len, rep, uiFontHasGlyph);
  free(text);
  DBG_PRINTF("[Layout] changes/%s: %d keys, %d problems\n", strrchr(path, '/') + 1, rep.keys, rep.problems);
  if (baseClean && rep.problems > 0) writeProblems(strrchr(path, '/') + 1, rep);
  g_problems += rep.problems;
}

// Loads file index i (1-based; 0 = US), then the user's changes for it. Falls back to US
// when the file cannot be read.
void loadIndex(int i) {
  g_typer.cancel();
  g_problems = 0;
  g_active = 0;
  if (i <= 0 || i > g_fileCount) {
    g_layout.loadUs();
    applyChanges(true);
    return;
  }
  char path[96];
  snprintf(path, sizeof(path), "%s/%s", kDir, g_files[i - 1]);
  size_t len = 0;
  char* buf = readWhole(path, &len);
  keymap::LoadReport rep;
  const bool ok = buf && len > 0 && g_layout.loadText(buf, len, rep, uiFontHasGlyph);
  free(buf);
  DBG_PRINTF("[Layout] %s: %s, %d keys, %d problems, %d unshown\n", g_files[i - 1], ok ? "loaded" : "unusable",
             rep.keys, rep.problems, rep.unshown);
  writeProblems(g_files[i - 1], rep);
  if (!ok) {   // the layout is US again
    g_layout.loadUs();
    applyChanges(true);
    return;
  }
  // Only real mistakes warn in Settings; characters the font cannot show (RU's №) are
  // listed in the problems file but are not the file's fault.
  g_problems = rep.problems;
  g_active = i;
  applyChanges(rep.problems == 0 && rep.unshown == 0);
}

int findFile(const char* name) {
  for (int i = 0; i < g_fileCount; i++) if (strcasecmp(g_files[i], name) == 0) return i + 1;
  return 0;
}

// The first layout editor (2026-10-28 builds) kept its edits in my-layout.txt, a copy of US
// it selected, and backups in layouts/originals/. Its key lines become the changes of the
// built-in US layout and the file goes. Returns true when it was moved.
bool migrateOwnLayout() {
  static const char kOwn[] = "/microslate/layouts/my-layout.txt";
  static const char kMark[] = "# My keyboard layout. Starts as US QWERTY";
  size_t len = 0;
  char* text = readWhole(kOwn, &len);
  if (!text) return false;
  if (strncmp(text, kMark, strlen(kMark)) != 0) { free(text); return false; }   // the user's own file
  char usPath[110];
  snprintf(usPath, sizeof(usPath), "%s/%s", kChangesDir, kUsChanges);
  bool ok = true;
  if (!SdMan.exists(usPath)) {
    // Keep the key lines only (no comments, no name/short/base).
    char* out = (char*)malloc(len + sizeof(kChangesHeader) + 1);
    ok = out != nullptr;
    if (ok) {
      size_t n = strlen(kChangesHeader);
      memcpy(out, kChangesHeader, n);
      for (const char* p = text; *p;) {
        const char* e = strchr(p, '\n');
        const size_t ln = e ? size_t(e - p + 1) : strlen(p);
        const bool header = *p == '#' || strncmp(p, "name", 4) == 0 || strncmp(p, "short", 5) == 0 ||
                            strncmp(p, "base", 4) == 0 || *p == '\n' || *p == '\r';
        if (!header) { memcpy(out + n, p, ln); n += ln; }
        p += ln;
      }
      if (!SdMan.exists(kChangesDir)) SdMan.mkdir(kChangesDir);
      ok = sdWriteSafely(usPath, out, n);
      free(out);
    }
  }
  free(text);
  if (!ok) return false;
  SdMan.remove(kOwn);
  SdMan.remove("/microslate/layouts/originals/my-layout.txt");
  SdMan.rmdir("/microslate/layouts/originals");   // only goes when empty
  DBG_PRINTF("[Layout] my-layout.txt moved into changes/%s\n", kUsChanges);
  return true;
}

// Rewrites the active layout's changes file with fn(text, len, out, cap, &fit) -> new
// length. An empty result removes the file.
template <typename Fn>
bool editChanges(const char* what, Fn fn) {
  char path[110];
  changesPath(path, sizeof(path));
  size_t len = 0;
  char* in = readWhole(path, &len);
  if (!in) {
    in = strdup(kChangesHeader);
    len = in ? strlen(in) : 0;
  }
  if (!in) return false;
  const size_t cap = len + 200;
  char* out = (char*)malloc(cap);
  bool fit = false;
  const size_t n = out ? fn(in, len, out, cap, &fit) : 0;
  bool ok = out && fit;
  if (ok) {
    const bool onlyHeader = n <= strlen(kChangesHeader) && strncmp(out, kChangesHeader, n) == 0;
    if (onlyHeader || n == 0) {
      ok = !SdMan.exists(path) || SdMan.remove(path);
    } else {
      if (!SdMan.exists(kChangesDir)) SdMan.mkdir(kChangesDir);
      ok = sdWriteSafely(path, out, n);
    }
  }
  free(in);
  free(out);
  DBG_PRINTF("[Layout] %s: %s\n", what, ok ? "saved" : "FAILED");
  loadIndex(g_active);   // reload what is on the card, saved or not
  return ok;
}

}  // namespace

void keyboardLayoutsInit(const char* savedFile, int legacyIndex) {
  const bool moved = migrateOwnLayout();
  scanFiles();
  int pick = 0;
  if (moved && savedFile && strcasecmp(savedFile, "my-layout.txt") == 0) pick = 0;   // its keys are now on US
  else if (savedFile && savedFile[0]) pick = findFile(savedFile);
  else if (legacyIndex == 1) pick = findFile("ru.txt");
  else if (legacyIndex == 2) pick = findFile("de.txt");
  loadIndex(pick);
}

void keyboardLayoutNext() {
  scanFiles();
  loadIndex((g_active + 1) % (1 + g_fileCount));
}

void keyboardLayoutPrev() {
  scanFiles();
  loadIndex((g_active + g_fileCount) % (1 + g_fileCount));
}

const char* keyboardLayoutName() { return g_layout.name(); }
const char* keyboardLayoutShortName() { return g_layout.shortName(); }
const char* keyboardLayoutFile() { return g_active > 0 ? g_files[g_active - 1] : ""; }
bool keyboardLayoutIsUs() { return g_active == 0; }
int keyboardLayoutProblems() { return g_problems; }

const char* keyboardLayoutText(uint8_t hid, uint8_t modifiers, bool capsLockOn) {
  return g_typer.press(g_layout, hid, modifiers, capsLockOn);
}

void keyboardLayoutCancelDead() { g_typer.cancel(); }
bool keyboardLayoutDeadPending() { return g_typer.pending(); }

bool keyboardLayoutIsTextKey(uint8_t hid) {
  return keymap::isLayoutKey(hid) || hid == HID_KEY_SPACE || hid == HID_KEY_ENTER;
}

char keyboardLayoutShortcutLetter(uint8_t hid) { return keymap::shortcutLetter(g_layout, hid); }

const char* keyboardLayoutQwertyText(uint8_t hid, uint8_t modifiers, bool capsLockOn) {
  return keymap::usText(hid, modifiers, capsLockOn);
}

// --- Layout editor support ---

const char* keyboardLayoutSlot(uint8_t hid, int layer, bool* dead) { return g_layout.text(hid, layer, dead); }

bool keyboardLayoutKeyChanged(uint8_t hid) { return g_layout.changed(hid); }
int keyboardLayoutChangedCount() { return g_layout.changedCount(); }

bool keyboardLayoutHasAltGr() {
  for (int k = 0; k < keymap::kKeys; k++)
    if (g_layout.text(uint8_t(k), keymap::kAltGr) || g_layout.text(uint8_t(k), keymap::kShiftAltGr)) return true;
  return false;
}

bool keyboardLayoutSlotShown(const char* text) {
  if (!text) return true;
  for (const char* p = text; *p;) {
    const uint8_t c = uint8_t(*p);
    uint32_t cp = c;
    int n = 1;
    if (c >= 0xF0) { cp = c & 0x07; n = 4; }
    else if (c >= 0xE0) { cp = c & 0x0F; n = 3; }
    else if (c >= 0xC0) { cp = c & 0x1F; n = 2; }
    for (int k = 1; k < n; k++) {
      if (!p[k]) { n = k; break; }   // cut short: never step past the end of the string
      cp = (cp << 6) | (uint8_t(p[k]) & 0x3F);
    }
    if (cp >= 0x20 && !uiFontHasGlyph(cp)) return false;
    p += n;
  }
  return true;
}

bool keyboardLayoutSetSlot(uint8_t hid, int layer, const char* value) {
  if (layer < 0 || layer >= keymap::kLayers) return false;
  const char* vals[keymap::kLayers];
  bool deads[keymap::kLayers];
  for (int i = 0; i < keymap::kLayers; i++) vals[i] = g_layout.text(hid, i, &deads[i]);
  vals[layer] = value;
  deads[layer] = false;
  char line[160];
  if (!keymap::formatKeyLine(hid, vals, deads, g_layout.capsFlag(hid), line, sizeof(line))) return false;
  char what[180];
  snprintf(what, sizeof(what), "change %s", line);
  return editChanges(what, [&](const char* in, size_t len, char* out, size_t cap, bool* fit) {
    const size_t n = keymap::withKeyLine(in, len, hid, line, out, cap);
    *fit = n > 0;
    return n;
  });
}

int keyboardLayoutResetKey(uint8_t hid) {
  if (!g_layout.changed(hid)) return 0;
  char kn[16];
  char what[48];
  snprintf(what, sizeof(what), "reset %s", keymap::keyName(hid, kn, sizeof(kn)));
  const bool ok = editChanges(what, [&](const char* in, size_t len, char* out, size_t cap, bool* fit) {
    return keymap::withoutKey(in, len, hid, out, cap, fit);
  });
  return ok ? 1 : -1;
}

bool keyboardLayoutResetAll() {
  char path[110];
  changesPath(path, sizeof(path));
  const bool ok = !SdMan.exists(path) || SdMan.remove(path);
  DBG_PRINTF("[Layout] reset all changes: %s\n", ok ? "done" : "FAILED");
  loadIndex(g_active);
  return ok;
}
