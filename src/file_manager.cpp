#include "file_manager.h"
#include "sd_safe.h"
#include "note_format.h"
#include "note_ext.h"
#include "text_editor.h"
#include "editor_prof.h"
#include "sync/cloud_service.h"
#include <Arduino.h>
#include <SDCardManager.h>
#include <Preferences.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/time.h>
#include <esp_timer.h>
#if defined(CONFIG_SPIRAM)
#include <esp_heap_caps.h>
#endif

// --- File list ---
static FileInfo* fileList = nullptr;   // grows by doubling up to MAX_FILES
static int fileCap = 0;
extern bool screenDirty;
static int fileCount = 0;

// Room for one more entry; false when the list is at MAX_FILES or memory ran out.
static bool growFileList() {
  if (fileCount < fileCap) return true;
  if (fileCap >= MAX_FILES) return false;
  int cap = fileCap ? fileCap * 2 : 64;
  if (cap > MAX_FILES) cap = MAX_FILES;
#if defined(CONFIG_SPIRAM)
  void* p = heap_caps_realloc(fileList, cap * sizeof(FileInfo), MALLOC_CAP_SPIRAM);
#else
  void* p = realloc(fileList, cap * sizeof(FileInfo));
#endif
  if (!p) return false;
  fileList = static_cast<FileInfo*>(p);
  fileCap = cap;
  return true;
}

// Shared state
extern UIState currentState;

// --- Monotonic clock for file timestamps ---------------------------------
// The X4 has no RTC, so without this every file would get SdFat's identical
// default stamp and the browser couldn't tell new notes from old. We keep a
// coarse clock seeded from a persisted epoch that only ever moves forward:
// every save stamps the file with cardNow() and persists it, so after a
// reboot the clock resumes just past the last save. That makes each newly
// saved note strictly newer than everything before it — enough to sort by.
//
// This clock is the card's own: it no longer sets the system time (2026-11-02). It used to, and its 2030 date
// looked like a real one to the cloud code, which then never asked the internet for the time: uploads carried 2030
// dates and the date check before a sync did nothing. The system time is now set only from the internet (NTP).
static Preferences clockPrefs;
static uint32_t g_cardBase = 0;      // card clock at boot (seconds)
static int64_t g_cardBootUs = 0;     // esp_timer time of that reading

static time_t cardNow() {
  return (time_t)g_cardBase + (time_t)((esp_timer_get_time() - g_cardBootUs) / 1000000);
}
// Seed is a pure relative-ordering sentinel (no UI shows dates). It must sit
// ABOVE any plausible firmware build date, because legacy notes written by
// older builds carry that build's compile-date stamp (~2026) — if the seed
// were lower, those old files would outrank fresh saves and stick to the top.
static constexpr unsigned long CLOCK_DEFAULT_EPOCH = 1900000000UL;  // ~2030-03

