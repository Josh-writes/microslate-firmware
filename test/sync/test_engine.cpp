// Engine tests against an in-process fake of OneDrive. No device, no network.
// Run with: sh test/sync/run-on-hinton.sh
#include "fakes.h"

#include <cstdio>
#include <functional>

using namespace msync;
using namespace fk;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                      \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }       \
  } while (0)

struct World {
  OneDriveProvider provider;
  FakeCloud cloud;
  FakeStorage disk;
  FakeTokens tokens;
  FakeClock clock;
  SyncEngine::Config cfg;
  SyncEngine* eng = nullptr;

  World() { cfg.clientId = "CID"; cfg.maxNoteBytes = 16383; build(); }
  ~World() { delete eng; }
  void build() { delete eng; eng = new SyncEngine(provider, cloud, disk, tokens, clock, cfg); }

  // A signed-in device: the refresh token is known to the cloud and the device.
  void signedIn() {
    cloud.tokenSerial = 0;
    cloud.issue();
    tokens.t = cloud.refreshToken;
  }
  // Step the engine, advancing the clock 100 ms per step, until it stops. Returns steps used.
  int run(int maxSteps = 20000) {
    int i = 0;
    while (eng->busy() && i < maxSteps) { clock.ms += 100; eng->step(); i++; }
    return i;
  }
  const SyncEngine::Result& sync() {
    eng->startSync();
    run();
    return eng->result();
  }
  bool ok() { return eng->status().phase == SyncEngine::Phase::Done; }
};

static std::string body(int n) { return "note body number " + std::to_string(n) + "\n"; }

// ---- sign-in -------------------------------------------------------------------------------------------------

static void testSignIn() {
  World w;
  w.cloud.pendingPolls = 2;
  w.eng->startSignIn();
  // first step issues the device-code request
  for (int i = 0; i < 5; i++) { w.clock.ms += 100; w.eng->step(); }
  CHECK(w.eng->status().phase == SyncEngine::Phase::WaitingForUser);
  CHECK(w.eng->status().userCode == "ABCD1234");
  CHECK(w.eng->status().verifyUri == "https://www.microsoft.com/link");
  const size_t before = w.cloud.log.size();
  // no poll during the first 15 s
  uint32_t t0 = w.clock.ms;
  while (w.clock.ms - t0 < 14000) { w.clock.ms += 100; w.eng->step(); }
  CHECK(w.cloud.log.size() == before);
  w.run();
  CHECK(w.ok());
  CHECK(w.tokens.t == w.cloud.refreshToken && !w.tokens.t.empty());
  CHECK(w.eng->status().account == "Test User");
  CHECK(w.eng->status().signedIn);
  // pending, pending, success = 3 polls, spaced by at least the minimum poll interval
  int polls = 0;
  for (auto& l : w.cloud.log) if (l.find("/token") != std::string::npos) polls++;
  CHECK(polls == 3);
}

static void testSignInFailures() {
  {
    World w;
    w.cloud.declined = true;
    w.eng->startSignIn();
    w.run();
    CHECK(w.eng->result().error == SyncEngine::Error::SignInDeclined);
    CHECK(w.tokens.t.empty());
  }
  {  // transport failures while polling are retried, not fatal
    World w;
    w.eng->startSignIn();
    for (int i = 0; i < 5; i++) { w.clock.ms += 100; w.eng->step(); }
    w.cloud.dropNext = 3;
    w.run();
    CHECK(w.ok());
    CHECK(!w.tokens.t.empty());
  }
  {  // the code never gets entered: expires
    World w;
    w.cloud.pendingPolls = 100000;
    w.eng->startSignIn();
    w.run();
    CHECK(w.eng->result().error == SyncEngine::Error::SignInExpired);
  }
  {  // no network at all when asking for a code
    World w;
    w.cloud.dropNext = 1;
    w.eng->startSignIn();
    w.run();
    CHECK(w.eng->result().error == SyncEngine::Error::Network);
  }
  {  // storage refuses to keep the token: sign-in must fail loudly
    World w;
    w.tokens.failSave = true;
    w.eng->startSignIn();
    w.run();
    CHECK(w.eng->result().error == SyncEngine::Error::Storage);
  }
}

// ---- basic sync flows ----------------------------------------------------------------------------------------

