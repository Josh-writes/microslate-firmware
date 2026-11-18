#include "device_cloud.h"
#include "../sd_safe.h"
#include "sync_json.h"

#include "../config.h"
#include "../text_editor.h"
#include "../diag/mem_diag.h"
#include "../wall_clock.h"

#include <Arduino.h>
#include <HTTPClient.h>
#if defined(MS_TEST_PROXY)
#include <WiFi.h>
#endif
#include <NetworkClientSecure.h>
#include <Preferences.h>
#include <SDCardManager.h>
#include <esp_heap_caps.h>
#include <esp_random.h>

#include <atomic>
#include <ctime>

extern UIState currentState;

// Root certificate bundle compiled into the core (CONFIG_MBEDTLS_CERTIFICATE_BUNDLE).
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

// The core is built with CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC, so TLS would use only the scarce internal heap. mbedTLS
// lets the application replace its allocator at run time; on boards with PSRAM take TLS memory from there.
extern "C" int mbedtls_platform_set_calloc_free(void* (*calloc_func)(size_t, size_t), void (*free_func)(void*));

namespace msync {

// Public client ID of the "MicroSlate" app registration. Public by design: there is no client secret.
const char* const kOneDriveClientId = "0301fcf9-fc07-4182-903f-7c5f079583f9";

// ---------------------------------------------------------------------------------------------------------------
// clock

uint32_t DeviceClock::millisNow() { return millis(); }

uint32_t DeviceClock::random32() { return esp_random(); }

// Only once the internet time arrived (NTP; wall_clock.h). Kept through deep sleep, where the system clock keeps
// running; a restart asks again. A plausible-looking clock is not enough: the card clock's 2030 date once passed
// that test, and the clock chip's time is not trusted for a sync either.
uint32_t DeviceClock::epochNow() {
  if (!wallclock::netTimeSeen()) return 0;
  const time_t t = time(nullptr);
  return t > 1700000000 ? (uint32_t)t : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// tokens: NVS only

static constexpr const char* kNvsNs = "ms_cloud";

DeviceTokens::DeviceTokens(const char* providerId) {
  // NVS keys are at most 15 characters: "r_" plus the provider id for everything but OneDrive.
  key_ = std::string(providerId) == "onedrive" ? "od_refresh" : std::string("r_") + providerId;
}

std::string DeviceTokens::load() {
  Preferences p;
  if (!p.begin(kNvsNs, true)) return "";
  String s = p.getString(key_.c_str(), "");
  p.end();
  return std::string(s.c_str());
}

bool DeviceTokens::save(const std::string& refresh) {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return false;
  const size_t n = p.putString(key_.c_str(), refresh.c_str());
  p.end();
  return n == refresh.size();
}

void DeviceTokens::clear() {
  Preferences p;
  if (!p.begin(kNvsNs, false)) return;
  p.remove(key_.c_str());
  p.end();
}

// ---------------------------------------------------------------------------------------------------------------
// storage: /notes on the SD card

static constexpr const char* kNotesDir = "/notes";
static constexpr const char* kTrashDir = "/notes/.trash";
static constexpr size_t kMaxStateBytes = 512 * 1024;

// Parsed JSON trees (folder listings, state.json) live in PSRAM: as hundreds of small pieces they would otherwise
// fill internal RAM for a few ms and starve Wi-Fi's internal-only allocations (the sync reboots of 2026-10-30).
// PSRAM first, internal only if PSRAM is full. heap_caps_free frees either (it is what free() calls).
#if defined(CONFIG_SPIRAM_USE_MALLOC)
static void* jsonPsramAlloc(size_t n) {
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_malloc(n, MALLOC_CAP_8BIT);
}
#endif

DeviceStorage::DeviceStorage(const char* providerId) {
#if defined(CONFIG_SPIRAM_USE_MALLOC)
  msync::jsonSetAllocator(jsonPsramAlloc, heap_caps_free);
#endif
  if (std::string(providerId) == "onedrive") {
    stateDir_ = "/microslate/sync";
  } else {
    stateDir_ = std::string("/microslate/sync/") + providerId;
  }
  statePath_ = stateDir_ + "/state.json";
}

static bool readWhole(const char* path, std::string& out, size_t maxBytes) {
  auto f = SdMan.open(path, O_RDONLY);
  if (!f || f.isDirectory()) { if (f) f.close(); return false; }
  const size_t size = f.size();
  if (size > maxBytes) { f.close(); return false; }
  out.assign(size, '\0');
  size_t got = 0;
  while (got < size) {
    const int n = f.read(&out[got], size - got);
    if (n <= 0) break;
    got += (size_t)n;
  }
  f.close();
  return got == size;
}

bool DeviceStorage::listNotes(std::vector<std::string>& names) {
  memdiag::stage("list local notes");
  auto root = SdMan.open(kNotesDir);
  if (!root || !root.isDirectory()) { if (root) root.close(); return false; }
  root.rewindDirectory();
  char name[256];
  for (auto f = root.openNextFile(); f; f = root.openNextFile()) {
    const bool dir = f.isDirectory();
    f.getName(name, sizeof(name));
    f.close();
    if (dir || name[0] == '.') continue;
    names.emplace_back(name);
  }
  root.close();
  return true;
}

bool DeviceStorage::readNote(const std::string& name, std::string& out, size_t maxBytes) {
  memdiag::stagef("read %.40s", name.c_str());
  return readWhole((std::string(kNotesDir) + "/" + name).c_str(), out, maxBytes);
}

bool DeviceStorage::noteExists(const std::string& name) {
  return SdMan.exists((std::string(kNotesDir) + "/" + name).c_str());
}

bool DeviceStorage::writeNote(const std::string& name, const std::string& data) {
  memdiag::stagef("write %.40s", name.c_str());
  return sdWriteSafely((std::string(kNotesDir) + "/" + name).c_str(), data.data(), data.size(), true);
}

bool DeviceStorage::trashNote(const std::string& name) {
  const std::string from = std::string(kNotesDir) + "/" + name;
  if (!SdMan.exists(from.c_str())) return false;
  if (!SdMan.exists(kTrashDir)) SdMan.mkdir(kTrashDir);
  const std::string to = std::string(kTrashDir) + "/" + name;
  if (SdMan.exists(to.c_str())) sdRemoveLeftover(to.c_str(), from.c_str());  // only the latest deleted copy is kept
  return SdMan.rename(from.c_str(), to.c_str());
}

bool DeviceStorage::readState(std::string& out) {
  if (SdMan.exists(statePath_.c_str())) return readWhole(statePath_.c_str(), out, kMaxStateBytes);
  // Power lost between removing the old state and renaming the new one: the .tmp was read back before the old
  // one was removed, so it is the newest good state.
  const std::string tmp = statePath_ + ".tmp";
  if (!SdMan.exists(tmp.c_str()) || !readWhole(tmp.c_str(), out, kMaxStateBytes)) return false;
  DBG_PRINTF("[sync] state.json missing: using its copy %s\n", tmp.c_str());
  return true;
}

bool DeviceStorage::writeState(const std::string& data) {
  memdiag::stage("write sync state");
  if (!SdMan.exists(stateDir_.c_str())) SdMan.mkdir(stateDir_.c_str());
  return sdWriteSafely(statePath_.c_str(), data.data(), data.size());
}

bool DeviceStorage::isOpenNote(const std::string& name) {
  // The title screen opened from the editor still holds the note's text (it saves it on Enter): still open.
  return (currentState == UIState::TEXT_EDITOR || currentState == UIState::RENAME_FILE) &&
         name == editorGetCurrentFile();
}

// ---------------------------------------------------------------------------------------------------------------
// TLS memory policy

static void* tlsCalloc(size_t n, size_t size) {
  void* p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  return p;
}
static void tlsFree(void* p) { heap_caps_free(p); }

void DeviceTransport::psramBegin() {
#if defined(CONFIG_SPIRAM_USE_MALLOC)
  mbedtls_platform_set_calloc_free(tlsCalloc, tlsFree);
  heap_caps_malloc_extmem_enable(256);
#endif
}

void DeviceTransport::psramEnd() {
#if defined(CONFIG_SPIRAM_USE_MALLOC)
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);  // back to the build default
#endif
}

// ---------------------------------------------------------------------------------------------------------------
// HTTPS worker task

namespace {

// kCancelling: cancel() came while the request ran; the worker drops the answer and goes back to kIdle. Every hand-over
// between the main task and the worker is a compare-and-swap, so a cancel can never leave the transport stuck.
enum : int { kIdle = 0, kRequested = 1, kRunning = 2, kDone = 3, kCancelling = 4 };

std::atomic<int> g_state{kIdle};
HttpRequest g_req;    // owned by the worker while state is kRequested/kRunning, by the caller otherwise
HttpResponse g_resp;  // owned by the worker until kDone
TaskHandle_t g_task = nullptr;
NetworkClientSecure* g_client = nullptr;
std::string g_host;

std::string hostOf(const std::string& url) {
  const size_t s = url.find("://");
  if (s == std::string::npos) return "";
  const size_t b = s + 3;
  const size_t e = url.find_first_of("/?", b);
  return url.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

// HTTPClient writes Content-Length only for a body longer than 0 bytes. An upload of an empty
// note (PUT or POST with no body) then has no length at all, and OneDrive refuses it with 411
// Length Required (journal.txt, 2026-10-29). Say 0 explicitly.
static void addEmptyBodyLength(HTTPClient& http, const HttpRequest& r) {
  if (r.body.empty() && (r.method == "PUT" || r.method == "POST" || r.method == "PATCH"))
    http.addHeader("Content-Length", "0");
}

#if defined(MS_TEST_PROXY)
// TEST ONLY (never defined in a normal build). Some networks block Google and Dropbox. A test build can send those
// requests, as plain HTTP on the local network, to scripts/dev-proxy.py, which forwards them over HTTPS from a
// machine that can reach them. The proxy is never used for OneDrive, and release builds do not contain this code.
bool viaTestProxy(const std::string& host) {
  auto ends = [&](const char* s) {
    const size_t n = strlen(s);
    return host.size() >= n && host.compare(host.size() - n, n, s) == 0;
  };
  return ends("googleapis.com") || ends("dropboxapi.com") || ends("dropbox.com") || ends("workers.dev");
}

HttpResponse performViaProxy(const HttpRequest& r, const std::string& host) {
  HttpResponse out;
  const std::string url = std::string("http://") + MS_TEST_PROXY + "/" + host + r.url.substr(8 + host.size());
  DBG_PRINTF("[http] test proxy: %s %s\n", r.method.c_str(), host.c_str());
  WiFiClient cl;
  cl.setTimeout(30);
  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(60000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char* keys[] = {"Location"};
  http.collectHeaders(keys, 1);
  if (!http.begin(cl, url.c_str())) { out.error = "begin failed"; return out; }
  if (!r.bearer.empty()) http.addHeader("Authorization", String("Bearer ") + r.bearer.c_str());
  if (!r.contentType.empty()) http.addHeader("Content-Type", r.contentType.c_str());
  for (const auto& h : r.headers) http.addHeader(h.first.c_str(), h.second.c_str());
  addEmptyBodyLength(http, r);
  http.addHeader("X-MS-Proxy-Key", MS_TEST_PROXY_KEY);
  const int code = http.sendRequest(r.method.c_str(), const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(r.body.data())), r.body.size());
  if (code <= 0) {
    DBG_PRINTF("[http] test proxy failed: code %d\n", code);
    out.status = 0;
    out.error = HTTPClient::errorToString(code).c_str();
  } else {
    out.status = code;
    String body = http.getString();
    out.body.assign(body.c_str(), body.length());
    out.location = http.header("Location").c_str();
  }
  http.end();
  return out;
}
#endif

HttpResponse perform(const HttpRequest& r) {
  HttpResponse out;
  const std::string host = hostOf(r.url);
  if (host.empty() || r.url.compare(0, 8, "https://") != 0) { out.error = "bad url"; return out; }
#if defined(MS_TEST_PROXY)
  if (viaTestProxy(host)) return performViaProxy(r, host);
#endif
  if (!g_client) {
    g_client = new NetworkClientSecure();
    g_client->setCACertBundle(rootca_crt_bundle_start, rootca_crt_bundle_end - rootca_crt_bundle_start);
    g_client->setTimeout(30);
  }
  if (host != g_host) { g_client->stop(); g_host = host; }  // never reuse another host's connection
  // A kept-alive connection left idle (the scan of the card's notes takes 8-15 s) went stale: the first request after
  // it hung or failed (E1001 logs, 2026-11-19). Reopen after 5 s idle; back-to-back requests still reuse it.
  static uint32_t lastUse = 0;
  if (lastUse && millis() - lastUse > 5000) g_client->stop();

  // Log the request without its query string (it can carry codes) and without headers (they carry the token).
  const uint32_t t0 = millis();
  {
    const size_t pathAt = r.url.find('/', 8);
    std::string path = pathAt == std::string::npos ? "/" : r.url.substr(pathAt);
    const size_t q = path.find('?');
    if (q != std::string::npos) path.resize(q);
    if (path.size() > 40) path = "..." + path.substr(path.size() - 37);
    memdiag::stagef("%s %s", r.method.c_str(), path.c_str());
    DBG_PRINTF("[http] %s %s%s, %u bytes out\n", r.method.c_str(), host.c_str(), path.c_str(), (unsigned)r.body.size());
  }
  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(30000);
  http.setReuse(true);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  const char* keys[] = {"Location"};
  http.collectHeaders(keys, 1);
  if (!http.begin(*g_client, r.url.c_str())) { out.error = "begin failed"; return out; }
  if (!r.bearer.empty()) http.addHeader("Authorization", String("Bearer ") + r.bearer.c_str());
  if (!r.contentType.empty()) http.addHeader("Content-Type", r.contentType.c_str());
  for (const auto& h : r.headers) http.addHeader(h.first.c_str(), h.second.c_str());
  addEmptyBodyLength(http, r);
  const int code = http.sendRequest(r.method.c_str(), const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(r.body.data())), r.body.size());
  if (code <= 0) {
    char eb[96] = "";
    g_client->lastError(eb, sizeof(eb));
    DBG_PRINTF("[http] %s failed: code %d, %s\n", host.c_str(), code, eb);
    out.status = 0;
    out.error = eb[0] ? eb : HTTPClient::errorToString(code).c_str();
    g_client->stop();
  } else {
    out.status = code;
    // 204 No Content (a OneDrive delete) and 304 have no body and usually no length: getString() would then read
    // until the kept-alive connection closes, which stalled every delete for minutes (E1001 log, 2026-11-19).
    if (code != 204 && code != 304) {
      String body = http.getString();
      out.body.assign(body.c_str(), body.length());
    }
    out.location = http.header("Location").c_str();
  }
  http.end();
  lastUse = millis();
  DBG_PRINTF("[http] -> %d, %u bytes in, %u ms%s%s\n", out.status, (unsigned)out.body.size(), (unsigned)(millis() - t0),
             out.error.empty() ? "" : ", ", out.error.c_str());
  memdiag::stage("request done");
  return out;
}

void workerTask(void*) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    int s = kRequested;
    if (!g_state.compare_exchange_strong(s, kRunning)) {   // not a request (or cancelled before it started)
      if (s == kIdle && g_client) { g_client->stop(); g_host.clear(); }  // close request
      continue;
    }
    HttpResponse r = perform(g_req);
    g_req = HttpRequest();
    g_resp = std::move(r);
    s = kRunning;
    if (!g_state.compare_exchange_strong(s, kDone)) {   // kCancelling: nobody wants the answer
      g_resp = HttpResponse();
      g_state.store(kIdle);
    }
  }
}

}  // namespace

