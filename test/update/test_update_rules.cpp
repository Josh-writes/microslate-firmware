// Host tests for the update rules (Check for updates, reader releases on the SSD2677 Sticky).
// Run with: sh test/update/run-on-hinton.sh
#include "update_rules.h"

#include <cstdio>

using namespace updrules;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                      \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }       \
  } while (0)

static void testTagNumber() {
  CHECK(tagNumber("v3.0.6") == tagNumber("3.0.6"));
  CHECK(tagNumber("v3.0.10") > tagNumber("v3.0.9"));   // numbers, not text
  CHECK(tagNumber("v3.1.0") > tagNumber("v3.0.99"));
  CHECK(tagNumber("v10.0.0") > tagNumber("v9.9.9"));
  CHECK(tagNumber("v2.0.0-ms2") > tagNumber("v2.0.0-ms1"));
  CHECK(tagNumber("v2.0.0-ms10") > tagNumber("v2.0.0-ms9"));
  CHECK(tagNumber("v2.0.1-ms1") > tagNumber("v2.0.0-ms5"));
  CHECK(tagNumber("v2.0.0-ms1") > tagNumber("v2.0.0"));
  CHECK(tagNumber("v1.6.1-ms1") < tagNumber("v2.0.0-ms1"));
  CHECK(tagNumber("v2.0.0-rc3") == tagNumber("v2.0.0"));   // anything else after the version is ignored
  CHECK(tagNumber("") == 0);
  CHECK(tagNumber(nullptr) == 0);
  CHECK(tagNumber("latest") == 0);
  CHECK(tagNumber("v3.0") == 0);   // a version has three parts
  CHECK(tagNumber("v3.-1.0") == 0);
}

static void testCheckDue() {
  const uint32_t t = 1795000000u;   // 2026-11
  CHECK(checkDue(t, 0, 0));                        // never checked
  CHECK(!checkDue(t, t - 3600, 0));                // an hour ago
  CHECK(!checkDue(t, t - kDueS + 1, 0));
  CHECK(checkDue(t, t - kDueS, 0));                // 20 hours ago: the next morning's first wake
  CHECK(checkDue(t, t - 23 * 3600 - 50 * 60, 0));  // yesterday at 8:00, now 7:50
  CHECK(checkDue(t, t + 3600, 0));                 // the clock went back
  CHECK(checkDue(1000, t, 0));                     // the clock is not set: the check sets it
  CHECK(checkDue(1735689600u, t, 0));              // 2025-01-01: not set either
  CHECK(!checkDue(t, 0, t - 600));                 // failed 10 minutes ago: wait
  CHECK(!checkDue(t, t - 2 * kDueS, t - kRetryS + 1));
  CHECK(checkDue(t, t - 2 * kDueS, t - kRetryS));  // an hour after a failed try
  CHECK(!checkDue(t, t - 3600, t - 2 * kRetryS));  // an old failure does not make it due
  CHECK(checkDue(t, 0, t + 600));                  // a failure "in the future" (clock went back) is ignored
}

static void testFirmwareNewer() {
  CHECK(firmwareNewer("3.0.6", "3.0.5"));
  CHECK(firmwareNewer("v3.0.10", "3.0.9"));
  CHECK(!firmwareNewer("3.0.5", "3.0.5"));   // that version runs: no notice
  CHECK(!firmwareNewer("3.0.4", "3.0.5"));
  CHECK(!firmwareNewer("", "3.0.5"));
  CHECK(!firmwareNewer("junk", "3.0.5"));
}

static void testReaderFitsScreen() {
  // Older Sticky (SSD1677): anything goes.
  CHECK(readerFitsScreen("crossink", "v1.6.1-ms1", false));
  CHECK(readerFitsScreen("crossink", "", false));
  // SSD2677 Sticky.
  CHECK(readerFitsScreen("crosspoint", "v2.0.0-ms1", true));
  CHECK(readerFitsScreen("crosspoint", "v2.0.0-ms2", true));
  CHECK(readerFitsScreen("crosspoint", "v2.1.0-ms1", true));
  CHECK(!readerFitsScreen("crosspoint", "v1.9.9-ms3", true));
  CHECK(!readerFitsScreen("crossink", "v1.6.1-ms1", true));   // the released CrossInk freezes the new screen
  CHECK(readerFitsScreen("crossink", "v2.0.0-ms1", true));
  CHECK(!readerFitsScreen("crosspoint", "v2.0.0", true));   // an upstream tag is older than our -ms1 build
  CHECK(!readerFitsScreen("crosspoint", "", true));        // unknown release
  CHECK(!readerFitsScreen("other", "v9.0.0-ms1", true));   // unknown reader
  CHECK(!readerFitsScreen(nullptr, "v9.0.0-ms1", true));
}

int main() {
  testTagNumber();
  testCheckDue();
  testFirmwareNewer();
  testReaderFitsScreen();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
