#include "update_rules.h"

#include <cstdio>
#include <cstring>

namespace updrules {
namespace {

constexpr uint32_t kClockSet = 1767225600u;   // 2026-01-01: an earlier wall clock has not been set

struct MinReader {
  const char* id;
  const char* tag;   // the first release that drives the SSD2677
};
// CrossPoint v2.0.0-ms1 tested on the SSD2677 Sticky 2026-11-21 (FreeInk 5e94fe7). CrossInk v2.0.0-ms1 is the
// first release from CrossInk 2.0.0, whose FreeInk has the driver (1.6.1-ms1 does not).
constexpr MinReader kSsd2677Min[] = {
    {"crosspoint", "v2.0.0-ms1"},
    {"crossink", "v2.0.0-ms1"},
};

}  // namespace

long long tagNumber(const char* tag) {
  if (!tag) return 0;
  if (*tag == 'v' || *tag == 'V') tag++;
  int a = 0, b = 0, c = 0, n = 0;
  if (sscanf(tag, "%d.%d.%d%n", &a, &b, &c, &n) < 3 || a < 0 || b < 0 || c < 0 || b > 999 || c > 999) return 0;
  int ms = 0;
  const char* rest = tag + n;
  if (strncmp(rest, "-ms", 3) == 0 && sscanf(rest + 3, "%d", &ms) != 1) ms = 0;
  if (ms < 0 || ms > 999) ms = 0;
  return a * 1000000000LL + b * 1000000LL + c * 1000LL + ms;
}

bool checkDue(uint32_t now, uint32_t last, uint32_t failed) {
  if (now < kClockSet) return true;   // the clock is not set (X4 Pro after a power loss): the check sets it
  if (failed && now >= failed && now - failed < kRetryS) return false;
  return last == 0 || now < last || now - last >= kDueS;
}

bool firmwareNewer(const char* seen, const char* running) {
  const long long s = tagNumber(seen);
  return s > 0 && s > tagNumber(running);
}

bool readerFitsScreen(const char* id, const char* tag, bool ssd2677) {
  if (!ssd2677) return true;
  if (!id) return false;
  for (const MinReader& m : kSsd2677Min)
    if (strcmp(m.id, id) == 0) {
      const long long t = tagNumber(tag);
      return t > 0 && t >= tagNumber(m.tag);
    }
  return false;
}

}  // namespace updrules
