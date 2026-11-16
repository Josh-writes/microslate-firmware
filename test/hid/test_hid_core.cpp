// Host tests for the HID core (lib/HidCore): Report Map parsing and report decoding.
// Build and run (any g++ with C++17):  sh test/hid/run-on-hinton.sh
// The Logitech maps are the real ones captured from the Keys-To-Go 2 and the MX Keys Mini
// (ble-captures 2026-10-27, B0/B1).
#include "HidCore.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace hidcore;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

// Logitech Keys-To-Go 2 (046D:B38C, fw RBK92.01_0006), 269 bytes.
static const uint8_t kKeysToGo2[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x95, 0x08, 0x75, 0x01, 0x15, 0x00, 0x25, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x81, 0x02, 0x95, 0x05, 0x75, 0x01, 0x15, 0x00, 0x25, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x03, 0x91, 0x03, 0x95, 0x06, 0x75, 0x08,
    0x26, 0xFF, 0x00, 0x05, 0x07, 0x19, 0x00, 0x29, 0xFF, 0x81, 0x00, 0x06, 0xD1, 0xFF, 0x09, 0x01,
    0xA1, 0x02, 0x85, 0x09, 0x95, 0x0C, 0x75, 0x20, 0x17, 0x00, 0x00, 0x00, 0x80, 0x27, 0xFF, 0xFF,
    0xFF, 0x7F, 0x05, 0x0A, 0x19, 0x01, 0x29, 0x0C, 0xB1, 0x03, 0xC0, 0xC0, 0x06, 0x0C, 0xFF, 0x09,
    0x01, 0xA1, 0x01, 0x85, 0x03, 0x95, 0x02, 0x75, 0x10, 0x15, 0x01, 0x26, 0xFF, 0x02, 0x19, 0x01,
    0x2A, 0xFF, 0x02, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x04, 0x95, 0x01,
    0x75, 0x02, 0x15, 0x01, 0x25, 0x03, 0x09, 0x82, 0x09, 0x81, 0x09, 0x83, 0x81, 0x00, 0x95, 0x02,
    0x75, 0x01, 0x15, 0x00, 0x25, 0x01, 0x09, 0x9B, 0x09, 0xA9, 0x81, 0x06, 0x95, 0x01, 0x75, 0x04,
    0x81, 0x03, 0xC0, 0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x05, 0x95, 0x0A, 0x75, 0x01, 0x15,
    0x00, 0x25, 0x01, 0x0A, 0x70, 0x00, 0x0A, 0x6F, 0x00, 0x0A, 0x9F, 0x02, 0x0A, 0xD9, 0x00, 0x0A,
    0xCD, 0x00, 0x0A, 0xB5, 0x00, 0x0A, 0xE2, 0x00, 0x0A, 0xEA, 0x00, 0x0A, 0xE9, 0x00, 0x0A, 0x30,
    0x00, 0x81, 0x02, 0x95, 0x06, 0x81, 0x03, 0xC0, 0x05, 0x01, 0x09, 0x13, 0xA1, 0x01, 0x85, 0x0B,
    0x95, 0x01, 0x75, 0x01, 0x15, 0x00, 0x25, 0x01, 0x09, 0xE1, 0x81, 0x06, 0x75, 0x0F, 0x81, 0x03,
    0xC0, 0x06, 0x43, 0xFF, 0x0A, 0x02, 0x02, 0xA1, 0x01, 0x85, 0x11, 0x95, 0x13, 0x75, 0x08, 0x15,
    0x00, 0x26, 0xFF, 0x00, 0x09, 0x02, 0x81, 0x00, 0x09, 0x02, 0x91, 0x00, 0xC0,
};

