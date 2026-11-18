#pragma once
// In-process fakes for the sync engine tests: a small OneDrive/Microsoft-login, SD notes, tokens and a clock.
#include "onedrive.h"
#include "sync_engine.h"
#include "sync_json.h"

#include <algorithm>
#include <map>
#include <set>

namespace fk {
using namespace msync;

struct FakeClock : Clock {
  uint32_t ms = 1000;
  uint32_t epoch = 1790000000;  // 2026-10-21
  uint32_t millisNow() override { return ms; }
  uint32_t epochNow() override { return epoch; }
};

struct FakeTokens : TokenStore {
  std::string t;
  int saves = 0;
  bool failSave = false;
  std::string load() override { return t; }
  bool save(const std::string& r) override { if (failSave) return false; t = r; saves++; return true; }
  void clear() override { t.clear(); }
};

struct FakeStorage : SyncStorage {
  std::map<std::string, std::string> notes;
  std::map<std::string, std::string> trash;
  std::string state;
  bool haveState = false;
  std::string openNote;
  std::set<std::string> unreadable;  // exist but readNote fails (too large / SD error)
  bool failList = false, failWrite = false, failStateWrite = false;
  int stateWrites = 0;

  bool listNotes(std::vector<std::string>& names) override {
    if (failList) return false;
    for (auto& kv : notes) names.push_back(kv.first);
    for (auto& n : unreadable) names.push_back(n);
    return true;
  }
  bool readNote(const std::string& name, std::string& out, size_t maxBytes) override {
    if (unreadable.count(name)) return false;
    auto it = notes.find(name);
    if (it == notes.end() || it->second.size() > maxBytes) return false;
    out = it->second;
    return true;
  }
  bool noteExists(const std::string& name) override { return notes.count(name) || unreadable.count(name); }
  bool writeNote(const std::string& name, const std::string& data) override {
    if (failWrite) return false;
    notes[name] = data;
    return true;
  }
  bool trashNote(const std::string& name) override {
    auto it = notes.find(name);
    if (it == notes.end()) return false;
    trash[name] = it->second;
    notes.erase(it);
    return true;
  }
  bool readState(std::string& out) override { if (!haveState) return false; out = state; return true; }
  bool writeState(const std::string& d) override {
    if (failStateWrite) return false;
    state = d; haveState = true; stateWrites++; return true;
  }
  bool isOpenNote(const std::string& name) override { return !openNote.empty() && name == openNote; }
};

struct FakeCloud : Transport {
  struct Item { std::string id, ctag, content; };
  std::map<std::string, Item> files;
  std::set<std::string> folders;            // listed with a "folder" facet instead of "file"
  std::vector<std::string> weirdNames;      // listed as files with names the device must refuse
  int revCounter = 1, idCounter = 1, tokenSerial = 0;
  std::string refreshToken, accessToken;
  int pendingPolls = 0;                     // device-code polls answered "pending" before success
  bool declined = false;
  int pageSize = 200;
  std::string evilNextLink;                 // if set, the first page points paging at this URL
  int dropNext = 0;                         // next N requests: transport failure
  int force401 = 0;                         // next N Graph requests: 401
  int forceStatus = 0, forceStatusCount = 0;
  bool badSize = false;                     // download returns a truncated body
  std::vector<std::string> log;             // "METHOD path"
  int puts = 0, gets = 0, deletes = 0;
  bool bearerLeaked = false;                // a download-link request carried a token

  int hangNext = 0;                         // next N requests (of hangMethod, if set) never answer
  std::string hangMethod;
  int busyAfterCancel = 0;                  // a cancelled hung request keeps the transport busy for N begin() calls
  bool have_ = false, hanging_ = false;
  int busyLeft_ = 0;
  HttpResponse resp_;

  bool begin(const HttpRequest& r) override {
    if (busyLeft_ > 0) { busyLeft_--; return false; }
    if (have_ || hanging_) return false;
    if (hangNext > 0 && (hangMethod.empty() || r.method == hangMethod)) { hangNext--; hanging_ = true; log.push_back("HANG " + r.method); return true; }
    resp_ = process(r);
    have_ = true;
    return true;
  }
  bool poll(HttpResponse& out) override {
    if (!have_) return false;
    out = resp_;
    have_ = false;
    return true;
  }
  void cancel() override {
    if (hanging_) { hanging_ = false; busyLeft_ = busyAfterCancel; }
    have_ = false;
  }

  void put(const std::string& name, const std::string& content) {
    Item& it = files[name];
    if (it.id.empty()) it.id = "id" + std::to_string(idCounter++);
    it.ctag = "c:{" + it.id + "}," + std::to_string(revCounter++);
    it.content = content;
  }