static void testNotSignedIn() {
  World w;
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::NotSignedIn);
  CHECK(w.cloud.log.empty());
}

static void testFirstSyncAndIdle() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = body(1);
  w.disk.notes["b.txt"] = body(2);
  w.cloud.put("c.txt", body(3));
  auto r = w.sync();
  CHECK(w.ok());
  CHECK(r.uploaded == 2 && r.downloaded == 1 && r.errors == 0);
  CHECK(w.cloud.files["a.txt"].content == body(1) && w.cloud.files["b.txt"].content == body(2));
  CHECK(w.disk.notes["c.txt"] == body(3));
  CHECK(w.disk.haveState);
  // second sync changes nothing and transfers nothing
  w.cloud.puts = w.cloud.gets = w.cloud.deletes = 0;
  auto r2 = w.sync();
  CHECK(w.ok() && r2.uploaded == 0 && r2.downloaded == 0 && r2.conflicts == 0);
  CHECK(w.cloud.puts == 0 && w.cloud.gets == 0 && w.cloud.deletes == 0);
  CHECK(r2.summary() == "Everything is up to date");
  CHECK(!w.cloud.bearerLeaked);
}

static void testEditsBothWays() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = body(1);
  w.cloud.put("b.txt", body(2));
  w.sync();
  w.disk.notes["a.txt"] = body(10);   // edit here
  w.cloud.put("b.txt", body(20));     // edit there
  auto r = w.sync();
  CHECK(w.ok() && r.uploaded == 1 && r.downloaded == 1 && r.conflicts == 0);
  CHECK(w.cloud.files["a.txt"].content == body(10));
  CHECK(w.disk.notes["b.txt"] == body(20));
  auto r2 = w.sync();
  CHECK(r2.uploaded == 0 && r2.downloaded == 0);
}

static void testConflict() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "base";
  w.sync();
  w.disk.notes["a.txt"] = "local edit";
  w.cloud.put("a.txt", "cloud edit");
  auto r = w.sync();
  CHECK(w.ok() && r.conflicts == 1 && r.errors == 0);
  // the cloud version keeps the name; this device's version is kept as a_<device> and pushed up
  CHECK(w.disk.notes["a.txt"] == "cloud edit");
  CHECK(w.cloud.files["a.txt"].content == "cloud edit");
  CHECK(w.disk.notes.count("a_device.txt") && w.disk.notes["a_device.txt"] == "local edit");
  CHECK(w.cloud.files.count("a_device.txt") && w.cloud.files["a_device.txt"].content == "local edit");
  CHECK(r.summary() == "1 uploaded, 1 conflict (kept both)\nThis device's copy ends in _device");
  // nothing is lost and the next sync is quiet
  auto r2 = w.sync();
  CHECK(r2.uploaded == 0 && r2.downloaded == 0 && r2.conflicts == 0);
  // a second clash on the same note gets the next free name
  w.disk.notes["a.txt"] = "local edit 2";
  w.cloud.put("a.txt", "cloud edit 2");
  auto r3 = w.sync();
  CHECK(w.ok() && r3.conflicts == 1 && w.disk.notes["a_device_2.txt"] == "local edit 2");
  CHECK(w.disk.notes["a.txt"] == "cloud edit 2" && w.disk.notes["a_device.txt"] == "local edit");
}

static void testDeletes() {
  World w;
  w.signedIn();
  w.disk.notes["keep.txt"] = "k";
  w.disk.notes["gone-remote.txt"] = "g1";
  w.disk.notes["gone-local.txt"] = "g2";
  w.sync();
  CHECK(w.cloud.files.size() == 3);
  w.cloud.files.erase("gone-remote.txt");       // deleted in the cloud
  w.disk.notes.erase("gone-local.txt");         // deleted here
  auto r = w.sync();
  CHECK(w.ok() && r.deletedLocal == 1 && r.deletedRemote == 1);
  CHECK(!w.disk.notes.count("gone-remote.txt") && w.disk.trash.count("gone-remote.txt"));  // trash, never a hard delete
  CHECK(!w.cloud.files.count("gone-local.txt"));
  CHECK(w.disk.notes.count("keep.txt") && w.cloud.files.count("keep.txt"));
  auto r2 = w.sync();
  CHECK(r2.deletedLocal == 0 && r2.deletedRemote == 0 && w.disk.notes.size() == 1);
}