// SdFat calls this whenever it creates or syncs a file's directory entry.
static void sdDateTimeCallback(uint16_t* date, uint16_t* timeOut) {
  time_t now = cardNow();
  struct tm t;
  localtime_r(&now, &t);
  *date = FS_DATE(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
  *timeOut = FS_TIME(t.tm_hour, t.tm_min, t.tm_sec);
}

static void clockInit() {
  clockPrefs.begin("clock", false);
  unsigned long base = clockPrefs.getULong("base", CLOCK_DEFAULT_EPOCH);
  if (base < CLOCK_DEFAULT_EPOCH) base = CLOCK_DEFAULT_EPOCH;
  g_cardBase = (uint32_t)base;
  g_cardBootUs = esp_timer_get_time();
  FsDateTime::setCallback(sdDateTimeCallback);
}

// Persist the advancing clock so timestamps stay monotonic across reboots.
static void clockPersist() {
  clockPrefs.putULong("base", (unsigned long)cardNow());
}

// Convert filename to a readable display title.
// "my_note_2.txt" -> "My Note 2"
static void filenameToTitle(const char* filename, char* out, int maxLen) {
  int j = 0;
  bool capitalizeNext = true;
  for (int i = 0; filename[i] != '\0' && filename[i] != '.' && j < maxLen - 1; i++) {
    char c = filename[i];
    if (c == '_') {
      if (j > 0) out[j++] = ' ';
      capitalizeNext = true;
    } else {
      if (capitalizeNext && c >= 'a' && c <= 'z') c -= 32;
      capitalizeNext = false;
      out[j++] = c;
    }
  }
  out[j] = '\0';
  if (j == 0) strncpy(out, "Untitled", maxLen - 1);
}

bool isNoteFileName(const char* name) { return noteExtLen(name) != 0; }

const char* noteExtOf(const char* filename) { return noteExtLen(filename) == 3 ? ".md" : ".txt"; }

// Convert a title to a valid FAT filename (lowercase, spaces->underscores,
// non-alphanumeric stripped, ext (".txt" or ".md") appended).
static void titleToFilename(const char* title, const char* ext, char* out, int maxLen) {
  int maxBase = maxLen - 1 - (int)strlen(ext); // room for the extension + null
  int j = 0;
  for (int i = 0; title[i] != '\0' && j < maxBase; i++) {
    char c = title[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else if (c == ' ' || c == '_' || c == '-') {
      if (j > 0 && out[j - 1] != '_') out[j++] = '_';
    }
  }
  while (j > 0 && out[j - 1] == '_') j--;
  if (j == 0) { strncpy(out, "note", maxLen - 1); j = 4; }
  strcpy(out + j, ext);
}

// Derive a unique /notes/ filename from a title, handling collisions with _2, _3 suffix.
void deriveUniqueFilename(const char* title, const char* ext, char* out, int maxLen) {
  titleToFilename(title, ext, out, maxLen);

  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", out);
  if (!SdMan.exists(path)) return;

  // Collision — strip the extension, try _2, _3 ...
  char base[MAX_FILENAME_LEN];
  strncpy(base, out, maxLen - 1);
  base[maxLen - 1] = '\0';
  base[strlen(base) - strlen(ext)] = '\0';

  int suffix = 2;
  while (SdMan.exists(path) && suffix <= 99) {
    snprintf(out, maxLen, "%s_%d%s", base, suffix++, ext);
    snprintf(path, sizeof(path), "/notes/%s", out);
  }
}

static bool sdMounted = false;
static bool sdGone = false;

static const char* kProbePath = "/microslate/.probe";   // 512 bytes, only ever read back

// SdFat answers exists() and small reads from its one-sector cache, so they cannot tell that the card was pulled. A
// whole-sector read of a file that is not in the cache goes to the card itself.
static bool sdSectorRead() {
  auto f = SdMan.open(kProbePath, O_RDONLY);
  if (!f) return false;
  static uint8_t sector[512];
  const int n = f.read(sector, sizeof(sector));
  f.close();
  return n == (int)sizeof(sector);
}

static void sdEnsureProbeFile() {
  if (SdMan.exists(kProbePath)) return;
  SdMan.mkdir("/microslate");
  uint8_t block[512];
  memset(block, 'x', sizeof(block));
  auto f = SdMan.open(kProbePath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return;
  f.write(block, sizeof(block));
  f.close();
}

// The sector read is only believed once it has worked on the freshly mounted card. If it never works here, fall back
// to exists() rather than declaring a good card missing (that would park the device at every boot).
static bool sdSectorTrusted = false;

static void sdCalibrate() {
  sdEnsureProbeFile();
  sdSectorTrusted = sdSectorRead();
  DBG_PRINTF("SD sector probe: %s\n", sdSectorTrusted ? "works, removal check on" : "FAILED on a mounted card, check off");
}

static bool sdCardAnswers() {
  if (!sdSectorTrusted) return SdMan.exists("/notes");
  if (sdSectorRead()) return true;
  delay(30);
  return sdSectorRead();   // one retry: a single failed read is not a pulled card
}

static void sdMarkLost(const char* why) {
  if (sdGone) return;
  sdGone = true;
  sdMounted = false;
  fileCount = 0;
  DBG_PRINTF("SD lost: %s\n", why);
  cloud::onSdLost();
  screenDirty = true;
}

static void finishSdSetup() {
  clockInit();          // seed monotonic clock + register SdFat timestamp callback

  if (!SdMan.exists("/notes")) {
    SdMan.mkdir("/notes");
  }

  recoverTempFiles();   // reconcile any .tmp left by a crash mid-save

  DBG_PRINTLN("SD Card initialized");
  refreshFileList();
}

void fileManagerSetup() {
  if (!SdMan.begin()) {
    DBG_PRINTLN("SD Card mount failed!");
    sdMarkLost("no card at boot");
    return;
  }
  sdMounted = true;
  sdCalibrate();
  finishSdSetup();
}

bool sdCardMissing() { return sdGone; }
bool sdLost() { return sdGone; }

// Called from the main loop outside the editor: notices a pulled card. The editor notices when a save fails.
void fileManagerPoll() {
  static unsigned long lastMs = 0;
  if (sdGone || millis() - lastMs < 3000) return;
  lastMs = millis();
  if (!sdCardAnswers()) sdMarkLost("card not readable");
}

bool sdProbe() {
  static unsigned long lastMs = 0;
  if (millis() - lastMs < 1000) return false;
  lastMs = millis();
  const bool ok = SdMan.begin();
  DBG_PRINTF("SD probe: %s\n", ok ? "card found" : "no card");
  return ok;
}

bool sdResume() {
  static unsigned long lastMs = 0;
  if (millis() - lastMs < 10000) return false;   // a failed mount stalls typing for a moment: not on every key
  lastMs = millis();
  if (!SdMan.begin()) { DBG_PRINTLN("SD resume: no card"); return false; }
  sdGone = false;
  sdMounted = true;
  sdCalibrate();
  if (editorHasUnsavedChanges() && !saveCurrentFile(false)) {
    sdMarkLost("save failed after mount");
    return false;
  }
  finishSdSetup();
  cloud::onSdBack();
  screenDirty = true;
  DBG_PRINTLN("SD resume: note saved, card back");
  return true;
}

void recoverTempFiles() {
  if (!SdMan.exists("/notes")) return;
  auto root = SdMan.open("/notes");
  if (!root || !root.isDirectory()) { if (root) root.close(); return; }

  // Collect .tmp names first — renaming/removing during directory iteration is unsafe.
  char tmpNames[16][MAX_FILENAME_LEN + 8];
  int n = 0;
  root.rewindDirectory();
  char name[256];
  for (auto f = root.openNextFile(); f; f = root.openNextFile()) {
    f.getName(name, sizeof(name));
    int len = (int)strlen(name);
    if (len > 4 && strcasecmp(name + len - 4, ".tmp") == 0 && n < 16) {
      strncpy(tmpNames[n], name, sizeof(tmpNames[0]) - 1);
      tmpNames[n][sizeof(tmpNames[0]) - 1] = '\0';
      n++;
    }
    f.close();
  }
  root.close();

  for (int i = 0; i < n; i++) {
    char real[MAX_FILENAME_LEN + 8];
    strncpy(real, tmpNames[i], sizeof(real) - 1);
    real[sizeof(real) - 1] = '\0';
    int rl = (int)strlen(real);
    if (rl > 4) real[rl - 4] = '\0';   // strip ".tmp"

    char tmpPath[320], realPath[320];
    snprintf(tmpPath, sizeof(tmpPath), "/notes/%s", tmpNames[i]);
    snprintf(realPath, sizeof(realPath), "/notes/%s", real);

    if (SdMan.exists(realPath)) {
      // Original intact → the .tmp is a stale partial from a crash before the swap, or the old name of a rename to
      // the original that was cut short (then both names hold the same clusters: only the name may go).
      sdRemoveLeftover(tmpPath, realPath);
      DBG_PRINTF("[recover] removed stale %s\n", tmpNames[i]);
    } else {
      // Original missing → crash between rotate and rename; the verified .tmp is the
      // newest good content. Promote it (previous version is still in .bak as backup).
      SdMan.rename(tmpPath, realPath);
      DBG_PRINTF("[recover] promoted %s -> %s\n", tmpNames[i], real);
    }
  }
}

void refreshFileList() {
  fileCount = 0;

  auto root = SdMan.open("/notes");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();
  char name[256];

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.') {
      file.close();
      continue;
    }

    if (isNoteFileName(name)) {   // .txt and .md notes
      // Pack the FAT modify date/time into a single comparable key (date is
      // the high half so it dominates the ordering).
      uint16_t d = 0, t = 0;
      file.getModifyDateTime(&d, &t);
      const unsigned long mod = ((unsigned long)d << 16) | t;

      int slot = fileCount;
      if (growFileList()) {
        fileCount++;
      } else {
        // Full: this note takes the oldest one's place if it is newer, so the list keeps the newest notes.
        slot = -1;
        for (int i = 0; i < fileCount; i++)
          if (fileList[i].modTime < mod && (slot < 0 || fileList[i].modTime < fileList[slot].modTime)) slot = i;
      }
      if (slot >= 0) {
        strncpy(fileList[slot].filename, name, MAX_FILENAME_LEN - 1);
        fileList[slot].filename[MAX_FILENAME_LEN - 1] = '\0';
        filenameToTitle(name, fileList[slot].title, MAX_TITLE_LEN);
        fileList[slot].modTime = mod;
      }
    }
    file.close();
  }
  root.close();

  // Newest first; equal stamps keep directory order.
  if (fileCount > 1)
    std::stable_sort(fileList, fileList + fileCount,
                     [](const FileInfo& a, const FileInfo& b) { return a.modTime > b.modTime; });

  DBG_PRINTF("File listing: %d files found\n", fileCount);
}

int getFileCount() { return fileCount; }
FileInfo* getFileList() { return fileList; }

static size_t g_loadRefusedBytes = 0;
size_t loadRefusedBytes() { return g_loadRefusedBytes; }
void loadRefusedClear() { g_loadRefusedBytes = 0; }

// The stored style of the open note (line endings, byte-order mark); a note it does not name saves as plain LF.
static NoteStyle g_noteStyle;
static char g_styleFile[MAX_FILENAME_LEN] = "";

static NoteStyle openNoteStyle() {
  return strcmp(editorGetCurrentFile(), g_styleFile) == 0 ? g_noteStyle : NoteStyle();
}

// Fixed buffers for opening and saving notes (internal RAM, so kept to the 2 KB the save check always used).
static uint8_t g_ioChunk[1024];   // card reads (open, read-back check) and the converted text the save writes
static uint8_t g_ioExpect[1024];  // read-back check: what the card should hold

void loadFile(const char* filename) {
  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename);

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    DBG_PRINTF("Could not open: %s\n", path);
    return;
  }

  // A note longer than the editor holds is never opened: editing it would cut it short on the next save. The limit
  // is on the text as the editor holds it, so a Windows note (CR LF) may be a little larger on the card; that case
  // is measured first, without touching the editor's buffer (it may still hold the last note).
  const size_t stored = file.size();
  if (stored >= TEXT_BUFFER_SIZE) {
    NoteDecoder count;
    size_t textLen = 0;
    for (;;) {
      const int n = file.read(g_ioChunk, sizeof(g_ioChunk));
      if (n <= 0) break;
      textLen += count.feed(g_ioChunk, (size_t)n, nullptr);
    }
    textLen += count.finish(nullptr);
    if (textLen >= TEXT_BUFFER_SIZE) {
      DBG_PRINTF("Not opening %s: %d bytes is over the %d byte editor limit\n", filename, (int)textLen, (int)(TEXT_BUFFER_SIZE - 1));
      g_loadRefusedBytes = stored;   // the notes list says so (loadRefusedBytes)
      file.close();
      return;
    }
    file.seek(0);
  }

  // Stored bytes become editor text: CR LF and a stray CR turn into LF, a byte-order mark is dropped; the save puts
  // them back (note_format.h). The text is never longer than what was read, so it decodes straight in.
  char* buf = editorGetBuffer();
  NoteDecoder dec;
  size_t bytesRead = 0;
  for (;;) {
    const size_t room = TEXT_BUFFER_SIZE - 1 - bytesRead;
    if (room == 0) break;
    const int n = file.read(g_ioChunk, room < sizeof(g_ioChunk) ? room : sizeof(g_ioChunk));
    if (n <= 0) break;
    bytesRead += dec.feed(g_ioChunk, (size_t)n, buf + bytesRead);
  }
  bytesRead += dec.finish(buf + bytesRead);
  buf[bytesRead] = '\0';
  file.close();

  editorSetCurrentFile(filename);
  g_noteStyle = dec.style();
  snprintf(g_styleFile, sizeof(g_styleFile), "%s", filename);
  editorLoadBuffer(bytesRead);

  // Title comes from the filename, not the file content
  char title[MAX_TITLE_LEN];
  filenameToTitle(filename, title, MAX_TITLE_LEN);
  editorSetCurrentTitle(title);
  editorSetUnsavedChanges(false);

  currentState = UIState::TEXT_EDITOR;
  DBG_PRINTF("Loaded: %s (%d bytes%s%s)\n", filename, (int)bytesRead, g_noteStyle.crlf ? ", CRLF" : "",
             g_noteStyle.bom ? ", BOM" : "");
}

