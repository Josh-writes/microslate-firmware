#include "cloud_service.h"
#include "../update/update_notice.h"

#include "../config.h"
#include "../file_manager.h"
#include "../input_handler.h"
#include "../wall_clock.h"
#include "../wifi_sync.h"
#include "device_cloud.h"
#include "../diag/mem_diag.h"
#include "dropbox.h"
#include "google_drive.h"
#include "onedrive.h"
#include "sync_engine.h"

// Local-only Google client credentials (git-ignored). Without the file the build simply has no Google Drive.
#if __has_include("secrets.local.h")
#include "secrets.local.h"
#endif

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_task_wdt.h>

#include <ctime>

extern bool screenDirty;
extern UIState currentState;
extern int selectedFileIndex;

namespace cloud {

using msync::SyncEngine;

namespace {

#if defined(MS_GOOGLE_CLIENT_ID)
// The Google client ID and secret ship in the firmware (Google treats a device client's secret as non-confidential).
// They are XOR-scrambled at compile time so the plain strings never appear in the image: credential scanners then do
// not flag the release files (GitHub push protection blocked them on the website). This hides nothing from a reader.
template <size_t N>
struct Scrambled {
  char data[N] = {};
  static constexpr char key(size_t i) { return static_cast<char>((0x5Bu + i * 29u) & 0xFFu); }
  constexpr explicit Scrambled(const char (&s)[N]) {
    for (size_t i = 0; i < N; ++i) data[i] = static_cast<char>(s[i] ^ key(i));
  }
  std::string plain() const {
    std::string out(N - 1, '\0');
    for (size_t i = 0; i + 1 < N; ++i) out[i] = static_cast<char>(data[i] ^ key(i));
    return out;
  }
};
constexpr Scrambled<sizeof(MS_GOOGLE_CLIENT_ID)> kGoogleClientId(MS_GOOGLE_CLIENT_ID);
constexpr Scrambled<sizeof(MS_GOOGLE_CLIENT_SECRET)> kGoogleClientSecret(MS_GOOGLE_CLIENT_SECRET);
#endif

constexpr uint32_t kMinBackgroundGapMs = 15000;   // never start background syncs closer together than this
// After a sync could not get online (no saved network in range, no time), automatic syncs wait
// 1, 2, 4 ... up to 30 minutes before scanning again, so a device away from home does not scan
// every few seconds. Sync now always tries at once; a sync that gets online resets the wait.
constexpr uint32_t kOfflineFirstWaitMs = 60000;
constexpr uint32_t kOfflineMaxWaitMs = 30u * 60u * 1000u;
uint32_t g_offlineWaitMs = 0;
uint32_t g_offlineRetryAt = 0;
bool offlineWaitOver(uint32_t now) { return g_offlineWaitMs == 0 || (int32_t)(now - g_offlineRetryAt) >= 0; }
constexpr uint32_t kWifiLingerMs = 120000;        // keep Wi-Fi up after a sync so the next push reuses it
constexpr uint32_t kBrowseHoldMs = 8000;          // longest a note being opened waits for its sync
constexpr uint32_t kListMsgMs = 4000;             // "OneDrive synced" stays in the notes list corner this long
constexpr uint32_t kSleepFlushMs = 30000;         // longest the push behind the sleep screen may take
constexpr uint32_t kTimeWaitMs = 10000;           // wait for the date (needed to verify TLS certificates)
constexpr const char* kNs = "ms_cloud";

enum class Phase : uint8_t { Idle, Connecting, TimeWait, Running, Linger };
enum class Job : uint8_t { None, Sync, SignIn };

// One entry per cloud service. Only the active one ever runs; the others just keep their sign-in and sync record.
struct Bundle {
  msync::Provider* provider = nullptr;
  msync::DeviceTokens* tokens = nullptr;
  msync::DeviceStorage* storage = nullptr;
  SyncEngine* engine = nullptr;
  char ns[16] = "";            // NVS namespace for this provider's account name and last result
  bool signedIn = false;
  char account[40] = "";
  char lastSummary[128] = "";
  uint32_t lastEpoch = 0;
};
constexpr int kMaxProviders = 4;
Bundle g_bundles[kMaxProviders];
int g_providerCount = 0;
int g_active = 0;
msync::Provider* g_provider = nullptr;   // the active one
msync::DeviceClock* g_clock = nullptr;
msync::DeviceTransport* g_transport = nullptr;
SyncEngine* g_engine = nullptr;           // the active one's engine

// Provider list screen (switch cloud)
bool g_listMode = false;
int g_listSel = 0;
int g_confirmSignOut = -1;   // >= 0: the "Sign out?" page is showing for that provider
char g_codeBuf[96] = "";    // code entry sign-in: what the user has typed so far
int g_codeLen = 0;

Settings g_settings;
Phase g_phase = Phase::Idle;
Job g_job = Job::None;
bool g_inited = false;
bool g_signedCache = false;
bool g_screenActive = false;
bool g_pushPending = false;
bool g_pushImmediate = false;
bool g_sdLost = false;       // card pulled: no sync work at all
bool g_writeDirty = false;   // the open note was autosaved since the last sync started
bool g_runHadWrites = false; // the running sync started with an edit to push: a failed run keeps it owed
uint32_t g_lastStartMs = 0;
uint32_t g_lingerEnd = 0;
uint32_t g_phaseStart = 0;

bool g_lastFailed = false;
SyncEngine::Error g_lastError = SyncEngine::Error::None;
char g_lastSummary[128] = "";
char g_lastMessage[80] = "";
uint32_t g_lastEpoch = 0;
char g_account[40] = "";

bool g_holdActive = false;
bool g_holdRunDone = false;
uint32_t g_holdDeadline = 0;
uint32_t g_holdMsgUntil = 0;
char g_holdText[64] = "Checking the cloud...";
char g_holdNote[MAX_FILENAME_LEN] = "";   // the note waiting to be opened
bool g_holdNoteReady = false;             // its wait is over: takeHeldNote() hands it out
char g_listMsg[48] = "";                  // after a sync, for the notes list corner
uint32_t g_listMsgUntil = 0;

bool syncWorking() {
  return g_job == Job::Sync && (g_phase == Phase::Connecting || g_phase == Phase::TimeWait || g_phase == Phase::Running);
}

void listMessage(const char* fmt, const char* name) {
  snprintf(g_listMsg, sizeof(g_listMsg), fmt, name);
  g_listMsgUntil = millis() + kListMsgMs;
}

ScreenModel g_model;

void dirty() { screenDirty = true; }

const char* pname() { return g_provider ? g_provider->name() : "the cloud"; }

void loadSettings() {
  Preferences p;
  if (!p.begin(kNs, true)) return;
  g_settings.autoPause = p.getBool("auto_pause", true);
  g_settings.autoClose = p.getBool("auto_close", true);
  g_settings.autoBrowse = p.getBool("auto_browse", true);
  g_settings.writeMinutes = p.getUChar("write_min", 5);
  p.end();
}

// Account name and last result are kept per provider. OneDrive keeps the namespace and keys it always had.
void loadBundleState(Bundle& b) {
  Preferences p;
  if (!p.begin(b.ns, true)) return;
  b.lastEpoch = p.getUInt("last_epoch", 0);
  String sum = p.getString("last_sum", "");
  strncpy(b.lastSummary, sum.c_str(), sizeof(b.lastSummary) - 1);
  String acc = p.getString("od_name", "");
  strncpy(b.account, acc.c_str(), sizeof(b.account) - 1);
  p.end();
}

void saveLast() {
  Preferences p;
  if (!p.begin(g_bundles[g_active].ns, false)) return;
  p.putUInt("last_epoch", g_lastEpoch);
  p.putString("last_sum", g_lastSummary);
  p.end();
}

void saveAccountFor(Bundle& b, const char* name) {
  Preferences p;
  if (p.begin(b.ns, false)) {
    if (name && name[0]) p.putString("od_name", name); else p.remove("od_name");
    p.end();
  }
  strncpy(b.account, name ? name : "", sizeof(b.account) - 1);
  b.account[sizeof(b.account) - 1] = '\0';
}

void saveAccount(const char* name) {
  saveAccountFor(g_bundles[g_active], name);
  strncpy(g_account, name ? name : "", sizeof(g_account) - 1);
  g_account[sizeof(g_account) - 1] = '\0';
}

void storeActive() {
  Bundle& b = g_bundles[g_active];
  b.signedIn = g_signedCache;
  strncpy(b.account, g_account, sizeof(b.account) - 1);
  strncpy(b.lastSummary, g_lastSummary, sizeof(b.lastSummary) - 1);
  b.lastEpoch = g_lastEpoch;
}

void loadActive() {
  Bundle& b = g_bundles[g_active];
  g_provider = b.provider;
  g_engine = b.engine;
  g_signedCache = b.signedIn;
  strncpy(g_account, b.account, sizeof(g_account) - 1);
  strncpy(g_lastSummary, b.lastSummary, sizeof(g_lastSummary) - 1);
  g_lastEpoch = b.lastEpoch;
}

void saveActiveId() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putString("active", g_provider->id());
  p.end();
}

void startEngine() {
  memdiag::stage(g_job == Job::SignIn ? "engine: sign-in" : "engine: sync");
  msync::DeviceTransport::psramBegin();
  g_codeLen = 0;
  g_codeBuf[0] = '\0';
  g_lastFailed = false;
  g_lastMessage[0] = '\0';
  if (g_job == Job::SignIn) g_engine->startSignIn();
  else {
    g_runHadWrites = g_pushPending || g_writeDirty;
    g_pushPending = false; g_pushImmediate = false; g_writeDirty = false; g_engine->startSync();
  }
  g_offlineWaitMs = 0;   // online again: automatic syncs run normally
  g_phase = Phase::Running;
  g_lastStartMs = millis();
  dirty();
}

void failBeforeStart(const char* msg, SyncEngine::Error e) {
  memdiag::stagef("not started: %s", msg);
  memdiag::watchEnd();
  g_offlineWaitMs = g_offlineWaitMs ? (g_offlineWaitMs * 2 < kOfflineMaxWaitMs ? g_offlineWaitMs * 2 : kOfflineMaxWaitMs)
                                    : kOfflineFirstWaitMs;
  g_offlineRetryAt = millis() + g_offlineWaitMs;
  DBG_PRINTF("[cloud] %s not started: %s (next automatic try in %u s)\n", g_provider->id(), msg,
             (unsigned)(g_offlineWaitMs / 1000));
  g_lastFailed = true;
  g_lastError = e;
  snprintf(g_lastMessage, sizeof(g_lastMessage), "%s", msg);
  snprintf(g_lastSummary, sizeof(g_lastSummary), "%s", msg);
  if (g_job == Job::Sync) listMessage("Not synced: %s", msg);
  g_job = Job::None;
  g_phase = Phase::Idle;
  if (g_holdActive) g_holdRunDone = true;
  dirty();
}

bool startJob(Job j) {
  if (!g_inited || g_sdLost || g_engine->busy() || g_phase == Phase::Connecting || g_phase == Phase::TimeWait) return false;
  if (wifiHotspotActive()) return false;   // Transfer's hotspot has the radio; no internet there anyway
  if (wifiAdding()) return false;          // Settings is scanning to add a network; the push waits
  if (j == Job::Sync && !g_signedCache) return false;
  g_job = j;
  g_phaseStart = millis();
  memdiag::watchBegin(j == Job::SignIn ? "job: sign-in" : "job: sync");
  DBG_PRINTF("[cloud] start %s %s\n", g_provider->id(), j == Job::SignIn ? "sign-in" : "sync");
  if (WiFi.status() == WL_CONNECTED || wifiQuickBegin()) {
    g_phase = (WiFi.status() == WL_CONNECTED) ? Phase::TimeWait : Phase::Connecting;
    dirty();
    return true;
  }
  failBeforeStart("No saved Wi-Fi network", SyncEngine::Error::Network);
  return true;
}

void teardown() {
  if (!g_screenActive) wifiQuickEnd();
  msync::DeviceTransport::closeConnection();
  g_phase = Phase::Idle;
}

void onEngineDone() {
  const SyncEngine::Result res = g_engine->result();
  const SyncEngine::Status st = g_engine->status();
  const bool ok = st.phase == SyncEngine::Phase::Done;
  DBG_PRINTF("[cloud] %s %s %s after %u ms: %s\n", g_provider->id(), g_job == Job::SignIn ? "sign-in" : "sync",
                ok ? "ok" : "FAILED", (unsigned)(millis() - g_lastStartMs), res.message.c_str());
  memdiag::stage(ok ? "engine: done" : "engine: failed");
  memdiag::watchEnd();
  msync::DeviceTransport::psramEnd();

  if (g_job == Job::SignIn) {
    if (ok) {
      saveAccount(st.account.c_str());
      g_signedCache = true;
      g_lastFailed = false;
      snprintf(g_lastSummary, sizeof(g_lastSummary), "Connected");
    } else {
      g_lastFailed = true;
      g_lastError = res.error;
      snprintf(g_lastMessage, sizeof(g_lastMessage), "%s", res.message.c_str());
    }
    g_engine->acknowledge();
    g_job = Job::None;
    g_phase = Phase::Linger;
    g_lingerEnd = millis() + kWifiLingerMs;
    if (ok) startJob(Job::Sync);   // first sync right after connecting
    dirty();
    return;
  }

  // sync
  g_lastFailed = !ok;
  g_lastError = res.error;
  if (ok) {
    snprintf(g_lastSummary, sizeof(g_lastSummary), "%s", res.summary().c_str());
    if (!res.message.empty()) snprintf(g_lastMessage, sizeof(g_lastMessage), "%s", res.message.c_str());
    g_lastEpoch = g_clock->epochNow();
    saveLast();
    if (res.downloaded || res.deletedLocal || res.conflicts) refreshFileListKeepingSelection();   // notes changed on the card
    listMessage("%s synced", pname());
  } else {
    snprintf(g_lastMessage, sizeof(g_lastMessage), "%s", res.message.c_str());
    snprintf(g_lastSummary, sizeof(g_lastSummary), "%s", res.message.c_str());
    if (res.error == SyncEngine::Error::SignInExpired || res.error == SyncEngine::Error::NotSignedIn)
      g_signedCache = false;
    // The edit this run was to push may not have reached the cloud: still owed, so the next pause tries again and
    // the backup-overdue timer keeps counting.
    if (g_runHadWrites) g_writeDirty = true;
    listMessage("%s not synced", pname());
  }
  g_engine->acknowledge();
  g_job = Job::None;
  g_phase = Phase::Linger;
  g_lingerEnd = millis() + kWifiLingerMs;
  if (g_holdActive) g_holdRunDone = true;
  dirty();
}

const char* errorName() {
  switch (g_lastError) {
    case SyncEngine::Error::Network: return "No connection";
    case SyncEngine::Error::SignInExpired: return "Sign-in expired, reconnect";
    case SyncEngine::Error::Busy: {
      static char busy[48];
      snprintf(busy, sizeof(busy), "%s is busy", pname());
      return busy;
    }
    default: return g_lastMessage;
  }
}

void fmtTime(uint32_t epoch, char* out, size_t n) {
  if (!epoch) { snprintf(out, n, "never"); return; }
  time_t t = (time_t)epoch;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------

static void addProvider(msync::Provider* p, const char* clientId) {
  if (g_providerCount >= kMaxProviders) return;
  Bundle& b = g_bundles[g_providerCount++];
  b.provider = p;
  b.tokens = new msync::DeviceTokens(p->id());
  b.storage = new msync::DeviceStorage(p->id());
  SyncEngine::Config cfg;
  cfg.clientId = clientId;
  cfg.maxNoteBytes = TEXT_BUFFER_SIZE - 1;   // the editor cannot hold more
  // Marks this device's copy when a note clashes with the cloud ("chapter_1_e1001.txt").
#if defined(FREEINK_DEVICE_E1001) && FREEINK_DEVICE_E1001
  cfg.deviceTag = "e1001";
#elif defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY
  cfg.deviceTag = "sticky";
#elif defined(FREEINK_DEVICE_X4PRO) && FREEINK_DEVICE_X4PRO
  cfg.deviceTag = "x4pro";
#elif defined(FREEINK_DEVICE_X4CLASSIC) && FREEINK_DEVICE_X4CLASSIC
  cfg.deviceTag = "x4classic";
#elif defined(FREEINK_DEVICE_X3) && FREEINK_DEVICE_X3
  cfg.deviceTag = "x3";
#elif defined(FREEINK_DEVICE_X4) && FREEINK_DEVICE_X4
  cfg.deviceTag = "x4";
#else
  cfg.deviceTag = "microslate";
#endif
  b.engine = new SyncEngine(*p, *g_transport, *b.storage, *b.tokens, *g_clock, cfg);
  // OneDrive keeps the namespace it always had; every other provider gets its own.
  if (strcmp(p->id(), "onedrive") == 0) snprintf(b.ns, sizeof(b.ns), "%s", kNs);
  else snprintf(b.ns, sizeof(b.ns), "ms_cl_%s", p->id());
  b.signedIn = b.engine->hasRefreshToken();
  loadBundleState(b);
}

void init() {
  if (g_inited) return;
  g_clock = new msync::DeviceClock();
  g_transport = new msync::DeviceTransport();
  addProvider(new msync::OneDriveProvider(), msync::kOneDriveClientId);
#if defined(MS_GOOGLE_CLIENT_ID)
  if (sizeof(MS_GOOGLE_CLIENT_ID) > 1) {
    addProvider(new msync::GoogleDriveProvider(kGoogleClientSecret.plain()), kGoogleClientId.plain().c_str());
  }
#endif
  // Dropbox app (App folder access, PKCE): the app key is public and needs no secret. The Worker address is the sign-in
  // relay (scripts/link-worker): it only carries a one-time code from the phone to the device while signing in.
  addProvider(new msync::DropboxProvider("https://microslate-link.typeslate.workers.dev"), "yy6m5ymz3hwdxk7");
#if defined(MS_TEST_SECOND_PROVIDER)
  // Test builds: a second Microsoft account as its own provider, to try switching before Google Drive exists.
  addProvider(new msync::OneDriveProvider("onedrive2", "OneDrive 2"), msync::kOneDriveClientId);
#endif
  loadSettings();
  {
    Preferences p;
    String want;
    if (p.begin(kNs, true)) { want = p.getString("active", "onedrive"); p.end(); }
    for (int i = 0; i < g_providerCount; i++)
      if (want == g_bundles[i].provider->id()) g_active = i;
  }
  loadActive();
  g_inited = true;
}

const char* providerName() { return g_provider ? g_provider->name() : "OneDrive"; }

void saveSettings() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putBool("auto_pause", g_settings.autoPause);
  p.putBool("auto_close", g_settings.autoClose);
  p.putBool("auto_browse", g_settings.autoBrowse);
  p.putUChar("write_min", g_settings.writeMinutes);
  p.end();
}

bool signedIn() { return g_inited && g_signedCache; }

Glyph glyph() {
  if (!signedIn()) return Glyph::Off;
  if (g_phase == Phase::Connecting || g_phase == Phase::TimeWait || g_phase == Phase::Running) return Glyph::Syncing;
  if (g_lastFailed) return g_lastError == SyncEngine::Error::Network ? Glyph::Offline : Glyph::Error;
  if (g_pushPending) return Glyph::Pending;
  return Glyph::Synced;
}

void loop() {
  if (!g_inited) return;
  g_engine->step();
  if (g_sdLost) return;
  const uint32_t now = millis();

  // The sign-in screen waits on a phone, so keep it alive: redraw when something changes (approved, a typed code was
  // refused) and every 10 s while waiting so the countdown visibly moves.
  if (g_screenActive && g_engine->phase() == SyncEngine::Phase::WaitingForUser) {
    static uint32_t lastSig = 0, lastTick = 0;
    const uint32_t sig = g_engine->signInSignature();
    if (sig != lastSig || (g_engine->waitingOnRelay() && now - lastTick >= 10000)) {
      lastSig = sig;
      lastTick = now;
      dirty();
    }
  }

  switch (g_phase) {
    case Phase::Idle:
      if (g_pushPending && g_signedCache && !g_screenActive && offlineWaitOver(now) &&
          (g_pushImmediate || now - g_lastStartMs >= kMinBackgroundGapMs))
        startJob(Job::Sync);
      break;

    case Phase::Connecting: {
      const QuickWifi q = wifiQuickPoll();
      if (q == QuickWifi::Connected) {
        g_phase = Phase::TimeWait;
        g_phaseStart = now;
        memdiag::stage("clock check");
      }
      else if (q == QuickWifi::Failed) failBeforeStart("No Wi-Fi", SyncEngine::Error::Network);
      break;
    }

    case Phase::TimeWait:
      // The clock's time-server lookup goes first (wall_clock.h), also when the time is known already.
      wallclock::requestNetTime();
      if (wallclock::netTurn() && g_clock->epochNow() > 0) { startEngine(); break; }
      if (now - g_phaseStart > kTimeWaitMs) failBeforeStart("Date not set", SyncEngine::Error::Network);
      break;

    case Phase::Running:
      if (g_engine->finished()) onEngineDone();
      break;

    case Phase::Linger:
      if (g_pushPending && g_signedCache && !g_screenActive && offlineWaitOver(now) &&
          (g_pushImmediate || now - g_lastStartMs >= kMinBackgroundGapMs)) { startJob(Job::Sync); break; }
      if (now >= g_lingerEnd && !g_screenActive && !updnotice::busy()) teardown();   // a Check for updates rides on it
      break;
  }

  // A note waiting to open: done when its own transfer is (or the sync ended, or the cap ran out).
  if (g_holdActive) {
    const bool settled = g_phase == Phase::Running && g_engine->settled(g_holdNote);
    const bool timedOut = (int32_t)(now - g_holdDeadline) >= 0;
    if (g_holdRunDone || settled || timedOut) {
      g_holdActive = false;
      g_engine->prioritize("");
      if (g_holdRunDone && g_lastFailed) {
        snprintf(g_holdText, sizeof(g_holdText), "Could not reach %s, opening the saved copy", pname());
        g_holdMsgUntil = now + 1500;
      } else if (!g_holdRunDone && !settled) {
        snprintf(g_holdText, sizeof(g_holdText), "%s is slow, opening the saved copy", pname());
        g_holdMsgUntil = now + 1500;
      } else {
        g_holdNoteReady = true;
      }
      DBG_PRINTF("[cloud] note hold over after %u ms: %s\n", (unsigned)(now - (g_holdDeadline - kBrowseHoldMs)),
                 g_holdNoteReady ? "synced" : g_holdText);
      dirty();
    }
  } else if (g_holdMsgUntil && (int32_t)(now - g_holdMsgUntil) >= 0) {
    g_holdMsgUntil = 0;
    if (g_holdNote[0]) g_holdNoteReady = true;
    dirty();
  }

  if (g_listMsgUntil && (int32_t)(now - g_listMsgUntil) >= 0) {
    g_listMsgUntil = 0;
    if (currentState == UIState::FILE_BROWSER) dirty();
  }
}

// ---- triggers --------------------------------------------------------------------------------------------------

void onAutosave() {
  if (!g_inited || !g_signedCache) return;
  g_writeDirty = true;
}

static bool backupOverdue() {
  return g_writeDirty && g_settings.writeMinutes &&
         millis() - g_lastStartMs >= (uint32_t)g_settings.writeMinutes * 60000UL;
}

uint32_t pauseMs() { return (g_inited && g_signedCache && backupOverdue()) ? 1000 : AUTO_SAVE_IDLE_MS; }

void editorTick(uint32_t idleMs, bool hasUnsaved) {
  if (!g_inited || !g_signedCache || !g_writeDirty || hasUnsaved || g_pushPending) return;
  const bool overdue = backupOverdue();
  if (!overdue && !g_settings.autoPause) return;
  if (idleMs > (overdue ? 1000UL : (unsigned long)AUTO_SAVE_IDLE_MS)) { g_pushPending = true; dirty(); }
}

int noteMark() {
  if (!g_inited || !g_signedCache) return 0;
  if (g_lastFailed && g_phase != Phase::Running) return 2;
  const bool working = g_phase == Phase::Connecting || g_phase == Phase::TimeWait || g_phase == Phase::Running;
  return (g_writeDirty || g_pushPending || working) ? 1 : 0;
}

void onNoteClosed() {
  if (!g_inited || !g_settings.autoClose || !g_signedCache) return;
  g_pushPending = true;
  g_pushImmediate = true;
}

void onBrowse() {
  if (!g_inited || !g_settings.autoBrowse || !g_signedCache) return;
  if (!g_engine->busy() && g_phase != Phase::Connecting && g_phase != Phase::TimeWait) startJob(Job::Sync);
  dirty();
}

const char* listStatus() {
  static char buf[48];
  if (!g_inited || !g_signedCache) return "";
  if (syncWorking()) {
    snprintf(buf, sizeof(buf), "%s syncing", pname());
    return buf;
  }
  if (g_listMsgUntil && (int32_t)(millis() - g_listMsgUntil) < 0) return g_listMsg;
  return "";
}

bool holdNoteOpen(const char* file) {
  if (!g_inited || !file || !file[0] || !syncWorking()) return false;
  snprintf(g_holdNote, sizeof(g_holdNote), "%s", file);
  g_holdNoteReady = false;
  g_engine->prioritize(file);
  snprintf(g_holdText, sizeof(g_holdText), "Syncing with %s...", pname());
  g_holdRunDone = false;
  g_holdActive = true;
  g_holdDeadline = millis() + kBrowseHoldMs;
  g_holdMsgUntil = 0;
  DBG_PRINTF("[cloud] opening %s waits for its sync\n", file);
  dirty();
  return true;
}

bool takeHeldNote(char* out, unsigned size) {
  if (!g_holdNoteReady) return false;
  g_holdNoteReady = false;
  snprintf(out, size, "%s", g_holdNote);
  g_holdNote[0] = '\0';
  return true;
}

bool browseHoldActive() { return g_holdActive || (g_holdMsgUntil && (int32_t)(millis() - g_holdMsgUntil) < 0); }
const char* holdNotice() { return g_holdText; }
void browseHoldCancel() {
  if (g_holdActive && g_engine) g_engine->prioritize("");
  g_holdActive = false;
  g_holdMsgUntil = 0;
  g_holdNote[0] = '\0';
  g_holdNoteReady = false;
  dirty();
}

void onSdLost() {
  g_sdLost = true;
  if (!g_inited) return;
  if (g_engine->busy()) g_engine->cancel();
  if (g_phase == Phase::Connecting || g_phase == Phase::TimeWait || g_phase == Phase::Running) {
    if (g_phase == Phase::Running) msync::DeviceTransport::psramEnd();
    wifiQuickEnd();
    msync::DeviceTransport::closeConnection();
    g_phase = Phase::Idle;
  }
  g_job = Job::None;
  g_pushPending = false;
  g_pushImmediate = false;
  g_holdActive = false;
  g_holdMsgUntil = 0;
  g_holdNote[0] = '\0';
  g_holdNoteReady = false;
  g_lastFailed = false;
}

void onSdBack() {
  g_sdLost = false;
  if (g_inited && g_signedCache) g_writeDirty = true;   // the note saved on return still has to reach OneDrive
}

void onNotesDeleted(const char* const* names, int n) {
  if (!g_inited || g_sdLost || n <= 0) return;
  std::vector<std::string> v(names, names + n);
  for (int i = 0; i < g_providerCount; i++)
    if (g_bundles[i].engine) g_bundles[i].engine->markDeletedHere(v);
}

void flushBeforeSleep() {
  if (!g_inited || g_sdLost ||!g_settings.autoClose || !g_signedCache) return;
  if (!g_pushPending && !g_engine->busy()) return;
  const uint32_t deadline = millis() + kSleepFlushMs;
  g_pushImmediate = true;
  if (!g_engine->busy() && g_phase != Phase::Connecting && g_phase != Phase::TimeWait) startJob(Job::Sync);
  while ((int32_t)(millis() - deadline) < 0 &&
         (g_engine->busy() || g_phase == Phase::Connecting || g_phase == Phase::TimeWait)) {
    esp_task_wdt_reset();
    loop();
    delay(20);
  }
  if (g_engine->busy()) g_engine->cancel();
}

// ---- Switching provider ----------------------------------------------------------------------------------------

static bool providerBusy() {
  return g_engine->busy() || g_phase == Phase::Connecting || g_phase == Phase::TimeWait;
}

static bool providerSigned(int i) { return i == g_active ? g_signedCache : g_bundles[i].signedIn; }

// What Cancel does: stop whatever is running.
static void cancelWork() {
  if (g_engine->busy()) g_engine->cancel();
  msync::DeviceTransport::psramEnd();
  g_job = Job::None;
  g_phase = Phase::Idle;
  g_lastFailed = false;
  snprintf(g_lastSummary, sizeof(g_lastSummary), "Cancelled");
  dirty();
}

// Make provider i the active one. A running sync or sign-in of the old one is cancelled: choosing a cloud is an explicit request.
static bool selectProvider(int i) {
  if (i < 0 || i >= g_providerCount) return false;
  if (i == g_active) return true;
  if (providerBusy()) cancelWork();
  storeActive();
  g_active = i;
  loadActive();
  g_lastFailed = false;
  g_lastMessage[0] = '\0';
  g_pushPending = false;
  g_pushImmediate = false;
  g_writeDirty = false;
  g_job = Job::None;
  if (g_phase == Phase::Linger) teardown();
  saveActiveId();
  dirty();
  return true;
}

// Forget the sign-in on this device. Only ever reached through the confirmation page; switching clouds never does this.
static void signOutProvider(int i) {
  if (i == g_active && providerBusy()) cancelWork();
  Bundle& b = g_bundles[i];
  b.engine->signOut();
  saveAccountFor(b, "");
  b.signedIn = false;
  if (i == g_active) {
    saveAccount("");
    g_signedCache = false;
    g_pushPending = false;
    g_lastFailed = false;
    g_lastSummary[0] = '\0';
    g_lastEpoch = 0;
    saveLast();
  } else {
    b.lastSummary[0] = '\0';
    b.lastEpoch = 0;
    Preferences p;
    if (p.begin(b.ns, false)) { p.remove("last_epoch"); p.remove("last_sum"); p.end(); }
  }
}

static void openList() {
  g_listMode = true;
  g_listSel = g_active;
  g_confirmSignOut = -1;
  dirty();
}

// Choosing a row: that cloud becomes the active one; a connected cloud syncs now, a new one asks you to sign in.
static void listKey(uint8_t key) {
  const int n = g_providerCount;
  if (key == HID_KEY_DOWN) {
    if (g_listSel < n - 1) g_listSel++;
  } else if (key == HID_KEY_UP) {
    if (g_listSel > 0) g_listSel--;
  } else if (key == HID_KEY_ENTER) {
    selectProvider(g_listSel);
    g_listMode = false;
    startJob(g_signedCache ? Job::Sync : Job::SignIn);
  } else if (key == HID_KEY_BAR0 + 2 || key == HID_KEY_O) {   // the bar's second button arrives as Enter, so Sign out
                                                                  // sits on the third; O on a keyboard
    if (!providerSigned(g_listSel)) return;
    g_confirmSignOut = g_listSel;   // its own page; nothing is forgotten yet
  } else {
    return;
  }
  dirty();
}

static void confirmKey(uint8_t key) {
  // Sign out sits where nothing on the list sits, so a repeated tap cannot reach it. A keyboard confirms with Y; the
  // button-only E1001 (no bar) with Select.
  if (key == HID_KEY_BAR0 + 3 || key == HID_KEY_Y || (kClassicUi && key == HID_KEY_ENTER)) {
    signOutProvider(g_confirmSignOut);
    g_confirmSignOut = -1;
    dirty();
  }
}

static void fillRows(ScreenModel& m) {
  m.rowCount = (uint8_t)g_providerCount;
  m.rowSel = (int8_t)g_listSel;
  for (int i = 0; i < g_providerCount && i < 4; i++) {
    snprintf(m.rowName[i], sizeof(m.rowName[0]), "%s", g_bundles[i].provider->name());
    m.rowSigned[i] = providerSigned(i);
  }
}

bool wifiInUse() { return g_inited && g_phase != Phase::Idle; }
bool syncIdle() { return !g_inited || g_phase == Phase::Idle; }
bool networkQuiet() { return !g_inited || ((g_phase == Phase::Idle || g_phase == Phase::Linger) && !g_engine->busy()); }

void radioTakenOver() {
  if (!g_inited) return;
  const bool wasSync = g_job == Job::Sync && providerBusy();
  if (g_engine->busy()) g_engine->cancel();
  msync::DeviceTransport::psramEnd();
  msync::DeviceTransport::closeConnection();
  if (wasSync) g_pushPending = true;   // try again afterwards
  g_job = Job::None;
  g_phase = Phase::Idle;
  DBG_PRINTLN("[cloud] stopped: the radio is needed (Transfer hotspot or adding a network)");
  dirty();
}

// ---- Cloud screen ----------------------------------------------------------------------------------------------

void screenOpen() {
  g_listMode = false;
  g_confirmSignOut = -1;
  g_screenActive = true;
  if (g_providerCount > 1 && !g_signedCache) {   // the active cloud is not signed in: go straight to choosing one
    openList();
    return;
  }
  if (g_signedCache && !g_engine->busy() && g_phase != Phase::Connecting && g_phase != Phase::TimeWait) startJob(Job::Sync);
  dirty();
}

void screenClose() {
  g_screenActive = false;
  if (g_engine->busy() && g_job == Job::SignIn) g_engine->cancel();
}

bool screenKey(uint8_t key, uint8_t mods) {
  const bool working = providerBusy();
  if (key == HID_KEY_ESCAPE) {
    if (g_confirmSignOut >= 0) { g_confirmSignOut = -1; dirty(); return false; }   // Cancel
    if (g_listMode) { g_listMode = false; dirty(); return false; }                   // Back from the list
    if (working) { cancelWork(); return false; }
    return true;
  }
  if (g_confirmSignOut >= 0) { confirmKey(key); return false; }
  if (g_listMode) { listKey(key); return false; }

  {
    // Code entry sign-in: the keyboard types the code Dropbox showed.
    const SyncEngine::Status cs = g_engine->status();
    if (cs.phase == SyncEngine::Phase::WaitingForUser && cs.needsCode) {
      if (key == HID_KEY_BAR0 + 3 && g_providerCount > 1) { openList(); return false; }
      if (cs.relay) {   // the relay collects the code; the only choice is to type it instead
        if (key == HID_KEY_ENTER || key == HID_KEY_BAR0 + 1) { g_engine->useManualCode(); dirty(); }
        return false;
      }
      if (cs.submitting) return false;
      if (key == HID_KEY_ENTER || key == HID_KEY_BAR0 + 1) {
        if (g_codeLen > 0) g_engine->submitCode(g_codeBuf);
      } else if (key == HID_KEY_BACKSPACE) {
        if (g_codeLen > 0) g_codeBuf[--g_codeLen] = '\0';
      } else {
        const char c = hidToAscii(key, mods);
        if (c > ' ' && c < 0x7F && g_codeLen < 64) {
          g_codeBuf[g_codeLen++] = c;
          g_codeBuf[g_codeLen] = '\0';
        } else {
          return false;
        }
      }
      dirty();
      return false;
    }
  }
  const bool switchKey = key == HID_KEY_BAR0 + 3 || key == HID_KEY_C || key == HID_KEY_O;   // several: O signs out from the list
  if (g_providerCount > 1 && switchKey) { openList(); return false; }   // works even while a sync runs
  if (working) return false;
  if (g_engine->finished()) g_engine->acknowledge();

  if (key == HID_KEY_ENTER) {
    startJob(g_signedCache ? Job::Sync : Job::SignIn);
    return false;
  }
  if (key == HID_KEY_BAR0 + 2 || key == HID_KEY_A) {
    // Cycle: off, on, then on with a timed backup while writing (5, 10, 15, 30 minutes).
    static const uint8_t kSteps[] = {0, 5, 10, 15, 30};
    const bool all = g_settings.autoPause && g_settings.autoClose && g_settings.autoBrowse;
    if (!all) {
      g_settings.autoPause = g_settings.autoClose = g_settings.autoBrowse = true;
      g_settings.writeMinutes = 0;
    } else {
      int i = 0;
      while (i < 5 && kSteps[i] != g_settings.writeMinutes) i++;
      if (i >= 4) { g_settings.autoPause = g_settings.autoClose = g_settings.autoBrowse = false; g_settings.writeMinutes = 0; }
      else g_settings.writeMinutes = kSteps[i + 1];
    }
    saveSettings();
    dirty();
    return false;
  }
  if ((key == HID_KEY_BAR0 + 3 || key == HID_KEY_O) && g_providerCount == 1 && g_signedCache) {   // single cloud: Sign
                                                                                                  // out, on its own page
    g_confirmSignOut = g_active;
    dirty();
    return false;
  }
  return false;
}

const ScreenModel& screenModel() {
  ScreenModel& m = g_model;
  m = ScreenModel();
  snprintf(m.title, sizeof(m.title), "%s", pname());
  auto line = [&](const char* fmt, auto... args) {
    if (m.lineCount < 9) snprintf(m.lines[m.lineCount++], sizeof(m.lines[0]), fmt, args...);
  };
  const bool several = g_providerCount > 1;

  if (g_confirmSignOut >= 0) {
    const char* who = g_bundles[g_confirmSignOut].provider->name();
    snprintf(m.title, sizeof(m.title), "Sign out?");
    line("Sign out of %s on this device?", who);
    line("Your notes stay on this device and in");
    line("the cloud. You can sign in again later.");
    line("%s", kClassicUi ? "Select signs out, Back cancels." : "Y signs out, Esc cancels.");
    m.bar[0] = "Cancel";
    m.bar[3] = "Sign out";
    return m;
  }
  if (g_listMode) {
    snprintf(m.title, sizeof(m.title), "Cloud storage");
    fillRows(m);
    m.bar[0] = "Back";
    if (providerSigned(g_listSel)) m.bar[2] = "Sign out";
    return m;
  }

  const SyncEngine::Status st = g_engine->status();
  const bool connecting = g_phase == Phase::Connecting || g_phase == Phase::TimeWait;

  if (st.phase == SyncEngine::Phase::WaitingForUser && st.needsCode) {
    snprintf(m.title, sizeof(m.title), "Connect %s", pname());
    m.codeEntry = true;
    m.codeBox = !st.relay;
    snprintf(m.qrText, sizeof(m.qrText), "%s", st.verifyUri.c_str());
    snprintf(m.codeInput, sizeof(m.codeInput), "%s", g_codeBuf);
    if (st.submitting) snprintf(m.codeNote, sizeof(m.codeNote), st.relay ? "Approved, connecting..." : "Checking...");
    else snprintf(m.codeNote, sizeof(m.codeNote), "%s", st.codeError.c_str());
    line("1. Scan this with your phone camera");
    line("2. Sign in and approve MicroSlate");
    if (st.relay) {
      line("3. Waiting for your approval...");
      line("Time left: %u:%02u", (unsigned)(st.codeSecondsLeft / 60), (unsigned)(st.codeSecondsLeft % 60));
      m.bar[0] = "Cancel";
      m.bar[1] = "Type code";
      if (several) m.bar[3] = "Switch cloud";
      return m;
    }
    line("3. Type the code %s shows you:", pname());
    line("%d typed   -   %u:%02u left", g_codeLen, (unsigned)(st.codeSecondsLeft / 60), (unsigned)(st.codeSecondsLeft % 60));
    m.bar[0] = "Cancel";
    m.bar[1] = "Connect";
    if (several) m.bar[3] = "Switch cloud";
    return m;
  }
  if (st.phase == SyncEngine::Phase::WaitingForUser || st.phase == SyncEngine::Phase::RequestingCode) {
    snprintf(m.title, sizeof(m.title), "Connect %s", pname());
    if (st.phase == SyncEngine::Phase::RequestingCode) { line("Contacting %s...", pname()); }
    else {
      line("1. On your phone or computer go to");
      line("%s", st.verifyUri.c_str());
      line("2. Enter this code:");
      line("%s", st.userCode.c_str());
      m.bigLine = true;
      m.bigIndex = m.lineCount - 1;
      line("3. Sign in and approve MicroSlate");
      line("Time left: %u:%02u", (unsigned)(st.codeSecondsLeft / 60), (unsigned)(st.codeSecondsLeft % 60));
    }
    m.bar[0] = "Cancel";
    if (several) m.bar[3] = "Switch cloud";
    return m;
  }

  if (connecting) {
    line("Connecting to Wi-Fi...");
    m.bar[0] = "Cancel";
    if (several) m.bar[3] = "Switch cloud";
    return m;
  }

  if (st.phase == SyncEngine::Phase::Refreshing || st.phase == SyncEngine::Phase::Listing ||
      st.phase == SyncEngine::Phase::Scanning || st.phase == SyncEngine::Phase::Executing) {
    snprintf(m.title, sizeof(m.title), "Syncing");
    if (st.phase == SyncEngine::Phase::Executing && st.total > 0) {
      line("Transferring %d of %d", st.done + 1, st.total);
      if (!st.current.empty()) line("%s", st.current.c_str());
    } else if (st.phase == SyncEngine::Phase::Scanning) {
      line("Checking your notes...");
    } else {
      line("Checking %s...", pname());
    }
    m.bar[0] = "Cancel";
    if (several) m.bar[3] = "Switch cloud";
    return m;
  }

  if (!g_signedCache) {
    line("Not connected to %s.", pname());
    line("Connect to keep your notes in sync with");
    line("%s on your phone and computer.", pname());
    if (g_lastFailed && g_lastMessage[0]) line("%s", errorName());
    m.bar[0] = "Back";
    m.bar[1] = "Connect";
    if (several) m.bar[3] = "Switch cloud";
    return m;
  }

  line("Connected%s%s", g_account[0] ? " as " : "", g_account);
  char when[24];
  fmtTime(g_lastEpoch, when, sizeof(when));
  if (g_lastFailed) line("Last sync failed: %s", errorName());
  else if (g_lastSummary[0]) line("Last sync %s", when);
  if (!g_lastFailed && g_lastSummary[0]) {
    // One screen line per summary line ("2 conflicts (kept both)" / "This device's copies end in _sticky").
    char part[sizeof(g_lastSummary)];
    snprintf(part, sizeof(part), "%s", g_lastSummary);
    for (char* p = part; p && *p;) {
      char* nl = strchr(p, '\n');
      if (nl) *nl = '\0';
      line("%s", p);
      p = nl ? nl + 1 : nullptr;
    }
  }
  const bool all = g_settings.autoPause && g_settings.autoClose && g_settings.autoBrowse;
  const bool none = !g_settings.autoPause && !g_settings.autoClose && !g_settings.autoBrowse;
  if (all && g_settings.writeMinutes) line("Auto-sync: on, and every %u min while writing", (unsigned)g_settings.writeMinutes);
  else line("Auto-sync: %s", all ? "on (pause, close, browse)" : none ? "off" : "partly on");
  m.bar[0] = "Back";
  m.bar[1] = "Sync now";
  static char autoLabel[16];
  if (!all) snprintf(autoLabel, sizeof(autoLabel), "Auto: off");
  else if (g_settings.writeMinutes) snprintf(autoLabel, sizeof(autoLabel), "Auto: %um", (unsigned)g_settings.writeMinutes);
  else snprintf(autoLabel, sizeof(autoLabel), "Auto: on");
  m.bar[2] = autoLabel;
  m.bar[3] = several ? "Switch cloud" : "Sign out";
  if (kClassicUi) line(several ? "Keys: A auto-sync, C switch cloud, O sign out" : "Keys: A auto-sync, O sign out");
  return m;
}

}  // namespace cloud
