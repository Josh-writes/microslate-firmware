#include "update_notice.h"

#include "fw_update.h"
#include "readers.h"
#include "update_rules.h"
#include "../config.h"
#include "../wifi_sync.h"
#include "../sync/cloud_service.h"
#include "../sync/device_cloud.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>

#include <atomic>
#include <cstdio>
#include <ctime>

extern UIState currentState;
extern bool screenDirty;

namespace updnotice {

bool enabled = true;

namespace {

constexpr const char* kNs = "updnote";   // NVS: last (Unix time of the last check), fail (of the last failed try),
                                          // fw (the newest MicroSlate version seen)
constexpr uint32_t kSettleMs = 3000;     // after a wake: the keyboard reconnects first
constexpr uint32_t kResultMs = 5000;     // "Up to date" / "Couldn't check" stays this long, so the check visibly ends

// Once per wake (every wake from deep sleep is a new start): Wait for the main menu, Joining a saved network,
// Checking in the task, then Done until the next wake.
enum class Step : uint8_t { Wait, Joining, Checking, Done };
Step g_step = Step::Wait;

std::atomic<bool> g_done{false};   // the task ended: the loop saves what it found and lets Wi-Fi go
bool g_loaded = false;
const char* g_outcome = nullptr;   // "Up to date" or "Couldn't check for updates", until g_outcomeEnd
uint32_t g_outcomeEnd = 0;
uint32_t g_last = 0, g_fail = 0;
char g_fw[24] = "";                // the newest MicroSlate version seen

// Handed back by the task (the loop reads it only after g_done).
struct Result {
  bool ok;
  char fw[24];
} g_result;

uint32_t nowUnix() {
  const time_t now = time(nullptr);
  return now > 0 ? (uint32_t)now : 0;
}

void load() {
  if (g_loaded) return;
  g_loaded = true;
  Preferences p;
  if (!p.begin(kNs, true)) return;
  g_last = p.getUInt("last", 0);
  g_fail = p.getUInt("fail", 0);
  snprintf(g_fw, sizeof(g_fw), "%s", p.getString("fw", "").c_str());
  p.end();
}

void save() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putUInt("last", g_last);
  p.putUInt("fail", g_fail);
  p.putString("fw", g_fw);
  p.end();
}

void checkTask(void*) {
  Result r = {};
  char ver[24] = "";
  const int fw = fwupdate::latestRelease(ver, sizeof(ver));
  if (fw == 1) snprintf(r.fw, sizeof(r.fw), "%s", ver);
  r.ok = fw >= 0;
  DBG_PRINTF("[NOTICE] check: MicroSlate %s\n", fw < 0 ? "failed" : (r.fw[0] ? r.fw : "up to date"));
  g_result = r;
  g_done.store(true);
  vTaskDelete(nullptr);
}

// Wi-Fi is up and nothing else is talking to GitHub or a cloud right now.
bool wifiFree() {
  if (WiFi.status() != WL_CONNECTED || wifiHotspotActive() || wifiAdding() || isWifiSyncActive()) return false;
  if (!cloud::networkQuiet()) return false;   // a sync is connecting or running (its Linger afterwards is fine)
  if (fwupdate::keepAwake() || readers::keepAwake()) return false;
  return currentState != UIState::FW_UPDATE && currentState != UIState::READER;
}

// Wi-Fi off again, unless a sync, the Sync screen or Transfer is using it (wifiQuickEnd leaves those alone).
void releaseWifi() {
  if (cloud::syncIdle()) wifiQuickEnd();
}

// The footer says "Checking for updates..." while the step is Joining or Checking.
void setStep(Step s) {
  g_step = s;
  if (currentState == UIState::MAIN_MENU) screenDirty = true;
}

// The check ended: its outcome shows for a few seconds (an update found shows until it is installed instead).
void showResult(const char* text) {
  g_outcome = text;
  g_outcomeEnd = millis() + kResultMs;
}

// A failed try: the next one an hour later. `shown`: Wi-Fi was up but GitHub did not answer, which the footer says;
// no saved network in range is not a failed check to the writer (nothing shows, as with no network saved).
void failed(bool shown) {
  g_fail = nowUnix();
  save();
  if (shown) showResult("Couldn't check for updates");
  setStep(Step::Done);
}

void startCheck() {
  msync::DeviceTransport::psramBegin();   // TLS memory from PSRAM, as for sync
  setStep(Step::Checking);
  // 16 KB stack: the TLS handshake runs here (as the firmware update).
  if (xTaskCreate(checkTask, "ms_notice", 16384, nullptr, 1, nullptr) != pdPASS) {
    if (cloud::networkQuiet()) msync::DeviceTransport::psramEnd();
    releaseWifi();
    setStep(Step::Done);   // the next wake tries again
  }
}

void finishCheck() {
  g_done.store(false);
  if (cloud::networkQuiet()) msync::DeviceTransport::psramEnd();
  const Result& r = g_result;
  if (r.ok) {
    g_last = nowUnix();   // the internet time has arrived by now (the downloads wait for the clock's lookup)
    g_fail = 0;
    snprintf(g_fw, sizeof(g_fw), "%s", r.fw);
    save();
    if (!r.fw[0]) showResult("Up to date");
    setStep(Step::Done);
  } else {
    failed(true);
  }
  releaseWifi();
  screenDirty = true;   // the main menu's footer, or Settings > Wi-Fi
}

// The log says why the check is waiting or ended without a check, once per reason.
void why(const char* reason) {
  static const char* last = nullptr;
  if (reason == last) return;
  last = reason;
  DBG_PRINTF("[NOTICE] %s\n", reason);
}

}  // namespace