// Switching a note between .txt and .md is a rename on the device (work order 1.10): the cloud ends up with the new
// name and the same text, and the old name is removed there (the providers keep it recoverable).
static void testFormatSwitchIsARename() {
  World w;
  w.signedIn();
  w.disk.notes["chapter.txt"] = "the text";
  w.sync();
  CHECK(w.cloud.files.count("chapter.txt"));
  w.disk.notes.erase("chapter.txt");
  w.disk.notes["chapter.md"] = "the text";
  auto r = w.sync();
  CHECK(w.ok() && r.uploaded == 1 && r.deletedRemote == 1);
  CHECK(w.cloud.files.count("chapter.md") && !w.cloud.files.count("chapter.txt"));
  auto r2 = w.sync();
  CHECK(r2.uploaded == 0 && r2.deletedRemote == 0 && r2.downloaded == 0 && w.disk.notes.size() == 1);
}

static void testDeleteVersusChange() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "1";
  w.disk.notes["b.txt"] = "2";
  w.sync();
  w.cloud.files.erase("a.txt");        // deleted in the cloud...
  w.disk.notes["a.txt"] = "1 edited";  // ...but edited here: the change wins, the note comes back
  w.disk.notes.erase("b.txt");         // deleted here...
  w.cloud.put("b.txt", "2 edited");    // ...but edited in the cloud: the change wins
  auto r = w.sync();
  CHECK(w.ok() && r.deletedLocal == 0 && r.deletedRemote == 0);
  CHECK(w.cloud.files.count("a.txt") && w.cloud.files["a.txt"].content == "1 edited");
  CHECK(w.disk.notes["b.txt"] == "2 edited");
}

// ---- first sync and lost state ---------------------------------------------------------------------------------

static void testLostStateNeverDeletes() {
  World w;
  w.signedIn();
  w.disk.notes["same.txt"] = "same";
  w.disk.notes["only-local.txt"] = "L";
  w.cloud.put("same.txt", "same");
  w.cloud.put("only-remote.txt", "R");
  w.cloud.put("differs.txt", "cloud");
  w.disk.notes["differs.txt"] = "local";
  auto r = w.sync();
  CHECK(w.ok());
  CHECK(r.deletedLocal == 0 && r.deletedRemote == 0);
  CHECK(r.unchanged == 1);                 // same.txt: identical, adopted without a transfer
  CHECK(r.conflicts == 1);                 // differs.txt kept both: differs_device.txt is ours
  CHECK(w.disk.notes.count("only-local.txt") && w.cloud.files.count("only-local.txt"));
  CHECK(w.disk.notes.count("only-remote.txt") && w.cloud.files.count("only-remote.txt"));
  CHECK(w.disk.notes["differs.txt"] == "cloud" && w.cloud.files["differs.txt"].content == "cloud");
  CHECK(w.disk.notes["differs_device.txt"] == "local" && w.cloud.files["differs_device.txt"].content == "local");
  // a corrupt state file behaves the same: nothing deleted
  w.disk.state = "garbage{";
  w.disk.notes.erase("only-remote.txt");
  w.cloud.files.erase("only-local.txt");
  auto r2 = w.sync();
  CHECK(w.ok() && r2.deletedLocal == 0 && r2.deletedRemote == 0);
  CHECK(w.cloud.files.count("only-remote.txt"));   // re-downloaded, not deleted
  CHECK(w.disk.notes.count("only-local.txt"));     // re-uploaded, not deleted
}

static void testMassDeleteGuard() {
  World w;
  w.signedIn();
  for (int i = 0; i < 10; i++) w.disk.notes["n" + std::to_string(i) + ".txt"] = body(i);
  w.sync();
  w.cloud.files.clear();   // cloud folder suddenly empty (wrong account, renamed folder...)
  auto r = w.sync();
  CHECK(w.ok() && r.deletedLocal == 0 && r.held == 10);
  CHECK(w.disk.notes.size() == 10 && w.disk.trash.empty());
  // and the reverse: the SD card comes back empty
  World v;
  v.signedIn();
  for (int i = 0; i < 10; i++) v.disk.notes["n" + std::to_string(i) + ".txt"] = body(i);
  v.sync();
  v.disk.notes.clear();
  auto r2 = v.sync();
  CHECK(v.ok() && r2.deletedRemote == 0 && v.cloud.files.size() == 10);
}