// Logitech MX Keys Mini for Business (046D:B36E, fw RBK73.04_0016), 258 bytes.
static const uint8_t kMxKeysMini[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x05, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x26, 0xA4,
    0x00, 0x05, 0x07, 0x19, 0x00, 0x2A, 0xA4, 0x00, 0x81, 0x00, 0x06, 0xD1, 0xFF, 0x09, 0x01, 0xA1,
    0x02, 0x85, 0x09, 0x95, 0x0C, 0x75, 0x20, 0x17, 0x00, 0x00, 0x00, 0x80, 0x27, 0xFF, 0xFF, 0xFF,
    0x7F, 0x05, 0x0A, 0x19, 0x01, 0x29, 0x0C, 0xB1, 0x03, 0xC0, 0xC0, 0x06, 0x0C, 0xFF, 0x09, 0x01,
    0xA1, 0x01, 0x85, 0x03, 0x95, 0x02, 0x75, 0x10, 0x15, 0x01, 0x26, 0xFF, 0x02, 0x19, 0x01, 0x2A,
    0xFF, 0x02, 0x81, 0x00, 0xC0, 0x05, 0x01, 0x09, 0x80, 0xA1, 0x01, 0x85, 0x04, 0x95, 0x01, 0x75,
    0x02, 0x15, 0x01, 0x25, 0x03, 0x09, 0x82, 0x09, 0x81, 0x09, 0x83, 0x81, 0x00, 0x95, 0x02, 0x75,
    0x01, 0x15, 0x00, 0x25, 0x01, 0x09, 0x9B, 0x09, 0xA9, 0x81, 0x06, 0x95, 0x01, 0x75, 0x04, 0x81,
    0x03, 0xC0, 0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x05, 0x75, 0x01, 0x95, 0x08, 0x15, 0x00,
    0x25, 0x01, 0x0A, 0xD8, 0x00, 0x0A, 0xD9, 0x00, 0x0A, 0xD5, 0x00, 0x0A, 0xCD, 0x00, 0x0A, 0xE2,
    0x00, 0x0A, 0xEA, 0x00, 0x0A, 0xE9, 0x00, 0x0A, 0x30, 0x00, 0x81, 0x02, 0xC0, 0x05, 0x01, 0x09,
    0x13, 0xA1, 0x01, 0x85, 0x0B, 0x95, 0x01, 0x75, 0x01, 0x15, 0x00, 0x25, 0x01, 0x09, 0xE1, 0x81,
    0x06, 0x75, 0x0F, 0x81, 0x03, 0xC0, 0x06, 0x43, 0xFF, 0x0A, 0x02, 0x02, 0xA1, 0x01, 0x85, 0x11,
    0x75, 0x08, 0x95, 0x13, 0x15, 0x00, 0x26, 0xFF, 0x00, 0x09, 0x02, 0x81, 0x00, 0x09, 0x02, 0x91,
    0x00, 0xC0,
};

// Boot keyboard from the HID specification (appendix B.1): no report IDs, 8-byte report
// with a reserved byte.
static const uint8_t kBootStyle[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00, 0x25, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01, 0x95, 0x05, 0x75, 0x01,
    0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
};

// ZMK-style NKRO: report 1 = modifiers, reserved byte, bitmap of usages 0x00-0x67;
// report 2 = consumer array.
static const uint8_t kZmkNkro[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01, 0x05, 0x07, 0x15, 0x00, 0x25, 0x01, 0x19, 0xE0,
    0x29, 0xE7, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x75, 0x08, 0x95, 0x01, 0x81, 0x03, 0x05, 0x07,
    0x15, 0x00, 0x25, 0x01, 0x19, 0x00, 0x29, 0x67, 0x75, 0x01, 0x95, 0x68, 0x81, 0x02, 0x05, 0x08,
    0x19, 0x01, 0x29, 0x05, 0x95, 0x05, 0x75, 0x01, 0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x03,
    0xC0, 0x05, 0x0C, 0x09, 0x01, 0xA1, 0x01, 0x85, 0x02, 0x05, 0x0C, 0x15, 0x00, 0x26, 0xFF, 0x00,
    0x19, 0x00, 0x2A, 0xFF, 0x00, 0x75, 0x08, 0x95, 0x06, 0x81, 0x00, 0xC0,
};

