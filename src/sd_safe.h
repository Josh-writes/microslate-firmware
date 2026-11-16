#pragma once

#include <cstddef>

// Deleting leftover copies without hurting the file they came from (safety audit 1.5, 2026-11-01).
//
// SdFat renames in two steps: it writes the new directory entry, then deletes the old one. A restart in between
// leaves two names for the same clusters ("twins"). Deleting either twin the normal way frees those clusters while
// the other name still uses them; the next file written then lands on top of it (seen on the Sticky: a restart
// during a save, then the boot clean-up or the next save deleted the twin, and the note was overwritten).
//
// sdRemoveLeftover(leftover, keep) deletes `leftover` (a .tmp or .bak), but when it shares its first cluster with
// `keep` only its name goes and the clusters stay with `keep`. Use it for every delete of a copy that a rename of
// `keep` could have left behind, and before opening such a copy with O_TRUNC (truncating a twin frees the same
// clusters). Returns true when `leftover` is gone (or was never there).
bool sdRemoveLeftover(const char* leftover, const char* keep);

// True when both files exist and start at the same cluster (twins).
bool sdTwins(const char* a, const char* b);

// Replaces `path` with `len` bytes of `data` so that a power cut or a pulled card leaves the old file or the new
// one, never half of one: the data goes to path.tmp, is read back and compared, then swapped in (sdPromoteTmp).
// With keepBackup the old file is kept as path.bak. Returns false when anything failed; the old file is then
// still in place (or, if only the last rename failed, the complete new copy is path.tmp).
bool sdWriteSafely(const char* path, const void* data, size_t len, bool keepBackup = false);

// The swap step on its own, for a writer that fills and checks `tmp` itself (the editor streams large notes):
// moves `path` aside (to path.bak with keepBackup, otherwise removes it), then renames `tmp` to `path`. If moving
// `path` aside fails, `tmp` is removed and `path` is untouched. If the final rename fails, the .bak goes back to
// `path` and `tmp` is kept.
bool sdPromoteTmp(const char* path, const char* tmp, bool keepBackup);
