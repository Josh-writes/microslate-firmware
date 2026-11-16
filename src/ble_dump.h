#pragma once
// Bluetooth capture for the BLE rebuild baseline, compiled only with -DMS_BLE_DUMP
// (env sticky_bledump). Every line starts with "[BLEDUMP] <millis>" so a serial log can
// be filtered. Logs pairing state, connect and disconnect reasons, the HID Report Map,
// Report References, device information and every raw input report. It does not change
// how the keyboard connects; the one addition is that the other input reports are also
// subscribed so their raw bytes can be logged (they are not decoded).
// Raw reports are key codes of what is typed, so this build is for test devices only.

#if defined(MS_BLE_DUMP)
#include <Arduino.h>
#define BLEDUMP(fmt, ...) Serial.printf("[BLEDUMP] %lu " fmt "\n", (unsigned long)millis(), ##__VA_ARGS__)
// Hex dump in 32-byte lines: "[BLEDUMP] t <tag> <offset> <hex>".
inline void bleDumpHex(const char* tag, const uint8_t* data, size_t len) {
  char line[3 * 32 + 1];
  for (size_t off = 0; off < len; off += 32) {
    size_t n = len - off < 32 ? len - off : 32;
    for (size_t i = 0; i < n; i++) snprintf(line + 3 * i, 4, "%02X ", data[off + i]);
    line[3 * n] = '\0';
    BLEDUMP("%s %04u %s", tag, (unsigned)off, line);
  }
}
#else
#define BLEDUMP(fmt, ...) do {} while (0)
inline void bleDumpHex(const char*, const uint8_t*, size_t) {}
#endif
