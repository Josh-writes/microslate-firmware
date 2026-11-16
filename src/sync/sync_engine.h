#pragma once
// The cloud sync engine: sign-in (device code), listing, planning, transfers, state. A non-blocking state machine:
// call step() from the main loop. Every network call goes through a Transport (worker task on the device), every
// SD access through SyncStorage (main thread), so typing never waits for the network and the shared SD/display SPI
// bus is only touched by one thread. Pure C++: the whole engine is tested on a PC against a fake cloud.

#include "provider.h"
#include "sync_http.h"
#include "sync_interfaces.h"
#include "sync_plan.h"
#include "sync_state.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace msync {

class SyncEngine {
 public:
  struct Config {
    std::string clientId;
    uint32_t maxNoteBytes = 16383;            // largest note this device can edit (downloads above it are skipped)
    uint32_t maxUploadBytes = 4u * 1024 * 1024 - 1024;  // the device hard limit (4 MB): larger notes are never opened
    bool pushOnly = false;                    // X4 tier
    std::string deviceTag = "device";         // marks this device's copy of a clashing note: "a_sticky.txt"
    uint32_t firstPollDelayMs = 15000;        // after showing the code, give the user time to type it
    uint32_t minPollMs = 10000;               // Microsoft's minimum is 5 s; slower is allowed
    uint32_t requestTimeoutMs = 45000;
    int maxConsecutiveNetworkFailures = 3;
    uint32_t codeEntryMs = 10u * 60u * 1000u;   // code entry sign-in (Dropbox): how long the user has to type the code
  };

  enum class Phase : uint8_t {
    Idle,
    RequestingCode,   // sign-in: asking for a device code
    WaitingForUser,   // sign-in: showing the code, polling
    Refreshing,       // getting an access token
    Listing,          // reading the cloud folder
    Scanning,         // hashing local notes
    Executing,        // transfers
    Done,             // finished (see result())
    Failed,           // stopped (see result().error)
  };

  enum class Error : uint8_t {
    None,
    NotSignedIn,      // no refresh token
    SignInExpired,    // refresh refused: the user must sign in again
    SignInDeclined,
    Network,          // no answer from the cloud
    Cloud,            // the cloud refused (4xx/5xx)
    Busy,             // 429 / 503: try again later
    Storage,          // SD problem: the run was stopped before anything unsafe happened
    Cancelled,
  };

  struct Result {
    int uploaded = 0, downloaded = 0, conflicts = 0, deletedLocal = 0, deletedRemote = 0;
    int unchanged = 0;   // adopted without a transfer (identical on both sides)
    int held = 0;        // changes waiting because the note is open in the editor
    int tooLarge = 0;    // notes this device cannot hold or the cloud will not take
    int skippedRemote = 0;  // cloud items ignored (folders, bad names)
    int errors = 0;
    std::string deviceTag;  // set when there were conflicts, for the summary
    Error error = Error::None;
    std::string message;    // first error, plain words
    std::string summary() const;
  };

  struct Status {
    Phase phase = Phase::Idle;
    std::string userCode, verifyUri;   // while WaitingForUser
    bool needsCode = false;            // code entry sign-in: verifyUri is for a QR code and the user types the code back
    bool submitting = false;           // the typed code is being checked
    bool relay = false;                // verifyUri goes through the relay: the device collects the code itself
    std::string codeError;             // the last typed code did not work
    uint32_t codeSecondsLeft = 0;
    std::string account;               // display name after sign-in
    std::string current;               // note being transferred
    int done = 0, total = 0;           // progress while Executing
    bool signedIn = false;
  };

  SyncEngine(Provider& p, Transport& t, SyncStorage& s, TokenStore& ts, Clock& c, const Config& cfg);

