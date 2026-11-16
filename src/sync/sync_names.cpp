#include "sync_names.h"

#include <cctype>
#include <cstring>

#include "../note_ext.h"

namespace msync {

static size_t noteExtLen(const std::string& n) { return ::noteExtLen(n.c_str(), n.size()); }

static bool reservedFatName(const std::string& base) {
  std::string b;
  for (char c : base) b += (char)tolower((unsigned char)c);
  if (b == "con" || b == "prn" || b == "aux" || b == "nul") return true;
  if (b.size() == 4 && (b.compare(0, 3, "com") == 0 || b.compare(0, 3, "lpt") == 0) && b[3] >= '1' && b[3] <= '9')
    return true;
  return false;
}

bool noteNameValid(const std::string& name) {
  const size_t extLen = noteExtLen(name);
  if (name.size() > kMaxNoteNameLen || extLen == 0) return false;
  if (name[0] == '.' || name[0] == ' ') return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == 0x7F) return false;
    if (strchr("/\\:*?\"<>|", c)) return false;
  }
  if (name.find("..") != std::string::npos) return false;
  const std::string base = name.substr(0, name.size() - extLen);
  if (base.empty() || base.back() == ' ' || base.back() == '.') return false;
  return !reservedFatName(base);
}

std::string conflictName(const std::string& name, const std::string& tag) {
  // The copy keeps the note's own extension: "a.md" -> "a_sticky.md".
  const size_t extLen = noteExtLen(name);
  std::string base = extLen ? name.substr(0, name.size() - extLen) : name;
  const std::string tail = "_" + tag + (extLen ? name.substr(name.size() - extLen) : std::string(".txt"));
  if (base.size() + tail.size() > kMaxNoteNameLen) {
    size_t keep = kMaxNoteNameLen > tail.size() ? kMaxNoteNameLen - tail.size() : 1;
    // never cut a UTF-8 sequence in half
    while (keep > 1 && (static_cast<unsigned char>(base[keep]) & 0xC0) == 0x80) keep--;
    base.resize(keep);
  }
  return base + tail;
}

}  // namespace msync
