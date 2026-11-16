#include "mem_diag.h"

#if !defined(RELEASE_BUILD)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace memdiag {
namespace {

char g_stage[64] = "idle";          // the step running now (written by the loop and the HTTPS task; read by the sampler)
esp_timer_handle_t g_timer = nullptr;
std::atomic<bool> g_watching{false};
uint32_t g_watchStartMs = 0;

// Sampler results for the current sync (written only by the esp_timer task while watching).
size_t g_lowFree = SIZE_MAX;
size_t g_lowLargest = SIZE_MAX;
char g_lowStage[64] = "";
bool g_warned16 = false, g_warned8 = false, g_warned4 = false;

unsigned kb(size_t b) { return (unsigned)(b / 1024); }

void sample(void*) {
  const size_t f = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t l = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  if (f < g_lowFree) {
    g_lowFree = f;
    memcpy(g_lowStage, g_stage, sizeof(g_lowStage));
    g_lowStage[sizeof(g_lowStage) - 1] = '\0';
  }
  if (l < g_lowLargest) g_lowLargest = l;
  // esp_rom_printf allocates nothing: safe even when memory is gone.
  if (!g_warned16 && f < 16 * 1024) { g_warned16 = true; esp_rom_printf("[MEM] under 16 KB: %u B free, %u B largest, during %s\n", (unsigned)f, (unsigned)l, g_stage); }
  if (!g_warned8 && f < 8 * 1024) { g_warned8 = true; esp_rom_printf("[MEM] under 8 KB: %u B free, %u B largest, during %s\n", (unsigned)f, (unsigned)l, g_stage); }
  if (!g_warned4 && f < 4 * 1024) { g_warned4 = true; esp_rom_printf("[MEM] under 4 KB: %u B free, %u B largest, during %s\n", (unsigned)f, (unsigned)l, g_stage); }
}

void setStage(const char* s) {
  strncpy(g_stage, s, sizeof(g_stage) - 1);
  g_stage[sizeof(g_stage) - 1] = '\0';
}

// Per-step low: while a sync is watched, the heap's own low-water mark is restarted at every step, so each step's
// line also reports how low the step before it went. The heap updates this mark on every allocation, so it catches
// dips far shorter than the 20 ms sampler. (Like the since-boot figure, it adds up each RAM region's own low, so it
// can read lower than any single moment; a low near 0 means at least one region was emptied.)
bool g_stepWindow = false;

void logStage() {
  char prev[48] = "";
  if (g_stepWindow) {
    const size_t stepLow = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    heap_caps_monitor_local_minimum_free_size_stop();
    g_stepWindow = false;
    snprintf(prev, sizeof(prev), " | step before: low %u B", (unsigned)stepLow);
  }
  Serial.printf("[MEM] %-28s internal %u KB free, %u KB largest, %u KB lowest since boot%s | PSRAM %u KB free\n", g_stage,
                kb(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)), kb(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                kb(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)), prev, kb(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  if (g_watching.load() && heap_caps_monitor_local_minimum_free_size_start() == ESP_OK) g_stepWindow = true;
}

// Stack each task has never touched (bytes). Tasks are found by name, so a missing one is skipped.
void logStacks() {
  static const char* names[] = {"loopTask", "ms_https", "ble_conn", "nimble_host", "wifi", "tiT", "esp_timer", "sys_evt"};
  char line[256];
  int n = snprintf(line, sizeof(line), "[MEM] stack never used (bytes):");
  for (const char* name : names) {
    TaskHandle_t h = xTaskGetHandle(name);
    if (!h || n >= (int)sizeof(line)) continue;
    n += snprintf(line + n, sizeof(line) - n, " %s=%u", name, (unsigned)uxTaskGetStackHighWaterMark(h));
  }
  Serial.println(line);
}

}  // namespace

void stage(const char* label) {
  setStage(label);
  logStage();
}

void stagef(const char* fmt, ...) {
  char buf[64];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  stage(buf);
}

void watchBegin(const char* what) {
  if (!g_timer) {
    esp_timer_create_args_t a = {};
    a.callback = sample;
    a.name = "memdiag";
    if (esp_timer_create(&a, &g_timer) != ESP_OK) g_timer = nullptr;
  }
  if (g_watching.load()) return;   // one watch at a time: a sync that starts while one runs keeps the same watch
  g_lowFree = g_lowLargest = SIZE_MAX;
  g_lowStage[0] = '\0';
  g_warned16 = g_warned8 = g_warned4 = false;
  g_watchStartMs = millis();
  setStage(what);
  if (g_timer && esp_timer_start_periodic(g_timer, 20 * 1000) == ESP_OK) g_watching.store(true);
  logStage();
}

void watchEnd() {
  if (!g_watching.exchange(false)) return;
  if (g_timer) esp_timer_stop(g_timer);
  if (g_stepWindow) {
    Serial.printf("[MEM] last step (%s): low %u B\n", g_stage, (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    heap_caps_monitor_local_minimum_free_size_stop();
    g_stepWindow = false;
  }
  Serial.printf("[MEM] sync low point: %u KB free, %u KB largest piece, during %s (%u s watched)\n", kb(g_lowFree),
                kb(g_lowLargest), g_lowStage[0] ? g_lowStage : "?", (unsigned)((millis() - g_watchStartMs) / 1000));
  logStacks();
  setStage("idle");
}

void wifiOff() {
  setStage("Wi-Fi off");
  logStage();
}

}  // namespace memdiag

#endif
