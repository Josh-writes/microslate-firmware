#pragma once
// A note is a .txt or an .md file (work order 1.10), the extension in any letter case. The one check used by the
// file list, sync and Transfer. Pure C++, so the sync and Transfer host tests build it too.
#include <cctype>
#include <cstddef>
#include <cstring>

// The length of the note extension at the end of `name`: 4 for ".txt", 3 for ".md", 0 for anything else.
inline size_t noteExtLen(const char* name, size_t len) {
  auto ends = [&](const char* ext) {
    const size_t e = strlen(ext);
    if (len < e) return false;
    for (size_t i = 0; i < e; i++)
      if (tolower(static_cast<unsigned char>(name[len - e + i])) != ext[i]) return false;
    return true;
  };
  return ends(".txt") ? 4 : ends(".md") ? 3 : 0;
}

inline size_t noteExtLen(const char* name) { return noteExtLen(name, strlen(name)); }