// Notes the user deleted on the device (the notes list marks them) leave the cloud even when they are most of the
// notes; deletions nobody asked for are still held, also when both happen in one sync. A cloud edit still wins.
static void testDeletedHereIsNotHeld() {
  {
    World w;
    w.signedIn();
    for (int i = 0; i < 10; i++) w.disk.notes["n" + std::to_string(i) + ".txt"] = body(i);
    w.sync();
    std::vector<std::string> gone;
    for (int i = 1; i < 10; i++) {
      const std::string n = "n" + std::to_string(i) + ".txt";
      w.disk.notes.erase(n);
      gone.push_back(n);
    }
    w.eng->markDeletedHere(gone);
    CHECK(w.disk.state.find("\"del\":1") != std::string::npos);   // the mark is on the card
    w.build();                                                     // and survives a restart
    auto r = w.sync();
    CHECK(w.ok() && r.deletedRemote == 9 && r.held == 0);
    CHECK(w.cloud.files.size() == 1 && w.cloud.files.count("n0.txt"));
  }
  {
    World w;   // 2 deleted on purpose, 6 lost without a word: the 2 go, the 6 are held
    w.signedIn();
    for (int i = 0; i < 10; i++) w.disk.notes["n" + std::to_string(i) + ".txt"] = body(i);
    w.sync();
    w.disk.notes.erase("n0.txt");
    w.disk.notes.erase("n1.txt");
    w.eng->markDeletedHere({"n0.txt", "n1.txt"});
    for (int i = 2; i < 8; i++) w.disk.notes.erase("n" + std::to_string(i) + ".txt");
    auto r = w.sync();
    CHECK(w.ok() && r.deletedRemote == 2 && r.held == 6);
    CHECK(w.cloud.files.size() == 8 && !w.cloud.files.count("n0.txt") && w.cloud.files.count("n2.txt"));
  }
  {
    World w;   // deleted here on purpose, edited in the cloud meanwhile: the edit comes back, without the mark
    w.signedIn();
    for (int i = 0; i < 6; i++) w.disk.notes["n" + std::to_string(i) + ".txt"] = body(i);
    w.sync();
    std::vector<std::string> gone;
    for (int i = 0; i < 6; i++) {
      w.disk.notes.erase("n" + std::to_string(i) + ".txt");
      gone.push_back("n" + std::to_string(i) + ".txt");
    }
    w.eng->markDeletedHere(gone);
    w.cloud.put("n5.txt", body(50));
    auto r = w.sync();
    CHECK(w.ok() && r.deletedRemote == 5 && r.downloaded == 1 && r.held == 0);
    CHECK(w.disk.notes.count("n5.txt") && w.disk.notes["n5.txt"] == body(50));
    CHECK(w.disk.state.find("\"del\":1") == std::string::npos);
  }
}

// A request that never answers is timed out; the HTTPS task can stay busy finishing it for a while. The next request
// waits for it instead of failing the sync with "Network is busy" (E1001, 2026-11-19: OneDrive deletes stalled).
static void testBusyTransportAfterTimeoutWaits() {
  World w;
  w.signedIn();
  for (int i = 0; i < 6; i++) w.disk.notes["n" + std::to_string(i) + ".txt"] = body(i);
  w.sync();
  w.disk.notes.erase("n1.txt");
  w.disk.notes.erase("n2.txt");
  w.cloud.hangNext = 1;           // the first cloud delete hangs
  w.cloud.hangMethod = "DELETE";
  w.cloud.busyAfterCancel = 20;   // and keeps the transport busy for 20 tries (~10 s) after the timeout
  auto r = w.sync();
  CHECK(w.ok() && r.deletedRemote == 1 && r.errors == 1);   // the timed-out delete failed, the run went on
  auto r2 = w.sync();                                         // and the next sync finishes it
  CHECK(w.ok() && r2.deletedRemote == 1 && r2.errors == 0);
  CHECK(w.cloud.files.size() == 4);
}

// ---- hostile / odd cloud content --------------------------------------------------------------------------------