// True when the file holds exactly the editor's first `len` bytes in the stored style (the save's read-back check).
static bool tmpMatchesEditor(const char* tmpPath, size_t len, NoteStyle style) {
  MS_PROF("save-verify");
  auto f = SdMan.open(tmpPath, O_RDONLY);
  if (!f) return false;
  const uint8_t* buf = (const uint8_t*)editorGetBuffer();
  bool same = (size_t)f.size() == NoteEncoder::storedSize((const char*)buf, len, style);
  if (!style.crlf && !style.bom) {
    for (size_t at = 0; same && at < len;) {
      const size_t want = len - at < sizeof(g_ioChunk) ? len - at : sizeof(g_ioChunk);
      const int got = f.read(g_ioChunk, want);
      same = got == (int)want && memcmp(g_ioChunk, buf + at, want) == 0;
      at += want;
    }
  } else {
    NoteEncoder enc((const char*)buf, len, style);
    while (same) {
      const size_t want = enc.next(g_ioExpect, sizeof(g_ioExpect));
      if (want == 0) break;
      const int got = f.read(g_ioChunk, want);
      same = got == (int)want && memcmp(g_ioChunk, g_ioExpect, want) == 0;
    }
  }
  f.close();
  return same;
}

bool saveCurrentFile(bool refreshList) {
  MS_PROF("save");
  const char* filename = editorGetCurrentFile();
  if (filename[0] == '\0') return false;
  if (sdGone) return false;   // card is out: the text stays in the editor until it is back

  char path[320], tmpPath[336];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

  // Step 1: Write new content to .tmp (a .tmp left by a cut-short rename shares the note's clusters: truncating it
  // would free them, so it goes first, by name only).
  sdRemoveLeftover(tmpPath, path);
  auto file = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    DBG_PRINTF("saveCurrentFile: could not create tmp: %s\n", tmpPath);
    if (!sdCardAnswers()) sdMarkLost("save: card not readable");
    return false;
  }

  // A note that came with CR LF or a byte-order mark is written back that way (note_format.h); device notes as is.
  const NoteStyle style = openNoteStyle();
  const size_t textLen = editorGetLength();
  const size_t toWrite = NoteEncoder::storedSize(editorGetBuffer(), textLen, style);
  size_t written = 0;
  if (!style.crlf && !style.bom) {
    written = file.write((const uint8_t*)editorGetBuffer(), textLen);
  } else {
    NoteEncoder enc(editorGetBuffer(), textLen, style);
    for (;;) {
      const size_t n = enc.next(g_ioChunk, sizeof(g_ioChunk));
      if (n == 0) break;
      const size_t w = file.write(g_ioChunk, n);
      written += w;
      if (w != n) break;
    }
  }
  file.close();

  // Step 2: Verify bytes written match expected length, then read the copy back: a card that takes the write but
  // keeps something else must not replace the good note.
  if (written != toWrite || !tmpMatchesEditor(tmpPath, textLen, style)) {
    DBG_PRINTF("saveCurrentFile: %s (%d/%d) — aborting\n", written != toWrite ? "write mismatch" : "read-back differs",
               (int)written, (int)toWrite);
    SdMan.remove(tmpPath);
    if (!sdCardAnswers()) sdMarkLost("save: write failed, card gone");
    return false;
  }

  // Step 3: the original becomes the .bak and the .tmp takes its place (sd_safe.h). If the original cannot move,
  // the note on the card is untouched and the text stays unsaved in the editor. If the last rename fails, the old
  // version goes back in place and the .tmp is kept (the next start promotes it if the original is missing).
  if (!sdPromoteTmp(path, tmpPath, true)) {
    DBG_PRINTF("saveCurrentFile: %s not saved\n", filename);
    if (!sdCardAnswers()) sdMarkLost("save: rename failed, card gone");
    return false;
  }

  editorSetUnsavedChanges(false);
  clockPersist();   // keep timestamps monotonic across reboots
  if (refreshList) refreshFileList();
  DBG_PRINTF("Saved: %s (%u bytes)\n", filename, (unsigned)toWrite);
  return true;
}