DeviceTransport::DeviceTransport() {
  if (g_task) return;
  // 16 KB stack: the TLS handshake runs here. Core 0 on the S3 (the Arduino loop runs on core 1); unpinned on the C3.
#if CONFIG_FREERTOS_UNICORE
  xTaskCreate(workerTask, "ms_https", 16384, nullptr, 1, &g_task);
#else
  xTaskCreatePinnedToCore(workerTask, "ms_https", 16384, nullptr, 1, &g_task, 0);
#endif
}

bool DeviceTransport::begin(const HttpRequest& req) {
  if (g_state.load() != kIdle || !g_task) return false;
  g_req = req;
  g_state.store(kRequested);
  xTaskNotifyGive(g_task);
  return true;
}

bool DeviceTransport::poll(HttpResponse& out) {
  if (g_state.load() != kDone) return false;
  out = std::move(g_resp);
  g_resp = HttpResponse();
  g_state.store(kIdle);
  return true;
}

void DeviceTransport::closeConnection() {
  if (g_task && g_state.load() == kIdle) xTaskNotifyGive(g_task);  // idle + notify = close request
}

void DeviceTransport::cancel() {
  for (;;) {
    int s = g_state.load();
    if (s == kRequested) {   // not started yet: it never will (the worker's wake-up then finds kIdle)
      if (g_state.compare_exchange_strong(s, kIdle)) return;
    } else if (s == kRunning) {   // the blocking call finishes on its own; its answer is dropped
      if (g_state.compare_exchange_strong(s, kCancelling)) return;
    } else if (s == kDone) {
      g_resp = HttpResponse();
      g_state.store(kIdle);
      return;
    } else {
      return;   // kIdle, or already cancelling
    }
    // The worker moved on between the load and the swap: look again.
  }
}

}  // namespace msync
