#include "sync_json.h"

#include <cstdio>
#include <cstdlib>

namespace msync {

namespace {
JsonAllocFn g_alloc = std::malloc;
JsonFreeFn g_free = std::free;
}  // namespace

void jsonSetAllocator(JsonAllocFn alloc, JsonFreeFn release) {
  g_alloc = alloc ? alloc : std::malloc;
  g_free = release ? release : std::free;
}

void* jsonAlloc(size_t bytes) {
  void* p = g_alloc(bytes ? bytes : 1);
  if (!p) std::abort();
  return p;
}

void jsonFree(void* p) { g_free(p); }

const Json* Json::get(const char* key) const {
  if (type != Object) return nullptr;
  for (const auto& kv : o)
    if (kv.first == key) return &kv.second;
  return nullptr;
}

std::string Json::str(const char* key, const std::string& def) const {
  const Json* j = get(key);
  if (!j) return def;
  if (j->type == String) return j->text();
  if (j->type == Number) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f", j->n);
    return buf;
  }
  return def;
}

double Json::num(const char* key, double def) const {
  const Json* j = get(key);
  if (!j) return def;
  if (j->type == Number) return j->n;
  if (j->type == String) return atof(j->s.c_str());
  return def;
}

namespace {

struct Parser {
  const char* p;
  const char* end;

  void ws() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
  }

  static void putUtf8(JsonString& out, unsigned cp) {
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    } else {
      out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F));
      out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F));
    }
  }

  bool hex4(unsigned& v) {
    if (end - p < 4) return false;
    v = 0;
    for (int i = 0; i < 4; i++) {
      char c = *p++;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= c - '0';
      else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
      else return false;
    }
    return true;
  }

  bool str(JsonString& out) {
    if (p >= end || *p != '"') return false;
    p++;
    while (p < end && *p != '"') {
      char c = *p++;
      if (c != '\\') { out += c; continue; }
      if (p >= end) return false;
      char e = *p++;
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return false;
          if (cp >= 0xD800 && cp < 0xDC00) {  // surrogate pair
            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
            p += 2;
            unsigned lo;
            if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          putUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    if (p >= end) return false;
    p++;  // closing quote
    return true;
  }

  bool value(Json& j, int depth) {
    if (depth > 16) return false;
    ws();
    if (p >= end) return false;
    char c = *p;
    if (c == '{') {
      p++;
      j.type = Json::Object;
      ws();
      if (p < end && *p == '}') { p++; return true; }
      for (;;) {
        ws();
        JsonString key;
        if (!str(key)) return false;
        ws();
        if (p >= end || *p != ':') return false;
        p++;
        Json v;
        if (!value(v, depth + 1)) return false;
        j.o.emplace_back(std::move(key), std::move(v));
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == '}') { p++; return true; }
        return false;
      }
    }
    if (c == '[') {
      p++;
      j.type = Json::Array;
      ws();
      if (p < end && *p == ']') { p++; return true; }
      for (;;) {
        Json v;
        if (!value(v, depth + 1)) return false;
        j.a.push_back(std::move(v));
        ws();
        if (p < end && *p == ',') { p++; continue; }
        if (p < end && *p == ']') { p++; return true; }
        return false;
      }
    }
    if (c == '"') { j.type = Json::String; return str(j.s); }
    if (end - p >= 4 && p[0] == 't' && p[1] == 'r' && p[2] == 'u' && p[3] == 'e') { p += 4; j.type = Json::Bool; j.b = true; return true; }
    if (end - p >= 5 && p[0] == 'f' && p[1] == 'a' && p[2] == 'l' && p[3] == 's' && p[4] == 'e') { p += 5; j.type = Json::Bool; return true; }
    if (end - p >= 4 && p[0] == 'n' && p[1] == 'u' && p[2] == 'l' && p[3] == 'l') { p += 4; j.type = Json::Null; return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      char* e = nullptr;
      j.n = strtod(p, &e);
      if (e == p) return false;
      p = e;
      j.type = Json::Number;
      return true;
    }
    return false;
  }
};

}  // namespace

bool jsonParse(const std::string& text, Json& out) {
  Parser ps{text.data(), text.data() + text.size()};
  out = Json();
  if (!ps.value(out, 0)) return false;
  ps.ws();
  return ps.p == ps.end;
}

std::string jsonEscape(const std::string& in) {
  std::string out;
  for (unsigned char c : in) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += (char)c;
        }
    }
  }
  return out;
}

}  // namespace msync
