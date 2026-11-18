#include "wall_clock.h"
#include "config.h"
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <atomic>
#include <sys/time.h>
#include <esp_attr.h>
#include <esp_sleep.h>

// The clock chip is used on the Sticky and the E1001. The X4 Pro has one too, on the touch controller's bus, but it
// is not tested there: it keeps the internet time only, like the X4.
#if (defined(FREEINK_DEVICE_STICKY) && FREEINK_DEVICE_STICKY) || (defined(FREEINK_DEVICE_E1001) && FREEINK_DEVICE_E1001)
#define MS_CLOCK_CHIP 1
#include <Rtc.h>
#else
#define MS_CLOCK_CHIP 0
#endif

namespace wallclock {
namespace {

constexpr time_t kPlausible = 1767225600;   // 2026-01-01: a clock showing less was never set

RTC_DATA_ATTR bool g_sysReal = false;       // the system time was real when the device went into deep sleep
RTC_DATA_ATTR bool g_netSeen = false;       // the internet time arrived (DeviceClock::epochNow trusts only this)
bool g_known = false;
bool g_asked = false;                       // configTime() was called since boot
std::atomic<bool> g_netFresh{false};

// SNTP's task calls this each time it sets the system clock (the first time and on its hourly checks).
void onNetTime(struct timeval*) {
  g_netSeen = true;
  g_netFresh.store(true);
}

#if MS_CLOCK_CHIP
Rtc g_chip;

// The chip's time is used only once MicroSlate has set it from the internet (NVS "wallclock/chip"). A chip comes with
// whatever time it was given before (the first Sticky read 2026-03-17 in November): a time behind is harmless, but one
// ahead (a factory's local time) would date notes in the future, above everything written after the next correction.
bool chipTrusted() {
  Preferences p;
  if (!p.begin("wallclock", true)) return false;
  const bool v = p.getUChar("chip", 0) != 0;
  p.end();
  return v;
}

void chipSetTrusted(bool on) {
  Preferences p;
  if (!p.begin("wallclock", false)) return;
  p.putUChar("chip", on ? 1 : 0);
  p.end();
}
#endif

}  // namespace

time_t fromUtc(int year, int month, int day, int hour, int minute, int second) {
  // Days since 1970-01-01 for a proleptic Gregorian date (H. Hinnant's days_from_civil).
  const int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long)era * 146097 + doe - 719468;
  return (time_t)days * 86400 + hour * 3600 + minute * 60 + second;
}

void begin() {
  sntp_set_time_sync_notification_cb(onNetTime);   // before any configTime (here or in the cloud code)
  // Deep sleep keeps the system clock running; a restart or power loss does not.
  if (g_sysReal && esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED && time(nullptr) > kPlausible)
    g_known = true;
#if MS_CLOCK_CHIP
  Rtc::DateTime dt;
  if (g_chip.begin() && g_chip.now(dt)) {
    const time_t t = fromUtc(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second);
    const bool use = t > kPlausible && chipTrusted();
    if (use) {
      const timeval tv{t, 0};
      settimeofday(&tv, nullptr);
      g_known = true;
    }
    DBG_PRINTF("Clock chip: %04d-%02d-%02d %02d:%02d:%02d UTC%s\n", dt.year, dt.month, dt.day, dt.hour, dt.minute,
               dt.second, use ? "" : " (not set from the internet yet, not used)");
  } else {
    if (g_chip.present()) chipSetTrusted(false);   // it stopped (flat battery): wait for the internet time again
    DBG_PRINTF("Clock chip: %s\n", g_chip.present() ? "lost its time (not used)" : "not answering");
  }
#endif
  g_sysReal = g_known;
}

bool known() { return g_known; }

bool netTimeSeen() { return g_netSeen; }

bool poll() {
  // Any Wi-Fi connection (not the transfer hotspot) asks once per boot; SNTP keeps the time up to date after that.
  if (!g_asked && WiFi.status() == WL_CONNECTED) {
    configTime(0, 0, "pool.ntp.org", "time.google.com");
    g_asked = true;
  }
  if (!g_netFresh.exchange(false)) return false;
  const time_t t = time(nullptr);
  if (t <= kPlausible) return false;
  g_known = g_sysReal = true;
#if MS_CLOCK_CHIP
  if (g_chip.present()) {
    tm u;
    gmtime_r(&t, &u);
    Rtc::DateTime dt;
    dt.year = (uint16_t)(u.tm_year + 1900);
    dt.month = (uint8_t)(u.tm_mon + 1);
    dt.day = (uint8_t)u.tm_mday;
    dt.hour = (uint8_t)u.tm_hour;
    dt.minute = (uint8_t)u.tm_min;
    dt.second = (uint8_t)u.tm_sec;
    dt.weekday = (uint8_t)u.tm_wday;
    const bool ok = g_chip.set(dt);
    if (ok) chipSetTrusted(true);
    DBG_PRINTF("Clock chip set from the internet time: %s\n", ok ? "done" : "FAILED");
  }
#endif
  return true;
}

}  // namespace wallclock
