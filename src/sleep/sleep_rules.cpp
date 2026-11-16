#include "sleep_rules.h"

#include <cstdio>
#include <cstring>

namespace sleepscr {

static uint32_t le32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool pictureLooksValid(const uint8_t* head, size_t headLen, size_t fileLen, int& w, int& h) {
  if (!head || headLen < 62 || fileLen > kMaxPictureBytes) return false;
  if (head[0] != 'B' || head[1] != 'M') return false;
  const uint32_t fileSize = le32(head + 2);
  const uint32_t offBits = le32(head + 10);
  const uint32_t dibSize = le32(head + 14);
  const int32_t width = (int32_t)le32(head + 18);
  const int32_t rawHeight = (int32_t)le32(head + 22);
  const uint16_t planes = le16(head + 26);
  const uint16_t bpp = le16(head + 28);
  const uint32_t comp = le32(head + 30);
  if (dibSize < 40 || planes != 1 || bpp != 1 || comp != 0) return false;
  const int32_t height = rawHeight < 0 ? -rawHeight : rawHeight;
  const bool screenSize = (width == 800 && height == 480) || (width == 480 && height == 800);
  if (!screenSize) return false;
  if (offBits < 14 + dibSize + 8 || offBits > 1024) return false;   // room for the two-colour palette
  const size_t rowBytes = ((size_t)width + 31) / 32 * 4;
  const size_t want = offBits + rowBytes * (size_t)height;
  if (fileLen != want) return false;
  if (fileSize != 0 && fileSize != fileLen) return false;   // 0 is allowed by the format for BI_RGB
  w = width;
  h = height;
  return true;
}

// The NNN of "sleep-NNN.bmp", or -1.
static int pictureNumber(const std::string& name) {
  if (name.size() < 11 || name.compare(0, 6, "sleep-") != 0) return -1;
  if (name.compare(name.size() - 4, 4, ".bmp") != 0) return -1;
  int v = 0;
  for (size_t i = 6; i < name.size() - 4; i++) {
    if (name[i] < '0' || name[i] > '9' || v > 99999) return -1;
    v = v * 10 + (name[i] - '0');
  }
  return v;
}

std::string nextPictureName(const std::vector<std::string>& existing) {
  int top = 0;
  for (const std::string& n : existing) {
    const int v = pictureNumber(n);
    if (v > top) top = v;
  }
  char buf[24];
  snprintf(buf, sizeof(buf), "sleep-%03d.bmp", top + 1);
  return buf;
}

std::string pictureLabel(const std::string& name) {
  const int v = pictureNumber(name);
  if (v >= 0) return "Picture " + std::to_string(v);
  const size_t dot = name.rfind('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

Choice settleChoice(Choice c, bool chosenThere, int count) {
  if (c == Choice::Picture && chosenThere) return c;
  if (c == Choice::Stock) return c;
  if (count <= 0) return Choice::Stock;
  if (count == 1) return Choice::Picture;
  return Choice::Random;   // Random kept, or the chosen picture gone with several left
}

int pickRandom(int n, int last, uint32_t random) {
  if (n <= 0) return -1;
  if (n == 1) return 0;
  if (last < 0 || last >= n) return (int)(random % (uint32_t)n);
  const int i = (int)(random % (uint32_t)(n - 1));   // every picture but the last one, evenly
  return i >= last ? i + 1 : i;
}

}  // namespace sleepscr
