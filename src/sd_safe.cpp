#include "sd_safe.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include <cstdio>
#include <cstring>

#include "config.h"

// SdFat has no public "remove the name only" (its own rename does it internally, by zeroing the old entry's first
// cluster before removing it). These give this file that one field, through the standard explicit-instantiation
// rule that lets a template name a private member; nothing else of SdFat's is touched and the library is unchanged.
namespace {

template <typename Tag, typename Tag::type M>
struct Grant {
  friend typename Tag::type get(Tag) { return M; }
};

struct FsExFat { using type = ExFatFile* FsBaseFile::*; friend type get(FsExFat); };
struct FsFat { using type = FatFile* FsBaseFile::*; friend type get(FsFat); };
struct ExFirst { using type = Cluster_t ExFatFile::*; friend type get(ExFirst); };
struct FatFirst { using type = Cluster_t FatFile::*; friend type get(FatFirst); };

template struct Grant<FsExFat, &FsBaseFile::m_xFile>;
template struct Grant<FsFat, &FsBaseFile::m_fFile>;
template struct Grant<ExFirst, &ExFatFile::m_firstCluster>;
template struct Grant<FatFirst, &FatFile::m_firstCluster>;

uint32_t firstClusterOf(const char* path) {
  if (!SdMan.exists(path)) return 0;
  auto f = SdMan.open(path, O_RDONLY);
  if (!f) return 0;
  const uint32_t c = f.firstCluster();
  f.close();
  return c;
}

// Deletes the directory entry of `path` and leaves its clusters allocated (they belong to its twin).
bool removeNameOnly(const char* path) {
  auto f = SdMan.open(path, O_RDWR);
  if (!f) return false;
  FsBaseFile& base = f;
  if (ExFatFile* x = base.*get(FsExFat())) {
    x->*get(ExFirst()) = 0;
  } else if (FatFile* ff = base.*get(FsFat())) {
    ff->*get(FatFirst()) = 0;
  } else {
    f.close();
    return false;
  }
  return f.remove();
}

}  // namespace

bool sdTwins(const char* a, const char* b) {
  const uint32_t ca = firstClusterOf(a);
  return ca != 0 && ca == firstClusterOf(b);
}

bool sdRemoveLeftover(const char* leftover, const char* keep) {
  if (!SdMan.exists(leftover)) return true;
  if (keep && sdTwins(leftover, keep)) {
    const bool ok = removeNameOnly(leftover);
    DBG_PRINTF("[SD] %s shared its clusters with %s (a rename cut short): removed the name only%s\n", leftover, keep,
               ok ? "" : " - FAILED");
    return ok;
  }
  return SdMan.remove(leftover);
}

namespace {

bool withSuffix(char* out, size_t size, const char* path, const char* suffix) {
  const int n = snprintf(out, size, "%s%s", path, suffix);
  return n > 0 && static_cast<size_t>(n) < size;
}

// True when the file at `path` holds exactly `len` bytes of `data`.
bool fileMatches(const char* path, const uint8_t* data, size_t len) {
  auto f = SdMan.open(path, O_RDONLY);
  if (!f) return false;
  bool same = f.size() == len;
  uint8_t chunk[256];
  for (size_t at = 0; same && at < len;) {
    const size_t want = len - at < sizeof(chunk) ? len - at : sizeof(chunk);
    same = f.read(chunk, want) == static_cast<int>(want) && memcmp(chunk, data + at, want) == 0;
    at += want;
  }
  f.close();
  return same;
}

}  // namespace

bool sdPromoteTmp(const char* path, const char* tmp, bool keepBackup) {
  char bak[352];
  if (keepBackup && !withSuffix(bak, sizeof(bak), path, ".bak")) return false;
  if (SdMan.exists(path)) {
    // Remove an old .bak first (by name only if a cut-short rename left it a twin of `path`).
    const bool aside = keepBackup ? sdRemoveLeftover(bak, path) && SdMan.rename(path, bak) : SdMan.remove(path);
    if (!aside) {
      DBG_PRINTF("[SD] could not move %s aside: not replaced\n", path);
      SdMan.remove(tmp);
      return false;
    }
  }
  if (!SdMan.rename(tmp, path)) {
    DBG_PRINTF("[SD] could not rename %s to %s\n", tmp, path);
    if (keepBackup && !SdMan.exists(path) && SdMan.exists(bak)) SdMan.rename(bak, path);
    return false;
  }
  return true;
}

bool sdWriteSafely(const char* path, const void* data, size_t len, bool keepBackup) {
  char tmp[352];
  if (!withSuffix(tmp, sizeof(tmp), path, ".tmp")) return false;
  // A .tmp left by a cut-short rename shares the file's clusters: truncating it would free them, so it goes first.
  sdRemoveLeftover(tmp, path);
  auto f = SdMan.open(tmp, O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) return false;
  const uint8_t* bytes = static_cast<const uint8_t*>(data);
  const size_t written = len ? f.write(bytes, len) : 0;
  f.close();
  // Read the copy back: a card that takes the write but keeps something else must not replace the good file.
  if (written != len || !fileMatches(tmp, bytes, len)) {
    DBG_PRINTF("[SD] %s: the copy did not write back correctly, not replaced\n", path);
    SdMan.remove(tmp);
    return false;
  }
  return sdPromoteTmp(path, tmp, keepBackup);
}