  static std::string decode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
      if (s[i] == '%' && i + 2 < s.size()) { o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16); i += 2; }
      else o += s[i];
    }
    return o;
  }

  HttpResponse json(int status, const std::string& body) { HttpResponse r; r.status = status; r.body = body; return r; }
  std::string itemJson(const std::string& name, const Item& it, bool withFile = true) {
    std::string s = "{\"id\":\"" + it.id + "\",\"name\":\"" + jsonEscape(name) + "\",\"size\":" + std::to_string(it.content.size()) +
                    ",\"cTag\":\"" + jsonEscape(it.ctag) + "\"";
    if (withFile) s += ",\"file\":{}";
    return s + "}";
  }

  HttpResponse process(const HttpRequest& r) {
    log.push_back(r.method + " " + r.url);
    if (dropNext > 0) { dropNext--; HttpResponse e; e.status = 0; e.error = "dropped"; return e; }

    if (r.url.compare(0, std::string(od::kAuthBase).size(), od::kAuthBase) == 0) {
      const std::string path = r.url.substr(std::string(od::kAuthBase).size());
      if (path == "/devicecode")
        return json(200, "{\"device_code\":\"DC1\",\"user_code\":\"ABCD1234\",\"verification_uri\":\"https://www.microsoft.com/link\",\"expires_in\":900,\"interval\":5}");
      if (path == "/token") {
        if (r.body.find("grant_type=refresh_token") != std::string::npos) {
          if (r.body.find("refresh_token=" + urlEncode(refreshToken)) == std::string::npos || refreshToken.empty())
            return json(400, "{\"error\":\"invalid_grant\"}");
          return issue();
        }
        if (declined) return json(400, "{\"error\":\"authorization_declined\"}");
        if (pendingPolls > 0) { pendingPolls--; return json(400, "{\"error\":\"authorization_pending\"}"); }
        return issue();
      }
      return json(404, "{}");
    }

    if (r.url.compare(0, 24, "https://download.example") == 0) {
      if (!r.bearer.empty()) { bearerLeaked = true; return json(401, "{}"); }
      std::string id = r.url.substr(r.url.rfind('/') + 1);
      for (auto& kv : files)
        if (kv.second.id == id) {
          gets++;
          std::string c = kv.second.content;
          if (badSize && !c.empty()) c.pop_back();
          return json(200, c);
        }
      return json(404, "{}");
    }

    // Graph
    if (force401 > 0) { force401--; return json(401, "{\"error\":{\"code\":\"InvalidAuthenticationToken\"}}"); }
    if (accessToken.empty() || r.bearer != accessToken) return json(401, "{}");
    if (forceStatusCount > 0) { forceStatusCount--; return json(forceStatus, "{}"); }
    const std::string g = std::string(od::kGraph);
    if (r.url == od::meUrl()) return json(200, "{\"displayName\":\"Test User\"}");

    if (r.url.compare(0, (g + "/me/drive/special/approot/children").size(), g + "/me/drive/special/approot/children") == 0) {
      size_t skip = 0;
      size_t p = r.url.find("skip=");
      if (p != std::string::npos) skip = (size_t)atoi(r.url.c_str() + p + 5);
      std::vector<std::string> names;
      for (auto& kv : files) names.push_back(kv.first);
      std::string body = "{\"value\":[";
      bool first = true;
      size_t emitted = 0, idx = 0;
      auto add = [&](const std::string& s) { if (!first) body += ","; body += s; first = false; };
      std::vector<std::string> rows;
      for (auto& n : names) rows.push_back(itemJson(n, files[n]));
      for (auto& f : folders) {
        Item it;
        it.id = "fold-" + f;
        it.ctag = "c:folder";
        std::string j = itemJson(f, it, false);
        j.pop_back();
        rows.push_back(j + ",\"folder\":{}}");
      }
      for (auto& w : weirdNames) { Item it; it.id = "weird-" + std::to_string(idCounter++); it.ctag = "c:w"; it.content = "x"; rows.push_back(itemJson(w, it)); }
      for (idx = skip; idx < rows.size() && emitted < (size_t)pageSize; idx++, emitted++) add(rows[idx]);
      body += "]";
      if (idx < rows.size()) body += ",\"@odata.nextLink\":\"" + g + "/me/drive/special/approot/children?skip=" + std::to_string(idx) + "\"";
      else if (!evilNextLink.empty()) { body += ",\"@odata.nextLink\":\"" + evilNextLink + "\""; }
      body += "}";
      return json(200, body);
    }

    const std::string pre = g + "/me/drive/special/approot:/";
    if (r.url.compare(0, pre.size(), pre) == 0) {
      std::string rest = r.url.substr(pre.size());
      bool content = rest.size() > 9 && rest.compare(rest.size() - 9, 9, ":/content") == 0;
      if (content) rest.resize(rest.size() - 9);
      const std::string name = decode(rest);
      if (r.method == "PUT") {
        puts++;
        bool existed = files.count(name) > 0;
        put(name, r.body);
        return json(existed ? 200 : 201, itemJson(name, files[name]));
      }
      if (r.method == "DELETE") {
        deletes++;
        if (!files.count(name)) return json(404, "{}");
        files.erase(name);
        return json(204, "");
      }
      if (r.method == "GET" && content) {
        auto it = files.find(name);
        if (it == files.end()) return json(404, "{}");
        HttpResponse resp;
        resp.status = 302;
        resp.location = "https://download.example/" + it->second.id;
        return resp;
      }
    }
    return json(404, "{}");
  }

  HttpResponse issue() {
    tokenSerial++;
    accessToken = "AT" + std::to_string(tokenSerial);
    refreshToken = "RT" + std::to_string(tokenSerial);
    return json(200, "{\"token_type\":\"Bearer\",\"access_token\":\"" + accessToken + "\",\"refresh_token\":\"" + refreshToken +
                         "\",\"expires_in\":3599}");
  }
  // Simulate the access token expiring on the server side while the device still thinks it is valid.
  void expireAccess() { accessToken = "STALE" + std::to_string(++tokenSerial); }
};

}  // namespace fk
