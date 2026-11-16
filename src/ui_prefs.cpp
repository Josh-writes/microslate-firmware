#include "ui_prefs.h"

#include <Preferences.h>
#include <SDCardManager.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "keyboard_layout.h"
#include "sd_backup.h"
#include "spell/spell_service.h"

extern Orientation currentOrientation;
extern bool darkMode;
extern WritingMode writingMode;
extern FontSize fontSize;
extern bool showWordCount;
extern bool newNotesMd;

namespace uiprefs {
namespace {

constexpr const char* kSdCopy = "/microslate/ui_prefs.json";

// One setting that fits in a byte. NVS keeps a bool as a byte too (Preferences::putBool is putUChar), so every
// entry is read and written as a byte and the keys keep their names and types from older firmware.
struct Pref {
  const char* nvsKey;
  const char* jsonKey;   // its name in the SD copy
  uint8_t def;
  uint8_t (*get)();
  void (*set)(uint8_t);
};

// In the order of the SD copy.
const Pref kPrefs[] = {
    {"orient", "orient", 0, [] { return (uint8_t)currentOrientation; },
     [](uint8_t v) { currentOrientation = static_cast<Orientation>(v); }},
    {"darkMode", "dark", 0, [] { return (uint8_t)darkMode; }, [](uint8_t v) { darkMode = v != 0; }},
    {"writeMode", "writeMode", 0, [] { return (uint8_t)writingMode; },
     [](uint8_t v) { writingMode = static_cast<WritingMode>(v); }},
    {"fontSize", "fontSize", 2, [] { return (uint8_t)fontSize; },
     [](uint8_t v) { fontSize = static_cast<FontSize>(v); }},
    {"showWC", "showWC", 1, [] { return (uint8_t)showWordCount; }, [](uint8_t v) { showWordCount = v != 0; }},
    {"newMd", "newMd", 0, [] { return (uint8_t)newNotesMd; }, [](uint8_t v) { newNotesMd = v != 0; }},
    {"spell", "spell", 0, [] { return spellLang; }, [](uint8_t v) { spellLang = v; }},
    {"acorr", "acorr", 0, [] { return (uint8_t)autocorrectOn; }, [](uint8_t v) { autocorrectOn = v != 0; }},
};
constexpr size_t kCount = sizeof(kPrefs) / sizeof(kPrefs[0]);

Preferences g_nvs;   // open from load() on
char g_layoutFile[40] = "";
int g_legacyLayout = 0;

// The values last written, to notice a change.
uint8_t g_saved[kCount];
char g_savedLayout[40] = "";

void rememberSaved() {
  for (size_t i = 0; i < kCount; i++) g_saved[i] = kPrefs[i].get();
  strncpy(g_savedLayout, keyboardLayoutFile(), sizeof(g_savedLayout) - 1);
}

void writeSdCopy() {
  static char buf[224];
  strcpy(buf, "{");
  for (const Pref& p : kPrefs) {
    char item[32];
    snprintf(item, sizeof(item), "\"%s\":%d,", p.jsonKey, (int)p.get());
    strncat(buf, item, sizeof(buf) - strlen(buf) - 1);
  }
  strncat(buf, "\"layoutFile\":\"", sizeof(buf) - strlen(buf) - 1);
  jsonAppendStr(buf, sizeof(buf), keyboardLayoutFile());
  strncat(buf, "\"}", sizeof(buf) - strlen(buf) - 1);
  if (!SdMan.exists("/microslate")) SdMan.mkdir("/microslate");
  sdWriteFile(kSdCopy, buf);
}

}  // namespace

void load() {
  g_nvs.begin("ui_prefs", false);
  for (const Pref& p : kPrefs) p.set(g_nvs.getUChar(p.nvsKey, p.def));
  // Keyboard layout: a file name in /microslate/layouts ("" = built-in US). "layout" is the index of the old
  // compiled-in tables (1 RU, 2 DE), read once to pick ru.txt / de.txt.
  strncpy(g_layoutFile, g_nvs.getString("layoutFile", "").c_str(), sizeof(g_layoutFile) - 1);
  g_legacyLayout = g_nvs.isKey("layoutFile") ? 0 : g_nvs.getUChar("layout", 0);
}

bool restoreFromSd() {
  if (g_nvs.isKey("orient")) return false;
  static char buf[224];   // the size writeSdCopy uses
  if (!sdReadFile(kSdCopy, buf, sizeof(buf))) return false;
  for (const Pref& p : kPrefs) {
    const int v = jsonGetInt(buf, p.jsonKey);
    if (v < 0) continue;
    p.set((uint8_t)v);
    g_nvs.putUChar(p.nvsKey, p.get());
  }
  char lf[40] = "";
  if (jsonGetStr(buf, "layoutFile", lf, sizeof(lf))) {
    strncpy(g_layoutFile, lf, sizeof(g_layoutFile) - 1);
    g_legacyLayout = 0;
  } else {
    const int kl = jsonGetInt(buf, "layout");
    if (kl >= 0) g_legacyLayout = kl;
  }
  return true;
}

const char* layoutFile() { return g_layoutFile; }
int legacyLayout() { return g_legacyLayout; }

void saveIfChanged() {
  static bool started = false;
  if (!started) {
    started = true;
    rememberSaved();
    // Until the layout file name has been saved once (the first boot after the move from the built-in tables)
    // the remembered name is "\x01", never a file name, so this first pass saves it.
    if (!g_nvs.isKey("layoutFile")) strcpy(g_savedLayout, "\x01");
  }
  bool changed = strcmp(keyboardLayoutFile(), g_savedLayout) != 0;
  for (size_t i = 0; i < kCount; i++) changed = changed || kPrefs[i].get() != g_saved[i];
  if (!changed) return;

  for (const Pref& p : kPrefs) g_nvs.putUChar(p.nvsKey, p.get());
  g_nvs.putString("layoutFile", keyboardLayoutFile());
  if (g_nvs.isKey("layout")) g_nvs.remove("layout");   // old built-in index, migrated
  rememberSaved();
  writeSdCopy();   // so the settings survive a flash that erases NVS
}

}  // namespace uiprefs
