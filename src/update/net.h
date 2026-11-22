#pragma once
// HTTPS helpers shared by the firmware update and the reader install (src/update): one connection with the root
// certificates, timeouts, and GETs that report GitHub's redirects instead of following them.

#include "../config.h"
#include "../wall_clock.h"

#include <HTTPClient.h>
#include <NetworkClientSecure.h>

#include <string>

// Root certificate bundle compiled into the core (as device_cloud.cpp).
extern const uint8_t rootca_crt_bundle_start[] asm("_binary_x509_crt_bundle_start");
extern const uint8_t rootca_crt_bundle_end[] asm("_binary_x509_crt_bundle_end");

namespace fwnet {

// One connection: HTTPS with the root certificates. Debug builds also take plain http://, for a local test server
// that stands in for GitHub (scripts/update-test-server.py); release builds only ever use https://.
struct Conn {
  NetworkClientSecure* tls = nullptr;
  NetworkClient* plain = nullptr;
  explicit Conn(const std::string& url) {
    wallclock::waitNetTurn();   // the clock's time-server lookup goes first (wall_clock.h); only ever in a task
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

inline void setupHttp(HTTPClient& http) {
  http.setConnectTimeout(15000);
  http.setTimeout(30000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  static const char* keys[] = {"Location"};
  http.collectHeaders(keys, 1);
}

// A GET whose answer is only its status, Location header and size (GitHub's redirects, or a file's length).
inline int getRedirect(const std::string& url, std::string& location, int* size = nullptr) {
  Conn conn(url);
  HTTPClient http;
  setupHttp(http);
  int code = -1;
  if (http.begin(conn.client(), url.c_str())) {
    code = http.GET();
    if (code > 0) location = http.header("Location").c_str();
    if (size) *size = http.getSize();
    http.end();
  }
  DBG_PRINTF("[UPDATE] GET %s -> %d%s%s\n", url.c_str(), code, code < 0 ? " " : "",
             code < 0 ? HTTPClient::errorToString(code).c_str() : "");
  return code;
}

// The release tag in a GitHub download address (.../releases/download/<tag>/<file>), or "" when there is none.
inline std::string releaseTag(const std::string& url) {
  static const char kKey[] = "/releases/download/";
  const size_t at = url.find(kKey);
  if (at == std::string::npos) return "";
  const size_t start = at + sizeof(kKey) - 1;
  const size_t end = url.find('/', start);
  return end == std::string::npos ? "" : url.substr(start, end - start);
}

// Follows redirects from `url` (GitHub's releases/latest/download/<file> takes two). Returns the last status;
// `url` becomes the final address and `size` its length when the status is 200. With `tag`, the release tag of the
// first redirect that names one (releases/latest/download goes to releases/download/<tag>/ first).
inline int resolve(std::string& url, int& size, std::string* tag = nullptr) {
  int code = -1;
  for (int hop = 0; hop < 5; hop++) {
    std::string loc;
    size = -1;
    code = getRedirect(url, loc, &size);
    if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) && !loc.empty()) {
      if (tag && tag->empty()) *tag = releaseTag(loc);
      url = loc;
      continue;
    }
    break;
  }
  return code;
}

}  // namespace fwnet
