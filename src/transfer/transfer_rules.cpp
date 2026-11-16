#include "transfer_rules.h"

#include <cstring>

#include "../note_ext.h"

namespace transfer {

const char kCodeAlphabet[] = "23456789abcdefghjkmnpqrstuvwxyz";

std::string makeCode(int len, uint32_t (*rnd)()) {
  const size_t n = sizeof(kCodeAlphabet) - 1;
  std::string out;
  for (int i = 0; i < len; i++) out += kCodeAlphabet[rnd() % n];
  return out;
}

bool codeLooksValid(const std::string& s, int len) {
  if ((int)s.size() != len) return false;
  for (char c : s)
    if (!strchr(kCodeAlphabet, c) || c == '\0') return false;
  return true;
}

Request parse(const std::string& path, const std::string& code) {
  Request r;
  if (code.empty() || path.size() < code.size() + 1 || path[0] != '/') return r;
  if (path.compare(1, code.size(), code) != 0) return r;
  std::string rest = path.substr(1 + code.size());
  if (rest.empty()) { r.route = Route::NoCode; return r; }
  if (rest == "/" || rest == "/index.html") { r.route = Route::Page; return r; }
  if (rest == "/list") { r.route = Route::List; return r; }
  if (rest.compare(0, 3, "/n/") == 0 && rest.size() > 3 && rest.size() <= 3 + 4) {
    int v = 0;
    for (size_t i = 3; i < rest.size(); i++) {
      if (rest[i] < '0' || rest[i] > '9') return r;
      v = v * 10 + (rest[i] - '0');
    }
    r.route = Route::Note;
    r.index = v;
  }
  return r;
}

std::string downloadName(const std::string& fileName) {
  // The note keeps its own format: ".md" for an .md note, ".txt" for everything else (work order 1.10).
  const size_t extLen = noteExtLen(fileName.c_str(), fileName.size());
  const std::string ext = extLen == 3 ? ".md" : ".txt";
  const std::string stem = fileName.substr(0, fileName.size() - extLen);
  std::string out;
  for (unsigned char c : stem) {
    if (c < 0x20 || c == 0x7F || strchr("\"\\/:*?<>|;", c)) out += '_';
    else out += (char)c;
  }
  while (!out.empty() && (out[0] == '.' || out[0] == ' ')) out.erase(0, 1);
  if (out.empty()) out = "note";
  return out + ext;
}

std::string contentDisposition(const std::string& fileName) {
  const std::string name = downloadName(fileName);
  std::string ascii;
  bool plain = true;
  for (unsigned char c : name) {
    if (c >= 0x80) { plain = false; ascii += '_'; }
    else ascii += (char)c;
  }
  std::string out = "attachment; filename=\"" + ascii + "\"";
  if (plain) return out;
  static const char* hex = "0123456789ABCDEF";
  out += "; filename*=UTF-8''";
  for (unsigned char c : name) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || strchr("-._~", c)) {
      out += (char)c;
    } else {
      out += '%';
      out += hex[c >> 4];
      out += hex[c & 15];
    }
  }
  return out;
}

std::string jsonString(const std::string& s) {
  static const char* hex = "0123456789abcdef";
  std::string out = "\"";
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '<': out += "\\u003c"; break;   // never closes a script tag if the JSON is ever inlined
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += hex[c >> 4];
          out += hex[c & 15];
        } else {
          out += (char)c;
        }
    }
  }
  out += '"';
  return out;
}

static std::string qrEscape(const std::string& s) {
  std::string out;
  for (char c : s) {
    if (strchr("\\;,:\"", c) && c != '\0') out += '\\';
    out += c;
  }
  return out;
}

std::string wifiQrText(const std::string& ssid, const std::string& password) {
  return "WIFI:T:WPA;S:" + qrEscape(ssid) + ";P:" + qrEscape(password) + ";;";
}

}  // namespace transfer
