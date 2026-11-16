#pragma once
// Small JSON reader/writer helpers for the sync engine (Graph replies, state.json).
// Pure C++, depth-limited, no exceptions. Not a general library: just what sync needs.

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace msync {

// Memory for parsed trees. A parsed reply is hundreds of small pieces (nodes, member lists, strings), almost all
// under the size the device sends to PSRAM by itself, so on the Sticky a 16 KB folder listing used to fill
// internal RAM for a few ms and starve Wi-Fi (the sync out-of-memory reboots, 2026-10-30). The device points this
// at PSRAM with jsonSetAllocator(); the default is malloc/free. Set it before any tree exists.
using JsonAllocFn = void* (*)(size_t);
using JsonFreeFn = void (*)(void*);
void jsonSetAllocator(JsonAllocFn alloc, JsonFreeFn release);
void* jsonAlloc(size_t bytes);  // never returns null: aborts when memory is gone, as operator new would
void jsonFree(void* p);

template <class T>
struct JsonAllocator {
  using value_type = T;
  JsonAllocator() = default;
  template <class U>
  JsonAllocator(const JsonAllocator<U>&) {}
  T* allocate(size_t n) { return static_cast<T*>(jsonAlloc(n * sizeof(T))); }
  void deallocate(T* p, size_t) { jsonFree(p); }
  template <class U>
  bool operator==(const JsonAllocator<U>&) const { return true; }
  template <class U>
  bool operator!=(const JsonAllocator<U>&) const { return false; }
};

using JsonString = std::basic_string<char, std::char_traits<char>, JsonAllocator<char>>;

struct Json {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double n = 0;
  JsonString s;
  std::vector<Json, JsonAllocator<Json>> a;
  std::vector<std::pair<JsonString, Json>, JsonAllocator<std::pair<JsonString, Json>>> o;

  const Json* get(const char* key) const;                                   // object member or nullptr
  std::string str(const char* key, const std::string& def = "") const;     // string member (numbers are formatted)
  double num(const char* key, double def = 0) const;                       // number member
  std::string text() const { return std::string(s.data(), s.size()); }    // this string value as a std::string
  bool isObject() const { return type == Object; }
  bool isArray() const { return type == Array; }
};

// Returns false on a syntax error or when nesting is deeper than 16 levels.
bool jsonParse(const std::string& text, Json& out);

// Escape a string for use inside JSON quotes.
std::string jsonEscape(const std::string& in);

}  // namespace msync
