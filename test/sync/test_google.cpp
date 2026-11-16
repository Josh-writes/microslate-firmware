// Engine tests for the Google Drive provider, against an in-process fake of Google's login and Drive API.
// Run with: sh test/sync/run-on-hinton.sh
#include "fakes.h"
#include "google_drive.h"

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

static std::string decodeUrl(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
    else o += s[i];
  }
  return o;
}

static std::string hashOf(const std::string& c) {
  char b[24];
  snprintf(b, sizeof(b), "%016zx", std::hash<std::string>()(c));
  return b;
}

// Google login + Drive v3, just enough for the provider. Only files the app created are visible (drive.file).
struct FakeDrive : Transport {
  struct File { std::string id, name, parent, content; bool trashed = false; bool folder = false; };
  std::vector<File> items;
  int idCounter = 1, tokenSerial = 0, pageSize = 200;
  std::string refreshToken, accessToken;
  int pendingPolls = 0;
  bool declined = false;
  std::vector<std::string> log;
  int folderCreates = 0, fileCreates = 0, fileUpdates = 0, downloads = 0, trashes = 0, tokenRefreshes = 0;
  std::string lastCreateBody;
  bool bearerMissing = false;
  bool have_ = false;
  HttpResponse resp_;

  bool begin(const HttpRequest& r) override { if (have_) return false; resp_ = process(r); have_ = true; return true; }
  bool poll(HttpResponse& out) override { if (!have_) return false; out = resp_; have_ = false; return true; }
  void cancel() override { have_ = false; }

  static HttpResponse json(int status, const std::string& body) { HttpResponse r; r.status = status; r.body = body; return r; }
  File* byId(const std::string& id) { for (auto& f : items) if (f.id == id) return &f; return nullptr; }
  std::string folderId() { for (auto& f : items) if (f.folder && f.name == "MicroSlate" && !f.trashed) return f.id; return ""; }
  File& addFile(const std::string& name, const std::string& content) {
    File f; f.id = "f" + std::to_string(idCounter++); f.name = name; f.content = content; f.parent = folderId();
    items.push_back(f);
    return items.back();
  }
  File* find(const std::string& name) { for (auto& f : items) if (!f.folder && !f.trashed && f.name == name) return &f; return nullptr; }

  static std::string fileJson(const File& f) {
    return "{\"id\":\"" + f.id + "\",\"name\":\"" + jsonEscape(f.name) + "\",\"size\":\"" + std::to_string(f.content.size()) +
           "\",\"md5Checksum\":\"" + hashOf(f.content) + "\"}";
  }
  HttpResponse issue(bool withRefresh) {
    tokenSerial++;
    accessToken = "AT" + std::to_string(tokenSerial);
    std::string body = "{\"access_token\":\"" + accessToken + "\",\"expires_in\":3599,\"token_type\":\"Bearer\"";
    if (withRefresh) { refreshToken = "RT" + std::to_string(tokenSerial); body += ",\"refresh_token\":\"" + refreshToken + "\""; }
    return json(200, body + "}");
  }
  static std::string param(const std::string& url, const std::string& key) {
    size_t p = url.find(key + "=");
    if (p == std::string::npos) return "";
    size_t e = url.find('&', p);
    return decodeUrl(url.substr(p + key.size() + 1, e == std::string::npos ? std::string::npos : e - p - key.size() - 1));
  }