// Events from one report, as a compact string: "+28" press, "-28" release, "/01" mods.
static const char* run(const ReportLayout& l, KeyState& s, const char* hex) {
  static char buf[256];
  uint8_t d[64];
  size_t n = 0;
  for (const char* p = hex; *p && n < sizeof(d);) {
    if (*p == ' ') { p++; continue; }
    d[n++] = uint8_t(strtoul(std::string(p, 2).c_str(), nullptr, 16));
    p += 2;
  }
  KeyEvent ev[32];
  int c = decodeReport(l, d, n, s, ev, 32);
  buf[0] = '\0';
  for (int i = 0; i < c; i++) {
    char t[16];
    snprintf(t, sizeof(t), "%s%c%02X/%02X", i ? " " : "", ev[i].pressed ? '+' : '-', ev[i].usage, ev[i].mods);
    strcat(buf, t);
  }
  return buf;
}

static bool eq(const char* a, const char* b) {
  if (strcmp(a, b) != 0) { printf("  got \"%s\", want \"%s\"\n", a, b); return false; }
  return true;
}

static void testKeysToGo2() {
  ReportMap m;
  CHECK(parseReportMap(kKeysToGo2, sizeof(kKeysToGo2), m) == ParseResult::Ok);
  CHECK(m.keyboardReports() == 1);
  int k = m.find(1);
  CHECK(k >= 0);
  if (k < 0) return;
  const ReportLayout& r = m.reports[k];
  CHECK(r.kind == ReportKind::Keyboard);
  CHECK(r.bits == 56);  // 7 bytes: modifiers + 6 keys, no reserved byte
  CHECK(r.fieldCount == 2);
  CHECK(r.fields[0].type == FieldType::Bitmap && r.fields[0].usageMin == 0xE0 && r.fields[0].count == 8);
  CHECK(r.fields[1].type == FieldType::Array && r.fields[1].bitOffset == 8 && r.fields[1].count == 6);
  // Other inputs: 3 vendor page FF0C, 4 system control, 5 consumer bitmap, 11 generic, 17 HID++.
  CHECK(m.find(3) >= 0 && m.reports[m.find(3)].kind == ReportKind::Vendor);
  CHECK(m.find(4) >= 0 && m.reports[m.find(4)].kind == ReportKind::Other);
  CHECK(m.find(5) >= 0 && m.reports[m.find(5)].kind == ReportKind::Consumer);
  CHECK(m.find(11) >= 0 && m.reports[m.find(11)].kind == ReportKind::Other);
  CHECK(m.find(17) >= 0 && m.reports[m.find(17)].kind == ReportKind::Vendor);
  CHECK(m.find(9) < 0);  // feature only

  KeyState s{};
  CHECK(eq(run(r, s, "00 28 00 00 00 00 00"), "+28/00"));         // Enter
  CHECK(eq(run(r, s, "00 00 00 00 00 00 00"), "-28/00"));
  CHECK(eq(run(r, s, "01 00 00 00 00 00 00"), ""));               // Left Ctrl alone
  CHECK(eq(run(r, s, "01 16 00 00 00 00 00"), "+16/01"));         // Ctrl+S
  CHECK(eq(run(r, s, "00 00 00 00 00 00 00"), "-16/00"));
  // Fast typing overlaps keys: "the"
  CHECK(eq(run(r, s, "00 17 00 00 00 00 00"), "+17/00"));
  CHECK(eq(run(r, s, "00 17 0B 00 00 00 00"), "+0B/00"));
  CHECK(eq(run(r, s, "00 17 0B 08 00 00 00"), "+08/00"));
  CHECK(eq(run(r, s, "00 0B 08 00 00 00 00"), "-17/00"));
  CHECK(eq(run(r, s, "00 00 00 00 00 00 00"), "-0B/00 -08/00"));
  // Rollover error: ignored, state kept
  CHECK(eq(run(r, s, "00 04 00 00 00 00 00"), "+04/00"));
  CHECK(eq(run(r, s, "00 01 01 01 01 01 01"), ""));
  CHECK(eq(run(r, s, "00 00 00 00 00 00 00"), "-04/00"));
  // Too short for the layout: ignored
  CHECK(eq(run(r, s, "00 04 00"), ""));

  // The consumer and HID++ reports never type.
  KeyState c{};
  CHECK(eq(run(m.reports[m.find(5)], c, "08 00"), ""));
  CHECK(eq(run(m.reports[m.find(17)], c, "FF 0B 00 08 03 04 00 00 00 00 00 00 00 00 00 00 00 00 00"), ""));
}