static void testRemoteNamesAndFolders() {
  World w;
  w.signedIn();
  w.cloud.put("ok.txt", "fine");
  w.cloud.put("draft.md", "# an .md note");   // .md notes sync too (work order 1.10)
  w.cloud.folders.insert("Subfolder");
  w.cloud.weirdNames = {"../evil.txt", "notes.rtf", "a:b.txt", ".hidden.txt", "CON.txt"};
  auto r = w.sync();
  CHECK(w.ok() && r.downloaded == 2 && r.skippedRemote == 6);
  CHECK(w.disk.notes.size() == 2 && w.disk.notes.count("ok.txt") && w.disk.notes.count("draft.md"));
}

static void testPaging() {
  World w;
  w.signedIn();
  w.cloud.pageSize = 40;
  for (int i = 0; i < 130; i++) w.cloud.put("n" + std::to_string(i) + ".txt", body(i));
  auto r = w.sync();
  CHECK(w.ok() && r.downloaded == 130 && w.disk.notes.size() == 130);
}

static void testPagingLinkHostIsChecked() {
  World w;
  w.signedIn();
  w.cloud.put("a.txt", "x");
  w.cloud.evilNextLink = "https://evil.example/steal";
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::Cloud);
  for (auto& l : w.cloud.log) CHECK(l.find("evil.example") == std::string::npos);
  CHECK(w.disk.notes.empty());   // a failed listing must never lead to changes
}

static void testSizeMismatchAndLarge() {
  {
    World w;
    w.signedIn();
    w.cloud.put("a.txt", "twelve chars");
    w.cloud.badSize = true;
    auto r = w.sync();
    CHECK(w.ok() && r.errors == 1 && w.disk.notes.empty());
  }
  {
    World w;
    w.signedIn();
    w.cloud.put("big.txt", std::string(20000, 'x'));
    w.cloud.put("small.txt", "s");
    auto r = w.sync();
    CHECK(w.ok() && r.tooLarge == 1 && r.downloaded == 1);
    CHECK(!w.disk.notes.count("big.txt"));   // never written (the editor would truncate it)
    CHECK(w.cloud.files.count("big.txt"));   // and never touched in the cloud
    auto r2 = w.sync();                      // still skipped, and still not deleted
    CHECK(r2.deletedRemote == 0 && w.cloud.files.count("big.txt"));
  }
}

static void testUnreadableLocalIsNeverTreatedAsDeleted() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "a";
  w.disk.notes["b.txt"] = "b";
  w.sync();
  w.disk.unreadable.insert("a.txt");   // exists, but cannot be read (large file or SD hiccup)
  w.disk.notes.erase("a.txt");
  auto r = w.sync();
  CHECK(w.ok() && r.deletedRemote == 0 && w.cloud.files.count("a.txt"));
  CHECK(r.tooLarge == 1);
}

// ---- tokens ---------------------------------------------------------------------------------------------------

static void testExpiredAccessAndRotation() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "a";
  w.sync();
  const int savesBefore = w.tokens.saves;
  w.cloud.expireAccess();            // server drops the access token: first call gets 401
  w.disk.notes["a.txt"] = "a2";
  auto r = w.sync();
  CHECK(w.ok() && r.uploaded == 1 && r.errors == 0);
  CHECK(w.tokens.saves > savesBefore);
  CHECK(w.tokens.t == w.cloud.refreshToken);  // rotated token was stored
}

static void testRefreshRefused() {
  World w;
  w.signedIn();
  w.cloud.refreshToken = "someone-else";   // server no longer accepts what the device holds
  w.disk.notes["a.txt"] = "a";
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::SignInExpired);
  CHECK(w.tokens.t.empty());                 // cleared so the UI asks for a new sign-in
  CHECK(w.cloud.files.empty());
}

// ---- open note, failures, resume ------------------------------------------------------------------------------

