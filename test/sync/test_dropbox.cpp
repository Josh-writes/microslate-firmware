// Tests for the Dropbox provider (code entry sign-in with PKCE) against an in-process fake of Dropbox.
// Run with: sh test/sync/run-on-hinton.sh
#include "dropbox.h"
#include "fakes.h"
#include "sync_pkce.h"

#include <cstdio>

using namespace msync;
using namespace fk;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                      \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }       \
  } while (0)

static std::string hex(const std::string& raw) {
  static const char* d = "0123456789abcdef";
  std::string o;
  for (unsigned char c : raw) { o += d[c >> 4]; o += d[c & 15]; }
  return o;
}

static std::string decodeUrl(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
    else o += s[i];
  }
  return o;
}

static std::string param(const std::string& s, const std::string& key) {
  size_t p = s.find(key + "=");
  while (p != std::string::npos && p > 0 && s[p - 1] != '&' && s[p - 1] != '?') p = s.find(key + "=", p + 1);
  if (p == std::string::npos) return "";
  size_t e = s.find('&', p);
  return decodeUrl(s.substr(p + key.size() + 1, e == std::string::npos ? std::string::npos : e - p - key.size() - 1));
}

// Dropbox login + API v2 (files in the app folder, addressed by path), just enough for the provider.
struct FakeDropbox : Transport {
  struct File { std::string id, name, content; int rev = 1; };
  std::vector<File> files;
  int idCounter = 1, tokenSerial = 0, pageSize = 2000, revCounter = 100;
  std::string refreshToken, accessToken, challenge, goodCode = "GOODCODE", usedVerifier;
  bool apiArgNotAscii = false, wrongContentType = false, bearerMissing = false, sawSecret = false;
  int uploads = 0, downloads = 0, deletes = 0, refreshes = 0, codeTries = 0;
  // The sign-in relay (a Cloudflare Worker): holds the code the phone got until the device asks for it.
  std::string relayBase = "https://relay.test", relaySession, relayCode;
  int polls = 0;
  bool relayDown = false, goodNeedsRedirect = false;
  bool have_ = false;
  HttpResponse resp_;
  std::string lastArg;

  bool begin(const HttpRequest& r) override { if (have_) return false; resp_ = process(r); have_ = true; return true; }
  bool poll(HttpResponse& out) override { if (!have_) return false; out = resp_; have_ = false; return true; }
  void cancel() override { have_ = false; }

  static HttpResponse json(int status, const std::string& body) { HttpResponse r; r.status = status; r.body = body; return r; }
  File* find(const std::string& name) { for (auto& f : files) if (f.name == name) return &f; return nullptr; }
  File& add(const std::string& name, const std::string& content) {
    File f; f.id = "id:" + std::to_string(idCounter++); f.name = name; f.content = content; f.rev = ++revCounter;
    files.push_back(f);
    return files.back();
  }
  std::string meta(const File& f) {
    return "{\".tag\":\"file\",\"name\":\"" + jsonEscape(f.name) + "\",\"id\":\"" + f.id + "\",\"rev\":\"r" + std::to_string(f.rev) +
           "\",\"size\":" + std::to_string(f.content.size()) + ",\"is_downloadable\":true}";
  }
  HttpResponse issue(bool withRefresh) {
    tokenSerial++;
    accessToken = "AT" + std::to_string(tokenSerial);
    std::string body = "{\"access_token\":\"" + accessToken + "\",\"expires_in\":14400,\"token_type\":\"bearer\"";
    if (withRefresh) { refreshToken = "RT" + std::to_string(tokenSerial); body += ",\"refresh_token\":\"" + refreshToken + "\""; }
    return json(200, body + "}");
  }
  static std::string argOf(const HttpRequest& r) {
    for (auto& h : r.headers) if (h.first == "Dropbox-API-Arg") return h.second;
    return "";
  }

