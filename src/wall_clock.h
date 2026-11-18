#pragma once

#include <ctime>

// Real time for the dates on notes (2026-11-18). The Sticky and E1001 have a PCF8563 clock chip that keeps counting
// while the device sleeps or is switched off; the internet time (NTP) sets it. Elsewhere
// (or when the chip has lost its time) the time is real once the internet gave it, and through deep sleep after that,
// where the system clock keeps running. Until then the notes keep the card clock in file_manager.cpp.
//
// The system time is set from the chip at boot. The internet time is asked for whenever Wi-Fi is connected (sync,
// transfer, update). The cloud code still waits for the internet time (netTimeSeen), so a wrong chip time never
// reaches a sync.
namespace wallclock {

void begin();            // at boot, before the SD card: the chip's time becomes the system time when it is valid and
                         // MicroSlate set it from the internet before (a chip's earlier time is not trusted)
bool known();            // the system time is real (chip, internet, or kept through deep sleep)
bool netTimeSeen();      // the internet time arrived since the last restart (kept through deep sleep)
bool poll();             // main loop: asks for the internet time once Wi-Fi is up; when it arrives, writes it to the
                         // chip and returns true (once per arrival)

// Seconds since 1970 for a UTC date and time (no time zone: MicroSlate keeps every date in UTC).
time_t fromUtc(int year, int month, int day, int hour, int minute, int second);

}  // namespace wallclock
