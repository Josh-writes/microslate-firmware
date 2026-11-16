#include "frontlight.h"

#include <BoardConfig.h>

#if FREEINK_CAP_FRONTLIGHT
#include <FrontlightManager.h>
#include <Preferences.h>

namespace light {
namespace {
constexpr uint8_t kSteps[] = {0, 20, 40, 60, 80, 100};   // percent; FreeInk maps it through a perceptual curve
constexpr int kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);
FrontlightManager g_mgr;
int g_step = 0;
bool g_begun = false;
}  // namespace

bool present() { return g_mgr.present(); }

void begin() {
  if (!g_mgr.present()) return;
  g_mgr.begin();
  g_mgr.setColorTemperature(50);   // warm and cool equally
  Preferences p;
  p.begin("light", true);
  g_step = p.getUChar("step", 0);
  p.end();
  if (g_step < 0 || g_step >= kStepCount) g_step = 0;
  g_mgr.setBrightness(kSteps[g_step]);
  g_begun = true;
}

void cycle(int dir) {
  if (!g_begun) return;
  g_step = (g_step + (dir < 0 ? kStepCount - 1 : 1)) % kStepCount;
  g_mgr.setBrightness(kSteps[g_step]);
  Preferences p;
  p.begin("light", false);
  p.putUChar("step", static_cast<uint8_t>(g_step));
  p.end();
}

void off() {
  if (g_begun) g_mgr.setBrightness(0);
}

const char* label() {
  static const char* kLabels[] = {"Off", "20%", "40%", "60%", "80%", "100%"};
  return kLabels[g_step];
}

}  // namespace light

#else

namespace light {
bool present() { return false; }
void begin() {}
void cycle(int) {}
void off() {}
const char* label() { return ""; }
}  // namespace light

#endif