static void testOpenNoteIsNeverReplaced() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "1";
  w.disk.notes["b.txt"] = "2";
  w.sync();
  w.disk.openNote = "a.txt";
  w.cloud.put("a.txt", "remote edit of the open note");
  w.cloud.files.erase("b.txt");
  w.disk.openNote = "a.txt";
  auto r = w.sync();
  CHECK(w.ok() && r.held == 1 && r.deletedLocal == 1);
  CHECK(w.disk.notes["a.txt"] == "1");        // untouched while open
  w.disk.openNote = "b.txt";
  w.disk.notes["b.txt"] = "2";               // pretend b is open and still present
  w.cloud.files.erase("b.txt");
  // (b was already removed above; re-create state for the delete-while-open case)
  World v;
  v.signedIn();
  v.disk.notes["x.txt"] = "x";
  v.disk.notes["y.txt"] = "y";
  v.sync();
  v.cloud.files.erase("x.txt");
  v.disk.openNote = "x.txt";
  auto r2 = v.sync();
  CHECK(r2.held == 1 && v.disk.notes.count("x.txt") && v.disk.trash.empty());
  v.disk.openNote.clear();                   // note closed: the held deletion now happens
  auto r3 = v.sync();
  CHECK(r3.deletedLocal == 1 && !v.disk.notes.count("x.txt"));
}

static void testConflictOnOpenNoteWaits() {
  World w;
  w.cfg.deviceTag = "e1001";
  w.build();
  w.signedIn();
  w.disk.notes["c.txt"] = "base";
  w.sync();
  w.disk.notes["c.txt"] = "typed here";
  w.cloud.put("c.txt", "typed on the other device");
  w.disk.openNote = "c.txt";
  auto r = w.sync();
  CHECK(w.ok() && r.held == 1 && r.conflicts == 0);
  CHECK(w.disk.notes["c.txt"] == "typed here" && !w.disk.notes.count("c_e1001.txt"));   // untouched while open
  w.disk.openNote.clear();
  auto r2 = w.sync();
  CHECK(w.ok() && r2.conflicts == 1);
  CHECK(w.disk.notes["c.txt"] == "typed on the other device" && w.disk.notes["c_e1001.txt"] == "typed here");
  CHECK(w.cloud.files["c_e1001.txt"].content == "typed here");
}

static void testResumeAfterNetworkLoss() {
  World w;
  w.signedIn();
  for (int i = 0; i < 6; i++) w.disk.notes["n" + std::to_string(i) + ".txt"] = body(i);
  // let the listing and 2 uploads through, then lose the network
  w.eng->startSync();
  int puts = 0;
  while (w.eng->busy()) {
    w.clock.ms += 100;
    w.eng->step();
    if (w.cloud.puts >= 2 && puts == 0) { puts = 1; w.cloud.dropNext = 100; }
  }
  CHECK(w.eng->result().error == SyncEngine::Error::Network);
  CHECK(w.cloud.files.size() == 2);
  // the state file already records the two finished uploads
  SyncState s;
  CHECK(SyncState::fromJson(w.disk.state, s) && s.entries.size() == 2);
  w.cloud.dropNext = 0;
  const int putsBefore = w.cloud.puts;
  auto r = w.sync();
  CHECK(w.ok() && r.uploaded == 4);
  CHECK(w.cloud.puts - putsBefore == 4);      // the finished ones were not uploaded again
  CHECK(w.cloud.files.size() == 6);
}

static void testStorageFailuresStopSafely() {
  {
    World w;
    w.signedIn();
    w.cloud.put("a.txt", "a");
    w.disk.failWrite = true;
    w.sync();
    CHECK(w.eng->result().error == SyncEngine::Error::Storage);
  }
  {
    World w;
    w.signedIn();
    w.disk.failList = true;
    w.disk.notes["a.txt"] = "a";
    w.cloud.put("b.txt", "b");
    w.sync();
    CHECK(w.eng->result().error == SyncEngine::Error::Storage);
    CHECK(w.cloud.files.size() == 1);   // no partial view was ever acted on
  }
  {
    World w;
    w.signedIn();
    w.disk.failStateWrite = true;
    w.disk.notes["a.txt"] = "a";
    w.sync();
    CHECK(w.eng->result().error == SyncEngine::Error::Storage);
  }
}

static void testBusyAndCancel() {
  {
    World w;
    w.signedIn();
    w.disk.notes["a.txt"] = "a";
    w.cloud.forceStatus = 429;
    w.cloud.forceStatusCount = 1;
    w.sync();
    CHECK(w.eng->result().error == SyncEngine::Error::Busy);
  }
  {
    World w;
    w.signedIn();
    w.disk.notes["a.txt"] = "a";
    w.eng->startSync();
    for (int i = 0; i < 3; i++) { w.clock.ms += 100; w.eng->step(); }
    w.eng->cancel();
    CHECK(w.eng->result().error == SyncEngine::Error::Cancelled && !w.eng->busy());
  }
}