  HttpResponse process(const HttpRequest& r) {
    if (r.url.compare(0, relayBase.size(), relayBase) == 0) {
      polls++;
      if (relayDown) return json(0, "");
      const std::string want = relayBase + "/p?s=" + relaySession;
      if (!relaySession.empty() && !relayCode.empty() && r.url == want) return json(200, "{\"code\":\"" + relayCode + "\"}");
      return json(204, "");
    }
    if (r.url == "https://api.dropboxapi.com/oauth2/token") {
      if (r.body.find("client_secret") != std::string::npos) sawSecret = true;
      if (param(r.body, "grant_type") == "refresh_token") {
        if (refreshToken.empty() || param(r.body, "refresh_token") != refreshToken) return json(400, "{\"error\":\"invalid_grant\"}");
        refreshes++;
        return issue(false);   // Dropbox does not rotate refresh tokens
      }
      codeTries++;
      usedVerifier = param(r.body, "code_verifier");
      if (param(r.body, "code") != goodCode) return json(400, "{\"error\":\"invalid_grant\",\"error_description\":\"code has been used or expired\"}");
      // Dropbox insists that a code made with a redirect address is exchanged with the same address, and only then.
      if ((param(r.body, "redirect_uri") == relayBase + "/cb") != goodNeedsRedirect) return json(400, "{\"error\":\"invalid_grant\",\"error_description\":\"redirect mismatch\"}");
      if (base64Url(sha256(usedVerifier)) != challenge) return json(400, "{\"error\":\"invalid_grant\",\"error_description\":\"bad verifier\"}");
      return issue(true);
    }
    if (r.bearer != accessToken || accessToken.empty()) { bearerMissing = true; return json(401, "{\"error\":\"expired_access_token\"}"); }

    const std::string arg = argOf(r);
    lastArg = arg;
    for (unsigned char c : arg) if (c >= 0x80) apiArgNotAscii = true;

    if (r.url == "https://api.dropboxapi.com/2/users/get_current_account")
      return json(200, "{\"email\":\"josh@example.com\",\"name\":{\"display_name\":\"Josh H\"}}");

    if (r.url == "https://api.dropboxapi.com/2/files/list_folder" || r.url == "https://api.dropboxapi.com/2/files/list_folder/continue") {
      Json j;
      if (!jsonParse(r.body, j)) return json(400, "{}");
      size_t start = 0;
      if (r.url.find("continue") != std::string::npos) start = (size_t)atoi(j.str("cursor").c_str());
      else if (j.str("path", "x") != "") return json(409, "{}");
      std::string body = "{\"entries\":[";
      size_t i = start, emitted = 0;
      for (; i < files.size() && emitted < (size_t)pageSize; i++, emitted++) body += (emitted ? "," : "") + meta(files[i]);
      body += "],\"cursor\":\"" + std::to_string(i) + "\",\"has_more\":" + (i < files.size() ? "true" : "false") + "}";
      return json(200, body);
    }

    if (r.url == "https://content.dropboxapi.com/2/files/upload") {
      if (r.contentType != "application/octet-stream") wrongContentType = true;
      Json j;
      if (!jsonParse(arg, j) || j.str("mode") != "overwrite") return json(400, "{}");
      std::string path = j.str("path");
      if (path.empty() || path[0] != '/') return json(409, "{}");
      const std::string name = path.substr(1);
      File* f = find(name);
      if (!f) f = &add(name, r.body);
      else { f->content = r.body; f->rev = ++revCounter; }
      uploads++;
      return json(200, meta(*f));
    }

    if (r.url == "https://content.dropboxapi.com/2/files/download") {
      if (r.contentType != "application/octet-stream") wrongContentType = true;
      Json j;
      if (!jsonParse(arg, j)) return json(400, "{}");
      File* f = find(j.str("path").substr(1));
      if (!f) return json(409, "{\"error_summary\":\"path/not_found/\"}");
      downloads++;
      return json(200, f->content);
    }

    if (r.url == "https://api.dropboxapi.com/2/files/delete_v2") {
      Json j;
      if (!jsonParse(r.body, j)) return json(400, "{}");
      const std::string name = j.str("path").substr(1);
      for (size_t i = 0; i < files.size(); i++)
        if (files[i].name == name) { files.erase(files.begin() + i); deletes++; return json(200, "{\"metadata\":{}}"); }
      return json(409, "{\"error_summary\":\"path_lookup/not_found/\"}");
    }
    return json(404, "{}");
  }
};