  HttpResponse process(const HttpRequest& r) {
    log.push_back(r.method + " " + r.url);
    const std::string auth = "https://oauth2.googleapis.com";
    if (r.url.compare(0, auth.size(), auth) == 0) {
      const std::string path = r.url.substr(auth.size());
      if (path == "/device/code") {
        if (r.body.find("scope=" + urlEncode("https://www.googleapis.com/auth/drive.file")) == std::string::npos) return json(400, "{\"error\":\"invalid_scope\"}");
        return json(200, "{\"device_code\":\"DC1\",\"user_code\":\"ABC-DEF-GHI\",\"verification_url\":\"https://www.google.com/device\",\"expires_in\":1800,\"interval\":5}");
      }
      if (path == "/token") {
        if (r.body.find("client_secret=SECRET") == std::string::npos) return json(401, "{\"error\":\"invalid_client\"}");
        if (r.body.find("grant_type=refresh_token") != std::string::npos) {
          if (refreshToken.empty() || r.body.find("refresh_token=" + urlEncode(refreshToken)) == std::string::npos)
            return json(400, "{\"error\":\"invalid_grant\"}");
          tokenRefreshes++;
          return issue(false);   // Google does not rotate: no new refresh token in the reply
        }
        if (declined) return json(403, "{\"error\":\"access_denied\"}");
        if (pendingPolls > 0) { pendingPolls--; return json(428, "{\"error\":\"authorization_pending\"}"); }
        return issue(true);
      }
      return json(404, "{}");
    }

    if (r.bearer != accessToken || accessToken.empty()) { bearerMissing = true; return json(401, "{}"); }
    const std::string api = "https://www.googleapis.com/drive/v3";
    const std::string up = "https://www.googleapis.com/upload/drive/v3";

    if (r.url.compare(0, api.size() + 6, api + "/about") == 0) return json(200, "{\"user\":{\"displayName\":\"Test User\",\"emailAddress\":\"josh@example.com\"}}");

    if (r.url.compare(0, api.size() + 6, api + "/files") == 0 && r.method == "GET" && r.url.find("alt=media") == std::string::npos) {
      const std::string q = param(r.url, "q");
      std::vector<File*> hits;
      if (q.find("application/vnd.google-apps.folder") != std::string::npos) {
        for (auto& f : items) if (f.folder && f.name == "MicroSlate" && !f.trashed) hits.push_back(&f);
        std::string body = "{\"files\":[";
        for (size_t i = 0; i < hits.size(); i++) body += (i ? "," : "") + std::string("{\"id\":\"") + hits[i]->id + "\",\"name\":\"MicroSlate\"}";
        return json(200, body + "]}");
      }
      const std::string parent = q.substr(1, q.find('\'', 1) - 1);
      for (auto& f : items) if (f.parent == parent && !f.trashed) hits.push_back(&f);
      size_t start = (size_t)atoi(param(r.url, "pageToken").c_str());
      std::string body = "{\"files\":[";
      size_t i = start, emitted = 0;
      for (; i < hits.size() && emitted < (size_t)pageSize; i++, emitted++) {
        const File& f = *hits[i];
        std::string row = f.folder ? "{\"id\":\"" + f.id + "\",\"name\":\"" + f.name + "\",\"mimeType\":\"application/vnd.google-apps.folder\"}"
                                   : fileJson(f);
        body += (emitted ? "," : "") + row;
      }
      body += "]";
      if (i < hits.size()) body += ",\"nextPageToken\":\"" + std::to_string(i) + "\"";
      return json(200, body + "}");
    }

    if (r.method == "POST" && r.url.compare(0, (api + "/files").size(), api + "/files") == 0) {   // create the folder
      File f; f.id = "d" + std::to_string(idCounter++); f.name = "MicroSlate"; f.folder = true;
      items.push_back(f);
      folderCreates++;
      return json(200, "{\"id\":\"" + f.id + "\"}");
    }

    if (r.method == "POST" && r.url.compare(0, (up + "/files?uploadType=multipart").size(), up + "/files?uploadType=multipart") == 0) {
      lastCreateBody = r.body;
      const size_t a = r.body.find("\r\n\r\n");
      const size_t b = r.body.find("\r\n--", a);
      const std::string meta = r.body.substr(a + 4, b - a - 4);
      Json j;
      if (!jsonParse(meta, j)) return json(400, "{}");
      const size_t c = r.body.find("\r\n\r\n", b);
      const size_t d = r.body.rfind("\r\n--");
      File f; f.id = "f" + std::to_string(idCounter++); f.name = j.str("name");
      const Json* parents = j.get("parents");
      if (parents && parents->isArray() && !parents->a.empty()) f.parent = parents->a[0].text();
      f.content = r.body.substr(c + 4, d - c - 4);
      items.push_back(f);
      fileCreates++;
      return json(200, fileJson(f));
    }

    if (r.method == "PATCH" && r.url.compare(0, (up + "/files/").size(), up + "/files/") == 0) {
      std::string id = r.url.substr((up + "/files/").size());
      id = id.substr(0, id.find('?'));
      File* f = byId(id);
      if (!f) return json(404, "{}");
      f->content = r.body;
      fileUpdates++;
      return json(200, fileJson(*f));
    }

    if (r.method == "PATCH" && r.url.compare(0, (api + "/files/").size(), api + "/files/") == 0) {   // trash
      std::string id = r.url.substr((api + "/files/").size());
      id = id.substr(0, id.find('?'));
      File* f = byId(id);
      if (!f) return json(404, "{}");
      if (r.body.find("\"trashed\":true") != std::string::npos) { f->trashed = true; trashes++; }
      return json(200, "{\"id\":\"" + id + "\"}");
    }

    if (r.method == "GET" && r.url.compare(0, (api + "/files/").size(), api + "/files/") == 0 && r.url.find("alt=media") != std::string::npos) {
      std::string id = r.url.substr((api + "/files/").size());
      id = id.substr(0, id.find('?'));
      File* f = byId(id);
      if (!f || f->trashed) return json(404, "{}");
      downloads++;
      return json(200, f->content);
    }
    return json(404, "{}");
  }
};

