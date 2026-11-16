#pragma once
#include <cstdint>
#include <string>

namespace msync {

// Longest note file name the device handles (matches MAX_FILENAME_LEN - 1 in config.h).
static constexpr size_t kMaxNoteNameLen = 63;

// Remote names come from the internet: only `.txt` and `.md` notes with safe characters are accepted.
// A remote name can never reach outside /notes (no separators, no "..", no reserved FAT names).
bool noteNameValid(const std::string& name);

// Name for this device's copy when the same note differs here and in the cloud:
// "Chapter one.txt" + "sticky" -> "Chapter one_sticky.txt" (shown as "Chapter One Sticky"); an .md note stays .md.
// The base is shortened, on a UTF-8 boundary, so the result fits kMaxNoteNameLen.
std::string conflictName(const std::string& name, const std::string& tag);

}  // namespace msync