struct World {
  DropboxProvider provider;
  FakeDropbox cloud;
  FakeStorage disk;
  FakeTokens tokens;
  FakeClock clock;
  SyncEngine::Config cfg;
  SyncEngine* eng = nullptr;

  explicit World(bool relay = false) : provider(relay ? "https://relay.test" : "") {
    cfg.clientId = "APPKEY123";
    cfg.maxNoteBytes = 262143;
    build();
  }
  ~World() { delete eng; }
  void build() { delete eng; eng = new SyncEngine(provider, cloud, disk, tokens, clock, cfg); }
  void signedIn() { cloud.tokenSerial = 0; cloud.issue(true); tokens.t = cloud.refreshToken; }
  int run(int maxSteps = 20000) {
    int i = 0;
    while (eng->busy() && i < maxSteps) { clock.ms += 100; eng->step(); i++; }
    return i;
  }
  void sync() { eng->startSync(); run(); }
  // Start code entry sign-in and tell the fake which challenge the user's approval carried.
  void beginSignIn() {
    eng->startSignIn();
    cloud.challenge = param(eng->status().verifyUri, "code_challenge");
  }
};

static void testPkceVectors() {
  CHECK(hex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(hex(sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(hex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  CHECK(hex(sha256(std::string(1000, 'a'))) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
  // RFC 7636 appendix B
  CHECK(pkceChallenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  CHECK(base64Url("") == "" && base64Url("f") == "Zg" && base64Url("fo") == "Zm8" && base64Url("foo") == "Zm9v");
  uint32_t seed = 1;
  const std::string v = makeVerifier([&]() { seed = seed * 1664525u + 1013904223u; return seed; });
  CHECK(v.size() == 64);
  bool okChars = true;
  for (char c : v) if (!isalnum((unsigned char)c) && c != '-' && c != '.' && c != '_' && c != '~') okChars = false;
  CHECK(okChars);
}

static void testAuthorizeUrl() {
  DropboxProvider p;
  const std::string u = p.authorizeUrl("APPKEY123", "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk");
  CHECK(u.compare(0, 41, "https://www.dropbox.com/oauth2/authorize?") == 0);
  CHECK(param(u, "client_id") == "APPKEY123");
  CHECK(param(u, "response_type") == "code");
  CHECK(param(u, "code_challenge") == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
  CHECK(param(u, "code_challenge_method") == "S256");
  CHECK(param(u, "token_access_type") == "offline");
  CHECK(u.size() < 230);   // must fit a version 9 QR code at the lowest error correction
}

static void testCodeEntrySignIn() {
  World w;
  w.beginSignIn();
  CHECK(w.eng->busy());
  CHECK(w.eng->status().phase == SyncEngine::Phase::WaitingForUser);
  CHECK(w.eng->status().needsCode);
  CHECK(!w.eng->submitCode("   "));            // nothing typed
  CHECK(w.eng->submitCode("WRONG"));
  w.run(50);
  CHECK(w.eng->status().phase == SyncEngine::Phase::WaitingForUser);   // a wrong code is not the end
  CHECK(!w.eng->status().codeError.empty());
  CHECK(w.tokens.t.empty());
  CHECK(w.eng->submitCode("  GOODCODE \t"));   // spaces around the pasted code are ignored
  w.run();
  CHECK(w.eng->finished());
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.tokens.t == w.cloud.refreshToken && !w.tokens.t.empty());
  CHECK(w.eng->status().account == "josh@example.com");
  CHECK(!w.eng->status().needsCode);
  CHECK(!w.cloud.sawSecret);
  CHECK(w.cloud.codeTries == 2);
}

static void testCodeEntryExpiresAndCancels() {
  World w;
  w.beginSignIn();
  w.clock.ms += w.cfg.codeEntryMs + 1000;
  w.eng->step();
  CHECK(w.eng->result().error == SyncEngine::Error::SignInExpired);
  CHECK(!w.eng->submitCode("GOODCODE"));      // too late

  World w2;
  w2.beginSignIn();
  w2.eng->cancel();
  CHECK(w2.eng->result().error == SyncEngine::Error::Cancelled);
  CHECK(!w2.eng->submitCode("GOODCODE"));
  CHECK(w2.tokens.t.empty());
}

static void testEachSignInHasItsOwnVerifier() {
  World w;
  w.beginSignIn();
  const std::string a = param(w.eng->status().verifyUri, "code_challenge");
  w.eng->cancel();
  w.clock.ms += 7;
  w.beginSignIn();
  const std::string b = param(w.eng->status().verifyUri, "code_challenge");
  CHECK(!a.empty() && !b.empty() && a != b);
}

static void testFirstSyncUploads() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.disk.notes["b.txt"] = "world";
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.cloud.uploads == 2);
  CHECK(w.cloud.find("a.txt") && w.cloud.find("a.txt")->content == "hello");
  CHECK(!w.cloud.wrongContentType);
  CHECK(w.tokens.t == w.cloud.refreshToken);   // no rotation: the stored token survives refreshes
}

static void testSecondSyncIsQuiet() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  w.cloud.uploads = 0;
  w.build();
  w.sync();
  CHECK(w.cloud.uploads == 0 && w.cloud.downloads == 0);
  CHECK(w.eng->result().uploaded == 0 && w.eng->result().downloaded == 0);
}

static void testLocalChangeOverwrites() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  w.disk.notes["a.txt"] = "hello again";
  w.sync();
  CHECK(w.cloud.files.size() == 1 && w.cloud.find("a.txt")->content == "hello again");
  CHECK(w.cloud.uploads == 2);
}

static void testRemoteChangeAndNewFile() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  w.cloud.find("a.txt")->content = "edited elsewhere";
  w.cloud.find("a.txt")->rev = ++w.cloud.revCounter;
  w.cloud.add("b.txt", "made on another device");
  w.sync();
  CHECK(w.disk.notes["a.txt"] == "edited elsewhere");
  CHECK(w.disk.notes["b.txt"] == "made on another device");
  CHECK(w.eng->result().downloaded == 2);
}

static void testDeletes() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.disk.notes["b.txt"] = "keep";
  w.sync();
  w.disk.notes.erase("a.txt");
  w.sync();
  CHECK(w.cloud.deletes == 1 && w.cloud.find("a.txt") == nullptr && w.cloud.find("b.txt") != nullptr);
  w.cloud.files.erase(w.cloud.files.begin());   // b.txt deleted from Dropbox
  w.sync();
  CHECK(w.disk.notes.count("b.txt") == 0 && w.disk.trash.count("b.txt") == 1);
}