struct World {
  GoogleDriveProvider provider{"SECRET"};
  FakeDrive cloud;
  FakeStorage disk;
  FakeTokens tokens;
  FakeClock clock;
  SyncEngine::Config cfg;
  SyncEngine* eng = nullptr;

  World() { cfg.clientId = "GID.apps.googleusercontent.com"; cfg.maxNoteBytes = 262143; build(); }
  ~World() { delete eng; }
  void build() { delete eng; eng = new SyncEngine(provider, cloud, disk, tokens, clock, cfg); }
  void signedIn() { cloud.tokenSerial = 0; cloud.issue(true); tokens.t = cloud.refreshToken; }
  int run(int maxSteps = 20000) {
    int i = 0;
    while (eng->busy() && i < maxSteps) { clock.ms += 100; eng->step(); i++; }
    return i;
  }
  void sync() { eng->startSync(); run(); }
};

static void testSignIn() {
  World w;
  w.cloud.pendingPolls = 2;
  w.eng->startSignIn();
  w.run();
  CHECK(w.eng->finished());
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.tokens.t == w.cloud.refreshToken && !w.tokens.t.empty());
  CHECK(w.eng->status().account == "josh@example.com");
  CHECK(w.eng->status().verifyUri == "https://www.google.com/device");
}

static void testDeclined() {
  World w;
  w.cloud.declined = true;
  w.eng->startSignIn();
  w.run();
  CHECK(w.eng->result().error == SyncEngine::Error::SignInDeclined);
  CHECK(w.tokens.t.empty());
}

static void testFirstSyncCreatesFolderAndUploads() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.disk.notes["b.txt"] = "world";
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.cloud.folderCreates == 1);
  CHECK(w.cloud.fileCreates == 2);
  CHECK(w.cloud.find("a.txt") && w.cloud.find("a.txt")->content == "hello");
  CHECK(w.cloud.find("a.txt")->parent == w.cloud.folderId());
  CHECK(w.eng->result().uploaded == 2);
  // Google sends no new refresh token on refresh: the stored one must survive.
  CHECK(w.tokens.t == w.cloud.refreshToken);
}

static void testSecondSyncIsQuietAndReusesFolder() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  w.cloud.log.clear();
  w.build();   // a restart: the folder id is forgotten and must be found again, not created
  w.sync();
  CHECK(w.cloud.folderCreates == 1);
  CHECK(w.cloud.fileCreates == 1 && w.cloud.fileUpdates == 0 && w.cloud.downloads == 0);
  CHECK(w.eng->result().uploaded == 0 && w.eng->result().downloaded == 0);
}