static void testMxKeysMini() {
  ReportMap m;
  CHECK(parseReportMap(kMxKeysMini, sizeof(kMxKeysMini), m) == ParseResult::Ok);
  int k = m.find(1);
  CHECK(k >= 0);
  if (k < 0) return;
  const ReportLayout& r = m.reports[k];
  CHECK(r.bits == 56);
  CHECK(r.fieldCount == 2);
  CHECK(r.fields[1].bitOffset == 8);  // the LED output bits do not move the input fields
  CHECK(m.reports[m.find(5)].kind == ReportKind::Consumer);
  CHECK(m.reports[m.find(17)].kind == ReportKind::Vendor);
  KeyState s{};
  CHECK(eq(run(r, s, "08 0B 00 00 00 00 00"), "+0B/08"));  // dictation key: GUI+H
  CHECK(eq(run(r, s, "08 00 00 00 00 00 00"), "-0B/08"));
  CHECK(eq(run(r, s, "00 00 00 00 00 00 00"), ""));
  CHECK(eq(run(r, s, "0A 16 00 00 00 00 00"), "+16/0A"));  // screenshot: GUI+Shift+S
}

static void testBootStyle() {
  ReportMap m;
  CHECK(parseReportMap(kBootStyle, sizeof(kBootStyle), m) == ParseResult::Ok);
  CHECK(m.count == 1);
  int k = m.find(0);
  CHECK(k == 0);
  if (k < 0) return;
  const ReportLayout& r = m.reports[k];
  CHECK(r.bits == 64);
  CHECK(r.fields[1].bitOffset == 16);  // after the reserved byte
  KeyState s{};
  CHECK(eq(run(r, s, "02 00 04 00 00 00 00 00"), "+04/02"));  // Shift+A
  CHECK(eq(run(r, s, "00 00 00 00 00 00 00 00"), "-04/00"));
}

static void testZmkNkro() {
  ReportMap m;
  CHECK(parseReportMap(kZmkNkro, sizeof(kZmkNkro), m) == ParseResult::Ok);
  int k = m.find(1);
  CHECK(k >= 0);
  if (k < 0) return;
  const ReportLayout& r = m.reports[k];
  CHECK(r.bits == 8 + 8 + 0x68);
  CHECK(r.fields[1].type == FieldType::Bitmap && r.fields[1].bitOffset == 16 && r.fields[1].count == 0x68);
  CHECK(m.reports[m.find(2)].kind == ReportKind::Consumer);
  KeyState s{};
  // 'a' (0x04) is bit 4 of byte 2; 'b' (0x05) bit 5. Bitmaps lose press order: ascending.
  CHECK(eq(run(r, s, "00 00 10 00 00 00 00 00 00 00 00 00 00 00 00"), "+04/00"));
  CHECK(eq(run(r, s, "00 00 30 00 00 00 00 00 00 00 00 00 00 00 00"), "+05/00"));
  CHECK(eq(run(r, s, "00 00 20 00 00 00 00 00 00 00 00 00 00 00 00"), "-04/00"));
  // Eight keys at once: more than 6-key rollover allows.
  CHECK(eq(run(r, s, "00 00 F0 0F 00 00 00 00 00 00 00 00 00 00 00"),
           "+04/00 +06/00 +07/00 +08/00 +09/00 +0A/00 +0B/00"));
  // Usage 0x67 (last bit) and Enter (0x28 = byte 2 + 5, bit 0)
  CHECK(eq(run(r, s, "00 00 00 00 00 00 00 01 00 00 00 00 00 00 80"),
           "+28/00 +67/00 -04/00 -05/00 -06/00 -07/00 -08/00 -09/00 -0A/00 -0B/00"));
}

