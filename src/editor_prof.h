#pragma once
// Editor timing probe, compiled only with -DMS_EDITOR_PROF. MS_PROF("name") times the rest of the enclosing scope;
// editorProfReport() (called from loop) prints count, average and worst microseconds per name every 2 s.

#if defined(MS_EDITOR_PROF)
#include <Arduino.h>
#include <WiFi.h>
#include "sync/cloud_service.h"

struct EditorProfSlot {
  const char* name;
  uint32_t count, totalUs, maxUs;
};

constexpr int kEditorProfSlots = 12;

inline EditorProfSlot* editorProfTable() {
  static EditorProfSlot slots[kEditorProfSlots] = {};
  return slots;
}

inline EditorProfSlot* editorProfSlot(const char* name) {
  EditorProfSlot* t = editorProfTable();
  for (int i = 0; i < kEditorProfSlots; i++) {
    if (t[i].name == name) return &t[i];
    if (!t[i].name) { t[i].name = name; return &t[i]; }
  }
  return &t[kEditorProfSlots - 1];
}

struct EditorProfScope {
  EditorProfSlot* s;
  uint32_t t0;
  explicit EditorProfScope(const char* n) : s(editorProfSlot(n)), t0(micros()) {}
  ~EditorProfScope() {
    const uint32_t d = micros() - t0;
    s->count++;
    s->totalUs += d;
    if (d > s->maxUs) s->maxUs = d;
  }
};

#include <esp_heap_caps.h>
// One-off memory speed test, 15 s after boot: is PSRAM itself slow, or is the editor code the problem?
inline void editorProfBench() {
  static bool done = false;
  if (done || millis() < 15000) return;
  done = true;
  const size_t N = 128 * 1024;
  const uint32_t caps[2] = {MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT};
  const char* names[2] = {"psram", "internal"};
  for (int k = 0; k < 2; k++) {
    uint8_t* b = (uint8_t*)heap_caps_malloc(N, caps[k]);
    if (!b) { Serial.printf("[BENCH] %s: alloc failed\n", names[k]); continue; }
    memset(b, 'a', N);
    uint32_t t = micros();
    memmove(b + 1, b, N - 1);                      // what an insert at the start of a 128 KB note does
    const uint32_t mv = micros() - t;
    t = micros();
    uint32_t sum = 0;
    for (size_t i = 0; i < N; i++) sum += b[i];    // one sequential byte read per byte
    const uint32_t rd = micros() - t;
    t = micros();
    memset(b, 'b', N);
    const uint32_t ms = micros() - t;
    Serial.printf("[BENCH] %-8s 128KB: memmove=%u us  byte-loop read=%u us  memset=%u us (sum %u)\n", names[k],
                  (unsigned)mv, (unsigned)rd, (unsigned)ms, (unsigned)sum);
    // The same one-byte shift, copied backwards through a 4 KB internal buffer in blocks (no overlapping memmove).
    static uint8_t bounce[4096];
    t = micros();
    for (size_t end = N - 1; end > 0;) {
      const size_t n = end < sizeof(bounce) ? end : sizeof(bounce);
      end -= n;
      memcpy(bounce, b + end, n);
      memcpy(b + end + 1, bounce, n);
    }
    Serial.printf("[BENCH] %-8s 128KB: shift by 1 through a 4 KB buffer=%u us\n", names[k], (unsigned)(micros() - t));
    heap_caps_free(b);
  }
}

inline void editorProfReport() {
  editorProfBench();
  static uint32_t lastMs = 0;
  if (millis() - lastMs < 2000) return;
  lastMs = millis();
  EditorProfSlot* t = editorProfTable();
  bool any = false;
  for (int i = 0; i < kEditorProfSlots; i++) if (t[i].name && t[i].count) any = true;
  if (!any) return;
  Serial.printf("[PROF] --- wifi=%d sync=%d cpu=%uMHz heapfree=%u lowest=%u\n", (int)(WiFi.status() == WL_CONNECTED),
                (int)(cloud::glyph() == cloud::Glyph::Syncing), (unsigned)getCpuFrequencyMhz(), (unsigned)ESP.getFreeHeap(),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));   // lowest internal free since boot
  for (int i = 0; i < kEditorProfSlots; i++) {
    if (!t[i].name || !t[i].count) continue;
    Serial.printf("[PROF] %-10s n=%-4u avg=%-7u max=%-7u us\n", t[i].name, (unsigned)t[i].count,
                  (unsigned)(t[i].totalUs / t[i].count), (unsigned)t[i].maxUs);
    t[i].count = t[i].totalUs = t[i].maxUs = 0;
  }
}

#define MS_PROF_CAT2(a, b) a##b
#define MS_PROF_CAT(a, b) MS_PROF_CAT2(a, b)
#define MS_PROF(name) EditorProfScope MS_PROF_CAT(_prof_scope_, __LINE__)(name)
#else
#define MS_PROF(name) do {} while (0)
inline void editorProfReport() {}
#endif
