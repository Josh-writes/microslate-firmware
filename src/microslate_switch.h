#pragma once

// MicroSlate dual boot: the one file shared by MicroSlate and the readers it installs (CrossPoint, CrossInk).
// Keep it byte-identical in every repo; the copy in Josh-writes/microslate-firmware (src/) is the original.
//
// Header-only, ESP-IDF C APIs only. In exactly one .cpp file, at file scope:
//
//   MS_SWITCH_APP("crossink", "CrossInk", "sticky");   // id, display name, devices
//
// Then:
//
//   msswitch::begin();                                  // once at boot
//   if (msswitch::microslatePresent()) { ...show a "MicroSlate" entry on the home screen... }
//   msswitch::openMicroSlate();                         // does not return when it succeeds
//
//   // In the app's own firmware updaters (OTA and SD card):
//   if (!msswitch::selfUpdateAllowed()) { ...hide the item / refuse... }
//
// Layout: the stock two-slot table (app0 0x10000, app1 0x650000). MicroSlate is in one slot, the reader in the
// other; they swap slots when MicroSlate updates. Without MicroSlate in the other slot everything here is a no-op
// and the reader behaves exactly like its upstream build.

#if !__has_include(<esp_partition.h>)

// Host builds (simulators, unit tests): no flash partitions, behave like a device without MicroSlate.
#define MS_SWITCH_APP(ID, NAME, DEVICES) \
  static_assert(sizeof(ID) <= 24 && sizeof(NAME) <= 32 && sizeof(DEVICES) <= 64, "MS_SWITCH_APP field too long")
namespace msswitch {
inline bool begin() { return true; }
inline bool microslatePresent() { return false; }
inline void openMicroSlate() {}
inline bool selfUpdateAllowed() { return true; }
}  // namespace msswitch

#else  // ESP-IDF / Arduino-ESP32

#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace msswitch {

// The marker sits in .rodata_custom_desc, which the ESP-IDF linker scripts place directly after esp_app_desc_t:
//   file offset 0x120 = image header (24) + segment header (8) + esp_app_desc_t (256)
// so it can be read from a slot without parsing the image.
static constexpr uint32_t kMarkerOffset = 0x120;
static constexpr char kMagic[8] = {'M', 'S', 'S', 'W', 'I', 'T', 'C', 'H'};
static constexpr uint16_t kFormat = 1;
static constexpr const char* kMicroSlateId = "microslate";

struct AppMarker {
  char magic[8];      // "MSSWITCH" (not NUL-terminated)
  uint16_t format;    // kFormat
  uint16_t flags;     // reserved, 0
  uint32_t reserved;  // reserved, 0
  char id[24];        // stable id, lowercase: "microslate", "crosspoint", "crossink"
  char name[32];      // display name: "MicroSlate", "CrossPoint", "CrossInk"
  char devices[64];   // comma-separated devices this build runs on: "sticky", "x4pro"
};
static_assert(sizeof(AppMarker) == 136, "AppMarker layout is shared by every app");

}  // namespace msswitch

// Defined by MS_SWITCH_APP(). The section is not KEEP() in the linker script and `used` does not stop
// --gc-sections, so begin() leaves a real reference to it.
extern "C" const msswitch::AppMarker ms_switch_app_marker;

#define MS_SWITCH_APP(ID, NAME, DEVICES)                                                                               \
  extern "C" __attribute__((section(".rodata_custom_desc"), used)) const msswitch::AppMarker ms_switch_app_marker = { \
      {'M', 'S', 'S', 'W', 'I', 'T', 'C', 'H'}, msswitch::kFormat, 0, 0, ID, NAME, DEVICES}

namespace msswitch {

inline const AppMarker& self() { return ms_switch_app_marker; }

inline bool markerValid(const AppMarker& m) {
  return memcmp(m.magic, kMagic, sizeof(kMagic)) == 0 && m.format == kFormat && m.id[0] != '\0' &&
         memchr(m.id, 0, sizeof(m.id)) && memchr(m.name, 0, sizeof(m.name)) && memchr(m.devices, 0, sizeof(m.devices));
}

// Call once at boot. The marker is a compile-time constant, so without the empty asm the linker would drop it.
inline bool begin() {
  const AppMarker* m = &ms_switch_app_marker;
  __asm__ __volatile__("" : : "r"(m) : "memory");
  return markerValid(*m);
}

// The app slot that is not running, or nullptr.
inline const esp_partition_t* otherSlot() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* found = nullptr;
  esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, nullptr);
  for (; it; it = esp_partition_next(it)) {
    const esp_partition_t* p = esp_partition_get(it);
    if (p != running && p->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && p->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
      found = p;
      break;
    }
  }
  esp_partition_iterator_release(it);
  return found;
}

// Reads the marker of the app in `slot`. False when the slot is empty, damaged or holds an app without one.
inline bool readMarker(const esp_partition_t* slot, AppMarker& out) {
  if (!slot) return false;
  uint8_t magic = 0;
  if (esp_partition_read(slot, 0, &magic, 1) != ESP_OK || magic != 0xE9) return false;  // ESP image magic
  if (esp_partition_read(slot, kMarkerOffset, &out, sizeof(out)) != ESP_OK) return false;
  return markerValid(out);
}

// True when the other slot holds MicroSlate.
inline bool microslatePresent() {
  AppMarker m;
  return readMarker(otherSlot(), m) && strcmp(m.id, kMicroSlateId) == 0;
}

// Boots the app in `slot`. Returns only on failure.
inline void openSlot(const esp_partition_t* slot) {
  if (!slot || esp_ota_set_boot_partition(slot) != ESP_OK) return;
  esp_restart();
}

// Boots MicroSlate. Returns only on failure (MicroSlate not there, or the switch was refused).
inline void openMicroSlate() {
  if (microslatePresent()) openSlot(otherSlot());
}

// A reader must not flash itself next to MicroSlate: its updater writes the other slot, which is MicroSlate.
// MicroSlate installs and updates the reader instead.
inline bool selfUpdateAllowed() { return !microslatePresent(); }

}  // namespace msswitch

#endif  // ESP-IDF / Arduino-ESP32