static void testBroken() {
  ReportMap m;
  CHECK(parseReportMap(nullptr, 0, m) == ParseResult::Empty);
  CHECK(parseReportMap(kKeysToGo2, 49, m) == ParseResult::Malformed);  // cut inside an item
  CHECK(m.count == 0);
  const uint8_t endFirst[] = {0xC0, 0x05, 0x07};
  CHECK(parseReportMap(endFirst, sizeof(endFirst), m) == ParseResult::Malformed);
  const uint8_t popEmpty[] = {0xB4};
  CHECK(parseReportMap(popEmpty, sizeof(popEmpty), m) == ParseResult::Malformed);
  const uint8_t pushDeep[] = {0xA4, 0xA4, 0xA4, 0xA4, 0xA4};
  CHECK(parseReportMap(pushDeep, sizeof(pushDeep), m) == ParseResult::Malformed);
  const uint8_t idZero[] = {0x85, 0x00};
  CHECK(parseReportMap(idZero, sizeof(idZero), m) == ParseResult::Malformed);
  // A mouse only: parses, but there is nothing to type with.
  const uint8_t mouse[] = {0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03,
                           0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
                           0x75, 0x05, 0x81, 0x03, 0xC0};
  CHECK(parseReportMap(mouse, sizeof(mouse), m) == ParseResult::NoKeyboard);
  // Random bytes must never crash or read out of bounds (run under ASan).
  srand(1);
  uint8_t junk[300];
  for (int t = 0; t < 20000; t++) {
    size_t n = size_t(rand() % (int)sizeof(junk));
    for (size_t i = 0; i < n; i++) junk[i] = uint8_t(rand());
    if (parseReportMap(junk, n, m) == ParseResult::Ok) {
      KeyState s{};
      KeyEvent ev[32];
      for (int r = 0; r < m.count; r++) decodeReport(m.reports[r], junk, n < 64 ? n : 64, s, ev, 32);
    }
  }
}

static void testLegacy() {
  KeyState s{};
  KeyEvent ev[16];
  const uint8_t eight[] = {0x02, 0x00, 0x04, 0, 0, 0, 0, 0};
  CHECK(decodeLegacy(eight, 8, s, ev, 16) == 1 && ev[0].usage == 0x04 && ev[0].mods == 0x02 && ev[0].pressed);
  const uint8_t seven[] = {0x00, 0, 0, 0, 0, 0, 0};
  CHECK(decodeLegacy(seven, 7, s, ev, 16) == 1 && ev[0].usage == 0x04 && !ev[0].pressed);
  const uint8_t two[] = {0x08, 0x00};
  CHECK(decodeLegacy(two, 2, s, ev, 16) == 0);
}