bool busy() { return g_step == Step::Joining || g_step == Step::Checking || g_done.load(); }

void loop() {
  if (g_done.load()) finishCheck();
  if (g_outcome && (int32_t)(millis() - g_outcomeEnd) >= 0) {   // the few seconds are over: the footer clears
    g_outcome = nullptr;
    if (currentState == UIState::MAIN_MENU) screenDirty = true;
  }
  switch (g_step) {
    case Step::Wait: {
      if (!enabled || !fwupdate::supported()) return;
      if (currentState != UIState::MAIN_MENU || millis() < kSettleMs) return;
      load();
      if (!updrules::checkDue(nowUnix(), g_last, g_fail)) {
        DBG_PRINTF("[NOTICE] not due: now %lu, last check %lu, last failed try %lu\n", (unsigned long)nowUnix(),
                   (unsigned long)g_last, (unsigned long)g_fail);
        setStep(Step::Done);
        return;
      }
      if (WiFi.status() == WL_CONNECTED) {   // already up for something else: ride on it when it is free
        if (wifiFree()) startCheck();
        else why("waiting: Wi-Fi is in use");
        return;
      }
      if (wifiHotspotActive() || wifiAdding() || isWifiSyncActive() || !cloud::syncIdle()) {   // later
        why(!cloud::syncIdle() ? "waiting: a cloud sync is running" : "waiting: Transfer or Wi-Fi settings");
        return;
      }
      if (!wifiQuickBegin()) { why("no saved network: no check"); setStep(Step::Done); return; }
      DBG_PRINTF("[NOTICE] joining Wi-Fi for Check for updates\n");
      setStep(Step::Joining);
      return;
    }
    case Step::Joining: {
      if (currentState == UIState::TEXT_EDITOR) {   // the writer started typing: the next wake tries again
        DBG_PRINTF("[NOTICE] a note opened: check left for the next wake\n");
        releaseWifi();
        setStep(Step::Done);
        return;
      }
      const QuickWifi q = wifiQuickPoll();
      if (q == QuickWifi::Connecting) return;
      if (q == QuickWifi::Failed) {   // no saved network in range, or none joined: an hour's wait
        failed(false);
        releaseWifi();
        return;
      }
      if (q != QuickWifi::Connected) { setStep(Step::Done); return; }   // someone else ended the attempt
      if (wifiFree()) startCheck();
      else setStep(Step::Done);   // something else took Wi-Fi meanwhile (it switches it off); the next wake tries
      return;
    }
    default:
      return;
  }
}

const char* firmware() {
  if (!enabled) return "";
  load();
  return updrules::firmwareNewer(g_fw, fwupdate::version()) ? g_fw : "";
}

const char* footer() {
  static char text[48];
  if (busy()) return "Checking for updates...";
  const char* fw = firmware();
  if (fw[0]) snprintf(text, sizeof(text), "Update available: MicroSlate %s", fw);
  else snprintf(text, sizeof(text), "%s", g_outcome ? g_outcome : "");
  return text;
}

}  // namespace updnotice