  void startSignIn();
  // Code entry sign-in only: the code the user typed. False when the engine is not waiting for one.
  bool submitCode(const std::string& code);
  // Relay sign-in: give up waiting for the relay and let the user type the code Dropbox shows instead.
  void useManualCode();
  void startSync();
  // The writer is opening this note during a sync: its transfer goes next (or first, once the plan is built).
  // Stays set until prioritize("") clears it.
  void prioritize(const std::string& name);
  // Nothing is left to do for this note in the running sync (or no sync runs). False while the cloud is still being
  // listed, because the plan is not known yet.
  bool settled(const std::string& name) const;
  void cancel();
  void signOut();                      // forget the tokens on this device
  void step();                         // call from the main loop; never blocks
  bool busy() const;                   // a run is in progress
  bool finished() const { return phase_ == Phase::Done || phase_ == Phase::Failed; }
  void acknowledge() { if (finished()) phase_ = Phase::Idle; }  // UI has shown the result
  bool hasRefreshToken();

  // Cheap change detector for the sign-in screen: differs whenever something the user should see has changed.
  uint32_t signInSignature() const;
  Phase phase() const { return phase_; }   // cheap, unlike status()
  bool waitingOnRelay() const { return phase_ == Phase::WaitingForUser && relayMode_; }

  Status status() const;
  const Result& result() const { return result_; }

 private:
  using Handler = std::function<void(const HttpResponse&)>;

  void issue(const HttpRequest& req, Handler h);
  void issueAuthed(HttpRequest req, Handler h);
  void onAuthedResponse(const HttpResponse& r);
  void startRefresh(std::function<void()> then);
  void fail(Error e, const std::string& msg);
  void finish();
  bool persistState();

  void runSetup(int step);
  void requestListPage(const std::string& pageLink);
  void onListPage(const HttpResponse& r);
  void beginScan();
  bool scanOne();
  void beginPlan();
  void movePriority();
  void startAction();
  void actionDone(bool ok, const std::string& err = "");
  void doUpload(const std::string& name, const std::string& remoteName, bool asConflictCopy);
  void doFetch(const Action& a);
  void onFetched(const Action& a, const std::string& body);
  void resolveConflict(const Action& a, const std::string& body);
  void pollSignIn();
  void onSignInPoll(const HttpResponse& r);
  void completeSignIn(const TokenReply& t);
  void exchangeCode(const std::string& code, bool viaRelay, bool retried);
  void fetchAccountName();
  std::string uniqueConflictName(const std::string& name);
  const RemoteFile* findRemote(const std::string& name) const;
  const LocalFile* findLocal(const std::string& name) const;
  bool transportFailed(const HttpResponse& r);
  void failFromStatus(const HttpResponse& r, const std::string& what);

  Provider& prov_;
  Transport& tp_;
  SyncStorage& st_;
  TokenStore& tokens_;
  Clock& clock_;
  Config cfg_;

  Phase phase_ = Phase::Idle;
  Result result_;
  Status status_;

  // tokens (the refresh token is only ever written through tokens_)
  std::string access_;
  uint32_t accessExpiresAt_ = 0;
  bool haveAccess_ = false;

  // request in flight
  bool pending_ = false;
  uint32_t requestStartedAt_ = 0;
  Handler handler_;
  HttpRequest lastAuthed_;
  Handler lastAuthedHandler_;
  bool authRetried_ = false;
  int networkFailures_ = 0;

  // timers
  bool waiting_ = false;
  uint32_t wakeAt_ = 0;
  std::function<void()> wakeFn_;

  // sign-in
  std::string deviceCode_;
  std::string verifier_;   // code entry sign-in: the PKCE verifier, kept in memory only
  std::string session_;    // relay sign-in: random id shared with the phone through the QR code
  bool relayMode_ = false;
  bool relayPolling_ = false;   // the request in flight is a relay poll, not a code check
  uint32_t nextRelayPollMs_ = 0;
  uint32_t relayStartMs_ = 0;
  int relayFailures_ = 0;      // consecutive polls the relay did not answer
  bool relayHint_ = false;     // "cannot reach the sign-in service" is showing
  uint32_t pollIntervalMs_ = 10000;
  uint32_t codeExpiresAt_ = 0;
  int pollFailures_ = 0;

  // sync data
  SyncState state_;
  bool stateLost_ = false;
  std::vector<RemoteFile> remote_;
  std::vector<LocalFile> local_;
  std::vector<std::string> localNames_;
  size_t scanIdx_ = 0;
  std::set<std::string> excluded_;
  std::vector<Action> actions_;
  size_t actionIdx_ = 0;
  bool actionActive_ = false;
  std::string priority_;   // prioritize()
};

}  // namespace msync
