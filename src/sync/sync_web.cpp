#include "sync_web.h"

#include "sync_json.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>

namespace msync {

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}

std::string isoTime(uint32_t epoch) {
  const time_t t = (time_t)epoch;
  struct tm v;
  gmtime_r(&t, &v);
  char buf[80];   // room for any int the compiler imagines; the real text is 20 characters
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", v.tm_year + 1900, v.tm_mon + 1, v.tm_mday, v.tm_hour,
           v.tm_min, v.tm_sec);
  return buf;
}

HttpRequest webRequest(const char* method, const std::string& url) {
  HttpRequest q;
  q.method = method;
  q.url = url;
  return q;
}

HttpRequest formPost(const std::string& url, const std::string& body) {
  HttpRequest q = webRequest("POST", url);
  q.contentType = "application/x-www-form-urlencoded";
  q.body = body;
  return q;
}

DeviceCode parseDeviceCodeReply(int status, const std::string& body) {
  DeviceCode d;
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) { d.error = status > 0 ? "bad reply" : "no response"; return d; }
  if (status != 200) { d.error = j.str("error", "http error"); return d; }
  d.deviceCode = j.str("device_code");
  d.userCode = j.str("user_code");
  d.verificationUri = j.str("verification_uri", j.str("verification_url"));
  d.expiresIn = (int)j.num("expires_in");
  d.interval = (int)j.num("interval");
  d.ok = !d.deviceCode.empty() && !d.userCode.empty();
  if (!d.ok) d.error = "incomplete reply";
  return d;
}

TokenReply parseTokenReply(int status, const std::string& body) {
  TokenReply t;
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) { t.error = status > 0 ? "bad reply" : "no response"; return t; }
  if (status != 200) {
    t.error = j.str("error", "http error");
    t.errorDesc = j.str("error_description");
    return t;
  }
  t.access = j.str("access_token");
  t.refresh = j.str("refresh_token");
  t.expiresIn = (int)j.num("expires_in");
  t.ok = !t.access.empty();
  if (!t.ok) t.error = "no access token";
  return t;
}

bool parseFileObject(const std::string& body, const char* revKey, RemoteFile& f) {
  Json j;
  if (!jsonParse(body, j) || !j.isObject()) return false;
  f.name = j.str("name");
  f.id = j.str("id");
  f.rev = j.str(revKey);
  f.size = (uint32_t)strtoul(j.str("size").c_str(), nullptr, 10);
  return !f.id.empty() && !f.rev.empty();
}

}  // namespace msync
