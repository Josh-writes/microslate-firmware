#include "readers.h"

#include "net.h"
#include "update_rules.h"
#include "../config.h"
#include "../microslate_switch.h"
#include "../sd_safe.h"
#include "../wifi_sync.h"
#include "../sync/cloud_service.h"
#include "../sync/device_cloud.h"

#include <Arduino.h>
#include <HalDisplay.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <SDCardManager.h>
#include <WiFi.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern UIState currentState;
extern bool screenDirty;

namespace readers {
namespace {

// The device part of the readers' release file names (their release workflows use the same names).
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
constexpr const char* kDevice = "x4pro";
#elif defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC
constexpr const char* kDevice = "x4c";
#elif defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
constexpr const char* kDevice = "sticky";
#else
constexpr const char* kDevice = nullptr;   // E1001, X4, X3: no reader builds
#endif

// Where the readers' repos live. Debug builds can name a local test server instead
// (-DMS_READER_HOST=\"http://<machine>:<port>\", scripts/update-test-server.py); release builds use GitHub.
#ifndef MS_READER_HOST
#define MS_READER_HOST "https://github.com/Josh-writes"
#endif

struct Reader {
  const char* id;      // the id in the reader's marker (microslate_switch.h) and in its file name
  const char* name;
  const char* repo;    // under MS_READER_HOST
  const char* blurb;
};

constexpr Reader kReaders[] = {
    {"crosspoint", "CrossPoint", "crosspoint-microslate", "The open-source e-reader: EPUB books, fonts, Wi-Fi transfer."},
    {"crossink", "CrossInk", "crossink-microslate", "CrossPoint with finer fonts and reading stats."},
};
constexpr int kCount = static_cast<int>(sizeof(kReaders) / sizeof(kReaders[0]));
constexpr uint32_t kHeadBytes = 4096;   // the first flash sector: written last

// Saved copies (Josh, 2026-11-20): each downloaded reader is kept on the SD card as <id>-<device>.bin in kSaveDir,
// next to <id>-<device>.txt with its release tag and size. Installing it again uses the copy unless GitHub has a
// newer release; when Wi-Fi or GitHub cannot be reached the copy is used as it is. Remove asks whether to keep it.
// The card is only used from the main loop (on the Sticky it shares its bus with the screen, and sync uses it there):
// the download task writes flash only, and the copies to and from the card run a piece per loop pass.
constexpr const char* kSaveDir = "/microslate/readers";
constexpr size_t kCopyChunk = 32768;     // per loop pass: card <-> flash
constexpr uint32_t kEraseChunk = 65536;  // per loop pass: clearing the slot before a copy from the card

struct Saved {
  bool present;
  uint32_t size;
  char tag[48];
};

// A copy in progress, to or from the card (Saving, Restoring).
struct Copy {
  int stage;          // 0 start, 1 clear the slot, 2 copy, 3 finish
  FsFile file;
  uint32_t total, done, erased;
  uint8_t* head;      // Restoring: the first sector, written last
  uint8_t* buf;
};

View g_view = {};
std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<int> g_pct{0};
std::atomic<bool> g_taskRunning{false};
std::atomic<bool> g_cancel{false};   // the screen was left: the task's answer is dropped
bool g_resumePending = false;        // boot(): start the reinstall on the first loop

// What is in the other slot (read at boot and after every change; nothing else writes that slot while running).
msswitch::AppMarker g_marker = {};
bool g_hasReader = false;
char g_wanted[24] = "";   // NVS reader/id: the reader the writer chose
char g_instTag[48] = "";  // NVS reader/tag: the installed reader's release (set at every install)
Saved g_saved[kCount] = {};
char g_tag[48] = "";        // the release tag of the last download (from GitHub's redirect)
uint32_t g_total = 0;       // its size
Copy g_copy = {};
const char kUseSaved[] = "use the saved copy";   // install(): no download, the copy on the card goes in
const char kUpToDate[] = "up to date";           // install() for Check for updates: nothing newer

void setMessage(const char* text) { snprintf(g_view.message, sizeof(g_view.message), "%s", text); }

void finish(Phase phase, const char* message = nullptr) {
  if (g_cancel.load()) return;
  if (message) setMessage(message);
  g_phase.store(phase);
}

int indexOf(const char* id) {
  for (int i = 0; i < kCount; i++)
    if (strcmp(kReaders[i].id, id) == 0) return i;
  return -1;
}

void savedPath(int idx, const char* ext, char* out, size_t n) {
  snprintf(out, n, "%s/%s-%s.%s", kSaveDir, kReaders[idx].id, kDevice ? kDevice : "none", ext);
}

// Which readers have a whole copy on the card (the .txt says its size; a copy cut short does not count).
void readSaved() {
  for (int i = 0; i < kCount; i++) {
    Saved& s = g_saved[i];
    s = {};
    if (!kDevice || !SdMan.ready()) continue;
    char bin[64], info[64];
    savedPath(i, "bin", bin, sizeof(bin));
    savedPath(i, "txt", info, sizeof(info));
    if (!SdMan.exists(bin) || !SdMan.exists(info)) continue;
    char text[80];
    FsFile f = SdMan.open(info, O_RDONLY);
    if (!f) continue;
    const int n = f.read(text, sizeof(text) - 1);
    f.close();
    text[n > 0 ? n : 0] = '\0';
    char* nl = strchr(text, '\n');
    if (!nl) continue;
    *nl = '\0';
    const uint32_t size = strtoul(nl + 1, nullptr, 10);
    FsFile b = SdMan.open(bin, O_RDONLY);
    const uint32_t actual = b ? (uint32_t)b.size() : 0;
    if (b) b.close();
    if (size <= kHeadBytes || size != actual) continue;
    snprintf(s.tag, sizeof(s.tag), "%s", text);
    s.size = size;
    s.present = true;
  }
}

void deleteSaved(int idx) {
  char bin[64], tmp[72], info[64];
  savedPath(idx, "bin", bin, sizeof(bin));
  savedPath(idx, "txt", info, sizeof(info));
  snprintf(tmp, sizeof(tmp), "%s.tmp", bin);
  SdMan.remove(info);
  sdRemoveLeftover(tmp, bin);
  SdMan.remove(bin);
  readSaved();
  DBG_PRINTF("[READER] deleted the saved copy of %s\n", kReaders[idx].id);
}

// The image header's chip id (offset 12) matches this firmware's.
bool sameChip(const uint8_t* hdr) {
  static uint8_t running[16];
  static bool read = false;
  if (!read) {
    const esp_partition_t* p = esp_ota_get_running_partition();
    read = p && esp_partition_read(p, 0, running, sizeof(running)) == ESP_OK;
  }
  return read && hdr[0] == 0xE9 && memcmp(hdr + 12, running + 12, 2) == 0;
}

// The newer Sticky's SSD2677 screen needs a reader release built for it; an older one would freeze the screen, and
// the writer could not get back to MicroSlate without a computer.
bool screenNeedsNewReader() {
  const char* c = display.controllerName();
  return c && strcmp(c, "SSD2677") == 0;
}
bool fitsScreen(int idx, const char* tag) { return updrules::readerFitsScreen(kReaders[idx].id, tag, screenNeedsNewReader()); }

void refresh() {
  g_hasReader = msswitch::readMarker(msswitch::otherSlot(), g_marker) &&
                strcmp(g_marker.id, msswitch::kMicroSlateId) != 0;
  Preferences p;
  if (p.begin("reader", true)) {
    snprintf(g_wanted, sizeof(g_wanted), "%s", p.getString("id", "").c_str());
    snprintf(g_instTag, sizeof(g_instTag), "%s", p.getString("tag", "").c_str());
    p.end();
  }
  readSaved();
}

void saveInstalledTag(const char* tag) {
  snprintf(g_instTag, sizeof(g_instTag), "%s", tag);
  Preferences p;
  if (p.begin("reader", false)) {
    p.putString("tag", tag);
    p.end();
  }
}

void saveWanted(const char* id) {
  snprintf(g_wanted, sizeof(g_wanted), "%s", id);
  Preferences p;
  if (p.begin("reader", false)) {
    p.putString("id", id);
    p.end();
  }
}

void setReinstallFlag(bool on) {
  Preferences p;
  if (p.begin("reader", false)) {
    p.putBool("reinst", on);
    p.end();
  }
}

// Streams the reader's file into the other slot. Returns an error text, or nullptr when it is installed.
const char* install(int idx) {
  const Reader& r = kReaders[idx];
  const esp_partition_t* slot = msswitch::otherSlot();
  const esp_partition_t* running = esp_ota_get_running_partition();
  if (!slot || !running) return "This MicroSlate has no room for a reader.";

  std::string url = std::string(MS_READER_HOST) + "/" + r.repo + "/releases/latest/download/" + r.id + "-" + kDevice + ".bin";
  int total = -1;
  std::string tag;
  const int code = fwnet::resolve(url, total, &tag);
  // The newer Sticky's screen: the latest release must drive it (also when it is the installed one, or on the card).
  if (code == 200 && !fitsScreen(idx, tag.c_str())) {
    DBG_PRINTF("[READER] %s %s does not drive this screen (%s)\n", r.id, tag.c_str(), display.controllerName());
    return "This release does not support this Sticky's screen yet. Try again when a newer one is out.";
  }
  // Check for updates: GitHub must answer, and the same release as the installed one means nothing to do.
  if (g_view.update && code == 200 && !tag.empty() && tag == g_instTag) return kUpToDate;
  if (g_view.update && code == 404) return "There is no download for this device yet.";
  if (g_view.update && code != 200) return "Could not reach GitHub. Check the Wi-Fi and try again.";
  // The copy on the card goes in when GitHub has nothing newer, or cannot be asked.
  const Saved& saved = g_saved[idx];
  if (saved.present && (code != 200 || (tag == saved.tag && (uint32_t)total == saved.size))) {
    DBG_PRINTF("[READER] %s: the saved copy (%s); GitHub %d %s\n", r.id, saved.tag, code, tag.c_str());
    return kUseSaved;
  }
  if (code == 404) return "There is no download for this device yet.";
  if (code != 200) return "Could not reach GitHub. Check the Wi-Fi and try again.";
  if (total <= (int)kHeadBytes || (uint32_t)total > slot->size) return "The download is not valid.";
  if (g_cancel.load()) return "Cancelled.";
  snprintf(g_tag, sizeof(g_tag), "%s", tag.c_str());
  g_total = (uint32_t)total;

  // Clears the slot (the old reader goes here) for the whole image, so it can be written in any order.
  esp_ota_handle_t ota = 0;
  if (esp_ota_begin(slot, total, &ota) != ESP_OK) return "Could not prepare the space for the reader.";
  g_phase.store(Phase::Installing);
  screenDirty = true;

  fwnet::Conn conn(url);
  HTTPClient http;
  fwnet::setupHttp(http);
  const char* err = nullptr;
  bool otaOpen = true;
  bool headWritten = false;
  uint8_t* head = (uint8_t*)malloc(kHeadBytes);
  uint8_t* buf = (uint8_t*)malloc(4096);
  size_t written = 0;
  do {
    if (!head || !buf) { err = "Not enough memory."; break; }
    if (!http.begin(conn.client(), url.c_str())) { err = "Could not start the download."; break; }
    const int got = http.GET();
    DBG_PRINTF("[READER] download %s -> %d, %d bytes\n", r.id, got, http.getSize());
    if (got != 200 || http.getSize() != total) { err = "The download failed. Try again."; break; }

    NetworkClient* stream = http.getStreamPtr();
    uint32_t lastData = millis();
    while (written < (size_t)total) {
      if (g_cancel.load()) { err = "Cancelled."; break; }
      const size_t want = std::min((size_t)4096, (size_t)total - written);
      const int n = stream->available() ? stream->read(buf, want) : 0;
      if (n <= 0) {
        if (!stream->connected() || millis() - lastData > 30000) { err = "The download stopped. Try again."; break; }
        delay(5);
        continue;
      }
      lastData = millis();
      // The first sector waits in `head`; the rest goes straight to flash.
      const size_t inHead = written < kHeadBytes ? std::min((size_t)n, (size_t)kHeadBytes - written) : 0;
      if (inHead) memcpy(head + written, buf, inHead);
      // The image header: magic 0xE9, and the chip at offset 12 must be this one. A read can return less than the
      // header (a short TLS record), so it is checked in `head` once the first 16 bytes are in.
      if (written < 16 && written + n >= 16 && !sameChip(head)) {
        err = "This file is not for this MicroSlate.";
        break;
      }
      if ((size_t)n > inHead &&
          esp_ota_write_with_offset(ota, buf + inHead, n - inHead, written + inHead) != ESP_OK) {
        err = "Could not write the reader.";
        break;
      }
      written += n;
      g_pct.store((int)(written * 100 / (size_t)total));
    }
    if (err) break;
    if (esp_ota_write_with_offset(ota, head, kHeadBytes, 0) != ESP_OK) { err = "Could not write the reader."; break; }
    headWritten = true;
    otaOpen = false;
    if (esp_ota_end(ota) != ESP_OK) { err = "The download is damaged. Try again."; break; }   // checks the image
    msswitch::AppMarker m;
    if (!msswitch::readMarker(slot, m) || strcmp(m.id, r.id) != 0 || !strstr(m.devices, kDevice)) {
      err = "This file is not for this MicroSlate.";
      break;
    }
  } while (false);
  if (otaOpen) esp_ota_abort(ota);
  if (err && headWritten) esp_partition_erase_range(slot, 0, kHeadBytes);   // never leave a damaged reader that looks installed
  http.end();
  free(head);
  free(buf);
  DBG_PRINTF("[READER] install %s: %u bytes, %s\n", r.id, (unsigned)written, err ? err : "done");
  return err;
}

void installTask(void*) {
  const char* err = install(g_view.target);
  if (err == kUseSaved) finish(Phase::Restoring);
  else if (err == kUpToDate) finish(Phase::UpToDate);
  else if (err) finish(Phase::Failed, err);
  else finish(Phase::Saving);   // then Done
  g_taskRunning.store(false);
  vTaskDelete(nullptr);
}

void startDownload() {
  if (g_taskRunning.load()) return;
  g_phase.store(Phase::Preparing);
  msync::DeviceTransport::psramBegin();   // TLS memory from PSRAM, as for sync
  g_taskRunning.store(true);
  // 16 KB stack: the TLS handshake runs here (as the firmware update).
  if (xTaskCreate(installTask, "ms_reader", 16384, nullptr, 1, nullptr) != pdPASS) {
    g_taskRunning.store(false);
    finish(Phase::Failed, "Not enough memory.");
  }
}

void startInstall(int idx) {
  g_view.target = idx;
  g_pct.store(0);
  g_cancel.store(false);
  setMessage("");
  readSaved();
  wifiHold(true);   // background sync's Wi-Fi off must not cut the download
  if (WiFi.status() == WL_CONNECTED) {
    startDownload();
  } else if (wifiQuickBegin()) {
    g_phase.store(Phase::Connecting);
  } else if (g_saved[idx].present && !g_view.update) {
    g_phase.store(Phase::Restoring);   // no Wi-Fi: the copy on the card as it is
  } else {
    setMessage("Add your Wi-Fi network in Settings > Wi-Fi first.");
    g_phase.store(Phase::NoWifi);
  }
}

void releaseWifi() {
  wifiHold(false);
  if (!cloud::wifiInUse()) {
    wifiQuickEnd();
    msync::DeviceTransport::psramEnd();
  }
}

void toList() {
  g_cancel.store(true);
  releaseWifi();
  refresh();
  g_view.sel = std::min(g_view.sel, kCount - 1);
  g_phase.store(Phase::List);
}

void close(bool toMainMenu = false) {
  g_cancel.store(true);
  g_phase.store(Phase::Idle);
  releaseWifi();
  currentState = (toMainMenu || g_view.reinstall) ? UIState::MAIN_MENU : UIState::SETTINGS;
  screenDirty = true;
}

void removeReader() {
  const esp_partition_t* slot = msswitch::otherSlot();
  if (slot) esp_partition_erase_range(slot, 0, kHeadBytes);   // without its header the image is gone
  saveWanted("");
  saveInstalledTag("");
  refresh();
  DBG_PRINTLN("[READER] removed");
}

void copyEnd() {
  if (g_copy.file) g_copy.file.close();
  free(g_copy.head);
  free(g_copy.buf);
  g_copy.file = FsFile();
  g_copy.stage = 0;
  g_copy.total = g_copy.done = g_copy.erased = 0;
  g_copy.head = g_copy.buf = nullptr;
}

// One piece of saving the reader just downloaded (main loop): the slot is copied to <file>.tmp, which then replaces
// the old copy, and the .txt is written last. A failure only means no copy: the reader is installed either way.
void saveStep() {
  Copy& c = g_copy;
  const esp_partition_t* slot = msswitch::otherSlot();
  const int idx = g_view.target;
  char bin[64], tmp[72], info[64];
  savedPath(idx, "bin", bin, sizeof(bin));
  savedPath(idx, "txt", info, sizeof(info));
  snprintf(tmp, sizeof(tmp), "%s.tmp", bin);
  bool failed = false;
  if (c.stage == 0) {
    c.total = g_total;
    c.buf = (uint8_t*)malloc(kCopyChunk);
    if (!SdMan.ready() || !slot || !c.buf || c.total <= kHeadBytes || c.total > slot->size) {
      failed = true;
    } else {
      SdMan.mkdir(kSaveDir);
      SdMan.remove(info);   // first: until the new .txt is written, no copy counts as whole
      sdRemoveLeftover(tmp, bin);
      c.file = SdMan.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
      failed = !c.file;
      c.stage = 2;
    }
  } else if (c.stage == 2) {
    const size_t want = std::min(kCopyChunk, (size_t)(c.total - c.done));
    if (esp_partition_read(slot, c.done, c.buf, want) != ESP_OK || c.file.write(c.buf, want) != want) {
      failed = true;
    } else {
      c.done += want;
      if (c.done >= c.total) c.stage = 3;
    }
  } else {
    c.file.close();
    char text[80];
    snprintf(text, sizeof(text), "%s\n%u\n", g_tag, (unsigned)c.total);
    if (!sdPromoteTmp(bin, tmp, false) || !sdWriteSafely(info, text, strlen(text))) {
      failed = true;
    } else {
      DBG_PRINTF("[READER] saved %s (%s, %u bytes) on the SD card\n", kReaders[idx].id, g_tag, (unsigned)c.total);
      copyEnd();
      readSaved();
      finish(Phase::Done);
      return;
    }
  }
  if (c.total) g_pct.store((int)((uint64_t)c.done * 100 / c.total));
  if (failed) {
    DBG_PRINTF("[READER] could not save %s on the SD card\n", kReaders[idx].id);
    if (c.file) c.file.close();
    sdRemoveLeftover(tmp, bin);
    SdMan.remove(bin);   // without its .txt an older copy is no use
    copyEnd();
    readSaved();
    finish(Phase::Done);
  }
}

// One piece of installing the saved copy (main loop): clear the slot, copy the file with the first sector last, then
// check the image as esp_ota_end does (checksum and hash) and its marker before it counts as installed.
void restoreStep() {
  Copy& c = g_copy;
  const esp_partition_t* slot = msswitch::otherSlot();
  const int idx = g_view.target;
  const char* err = nullptr;
  if (c.stage == 0) {
    char bin[64];
    savedPath(idx, "bin", bin, sizeof(bin));
    c.total = g_saved[idx].size;
    snprintf(g_tag, sizeof(g_tag), "%s", g_saved[idx].tag);   // the release that is installed once this is done
    c.head = (uint8_t*)malloc(kHeadBytes);
    c.buf = (uint8_t*)malloc(kCopyChunk);
    c.file = SdMan.open(bin, O_RDONLY);
    if (!slot) err = "This MicroSlate has no room for a reader.";
    else if (!c.head || !c.buf) err = "Not enough memory.";
    else if (!g_saved[idx].present || !c.file || c.total > slot->size) err = "The copy on the SD card could not be read.";
    else if (!fitsScreen(idx, g_saved[idx].tag))
      err = "The copy on the SD card is too old for this Sticky's screen. Connect to Wi-Fi for a newer release.";
    c.stage = 1;
    DBG_PRINTF("[READER] installing %s from the SD card (%u bytes)\n", kReaders[idx].id, (unsigned)c.total);
  } else if (c.stage == 1) {
    const uint32_t end = (c.total + kHeadBytes - 1) & ~(kHeadBytes - 1);
    const uint32_t n = std::min(kEraseChunk, end - c.erased);
    if (esp_partition_erase_range(slot, c.erased, n) != ESP_OK) err = "Could not prepare the space for the reader.";
    c.erased += n;
    if (c.erased >= end) c.stage = 2;
  } else if (c.stage == 2) {
    const size_t want = std::min(kCopyChunk, (size_t)(c.total - c.done));
    const int n = c.file.read(c.buf, want);
    const size_t skip = c.done == 0 ? kHeadBytes : 0;   // the first sector waits in `head`
    if (n != (int)want) err = "The copy on the SD card could not be read.";
    else if (skip && !sameChip(c.buf)) err = "This file is not for this MicroSlate.";
    else {
      if (skip) memcpy(c.head, c.buf, kHeadBytes);
      if (esp_partition_write(slot, c.done + skip, c.buf + skip, want - skip) != ESP_OK) err = "Could not write the reader.";
      c.done += want;
      if (c.done >= c.total) c.stage = 3;
    }
  } else {
    const esp_partition_pos_t pos = {slot->address, slot->size};
    esp_image_metadata_t meta;
    msswitch::AppMarker m;
    if (esp_partition_write(slot, 0, c.head, kHeadBytes) != ESP_OK) {
      err = "Could not write the reader.";
    } else if (esp_image_verify(ESP_IMAGE_VERIFY_SILENT, &pos, &meta) != ESP_OK) {
      err = "The copy on the SD card is damaged. Try again to download it.";
      deleteSaved(idx);
    } else if (!msswitch::readMarker(slot, m) || strcmp(m.id, kReaders[idx].id) != 0 || !strstr(m.devices, kDevice)) {
      err = "This file is not for this MicroSlate.";
      deleteSaved(idx);
    }
    if (err) {
      esp_partition_erase_range(slot, 0, kHeadBytes);   // never leave a damaged reader that looks installed
    } else {
      DBG_PRINTF("[READER] installed %s from the SD card\n", kReaders[idx].id);
      copyEnd();
      finish(Phase::Done);
      return;
    }
  }
  if (c.total) {
    const uint32_t end = (c.total + kHeadBytes - 1) & ~(kHeadBytes - 1);
    g_pct.store(c.stage <= 1 ? (int)((uint64_t)c.erased * 40 / end) : 40 + (int)((uint64_t)c.done * 60 / c.total));
  }
  if (err) {
    DBG_PRINTF("[READER] install from the SD card: %s\n", err);
    copyEnd();
    finish(Phase::Failed, err);
  }
}

}  // namespace

int count() { return kCount; }
const char* name(int i) { return (i >= 0 && i < kCount) ? kReaders[i].name : ""; }
const char* blurb(int i) { return (i >= 0 && i < kCount) ? kReaders[i].blurb : ""; }

bool supported() { return kDevice != nullptr; }
bool saved(int i) { return i >= 0 && i < kCount && g_saved[i].present; }
uint32_t savedBytes(int i) { return saved(i) ? g_saved[i].size : 0; }
const char* installedTag() { return g_hasReader ? g_instTag : ""; }
const char* installed() { return g_hasReader ? g_marker.name : nullptr; }
int installedIndex() { return g_hasReader ? indexOf(g_marker.id) : -1; }
bool reinstallNeeded() { return supported() && indexOf(g_wanted) >= 0 && !(g_hasReader && strcmp(g_marker.id, g_wanted) == 0); }

void openInstalled() {
  if (!g_hasReader) return;
  // Installed before the check existed (3.0.4) and too old for the newer Sticky's screen: it would freeze there, so
  // the Reader screen says so and offers its update instead. A reader from elsewhere opens (it cannot be judged).
  const int idx = installedIndex();
  if (idx >= 0 && !fitsScreen(idx, g_instTag)) {
    DBG_PRINTF("[READER] %s %s does not drive this screen: not opened\n", g_marker.id, g_instTag);
    open();
    g_view.sel = g_view.target = idx;
    g_phase.store(Phase::TooOld);
    return;
  }
  DBG_PRINTF("[READER] opening %s\n", g_marker.name);
  Serial.flush();
  msswitch::openSlot(msswitch::otherSlot());
}

void settingValue(char* out, size_t n) {
  if (reinstallNeeded()) snprintf(out, n, "Reinstall %s", kReaders[indexOf(g_wanted)].name);
  else if (g_hasReader) snprintf(out, n, "%s", g_marker.name);
  else snprintf(out, n, "None");
}

void open() {
  currentState = UIState::READER;
  screenDirty = true;
  if (g_taskRunning.load()) return;   // a download is still running (the screen was left): show it
  g_view = {};
  g_view.target = -1;
  refresh();
  const int wanted = indexOf(g_wanted);
  if (reinstallNeeded()) g_view.sel = wanted;            // the reader to reinstall
  else if (installedIndex() >= 0) g_view.sel = installedIndex();
  g_cancel.store(false);
  g_phase.store(Phase::List);
}

void boot() {
  refresh();
  bool resume = false;
  Preferences p;
  if (p.begin("reader", true)) {
    resume = p.getBool("reinst", false);
    p.end();
  }
  if (!resume) return;
  setReinstallFlag(false);   // once: a failed reinstall waits for the writer (Settings > Wi-Fi > Reader)
  if (reinstallNeeded()) g_resumePending = true;
}

// Runs when the writer taps Install on a MicroSlate update, before anything is written: the download goes into the
// reader's slot, so afterwards the slot can no longer say which reader it held. The new version (or, after a failed
// update, the next start) downloads the chosen reader again.
void beforeMicroSlateUpdate() {
  refresh();
  if (g_hasReader && indexOf(g_marker.id) >= 0) saveWanted(g_marker.id);
  if (indexOf(g_wanted) >= 0) setReinstallFlag(true);
}

void key(uint8_t k) {
  const Phase p = g_phase.load();
  switch (p) {
    case Phase::List:
      // The readers are cards side by side: Left / Right (and Up / Down) move between them. A tap on a card sends
      // its number key (1, 2) and asks at once; Remove is the bar's last button (Delete).
      if ((k == HID_KEY_UP || k == HID_KEY_LEFT) && g_view.sel > 0) g_view.sel--;
      else if ((k == HID_KEY_DOWN || k == HID_KEY_RIGHT) && g_view.sel < kCount - 1) g_view.sel++;
      else if (k == HID_KEY_ESCAPE) close();
      else if (k >= HID_KEY_1 && k < HID_KEY_1 + kCount) {
        g_view.sel = g_view.target = k - HID_KEY_1;
        g_phase.store(Phase::Confirm);
      } else if (k == HID_KEY_ENTER) {
        g_view.target = g_view.sel;
        g_phase.store(Phase::Confirm);
      } else if ((k == HID_KEY_DELETE || k == HID_KEY_BACKSPACE) && g_hasReader) {
        g_view.target = -1;
        g_phase.store(Phase::Confirm);
      }
      break;
    case Phase::Confirm:
      if (k == HID_KEY_ESCAPE) g_phase.store(Phase::List);
      else if (k == HID_KEY_ENTER && g_view.target < 0) {
        const int was = installedIndex();
        removeReader();
        if (saved(was)) {   // ask about the copy on the card (kept unless the writer deletes it)
          g_view.target = was;
          g_phase.store(Phase::KeepCopy);
        } else {
          toList();
        }
      }
      else if (k == HID_KEY_ENTER) {
        g_view.update = g_view.target == installedIndex();   // the installed reader: Check for updates
        startInstall(g_view.target);
      }
      break;
    case Phase::KeepCopy:
      if (k == HID_KEY_ENTER) deleteSaved(g_view.target);
      if (k == HID_KEY_ESCAPE || k == HID_KEY_ENTER) toList();
      break;
    case Phase::TooOld:   // from the main menu: Back returns there, Update checks GitHub for a release that fits
      if (k == HID_KEY_ESCAPE) close(true);
      else if (k == HID_KEY_ENTER) {
        g_view.update = true;
        startInstall(g_view.target);
      }
      break;
    case Phase::NoWifi:
    case Phase::Failed:
      if (k == HID_KEY_ESCAPE) toList();
      else if (k == HID_KEY_ENTER) startInstall(g_view.target);
      break;
    case Phase::Connecting:
      if (k == HID_KEY_ESCAPE) toList();
      break;
    case Phase::Done:       // a moment's notice: any key goes on to the main menu at once
    case Phase::UpToDate:
      close(true);
      break;
    default:   // Preparing, Installing: no way out mid-install (Back would only abandon it)
      break;
  }
  screenDirty = true;
}

void loop() {
  if (g_resumePending) {   // after a MicroSlate update: bring the reader back
    g_resumePending = false;
    g_view = {};
    g_view.reinstall = true;
    g_view.sel = indexOf(g_wanted);
    currentState = UIState::READER;
    startInstall(indexOf(g_wanted));
    screenDirty = true;
  }

  static Phase shown = Phase::Idle;
  static int shownPct = -1;
  const Phase p = g_phase.load();
  if (p == Phase::Idle) { shown = p; return; }

  if (p == Phase::Connecting) {
    const QuickWifi q = wifiQuickPoll();
    if (q == QuickWifi::Connected) startDownload();
    else if (q == QuickWifi::Failed && saved(g_view.target) && !g_view.update) g_phase.store(Phase::Restoring);
    else if (q == QuickWifi::Failed) {
      setMessage("None of the saved Wi-Fi networks answered.");
      g_phase.store(Phase::NoWifi);
    }
  }
  if (p == Phase::Saving) saveStep();
  else if (p == Phase::Restoring) restoreStep();
  // Installed, updated or up to date: the notice stays a moment, then the main menu (the reader is there, one press
  // away; Josh, 2026-11-20).
  constexpr uint32_t kNoticeMs = 2500;
  static uint32_t noticeAt = 0;
  if ((p == Phase::Done || p == Phase::UpToDate) && p != shown) noticeAt = millis();
  if (p == Phase::UpToDate && shown != Phase::UpToDate) releaseWifi();
  if ((p == Phase::Done || p == Phase::UpToDate) && p == shown && millis() - noticeAt > kNoticeMs) {
    close(true);
    shown = Phase::Idle;
    return;
  }
  if (p == Phase::Done && shown != Phase::Done) {
    saveWanted(kReaders[g_view.target].id);
    saveInstalledTag(g_tag);
    releaseWifi();
    refresh();
    // After a MicroSlate update the reader coming back is the update's last step: straight to the main menu.
    if (g_view.reinstall) {
      close();
      shown = Phase::Idle;
      return;
    }
  }

  // Redraw on a new phase, and every 5% while installing (each e-ink refresh takes a moment).
  const int pct = g_pct.load();
  const bool progress = p == Phase::Installing || p == Phase::Saving || p == Phase::Restoring;
  if (p != shown || (progress && pct / 5 != shownPct / 5)) {
    shown = p;
    shownPct = pct;
    screenDirty = true;
  }
}

const View& view() {
  g_view.phase = g_phase.load();
  g_view.pct = g_pct.load();
  return g_view;
}

uint8_t barMask() {
  switch (g_phase.load()) {
    case Phase::List: return g_hasReader ? 0x9 : 0x1;   // Back (and Remove)
    case Phase::Connecting: return 0x1;   // Back
    case Phase::Confirm:                  // Cancel / Install (Remove)
    case Phase::KeepCopy:                 // Keep / Delete
    case Phase::NoWifi:                   // Back / Try again
    case Phase::TooOld:                   // Back / Update
    case Phase::Failed: return 0x3;       // Back / Try again
    default: return 0;
  }
}

bool keepAwake() {
  const Phase p = g_phase.load();
  return p == Phase::Connecting || p == Phase::Preparing || p == Phase::Installing || p == Phase::Saving ||
         p == Phase::Restoring;
}

}  // namespace readers
