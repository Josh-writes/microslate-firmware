#pragma once
// Internal-RAM diagnostics for the out-of-memory reboots during sync (Sticky, 2026-10-30). Debug builds only: in a
// release build every call is empty. Nothing here changes behaviour; it only logs.
//
// stage(label)   logs a step (Wi-Fi scan, join, a request...) with internal RAM free / largest piece / lowest since
//                boot, and remembers it as the step that is running. During a watched sync it also gives the
//                previous step's own low (the heap's low-water mark, restarted at each step).
// watchBegin()   a 20 ms sampler from the start of a sync: it records the true low point of the sync and the step that
//                was running then, and prints at once when free internal RAM first falls under 16, 8 and 4 KB.
// watchEnd()     prints that low point, and how much stack each known task has used.
// wifiOff()      internal RAM once Wi-Fi is off again: a falling baseline across syncs would mean a leak.

namespace memdiag {

#if defined(RELEASE_BUILD)
inline void stage(const char*) {}
inline void stagef(const char*, ...) {}
inline void watchBegin(const char*) {}
inline void watchEnd() {}
inline void wifiOff() {}
#else
void stage(const char* label);
void stagef(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void watchBegin(const char* what);
void watchEnd();
void wifiOff();
#endif

}  // namespace memdiag
