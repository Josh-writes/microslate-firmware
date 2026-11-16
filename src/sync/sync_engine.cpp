#include "sync_engine.h"

#include "sync_crc32.h"
#include "sync_names.h"
#include "sync_pkce.h"

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace msync {

static uint32_t crcOf(const std::string& s) {
  return crc32Update(0, reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

std::string SyncEngine::Result::summary() const {
  std::string out;
  auto add = [&](int n, const char* one, const char* many) {
    if (n <= 0) return;
    if (!out.empty()) out += ", ";
    out += std::to_string(n) + " " + (n == 1 ? one : many);
  };
  add(uploaded, "uploaded", "uploaded");
  add(downloaded, "downloaded", "downloaded");
  add(conflicts, "conflict (kept both)", "conflicts (kept both)");
  add(deletedLocal, "removed here", "removed here");
  add(deletedRemote, "removed from the cloud", "removed from the cloud");
  add(held, "waiting (note is open)", "waiting (notes are open)");
  add(tooLarge, "too large", "too large");
  add(errors, "error", "errors");
  if (out.empty() && error == Error::None) return "Everything is up to date";
  // A second line names this device's copies: "This device's copies end in _e1001".
  if (conflicts > 0)
    out += std::string("\nThis device's cop") + (conflicts == 1 ? "y ends" : "ies end") + " in _" + deviceTag;
  return out;
}

SyncEngine::SyncEngine(Provider& p, Transport& t, SyncStorage& s, TokenStore& ts, Clock& c, const Config& cfg)
    : prov_(p), tp_(t), st_(s), tokens_(ts), clock_(c), cfg_(cfg) {}

// ---------------------------------------------------------------------------------------------------------------
// plumbing

bool SyncEngine::busy() const {
  return phase_ != Phase::Idle && phase_ != Phase::Done && phase_ != Phase::Failed;
}

bool SyncEngine::hasRefreshToken() { return !tokens_.load().empty(); }

uint32_t SyncEngine::signInSignature() const {
  return (uint32_t)phase_ | ((pending_ && !relayPolling_ ? 1u : 0u) << 8) | ((relayMode_ ? 1u : 0u) << 9) |
         ((uint32_t)(status_.codeError.size() & 0xFF) << 10);
}

SyncEngine::Status SyncEngine::status() const {
  Status s = status_;
  s.phase = phase_;
  if (phase_ == Phase::WaitingForUser) {
    int32_t left = (int32_t)(codeExpiresAt_ - clock_.millisNow());
    s.codeSecondsLeft = left > 0 ? (uint32_t)(left / 1000) : 0;
  }
  s.total = (int)actions_.size();
  s.done = (int)(actionIdx_ < actions_.size() ? actionIdx_ : actions_.size());
  s.signedIn = !tokens_.load().empty();
  s.submitting = pending_ && phase_ == Phase::WaitingForUser && !relayPolling_;
  s.relay = relayMode_ && phase_ == Phase::WaitingForUser;
  return s;
}

void SyncEngine::issue(const HttpRequest& req, Handler h) {
  if (!tp_.begin(req)) { fail(Error::Network, "Network is busy"); return; }
  pending_ = true;
  handler_ = std::move(h);
  requestStartedAt_ = clock_.millisNow();
}

void SyncEngine::issueAuthed(HttpRequest req, Handler h) {
  const uint32_t now = clock_.millisNow();
  if (!haveAccess_ || (int32_t)(now - accessExpiresAt_) >= 0) {
    startRefresh([this, req, h]() { issueAuthed(req, h); });
    return;
  }
  req.bearer = access_;
  lastAuthed_ = req;
  lastAuthedHandler_ = std::move(h);
  authRetried_ = false;
  issue(req, [this](const HttpResponse& r) { onAuthedResponse(r); });
}

void SyncEngine::onAuthedResponse(const HttpResponse& r) {
  if (r.status == 401 && !authRetried_) {
    authRetried_ = true;
    haveAccess_ = false;
    startRefresh([this]() {
      HttpRequest q = lastAuthed_;
      q.bearer = access_;
      issue(q, [this](const HttpResponse& r2) { onAuthedResponse(r2); });
    });
    return;
  }
  Handler h = lastAuthedHandler_;  // copy: the handler may issue another request and replace the member
  h(r);
}

void SyncEngine::startRefresh(std::function<void()> then) {
  const std::string rt = tokens_.load();
  if (rt.empty()) { fail(Error::NotSignedIn, "Not signed in"); return; }
  issue(prov_.refreshRequest(cfg_.clientId, rt), [this, then](const HttpResponse& r) {
    if (r.status <= 0) {
      if (++networkFailures_ >= cfg_.maxConsecutiveNetworkFailures) { fail(Error::Network, std::string("No connection to ") + prov_.name()); return; }
      wakeAt_ = clock_.millisNow() + 2000;
      waiting_ = true;
      wakeFn_ = [this, then]() { startRefresh(then); };
      return;
    }
    TokenReply t = prov_.parseToken(r.status, r.body);
    if (!t.ok) {
      if (r.status == 429 || r.status >= 500) { fail(Error::Busy, std::string(prov_.name()) + " is busy, try again later"); return; }
      tokens_.clear();  // refused: the saved sign-in is no longer valid
      haveAccess_ = false;
      fail(Error::SignInExpired, "Sign-in expired, reconnect");
      return;
    }
    networkFailures_ = 0;
    // Some services rotate the refresh token (Microsoft does): store the new one before using anything else.
    if (!t.refresh.empty() && !tokens_.save(t.refresh)) { fail(Error::Storage, "Could not save sign-in"); return; }
    access_ = t.access;
    const int life = t.expiresIn > 180 ? t.expiresIn - 120 : 60;
    accessExpiresAt_ = clock_.millisNow() + (uint32_t)life * 1000u;
    haveAccess_ = true;
    then();
  });
}

void SyncEngine::fail(Error e, const std::string& msg) {
  if (phase_ == Phase::Failed) return;
  if (pending_) tp_.cancel();
  pending_ = false;
  waiting_ = false;
  actionActive_ = false;
  phase_ = Phase::Failed;
  result_.error = e;
  result_.message = msg;
}

void SyncEngine::failFromStatus(const HttpResponse& r, const std::string& what) {
  if (r.status <= 0) { fail(Error::Network, std::string("No connection to ") + prov_.name()); return; }
  if (r.status == 429 || r.status == 503) { fail(Error::Busy, std::string(prov_.name()) + " is busy, try again later"); return; }
  if (r.status == 507) { fail(Error::Cloud, "Cloud is full"); return; }
  fail(Error::Cloud, what + " (HTTP " + std::to_string(r.status) + ")");
}

bool SyncEngine::persistState() {
  if (!st_.writeState(state_.toJson())) { fail(Error::Storage, "Could not save sync state"); return false; }
  return true;
}

void SyncEngine::finish() {
  state_.lastSyncEpoch = clock_.epochNow();
  if (!persistState()) return;
  phase_ = Phase::Done;
  actionActive_ = false;
}

void SyncEngine::cancel() {
  if (!busy()) return;
  fail(Error::Cancelled, "Cancelled");
}

void SyncEngine::signOut() {
  if (busy()) cancel();
  tokens_.clear();
  prov_.forgetSession();
  access_.clear();
  haveAccess_ = false;
  // The state belongs to the account that produced it. With a different account an old state could make the next
  // sync look like "deleted in the cloud", so start over (first-sync rules never delete).
  state_.clear();
  st_.writeState(state_.toJson());
  phase_ = Phase::Idle;
}

void SyncEngine::step() {
  if (!busy()) return;
  const uint32_t now = clock_.millisNow();
  if (phase_ == Phase::WaitingForUser && status_.needsCode && !pending_ && (int32_t)(now - codeExpiresAt_) >= 0) {
    fail(Error::SignInExpired, "Took too long, try again");
    return;
  }
  if (phase_ == Phase::WaitingForUser && status_.needsCode && relayMode_ && !pending_ && !waiting_ &&
      (int32_t)(now - nextRelayPollMs_) >= 0) {
    // The phone approved (or not yet): ask the relay for the code. Slower after a minute, and slower still while the
    // relay does not answer, so a forgotten sign-in screen costs little.
    nextRelayPollMs_ = now + (relayFailures_ >= 4 ? 8000 : (now - relayStartMs_ > 60000 ? 6000 : 3000));
    relayPolling_ = true;
    issue(prov_.relayPollRequest(session_), [this](const HttpResponse& r) {
      relayPolling_ = false;
      std::string code;
      if (prov_.parseRelayPoll(r.status, r.body, code)) {
        relayFailures_ = 0;
        relayHint_ = false;
        status_.codeError.clear();
        exchangeCode(code, true, false);
        return;
      }
      if (r.status <= 0 || r.status >= 500) {   // the relay did not answer: say so, and keep trying
        if (++relayFailures_ >= 4 && !relayHint_) {
          relayHint_ = true;
          status_.codeError = "Can't reach the sign-in service. Press Type code.";
        }
        return;
      }
      relayFailures_ = 0;   // 204: reachable, just not approved yet
      if (relayHint_) { relayHint_ = false; status_.codeError.clear(); }
    });
    return;
  }
  if (waiting_) {
    if ((int32_t)(now - wakeAt_) >= 0) {
      waiting_ = false;
      std::function<void()> f = wakeFn_;
      f();
    }
    return;
  }
  if (pending_) {
    HttpResponse r;
    if (tp_.poll(r)) {
      pending_ = false;
      Handler h = handler_;
      h(r);
    } else if (now - requestStartedAt_ > cfg_.requestTimeoutMs) {
      tp_.cancel();
      pending_ = false;
      HttpResponse e;
      e.status = 0;
      e.error = "timeout";
      Handler h = handler_;
      h(e);
    }
    return;
  }
  if (phase_ == Phase::Scanning) {
    if (!scanOne()) return;
  } else if (phase_ == Phase::Executing && !actionActive_) {
    startAction();
  }
}

// ---------------------------------------------------------------------------------------------------------------
// sign-in (OAuth device code)

void SyncEngine::startSignIn() {
  if (busy()) return;
  result_ = Result();
  status_ = Status();
  phase_ = Phase::RequestingCode;
  pollFailures_ = 0;
  prov_.forgetSession();   // a new sign-in may be a different account
  verifier_.clear();
  session_.clear();
  relayMode_ = false;
  if (prov_.usesCodeEntry()) {
    // No request yet: the user opens the address (QR code), approves, and types the code back (submitCode).
    verifier_ = makeVerifier([this]() { return clock_.random32(); });
    relayMode_ = prov_.hasRelay();
    relayPolling_ = false;
    if (relayMode_) {
      session_ = makeSession([this]() { return clock_.random32(); });
      status_.verifyUri = prov_.relayAuthorizeUrl(session_, verifier_);
      nextRelayPollMs_ = clock_.millisNow() + 6000;   // the phone needs a moment to scan and approve
      relayStartMs_ = clock_.millisNow();
      relayFailures_ = 0;
      relayHint_ = false;
    } else {
      status_.verifyUri = prov_.authorizeUrl(cfg_.clientId, verifier_);
    }
    status_.needsCode = true;
    codeExpiresAt_ = clock_.millisNow() + cfg_.codeEntryMs;
    phase_ = Phase::WaitingForUser;
    return;
  }
  issue(prov_.deviceCodeRequest(cfg_.clientId), [this](const HttpResponse& r) {
    DeviceCode d = prov_.parseDeviceCode(r.status, r.body);
    if (!d.ok) {
      if (r.status <= 0) fail(Error::Network, std::string("No connection to ") + prov_.name());
      else fail(Error::Cloud, "Could not start sign-in: " + d.error);
      return;
    }
    deviceCode_ = d.deviceCode;
    status_.userCode = d.userCode;
    status_.verifyUri = d.verificationUri;
    codeExpiresAt_ = clock_.millisNow() + (uint32_t)d.expiresIn * 1000u;
    pollIntervalMs_ = (uint32_t)d.interval * 1000u;
    if (pollIntervalMs_ < cfg_.minPollMs) pollIntervalMs_ = cfg_.minPollMs;
    phase_ = Phase::WaitingForUser;
    wakeAt_ = clock_.millisNow() + cfg_.firstPollDelayMs;
    waiting_ = true;
    wakeFn_ = [this]() { pollSignIn(); };
  });
}

void SyncEngine::pollSignIn() {
  if ((int32_t)(clock_.millisNow() - codeExpiresAt_) >= 0) { fail(Error::SignInExpired, "The code expired, try again"); return; }
  issue(prov_.devicePollRequest(cfg_.clientId, deviceCode_), [this](const HttpResponse& r) { onSignInPoll(r); });
}

void SyncEngine::onSignInPoll(const HttpResponse& r) {
  auto again = [this]() {
    wakeAt_ = clock_.millisNow() + pollIntervalMs_;
    waiting_ = true;
    wakeFn_ = [this]() { pollSignIn(); };
  };
  // A failed poll is not a failed sign-in: the user may still be typing the code. Retry.
  if (r.status <= 0 || r.status >= 500) {
    if (++pollFailures_ >= 8) { fail(Error::Network, std::string("No connection to ") + prov_.name()); return; }
    again();
    return;
  }
  TokenReply t = prov_.parseToken(r.status, r.body);
  if (t.ok) { completeSignIn(t); return; }
  pollFailures_ = 0;
  if (t.error == "authorization_pending") { again(); return; }
  if (t.error == "slow_down") { pollIntervalMs_ += 5000; again(); return; }
  if (t.error == "authorization_declined") { fail(Error::SignInDeclined, "Sign-in was declined"); return; }
  if (t.error == "expired_token" || t.error == "bad_verification_code") { fail(Error::SignInExpired, "The code expired, try again"); return; }
  fail(Error::Cloud, "Sign-in failed: " + t.error);
}

// The tokens are in: remember the refresh token, then look up the account name for the screen.
void SyncEngine::completeSignIn(const TokenReply& t) {
  if (t.refresh.empty()) { fail(Error::Cloud, "Sign-in gave no refresh token"); return; }
  if (!tokens_.save(t.refresh)) { fail(Error::Storage, "Could not save sign-in"); return; }
  access_ = t.access;
  const int life = t.expiresIn > 180 ? t.expiresIn - 120 : 60;
  accessExpiresAt_ = clock_.millisNow() + (uint32_t)life * 1000u;
  haveAccess_ = true;
  pollFailures_ = 0;
  deviceCode_.clear();
  verifier_.clear();
  session_.clear();
  relayMode_ = false;
  status_.needsCode = false;
  fetchAccountName();
}

bool SyncEngine::submitCode(const std::string& typed) {
  if (phase_ != Phase::WaitingForUser || !status_.needsCode || pending_) return false;
  size_t b = 0, e = typed.size();
  while (b < e && (typed[b] == ' ' || typed[b] == '\t')) b++;
  while (e > b && (typed[e - 1] == ' ' || typed[e - 1] == '\t')) e--;
  const std::string code = typed.substr(b, e - b);
  if (code.empty()) return false;
  status_.codeError.clear();
  exchangeCode(code, false, false);
  return true;
}

void SyncEngine::useManualCode() {
  if (!relayMode_ || phase_ != Phase::WaitingForUser) return;
  if (pending_ && relayPolling_) {   // a poll is in flight: drop it, the user chose to type
    tp_.cancel();
    pending_ = false;
    relayPolling_ = false;
  }
  if (pending_) return;   // the relay's code is being checked right now
  relayMode_ = false;
  status_.verifyUri = prov_.authorizeUrl(cfg_.clientId, verifier_);
  status_.codeError.clear();
}

// Trade a code for tokens. A code from the relay flow needs the redirect address repeated in the request and a typed
// one does not; a typed code may still have come from the relay's own page, so one failure tries the other form.
void SyncEngine::exchangeCode(const std::string& code, bool viaRelay, bool retried) {
  issue(prov_.codeExchangeRequest(cfg_.clientId, code, verifier_, viaRelay), [this, code, viaRelay, retried](const HttpResponse& r) {
    if (r.status <= 0 || r.status >= 500) { status_.codeError = std::string("Could not reach ") + prov_.name() + ". Try again."; return; }
    TokenReply t = prov_.parseToken(r.status, r.body);
    if (t.ok) { completeSignIn(t); return; }
    if (!retried && prov_.hasRelay()) { exchangeCode(code, !viaRelay, true); return; }
    if (relayMode_) {   // the relay's code was refused: stop asking for it and let the user type instead
      useManualCode();
      status_.codeError = std::string(prov_.name()) + " did not accept that. Type a code instead.";
      return;
    }
    status_.codeError = "That code did not work. Check it and try again.";
  });
}

void SyncEngine::fetchAccountName() {
  issueAuthed(prov_.accountRequest(), [this](const HttpResponse& r) {
    if (r.status == 200) status_.account = prov_.parseAccount(r.body);
    phase_ = Phase::Done;  // signed in; a failed name lookup is cosmetic
    result_.message = status_.account.empty() ? "Signed in" : "Signed in as " + status_.account;
  });
}

// ---------------------------------------------------------------------------------------------------------------
// sync: list, scan, plan

void SyncEngine::startSync() {
  if (busy()) return;
  result_ = Result();
  result_.deviceTag = cfg_.deviceTag;
  status_ = Status();
  actions_.clear();
  actionIdx_ = 0;
  actionActive_ = false;
  excluded_.clear();
  remote_.clear();
  local_.clear();
  networkFailures_ = 0;
  if (!hasRefreshToken()) { phase_ = Phase::Refreshing; fail(Error::NotSignedIn, "Not signed in"); return; }

  std::string txt;
  stateLost_ = false;
  if (st_.readState(txt)) {
    if (!SyncState::fromJson(txt, state_)) { state_.clear(); stateLost_ = true; }
  } else {
    state_.clear();
    stateLost_ = true;
  }
  phase_ = Phase::Listing;
  runSetup(0);
}

// The provider's own preparation (Google Drive: find or create the notes folder), then the first listing page.
void SyncEngine::runSetup(int step) {
  HttpRequest q;
  if (!prov_.setupRequest(step, q)) { requestListPage(""); return; }
  issueAuthed(q, [this, step](const HttpResponse& r) {
    if (transportFailed(r)) { if (phase_ != Phase::Failed) fail(Error::Network, std::string("No answer from ") + prov_.name()); return; }
    if (r.status == 429 || r.status == 503) { failFromStatus(r, ""); return; }
    std::string err;
    if (!prov_.setupReply(step, r.status, r.body, err)) { fail(Error::Cloud, err.empty() ? "Could not prepare the cloud folder" : err); return; }
    runSetup(step + 1);
  });
}

void SyncEngine::requestListPage(const std::string& pageLink) {
  issueAuthed(prov_.listRequest(pageLink), [this](const HttpResponse& r) { onListPage(r); });
}

void SyncEngine::onListPage(const HttpResponse& r) {
  if (r.status != 200) { failFromStatus(r, "Could not read the cloud folder"); return; }
  std::string next;
  int skipped = 0;
  if (!prov_.parseList(r.body, remote_, next, skipped)) { fail(Error::Cloud, "Unreadable folder listing"); return; }
  result_.skippedRemote += skipped;
  if (!next.empty()) {
    if (!prov_.pagingLinkAllowed(next)) { fail(Error::Cloud, "Unexpected paging link"); return; }
    if (remote_.size() > 5000) { fail(Error::Cloud, "Too many files in the cloud folder"); return; }
    requestListPage(next);
    return;
  }
  beginScan();
}

void SyncEngine::beginScan() {
  localNames_.clear();
  if (!st_.listNotes(localNames_)) { fail(Error::Storage, "Could not read the notes folder"); return; }
  scanIdx_ = 0;
  local_.clear();
  phase_ = Phase::Scanning;
}

// Hash one local note per step so the main loop keeps running. Returns false when the run was stopped.
bool SyncEngine::scanOne() {
  if (scanIdx_ >= localNames_.size()) { beginPlan(); return phase_ != Phase::Failed; }
  const std::string name = localNames_[scanIdx_++];
  if (!noteNameValid(name)) return true;  // not a syncable note (hidden, wrong extension, odd characters)
  std::string data;
  if (!st_.readNote(name, data, cfg_.maxUploadBytes)) {
    if (st_.noteExists(name)) {
      // Too large or unreadable: leave this name out of the plan entirely. A name missing from the local list would
      // otherwise look "deleted here" and could delete the cloud copy.
      excluded_.insert(name);
      result_.tooLarge++;
    }
    return true;
  }
  LocalFile f;
  f.name = name;
  f.size = (uint32_t)data.size();
  f.crc = crcOf(data);
  local_.push_back(f);
  return true;
}

void SyncEngine::beginPlan() {
  std::vector<Action> plan = planSync(local_, remote_, state_.entries, cfg_.pushOnly);
  actions_.clear();
  int delLocal = 0, delRemote = 0;
  for (const Action& a : plan) {
    if (excluded_.count(a.name)) continue;
    if (a.type == ActionType::DeleteLocal) delLocal++;
    if (a.type == ActionType::DeleteRemote) delRemote++;
    actions_.push_back(a);
  }
  // Mass-deletion guard: if most tracked notes suddenly look deleted on one side (a reformatted SD card, a renamed
  // cloud folder, a wrong account), delete nothing and say so.
  const size_t tracked = state_.entries.size();
  auto guard = [&](ActionType t, int n) {
    if (n >= 5 && (size_t)n * 2 > tracked) {
      std::vector<Action> kept;
      for (const Action& a : actions_) if (a.type != t) kept.push_back(a);
      actions_ = kept;
      result_.held += n;
      if (result_.message.empty()) result_.message = "Many notes look deleted; nothing was removed";
    }
  };
  guard(ActionType::DeleteLocal, delLocal);
  guard(ActionType::DeleteRemote, delRemote);

  actionIdx_ = 0;
  actionActive_ = false;
  movePriority();
  phase_ = Phase::Executing;
  if (actions_.empty()) finish();
}

void SyncEngine::prioritize(const std::string& name) {
  priority_ = name;
  if (phase_ == Phase::Executing) movePriority();
}

// Moves the prioritized note's actions to the next free slot (after the one running), keeping the rest in order.
void SyncEngine::movePriority() {
  if (priority_.empty()) return;
  size_t slot = actionIdx_ + (actionActive_ ? 1 : 0);
  for (size_t j = slot; j < actions_.size(); j++) {
    if (actions_[j].name != priority_) continue;
    std::rotate(actions_.begin() + slot, actions_.begin() + j, actions_.begin() + j + 1);
    slot++;
  }
}

bool SyncEngine::settled(const std::string& name) const {
  if (!busy()) return true;
  if (phase_ != Phase::Executing) return false;
  for (size_t j = actionIdx_; j < actions_.size(); j++)   // actionIdx_ is the one running, if any
    if (actions_[j].name == name) return false;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// actions

const RemoteFile* SyncEngine::findRemote(const std::string& name) const {
  for (const auto& f : remote_) if (f.name == name) return &f;
  return nullptr;
}
const LocalFile* SyncEngine::findLocal(const std::string& name) const {
  for (const auto& f : local_) if (f.name == name) return &f;
  return nullptr;
}

// This device's copy of a clashing note: "a_sticky.txt", then "a_sticky_2.txt" ... never a name
// already used here or in the cloud.
std::string SyncEngine::uniqueConflictName(const std::string& name) {
  std::string cand = conflictName(name, cfg_.deviceTag);
  int n = 2;
  while (st_.noteExists(cand) || findLocal(cand) || findRemote(cand))
    cand = conflictName(name, cfg_.deviceTag + "_" + std::to_string(n++));
  return cand;
}

bool SyncEngine::transportFailed(const HttpResponse& r) {
  if (r.status > 0) { networkFailures_ = 0; return false; }
  if (++networkFailures_ >= cfg_.maxConsecutiveNetworkFailures) fail(Error::Network, std::string("Lost the connection to ") + prov_.name());
  return true;
}

void SyncEngine::actionDone(bool ok, const std::string& err) {
  if (!ok) {
    result_.errors++;
    if (result_.message.empty()) result_.message = err;
  }
  actionActive_ = false;
  if (phase_ == Phase::Executing) actionIdx_++;
}

void SyncEngine::startAction() {
  if (actionIdx_ >= actions_.size()) { finish(); return; }
  actionActive_ = true;
  const Action a = actions_[actionIdx_];
  status_.current = a.name;
  switch (a.type) {
    case ActionType::Upload: doUpload(a.name, a.name, false); break;
    case ActionType::Download:
    case ActionType::Compare: doFetch(a); break;
    case ActionType::Conflict:
      if (cfg_.pushOnly) doUpload(a.name, uniqueConflictName(a.name), true);  // never touch the cloud copy
      else doFetch(a);
      break;
    case ActionType::DeleteLocal:
      if (st_.isOpenNote(a.name)) { result_.held++; actionDone(true); break; }
      if (!st_.trashNote(a.name)) { actionDone(false, "Could not move " + a.name + " to the trash"); break; }
      state_.erase(a.name);
      result_.deletedLocal++;
      if (persistState()) actionDone(true);
      break;
    case ActionType::DeleteRemote: {
      const RemoteFile* rf = findRemote(a.name);
      issueAuthed(prov_.deleteRequest(a.name, rf ? rf->id : std::string()), [this, a](const HttpResponse& r) {
        if (transportFailed(r)) { if (phase_ != Phase::Failed) actionDone(false, "No connection"); return; }
        if (r.status == 429 || r.status == 503) { failFromStatus(r, ""); return; }
        if (r.status == 200 || r.status == 204 || r.status == 404) {
          state_.erase(a.name);
          result_.deletedRemote++;
          if (persistState()) actionDone(true);
        } else {
          actionDone(false, "Could not remove " + a.name + " from the cloud (HTTP " + std::to_string(r.status) + ")");
        }
      });
      break;
    }
    case ActionType::Forget:
      state_.erase(a.name);
      if (persistState()) actionDone(true);
      break;
    case ActionType::Skip: actionDone(true); break;
  }
}

void SyncEngine::doUpload(const std::string& name, const std::string& remoteName, bool asConflictCopy) {
  std::string data;
  if (!st_.readNote(name, data, cfg_.maxUploadBytes)) {
    if (st_.noteExists(name)) { result_.tooLarge++; actionDone(true); }
    else actionDone(false, name + " disappeared during sync");
    return;
  }
  const uint32_t crc = crcOf(data);
  const uint32_t size = (uint32_t)data.size();
  const RemoteFile* known = remoteName == name ? findRemote(name) : nullptr;
  const uint32_t modified = clock_.epochNow();
  issueAuthed(prov_.uploadRequest(remoteName, known ? known->id : std::string(), std::move(data), modified), [this, name, remoteName, crc, size, asConflictCopy, modified](const HttpResponse& r) {
    if (transportFailed(r)) { if (phase_ != Phase::Failed) actionDone(false, "No connection"); return; }
    if (r.status == 429 || r.status == 503 || r.status == 507) { failFromStatus(r, ""); return; }
    RemoteFile f;
    if ((r.status == 200 || r.status == 201) && prov_.parseUpload(r.body, f)) {
      auto record = [this, name, remoteName, crc, size, asConflictCopy](const RemoteFile& got) {
        if (remoteName == name) {  // a renamed push (X4 conflict) deliberately leaves the original's state alone
          StateEntry e;
          e.name = name;
          e.remoteId = got.id;
          e.remoteRev = got.rev;
          e.syncedCrc = crc;
          e.syncedSize = size;
          state_.set(e);
        }
        if (!asConflictCopy) result_.uploaded++;
        else result_.conflicts++;
        if (persistState()) actionDone(true);
      };
      HttpRequest dateReq;
      if (!prov_.dateRequest(remoteName, f, modified, dateReq)) { record(f); return; }
      // The text is up; now today's date on it. Its answer carries the latest revision; if it fails, the upload
      // still counts (the text is safe in the cloud).
      issueAuthed(dateReq, [this, f, record](const HttpResponse& r2) {
        RemoteFile g;
        if (!transportFailed(r2) && r2.status == 200 && prov_.parseUpload(r2.body, g) && !g.rev.empty()) record(g);
        else record(f);
      });
    } else {
      actionDone(false, "Could not upload " + name + " (HTTP " + std::to_string(r.status) + ")");
    }
  });
}

void SyncEngine::doFetch(const Action& a) {
  const RemoteFile* known = findRemote(a.name);
  issueAuthed(prov_.downloadRequest(a.name, known ? known->id : std::string()), [this, a](const HttpResponse& r) {
    if (transportFailed(r)) { if (phase_ != Phase::Failed) actionDone(false, "No connection"); return; }
    if (r.status == 429 || r.status == 503) { failFromStatus(r, ""); return; }
    if (r.status == 200) { onFetched(a, r.body); return; }
    if ((r.status == 301 || r.status == 302 || r.status == 303 || r.status == 307 || r.status == 308) &&
        prov_.redirectAllowed(r.location)) {
      // The download link is pre-authenticated: it must be fetched WITHOUT the bearer token.
      HttpRequest g;
      g.method = "GET";
      g.url = r.location;
      issue(g, [this, a](const HttpResponse& r2) {
        if (transportFailed(r2)) { if (phase_ != Phase::Failed) actionDone(false, "No connection"); return; }
        if (r2.status == 200) onFetched(a, r2.body);
        else actionDone(false, "Could not download " + a.name + " (HTTP " + std::to_string(r2.status) + ")");
      });
      return;
    }
    if (r.status == 404) { actionDone(false, a.name + " vanished from the cloud during sync"); return; }
    actionDone(false, "Could not download " + a.name + " (HTTP " + std::to_string(r.status) + ")");
  });
}

void SyncEngine::onFetched(const Action& a, const std::string& body) {
  const RemoteFile* rf = findRemote(a.name);
  if (!rf) { actionDone(false, a.name + " is not in the cloud listing"); return; }
  if (body.size() != rf->size) { actionDone(false, "Download of " + a.name + " was incomplete"); return; }

  if (a.type == ActionType::Download) {
    if (body.size() > cfg_.maxNoteBytes) { result_.tooLarge++; actionDone(true); return; }  // too big for this device
    if (st_.isOpenNote(a.name)) { result_.held++; actionDone(true); return; }
    if (!st_.writeNote(a.name, body)) { fail(Error::Storage, "Could not write " + a.name); return; }
    StateEntry e;
    e.name = a.name;
    e.remoteId = rf->id;
    e.remoteRev = rf->rev;
    e.syncedCrc = crcOf(body);
    e.syncedSize = (uint32_t)body.size();
    state_.set(e);
    result_.downloaded++;
    if (persistState()) actionDone(true);
    return;
  }
  if (a.type == ActionType::Compare) {
    const LocalFile* lf = findLocal(a.name);
    if (lf && lf->crc == crcOf(body) && lf->size == body.size()) {  // identical: just remember the pairing
      StateEntry e;
      e.name = a.name;
      e.remoteId = rf->id;
      e.remoteRev = rf->rev;
      e.syncedCrc = lf->crc;
      e.syncedSize = lf->size;
      state_.set(e);
      result_.unchanged++;
      if (persistState()) actionDone(true);
      return;
    }
  }
  resolveConflict(a, body);
}

// Both sides differ: keep the cloud version as a separate local note, then push the local version over the cloud
// one and the conflict copy as a new cloud note. Nothing is lost on either side.
void SyncEngine::resolveConflict(const Action& a, const std::string& body) {
  if (body.size() > cfg_.maxNoteBytes) {
    result_.tooLarge++;
    actionDone(false, a.name + " changed in both places and the cloud copy is too large for this device");
    return;
  }
  // The cloud version keeps the name; this device's version is kept as "<name>_<device>" and uploaded
  // as a new note, so every copy says which device wrote it. The open note is never replaced: it
  // waits for the next sync after it is closed.
  if (st_.isOpenNote(a.name)) { result_.held++; actionDone(true); return; }
  const RemoteFile* rf = findRemote(a.name);
  if (!rf) { actionDone(false, a.name + " is not in the cloud listing"); return; }
  std::string mine;
  if (!st_.readNote(a.name, mine, cfg_.maxUploadBytes)) { actionDone(false, "Could not read " + a.name); return; }
  const std::string cname = uniqueConflictName(a.name);
  // Our copy first: if the card is pulled before the next write, both names hold our text.
  if (!st_.writeNote(cname, mine)) { fail(Error::Storage, "Could not write " + cname); return; }
  if (!st_.writeNote(a.name, body)) { fail(Error::Storage, "Could not write " + a.name); return; }
  StateEntry e;
  e.name = a.name;
  e.remoteId = rf->id;
  e.remoteRev = rf->rev;
  e.syncedCrc = crcOf(body);
  e.syncedSize = (uint32_t)body.size();
  state_.set(e);
  actions_.insert(actions_.begin() + actionIdx_ + 1, Action{ActionType::Upload, cname});
  result_.conflicts++;
  if (persistState()) actionDone(true);
}

}  // namespace msync