static void testPushOnly() {
  World w;
  w.cfg.pushOnly = true;
  w.build();
  w.signedIn();
  w.disk.notes["a.txt"] = "a";
  w.cloud.put("remote-only.txt", "r");
  w.sync();
  CHECK(w.ok() && w.cloud.files.count("a.txt") && !w.disk.notes.count("remote-only.txt"));
  w.cloud.put("a.txt", "cloud edit");
  w.disk.notes["a.txt"] = "local edit";
  auto r = w.sync();
  CHECK(w.ok() && r.conflicts == 1);
  CHECK(w.cloud.files["a.txt"].content == "cloud edit");   // the cloud copy is never overwritten
  bool uploadedCopy = false;
  for (auto& kv : w.cloud.files) if (kv.first == "a_device.txt") uploadedCopy = kv.second.content == "local edit";
  CHECK(uploadedCopy);
}

static void testSignOut() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "a";
  w.sync();
  CHECK(w.disk.state.find("a.txt") != std::string::npos);
  w.eng->signOut();
  CHECK(w.tokens.t.empty());
  SyncState s;
  CHECK(SyncState::fromJson(w.disk.state, s) && s.entries.empty());
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::NotSignedIn);
}

static void testUtf8Names() {
  World w;
  w.signedIn();
  w.disk.notes["Привет мир (draft) 100%.txt"] = "x";
  w.cloud.put("Über Straße.txt", "y");
  auto r = w.sync();
  CHECK(w.ok() && r.uploaded == 1 && r.downloaded == 1);
  CHECK(w.cloud.files.count("Привет мир (draft) 100%.txt") && w.disk.notes.count("Über Straße.txt"));
}

// ---- a note opened during a sync goes to the front ----------------------------------------------------------

static void stepUntil(World& w, const std::function<bool()>& done, int maxSteps = 20000) {
  for (int i = 0; i < maxSteps && !done(); i++) { w.clock.ms += 100; w.eng->step(); }
}

static void testPrioritizedNoteGoesNext() {
  World w;
  w.signedIn();
  for (int i = 0; i < 6; i++) w.cloud.put("n" + std::to_string(i) + ".txt", body(i));
  w.eng->startSync();
  CHECK(!w.eng->settled("n5.txt"));   // listing: the plan is not known yet
  stepUntil(w, [&] { return w.eng->phase() == SyncEngine::Phase::Executing; });
  CHECK(w.eng->phase() == SyncEngine::Phase::Executing);
  CHECK(!w.eng->settled("n5.txt"));
  w.eng->prioritize("n5.txt");
  stepUntil(w, [&] { return w.eng->settled("n5.txt"); });
  CHECK(w.disk.notes.count("n5.txt") == 1 && w.disk.notes["n5.txt"] == body(5));
  CHECK(w.disk.notes.size() <= 2);    // at most the transfer already running, then n5
  CHECK(w.eng->busy());               // the rest of the sync carries on
  w.run();
  CHECK(w.ok() && w.disk.notes.size() == 6 && w.eng->result().downloaded == 6);
  CHECK(w.eng->settled("n5.txt"));
}

static void testPriorityBeforeThePlan() {
  World w;
  w.signedIn();
  for (int i = 0; i < 4; i++) w.cloud.put("n" + std::to_string(i) + ".txt", body(i));
  w.eng->prioritize("n3.txt");        // opened while the sync was still connecting
  w.eng->startSync();
  stepUntil(w, [&] { return w.eng->settled("n3.txt"); });
  CHECK(w.disk.notes.size() == 1 && w.disk.notes.count("n3.txt") == 1);   // first of all
  w.eng->prioritize("");
  w.run();
  CHECK(w.ok() && w.disk.notes.size() == 4);
  // A note with nothing to do is settled as soon as the plan exists.
  w.disk.notes["mine.txt"] = body(9);
  w.eng->startSync();
  stepUntil(w, [&] { return w.eng->phase() == SyncEngine::Phase::Executing || !w.eng->busy(); });
  CHECK(w.eng->settled("n0.txt"));
  CHECK(!w.eng->settled("mine.txt") || !w.eng->busy());
  w.run();
  CHECK(w.ok() && w.cloud.files["mine.txt"].content == body(9));
}