static void testPagingByCursor() {
  World w;
  w.signedIn();
  w.cloud.pageSize = 3;
  for (int i = 0; i < 8; i++) w.cloud.add("n" + std::to_string(i) + ".txt", "c" + std::to_string(i));
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.disk.notes.size() == 8);
}

static void testNonNotesSkipped() {
  World w;
  w.signedIn();
  w.cloud.add("real.txt", "x");
  w.cloud.add("picture.png", "y");
  w.sync();
  CHECK(w.disk.notes.count("real.txt") == 1 && w.disk.notes.count("picture.png") == 0);
  CHECK(w.eng->result().skippedRemote >= 1);
}

static void testUnicodeNameTravelsAsAscii() {
  World w;
  w.signedIn();
  w.disk.notes["caf\xC3\xA9 \xF0\x9F\x98\x80.txt"] = "bonjour";
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(!w.cloud.apiArgNotAscii);
  CHECK(w.cloud.find("caf\xC3\xA9 \xF0\x9F\x98\x80.txt") && w.cloud.find("caf\xC3\xA9 \xF0\x9F\x98\x80.txt")->content == "bonjour");
  w.cloud.add("na\xC3\xAFve \xF0\x9F\x98\x80.txt", "from elsewhere");   // and a Unicode name coming down
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.disk.notes["na\xC3\xAFve \xF0\x9F\x98\x80.txt"] == "from elsewhere");
  w.disk.notes.erase("caf\xC3\xA9 \xF0\x9F\x98\x80.txt");   // and a Unicode name being deleted
  w.sync();
  CHECK(w.cloud.deletes == 1 && w.cloud.find("caf\xC3\xA9 \xF0\x9F\x98\x80.txt") == nullptr);
}