void createNewFile() {
  editorClear();
  editorSetCurrentFile("");       // filename derived from title when user confirms
  editorSetCurrentTitle("Untitled");
  editorSetUnsavedChanges(true);
}

// Rename a file on disk to match a new title and format (ext ".txt" or ".md"), updating editor state if needed.
void updateFileTitle(const char* filename, const char* newTitle, const char* ext) {
  char newFilename[MAX_FILENAME_LEN];
  titleToFilename(newTitle, ext, newFilename, MAX_FILENAME_LEN);
  // Same title and format: the name is already right (a unique name would add _2 against the note itself).
  if (strcasecmp(newFilename, filename) != 0) deriveUniqueFilename(newTitle, ext, newFilename, MAX_FILENAME_LEN);

  if (strcasecmp(newFilename, filename) != 0) {
    char oldPath[320], newPath[320];
    snprintf(oldPath, sizeof(oldPath), "/notes/%s", filename);
    snprintf(newPath, sizeof(newPath), "/notes/%s", newFilename);
    // A note not yet saved has no file: only the editor's name changes. Otherwise the editor follows only a rename
    // that worked (else the next save would write a second copy under the new name).
    const bool onCard = SdMan.exists(oldPath);
    const bool moved = !onCard || SdMan.rename(oldPath, newPath);
    if (!moved) {
      DBG_PRINTF("updateFileTitle: could not rename %s to %s\n", filename, newFilename);
      if (!sdCardAnswers()) sdMarkLost("rename: card gone");
    } else if (onCard) {
      // The backup copy follows the note, so no .bak is left under the old name.
      char oldBak[336], newBak[336];
      snprintf(oldBak, sizeof(oldBak), "%s.bak", oldPath);
      snprintf(newBak, sizeof(newBak), "%s.bak", newPath);
      if (SdMan.exists(oldBak)) {
        sdRemoveLeftover(newBak, newPath);
        SdMan.rename(oldBak, newBak);
      }
    }

    if (moved && strcmp(editorGetCurrentFile(), filename) == 0) {
      editorSetCurrentFile(newFilename);
    }
    // The stored style follows the note to its new name.
    if (moved && strcmp(g_styleFile, filename) == 0) snprintf(g_styleFile, sizeof(g_styleFile), "%s", newFilename);
  }

  refreshFileList();
}

void deleteFile(const char* filename) {
  char path[320], bakPath[336];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(bakPath, sizeof(bakPath), "%s.bak", path);
  sdRemoveLeftover(bakPath, path);   // first: a .bak twin of the note must not free the note's clusters twice
  SdMan.remove(path);
  refreshFileList();
  DBG_PRINTF("Deleted: %s\n", filename);
}