static void testKeyRepeat() {
  KeyRepeat r;
  const uint8_t A = 0x04, B = 0x05, ENTER = 0x28, BKSP = 0x2A, LEFT = 0x50;
  // Nothing before the delay, then one every kRateMs.
  r.onEvent(A, 0, true, 1000);
  CHECK(r.tick(1000, 0, true) == 0 && r.tick(1499, 0, true) == 0);
  CHECK(r.tick(1500, 0, true) == A);
  CHECK(r.tick(1500, 0, true) == 0 && r.tick(1569, 0, true) == 0 && r.tick(1570, 0, true) == A);
  // A stall gives one repeat, not a backlog.
  CHECK(r.tick(3000, 0, true) == A && r.tick(3001, 0, true) == 0);
  // Release stops it; releasing another key does not.
  r.onEvent(B, 0, false, 3010);
  CHECK(r.tick(3100, 0, true) == A);
  r.onEvent(A, 0, false, 3110);
  CHECK(r.tick(4000, 0, true) == 0 && r.key() == 0);
  // The last key pressed repeats, with its own delay.
  r.onEvent(A, 0, true, 5000);
  r.onEvent(B, 0, true, 5400);
  CHECK(r.tick(5600, 0, true) == 0 && r.tick(5900, 0, true) == B);
  r.onEvent(A, 0, false, 5950);   // the first key let go: B keeps going
  CHECK(r.tick(6000, 0, true) == B);
  // A key that does not repeat stops the held one.
  r.onEvent(ENTER, 0, true, 6100);
  CHECK(r.key() == 0 && r.tick(7000, 0, true) == 0);
  // Shift is fine; Ctrl only for editing keys; Win/Cmd never.
  CHECK(KeyRepeat::repeatable(A, 0x02) && KeyRepeat::repeatable(A, 0x40));   // Shift, AltGr
  CHECK(!KeyRepeat::repeatable(A, 0x01) && KeyRepeat::repeatable(BKSP, 0x01) && KeyRepeat::repeatable(LEFT, 0x10));
  CHECK(!KeyRepeat::repeatable(A, 0x08) && !KeyRepeat::repeatable(LEFT, 0x80));
  CHECK(!KeyRepeat::repeatable(ENTER, 0) && !KeyRepeat::repeatable(0x29, 0) && !KeyRepeat::repeatable(0x2B, 0));
  CHECK(!KeyRepeat::repeatable(0x3A, 0) && !KeyRepeat::repeatable(0x58, 0) && !KeyRepeat::repeatable(0xE1, 0));
  CHECK(KeyRepeat::repeatable(0x2C, 0) && KeyRepeat::repeatable(0x4C, 0) && KeyRepeat::repeatable(0x4A, 0));
  CHECK(KeyRepeat::repeatable(0x64, 0) && KeyRepeat::repeatable(0x59, 0));
  // Ctrl pressed while a letter is held (no key event): the repeat stops.
  r.onEvent(A, 0, true, 8000);
  CHECK(r.tick(8600, 0x01, true) == 0 && r.key() == 0);
  // Ctrl+Backspace held: repeats.
  r.onEvent(BKSP, 0x01, true, 9000);
  CHECK(r.tick(9500, 0x01, true) == BKSP);
  r.cancel();
  CHECK(r.tick(9600, 0x01, true) == 0);
  // millis() wrapping past 2^32 keeps working.
  r.onEvent(A, 0, true, 0xFFFFFF00u);
  CHECK(r.tick(0xFFFFFF00u + 499, 0, true) == 0 && r.tick(0xFFFFFF00u + 500, 0, true) == A);
}

static void testKeyRepeatPacing() {
  KeyRepeat r;
  const uint8_t A = 0x04;
  // First phase: one step, then nothing until the screen has shown it.
  r.onEvent(A, 0, true, 0);
  CHECK(r.tick(500, 0, true) == A);
  CHECK(r.tick(600, 0, false) == 0 && r.tick(900, 0, false) == 0);   // refresh still running
  CHECK(r.tick(950, 0, true) == A);                                  // caught up: next step
  CHECK(r.tick(1000, 0, true) == 0);                                 // and still the minimum gap
  // After kFastAfterMs of repeating (from 2000 ms): up to kBurst steps per refresh.
  CHECK(r.tick(2000, 0, true) == A);
  CHECK(r.tick(2070, 0, false) == A && r.tick(2140, 0, false) == A);
  CHECK(r.tick(2210, 0, false) == 0 && r.tick(2500, 0, false) == 0);   // three unshown: wait
  CHECK(r.tick(2510, 0, true) == A);
  // A new press starts slow again.
  r.onEvent(A, 0, false, 2600);
  r.onEvent(A, 0, true, 3000);
  CHECK(r.tick(3500, 0, true) == A && r.tick(3570, 0, false) == 0);
}

int main() {
  testKeyRepeat();
  testKeyRepeatPacing();
  testKeysToGo2();
  testMxKeysMini();
  testBootStyle();
  testZmkNkro();
  testBroken();
  testLegacy();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
