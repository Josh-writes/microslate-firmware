#include "screen_refresh.h"

#if defined(FREEINK_DEVICE_E1001) && FREEINK_DEVICE_E1001
#include <Preferences.h>

#include "driver/Uc8179E1001Driver.h"

namespace refresh {
namespace {
bool g_fast = false;

void apply() { freeink::uc8179E1001SetFastWaveform(g_fast); }
}  // namespace

bool present() { return true; }

void begin() {
  Preferences p;
  p.begin("refresh", true);
  g_fast = p.getBool("fast", false);
  p.end();
  apply();
}

void cycle(int) {
  g_fast = !g_fast;
  apply();
  Preferences p;
  p.begin("refresh", false);
  p.putBool("fast", g_fast);
  p.end();
}

const char* label() { return g_fast ? "Fast" : "Clear"; }

}  // namespace refresh

#else

namespace refresh {
bool present() { return false; }
void begin() {}
void cycle(int) {}
const char* label() { return ""; }
}  // namespace refresh

#endif