static void testLocalChangeUpdatesInPlace() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  w.disk.notes["a.txt"] = "hello again";
  w.sync();
  CHECK(w.cloud.fileCreates == 1 && w.cloud.fileUpdates == 1);
  CHECK(w.cloud.find("a.txt")->content == "hello again");
}

static void testRemoteChangeAndNewFileDownload() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  w.cloud.find("a.txt")->content = "edited elsewhere";
  w.cloud.addFile("b.txt", "made on another device");
  w.sync();
  CHECK(w.disk.notes["a.txt"] == "edited elsewhere");
  CHECK(w.disk.notes["b.txt"] == "made on another device");
  CHECK(w.eng->result().downloaded == 2);
}

static void testDeleteLocalTrashesRemote() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.sync();
  w.disk.notes.erase("a.txt");
  w.sync();
  CHECK(w.cloud.trashes == 1);
  CHECK(w.cloud.find("a.txt") == nullptr);
  CHECK(w.eng->result().deletedRemote == 1);
}

static void testDeleteRemoteTrashesLocal() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.disk.notes["b.txt"] = "keep";
  w.sync();
  w.cloud.find("a.txt")->trashed = true;
  w.sync();
  CHECK(w.disk.notes.count("a.txt") == 0 && w.disk.trash.count("a.txt") == 1);
  CHECK(w.disk.notes.count("b.txt") == 1);
}

static void testDuplicateNamesAndFoldersSkipped() {
  World w;
  w.signedIn();
  w.sync();   // creates the folder
  w.cloud.addFile("dup.txt", "first");
  w.cloud.addFile("dup.txt", "second");
  FakeDrive::File sub; sub.id = "d99"; sub.name = "sub"; sub.folder = true; sub.parent = w.cloud.folderId();
  w.cloud.items.push_back(sub);
  w.cloud.addFile("notanote.pdf", "x");
  w.sync();
  CHECK(w.disk.notes.count("dup.txt") == 1 && w.disk.notes["dup.txt"] == "first");
  CHECK(w.disk.notes.count("notanote.pdf") == 0);
  CHECK(w.eng->result().skippedRemote >= 3);
}

static void testPaging() {
  World w;
  w.signedIn();
  w.sync();
  w.cloud.pageSize = 3;
  for (int i = 0; i < 8; i++) w.cloud.addFile("n" + std::to_string(i) + ".txt", "c" + std::to_string(i));
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.disk.notes.size() == 8);
}

static void testMultipartBodyAndNameEscaping() {
  World w;
  w.signedIn();
  w.disk.notes["café notes.txt"] = "line1\r\nline2\r\n--not a boundary";
  w.sync();
  CHECK(w.cloud.fileCreates == 1);
  FakeDrive::File* f = w.cloud.find("café notes.txt");
  CHECK(f && f->content == "line1\r\nline2\r\n--not a boundary");
}

static void testExpiredAccessRefreshes() {
  World w;
  w.signedIn();
  w.disk.notes["a.txt"] = "hello";
  w.cloud.accessToken = "STALE";   // the device still believes its (nonexistent) token; first request refreshes
  w.sync();
  CHECK(w.eng->result().error == SyncEngine::Error::None);
  CHECK(w.cloud.tokenRefreshes >= 1);
}

static void testSignOutForgetsFolder() {
  World w;
  w.signedIn();
  w.sync();
  const int created = w.cloud.folderCreates;
  w.eng->signOut();
  w.cloud.items.clear();      // a different account has no folder
  w.signedIn();
  w.sync();
  CHECK(w.cloud.folderCreates == created + 1);
}

int main() {
  testSignIn();
  testDeclined();
  testFirstSyncCreatesFolderAndUploads();
  testSecondSyncIsQuietAndReusesFolder();
  testLocalChangeUpdatesInPlace();
  testRemoteChangeAndNewFileDownload();
  testDeleteLocalTrashesRemote();
  testDeleteRemoteTrashesLocal();
  testDuplicateNamesAndFoldersSkipped();
  testPaging();
  testMultipartBodyAndNameEscaping();
  testExpiredAccessRefreshes();
  testSignOutForgetsFolder();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