// The plan comes from the scan, and the editor autosaves during a background sync: a note saved after the scan read
// it must not be overwritten (Download) or trashed (DeleteLocal) on the scan's word.
static void scanThenEdit(World& w, const std::function<void()> edit) {
  w.eng->startSync();
  stepUntil(w, [&] { return w.eng->phase() == SyncEngine::Phase::Scanning; });
  for (int i = 0; i < 3; i++) { w.clock.ms += 100; w.eng->step(); }   // a.txt (first by name) is read
  CHECK(w.eng->phase() == SyncEngine::Phase::Scanning);                // the z*.txt notes are still to come
  edit();
  w.run();
}

static void testSavedAfterScanIsKept() {
  {  // the cloud changed it, and so did the writer after the scan: both copies are kept
    World w;
    w.signedIn();
    w.disk.notes["a.txt"] = "base";
    for (int i = 0; i < 8; i++) w.disk.notes["z" + std::to_string(i) + ".txt"] = body(i);
    w.sync();
    w.cloud.put("a.txt", "cloud edit");
    scanThenEdit(w, [&] { w.disk.notes["a.txt"] = "saved after the scan"; });
    CHECK(w.ok() && w.eng->result().conflicts == 1 && w.eng->result().downloaded == 0);
    CHECK(w.disk.notes["a.txt"] == "cloud edit");
    CHECK(w.disk.notes["a_device.txt"] == "saved after the scan");
    CHECK(w.cloud.files["a_device.txt"].content == "saved after the scan");
  }
  {  // deleted in the cloud, but saved here after the scan: kept, and the next sync puts it back in the cloud
    World w;
    w.signedIn();
    w.disk.notes["a.txt"] = "base";
    for (int i = 0; i < 8; i++) w.disk.notes["z" + std::to_string(i) + ".txt"] = body(i);
    w.sync();
    w.cloud.files.erase("a.txt");
    scanThenEdit(w, [&] { w.disk.notes["a.txt"] = "saved after the scan"; });
    CHECK(w.ok() && w.eng->result().held == 1 && w.eng->result().deletedLocal == 0);
    CHECK(w.disk.notes["a.txt"] == "saved after the scan" && w.disk.trash.empty());
    auto r = w.sync();
    CHECK(w.ok() && r.uploaded == 1 && w.cloud.files["a.txt"].content == "saved after the scan");
  }
  {  // unchanged after the scan: the download and the delete still happen
    World w;
    w.signedIn();
    w.disk.notes["a.txt"] = "base";
    w.disk.notes["b.txt"] = "bee";
    for (int i = 0; i < 8; i++) w.disk.notes["z" + std::to_string(i) + ".txt"] = body(i);
    w.sync();
    w.cloud.put("a.txt", "cloud edit");
    w.cloud.files.erase("b.txt");
    auto r = w.sync();
    CHECK(w.ok() && r.downloaded == 1 && r.deletedLocal == 1 && r.conflicts == 0);
    CHECK(w.disk.notes["a.txt"] == "cloud edit" && !w.disk.notes.count("b.txt"));
  }
}

int main() {
  testSignIn();
  testSignInFailures();
  testNotSignedIn();
  testFirstSyncAndIdle();
  testEditsBothWays();
  testConflict();
  testDeletes();
  testDeleteVersusChange();
  testLostStateNeverDeletes();
  testMassDeleteGuard();
  testDeletedHereIsNotHeld();
  testBusyTransportAfterTimeoutWaits();
  testRemoteNamesAndFolders();
  testFormatSwitchIsARename();
  testPaging();
  testPagingLinkHostIsChecked();
  testSizeMismatchAndLarge();
  testUnreadableLocalIsNeverTreatedAsDeleted();
  testExpiredAccessAndRotation();
  testRefreshRefused();
  testOpenNoteIsNeverReplaced();
  testConflictOnOpenNoteWaits();
  testResumeAfterNetworkLoss();
  testStorageFailuresStopSafely();
  testBusyAndCancel();
  testPushOnly();
  testSignOut();
  testUtf8Names();
  testPrioritizedNoteGoesNext();
  testPriorityBeforeThePlan();
  testSavedAfterScanIsKept();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