static void testExpiredAccessRefreshes() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.cloud.accessToken = "STALE";
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.cloud.refreshes >= 1);
}

static void testRevokedRefreshToken() {
  World w;
  w.signedIn();
  w.cloud.refreshToken = "SOMETHING ELSE";   // revoked on the Dropbox website
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::SignInExpired);
  CHECK(w.tokens.t.empty());
}

static void testPagingLinkPolicy() {
  DropboxProvider p;
  CHECK(p.pagingLinkAllowed("AAEbc-_d=/+x"));
  CHECK(!p.pagingLinkAllowed(""));
  CHECK(!p.pagingLinkAllowed("https://evil.example/x"));
  CHECK(!p.pagingLinkAllowed("a b"));
  CHECK(!p.redirectAllowed("https://anything.example/"));
}

// Start a relay sign-in and tell the fake which session and challenge the phone's QR scan carried.
static void beginRelay(World& w) {
  w.eng->startSignIn();
  const std::string u = w.eng->status().verifyUri;
  w.cloud.relaySession = param(u, "s");
  w.cloud.challenge = param(u, "c");
}

static void testRelaySignIn() {
  World w(true);
  beginRelay(w);
  const std::string u = w.eng->status().verifyUri;
  CHECK(w.eng->status().relay && w.eng->status().needsCode);
  CHECK(u.compare(0, 23, "https://relay.test/a?s=") == 0);
  CHECK(u.size() < 130);   // short enough for a small, easy-to-scan QR code
  CHECK(w.cloud.relaySession.size() == 24 && w.cloud.challenge.size() == 43);
  w.cloud.goodNeedsRedirect = true;
  w.run(300);   // thirty seconds with no approval
  CHECK(w.eng->busy() && w.eng->status().relay);
  CHECK(w.cloud.polls >= 4 && w.cloud.polls <= 10);
  CHECK(w.tokens.t.empty());
  w.cloud.relayCode = "GOODCODE";   // the phone approved
  w.run();
  CHECK(w.eng->finished() && w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.tokens.t == w.cloud.refreshToken && !w.tokens.t.empty());
  CHECK(w.eng->status().account == "josh@example.com");
  CHECK(!w.eng->status().relay);
}

static void testRelayDownThenTypeInstead() {
  World w(true);
  beginRelay(w);
  const std::string relayChallenge = w.cloud.challenge;
  w.cloud.relayDown = true;
  w.run(300);
  CHECK(w.eng->busy());   // an unreachable relay is not a failed sign-in
  w.eng->useManualCode();
  const std::string u = w.eng->status().verifyUri;
  CHECK(!w.eng->status().relay);
  CHECK(u.compare(0, 41, "https://www.dropbox.com/oauth2/authorize?") == 0);
  CHECK(u.find("redirect_uri") == std::string::npos);   // Dropbox shows the code itself
  CHECK(param(u, "code_challenge") == relayChallenge);  // same secret, so either kind of code still works
  const int polls = w.cloud.polls;
  w.run(100);
  CHECK(w.cloud.polls == polls);                        // no more relay polling once the user types
  CHECK(w.eng->submitCode("GOODCODE"));
  w.run();
  CHECK(w.eng->finished() && w.eng->result().error == SyncEngine::Error::None);
}

