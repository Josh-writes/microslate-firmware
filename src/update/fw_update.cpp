#include "fw_update.h"

#include "../config.h"
#include "../wifi_sync.h"
#include "../sync/cloud_service.h"
#include "../sync/device_cloud.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

extern UIState currentState;
extern bool screenDirty;

// Root certificate bundle compiled into the core (as device_cloud.cpp).
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace fwupdate {
namespace {

#ifndef MS_UPDATE_REPO
#define MS_UPDATE_REPO "https://github.com/Josh-writes/microslate-firmware"
#endif
constexpr const char* kRepo = MS_UPDATE_REPO;

// The device part of the release file name (release.yml's matrix).
#if defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
constexpr const char* kDevice = "x4pro";
#elif defined(FREEINK_DEVICE_E1001) && FREEINK_DEVICE_E1001
constexpr const char* kDevice = "e1001";
#elif defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
constexpr const char* kDevice = "sticky";
#else
constexpr const char* kDevice = nullptr;   // X4, X3: stay on v2.1 for now
#endif


View g_view = {};
std::atomic<Phase> g_phase{Phase::Idle};
std::atomic<int> g_pct{0};
std::atomic<bool> g_taskRunning{false};
std::atomic<bool> g_cancel{false};   // the screen was left: the task's answer is dropped
char g_tag[24] = "";                 // the release tag, "v3.0.1"
uint32_t g_doneAt = 0;

void setMessage(const char* text) { snprintf(g_view.message, sizeof(g_view.message), "%s", text); }

// The task's result, unless the screen was left meanwhile.
void finish(Phase phase, const char* message = nullptr) {
  if (g_cancel.load()) return;
  if (message) setMessage(message);
  g_phase.store(phase);
}

// "3.0.1" or "v3.0.1" -> 30001-style number; 0 when it is not a version.
long versionNumber(const char* v) {
  if (*v == 'v' || *v == 'V') v++;
  int a = 0, b = 0, c = 0;
  if (sscanf(v, "%d.%d.%d", &a, &b, &c) < 2) return 0;
  return a * 1000000L + b * 1000L + c;
}

// One connection: HTTPS with the root certificates. Debug builds also take plain http://, for a local test server
// that stands in for GitHub (scripts/update-test-server.py); release builds only ever use https://.
struct Conn {
  NetworkClientSecure* tls = nullptr;
  NetworkClient* plain = nullptr;
  explicit Conn(const std::string& url) {
#ifndef RELEASE_BUILD
    if (url.compare(0, 7, "http://") == 0) {
      plain = new NetworkClient();
      plain->setTimeout(30);
      return;
    }
#endif
    tls = new NetworkClientSecure();
    tls->setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
    tls->setTimeout(30);
  }
  ~Conn() { delete tls; delete plain; }
  NetworkClient& client() { return tls ? *tls : *plain; }
};

void setupHttp(HTTPClient& http) {
  http.setConnectTimeout(15000);
  http.setTimeout(30000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  static const char* keys[] = {"Location"};
  http.collectHeaders(keys, 1);
}

// A GET whose answer is only its status and Location header (GitHub's redirects).
int getRedirect(const std::string& url, std::string& location) {
  Conn conn(url);
  HTTPClient http;
  setupHttp(http);
  int code = -1;
  if (http.begin(conn.client(), url.c_str())) {
    code = http.GET();
    if (code > 0) location = http.header("Location").c_str();
    http.end();
  }
  DBG_PRINTF("[UPDATE] GET %s -> %d\n", url.c_str(), code);
  return code;
}

std::string assetUrl() {
  return std::string(kRepo) + "/releases/download/" + g_tag + "/microslate-" + kDevice + "-" + g_tag + ".bin";
}

void checkTask(void*) {
  std::string loc;
  const int code = getRedirect(std::string(kRepo) + "/releases/latest", loc);
  const size_t at = loc.find("/releases/tag/");
  if (code <= 0) {
    finish(Phase::Failed, "Could not reach GitHub. Check the Wi-Fi and try again.");
  } else if (code != 302 || at == std::string::npos) {
    finish(Phase::UpToDate, "No release found.");
  } else {
    snprintf(g_tag, sizeof(g_tag), "%s", loc.substr(at + 14).c_str());
    snprintf(g_view.latest, sizeof(g_view.latest), "%s", g_tag[0] == 'v' ? g_tag + 1 : g_tag);
    if (versionNumber(g_tag) <= versionNumber(MS_FIRMWARE_VERSION)) {
      finish(Phase::UpToDate, "This is the newest version.");
    } else {
      // The release may have no file for this device (an X4-only release, say).
      std::string assetLoc;
      const int a = getRedirect(assetUrl(), assetLoc);
      if (a == 302) finish(Phase::Available);
      else if (a == 404) finish(Phase::UpToDate, "The newest release has no update for this device yet.");
      else finish(Phase::Failed, "Could not reach GitHub. Check the Wi-Fi and try again.");
    }
  }
  g_taskRunning.store(false);
  vTaskDelete(nullptr);
}

// Streams the release file into the other slot. Returns an error text, or nullptr when it is installed.
const char* install() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* slot = esp_ota_get_next_update_partition(nullptr);
  if (!running || !slot) return "This device has no room for an update.";
  uint8_t runningHdr[16];
  if (esp_partition_read(running, 0, runningHdr, sizeof(runningHdr)) != ESP_OK) return "Could not read this firmware.";

  std::string fileUrl;
  if (getRedirect(assetUrl(), fileUrl) != 302 || fileUrl.empty()) return "Could not reach GitHub. Try again.";

  Conn conn(fileUrl);
  HTTPClient http;
  setupHttp(http);
  const char* err = nullptr;
  esp_ota_handle_t ota = 0;
  bool otaOpen = false;
  uint8_t* buf = (uint8_t*)malloc(4096);
  size_t written = 0;
  do {
    if (!buf) { err = "Not enough memory."; break; }
    if (!http.begin(conn.client(), fileUrl.c_str())) { err = "Could not start the download."; break; }
    const int code = http.GET();
    DBG_PRINTF("[UPDATE] download -> %d, %d bytes\n", code, http.getSize());
    if (code != 200) { err = "The download failed. Try again."; break; }
    const int total = http.getSize();
    if (total <= 0 || (uint32_t)total > slot->size) { err = "The update file is not valid."; break; }
    if (esp_ota_begin(slot, OTA_WITH_SEQUENTIAL_WRITES, &ota) != ESP_OK) { err = "Could not prepare the update."; break; }
    otaOpen = true;

    NetworkClient* stream = http.getStreamPtr();
    uint32_t lastData = millis();
    size_t held = 0;   // bytes in buf not yet written: the start of the file, until its header is whole
    while (written < (size_t)total) {
      if (g_cancel.load()) { err = "Cancelled."; break; }
      const size_t want = std::min((size_t)4096, (size_t)total - written);
      const int n = stream->available() ? stream->read(buf + held, want - held) : 0;
      if (n <= 0) {
        if (!stream->connected() || millis() - lastData > 30000) { err = "The download stopped. Try again."; break; }
        delay(5);
        continue;
      }
      lastData = millis();
      size_t len = held + (size_t)n;
      if (written == 0) {
        // A read can return less than the header (a short TLS record): keep collecting.
        if (len < 16 && len < want) { held = len; continue; }
        held = 0;
        // The first bytes are the image header: magic 0xE9, and the chip at offset 12 must be this one.
        if (len < 16 || buf[0] != 0xE9 || memcmp(buf + 12, runningHdr + 12, 2) != 0) {
          err = "This update is not for this device.";
          break;
        }
      }
      if (esp_ota_write(ota, buf, len) != ESP_OK) { err = "Could not write the update."; break; }
      written += len;
      g_pct.store((int)(written * 100 / (size_t)total));
    }
    if (err) break;
    otaOpen = false;
    if (esp_ota_end(ota) != ESP_OK) { err = "The update file is damaged. Try again."; break; }   // checks the image
    if (esp_ota_set_boot_partition(slot) != ESP_OK) { err = "Could not switch to the update."; break; }
  } while (false);
  if (otaOpen) esp_ota_abort(ota);
  http.end();
  free(buf);
  DBG_PRINTF("[UPDATE] install: %u bytes, %s\n", (unsigned)written, err ? err : "done");
  return err;
}

void installTask(void*) {
  const char* err = install();
  if (err) finish(Phase::Failed, err);
  else finish(Phase::Done);
  g_taskRunning.store(false);
  vTaskDelete(nullptr);
}

bool startTask(TaskFunction_t fn, const char* name) {
  if (g_taskRunning.load()) return false;
  msync::DeviceTransport::psramBegin();   // TLS memory from PSRAM, as for sync
  g_taskRunning.store(true);
  // 16 KB stack: the TLS handshake runs here (as the sync worker).
  if (xTaskCreate(fn, name, 16384, nullptr, 1, nullptr) != pdPASS) {
    g_taskRunning.store(false);
    return false;
  }
  return true;
}

void startCheck() {
  g_cancel.store(false);
  g_phase.store(Phase::Checking);
  if (!startTask(checkTask, "ms_update")) finish(Phase::Failed, "Not enough memory.");
}

void checkWhenOnline() {
  if (WiFi.status() == WL_CONNECTED) {
    startCheck();
  } else if (wifiQuickBegin()) {
    g_phase.store(Phase::Connecting);
  } else {
    setMessage("Add your Wi-Fi network in Settings > Wi-Fi first.");
    g_phase.store(Phase::NoWifi);
  }
}

void close() {
  g_cancel.store(true);
  g_phase.store(Phase::Idle);
  wifiHold(false);
  if (!cloud::wifiInUse()) {
    wifiQuickEnd();
    msync::DeviceTransport::psramEnd();
  }
  currentState = UIState::SETTINGS;
  screenDirty = true;
}

}  // namespace

const char* version() { return MS_FIRMWARE_VERSION; }
bool supported() { return kDevice != nullptr; }

void open() {
  g_view = {};
  g_pct.store(0);
  g_cancel.store(false);
  currentState = UIState::FW_UPDATE;
  screenDirty = true;
  if (!supported()) {
    setMessage("Updates for this device come with a later release.");
    g_phase.store(Phase::UpToDate);
    return;
  }
  wifiHold(true);   // background sync's Wi-Fi off must not cut the check or the download
  if (g_taskRunning.load()) {   // an earlier check is still finishing: take its answer (loop() asks again if it was lost)
    g_phase.store(Phase::Checking);
    return;
  }
  checkWhenOnline();
}

void key(uint8_t k) {
  const Phase p = g_phase.load();
  if (p == Phase::Installing || p == Phase::Done) return;   // no way out mid-install (Back would only abandon it)
  if (k == HID_KEY_ESCAPE) {
    close();
  } else if (k == HID_KEY_ENTER && p == Phase::Available) {
    g_pct.store(0);
    g_cancel.store(false);
    g_phase.store(Phase::Installing);
    if (!startTask(installTask, "ms_update")) finish(Phase::Failed, "Not enough memory.");
    screenDirty = true;
  } else if (k == HID_KEY_ENTER && (p == Phase::Failed || p == Phase::NoWifi)) {
    open();
  }
}

void loop() {
  static Phase shown = Phase::Idle;
  static int shownPct = -1;
  const Phase p = g_phase.load();
  if (p == Phase::Idle) { shown = p; return; }

  // The screen was opened again while an earlier check was ending: its answer was dropped (finish() ran while the
  // screen was closed). With no task left to answer, ask again.
  if (p == Phase::Checking && !g_taskRunning.load()) checkWhenOnline();

  if (p == Phase::Connecting) {
    const QuickWifi q = wifiQuickPoll();
    if (q == QuickWifi::Connected) startCheck();
    else if (q == QuickWifi::Failed) {
      setMessage("None of the saved Wi-Fi networks answered.");
      g_phase.store(Phase::NoWifi);
    }
  }
  if (p == Phase::Done && g_doneAt == 0) g_doneAt = millis();
  if (p == Phase::Done && millis() - g_doneAt > 3000) {
    DBG_PRINTLN("[UPDATE] restarting into the update");
    Serial.flush();
    esp_restart();
  }

  // Redraw on a new phase, and every 5% while installing (each e-ink refresh takes a moment).
  const int pct = g_pct.load();
  if (p != shown || (p == Phase::Installing && pct / 5 != shownPct / 5)) {
    shown = p;
    shownPct = pct;
    g_view.phase = p;
    g_view.pct = pct;
    screenDirty = true;
  }
}

const View& view() {
  g_view.phase = g_phase.load();
  g_view.pct = g_pct.load();
  return g_view;
}

bool keepAwake() {
  const Phase p = g_phase.load();
  return p == Phase::Connecting || p == Phase::Checking || p == Phase::Installing || p == Phase::Done;
}

bool installing() {
  const Phase p = g_phase.load();
  return p == Phase::Installing || p == Phase::Done;
}

}  // namespace fwupdate