static void testTypedCodeFromRelayPage() {
  World w(true);
  beginRelay(w);
  w.cloud.goodNeedsRedirect = true;   // the code the relay's own page shows was made with the redirect address
  w.eng->useManualCode();
  CHECK(w.eng->submitCode("GOODCODE"));
  w.run();
  CHECK(w.eng->finished() && w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.cloud.codeTries == 2);       // refused without the redirect address, accepted with it
}

static void testRelayRefusedCodeFallsBack() {
  World w(true);
  beginRelay(w);
  w.cloud.relayCode = "BADCODE";
  w.run(200);
  CHECK(w.eng->busy() && !w.eng->status().relay);       // switched to typing
  CHECK(!w.eng->status().codeError.empty());
  const int polls = w.cloud.polls;
  w.run(200);
  CHECK(w.cloud.polls == polls);                        // does not keep asking for the same bad code
  w.cloud.relayCode.clear();
  CHECK(w.eng->submitCode("GOODCODE"));
  w.run();
  CHECK(w.eng->finished() && w.eng->result().error == SyncEngine::Error::None);
}

static void testRelayUnreachableSaysSoAndRecovers() {
  World w(true);
  beginRelay(w);
  w.cloud.goodNeedsRedirect = true;
  w.cloud.relayDown = true;
  w.run(100);   // ten seconds: not enough failures to complain yet
  CHECK(w.eng->status().codeError.empty());
  w.run(400);   // another forty
  CHECK(w.eng->busy() && w.eng->status().relay);
  CHECK(!w.eng->status().codeError.empty());   // tells the user to press Type code, and keeps trying
  w.cloud.relayDown = false;
  w.run(200);
  CHECK(w.eng->status().codeError.empty());     // the relay is back: the message goes away
  w.cloud.relayCode = "GOODCODE";
  w.run();
  CHECK(w.eng->finished() && w.eng->result().error == SyncEngine::Error::None);
}

static void testRelayPollingSlowsDown() {
  World w(true);
  beginRelay(w);
  w.run(3000);   // five minutes with nobody approving
  CHECK(w.eng->busy());
  CHECK(w.cloud.polls >= 40 && w.cloud.polls <= 70);   // 3 s for the first minute, then 6 s
  w.run(4000);   // past the ten minute window
  CHECK(w.eng->finished() && w.eng->result().error == SyncEngine::Error::SignInExpired);
}

static void testNoRelayNeverPolls() {
  World w(false);
  w.beginSignIn();
  CHECK(!w.eng->status().relay);
  w.run(300);
  CHECK(w.cloud.polls == 0);
}

int main() {
  testRelaySignIn();
  testRelayDownThenTypeInstead();
  testTypedCodeFromRelayPage();
  testRelayRefusedCodeFallsBack();
  testRelayUnreachableSaysSoAndRecovers();
  testRelayPollingSlowsDown();
  testNoRelayNeverPolls();
  testPkceVectors();
  testAuthorizeUrl();
  testCodeEntrySignIn();
  testCodeEntryExpiresAndCancels();
  testEachSignInHasItsOwnVerifier();
  testFirstSyncUploads();
  testSecondSyncIsQuiet();
  testLocalChangeOverwrites();
  testRemoteChangeAndNewFile();
  testDeletes();
  testPagingByCursor();
  testNonNotesSkipped();
  testUnicodeNameTravelsAsAscii();
  testExpiredAccessRefreshes();
  testRevokedRefreshToken();
  testPagingLinkPolicy();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
