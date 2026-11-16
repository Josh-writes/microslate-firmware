// Host tests for the sleep screen rules. Run with: sh test/sleep/run-on-hinton.sh
#include "sleep_rules.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace sleepscr;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                      \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }       \
  } while (0)

static void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
  for (int i = 0; i < 4; i++) b[at + i] = (uint8_t)(v >> (8 * i));
}
static void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
  b[at] = (uint8_t)v;
  b[at + 1] = (uint8_t)(v >> 8);
}

// What the phone page builds: 14 + 40 header bytes, a two-colour palette, rows padded to 4 bytes.
static std::vector<uint8_t> bmp(int w, int h, uint16_t bpp = 1) {
  const size_t rowBytes = ((size_t)w + 31) / 32 * 4;
  const size_t off = 62;
  std::vector<uint8_t> b(off + rowBytes * (size_t)(h < 0 ? -h : h), 0);
  b[0] = 'B';
  b[1] = 'M';
  put32(b, 2, (uint32_t)b.size());
  put32(b, 10, off);
  put32(b, 14, 40);
  put32(b, 18, (uint32_t)w);
  put32(b, 22, (uint32_t)h);
  put16(b, 26, 1);
  put16(b, 28, bpp);
  put32(b, 46, 2);
  b[58] = b[59] = b[60] = 255;   // palette 1 = white
  return b;
}

static bool valid(const std::vector<uint8_t>& b, int& w, int& h) {
  return pictureLooksValid(b.data(), b.size(), b.size(), w, h);
}

static void testValidPictures() {
  int w = 0, h = 0;
  auto land = bmp(800, 480);
  CHECK(land.size() == 48062);
  CHECK(valid(land, w, h) && w == 800 && h == 480);
  auto port = bmp(480, 800);
  CHECK(valid(port, w, h) && w == 480 && h == 800);
  auto topDown = bmp(800, -480);
  CHECK(valid(topDown, w, h) && h == 480);
}

static void testRefused() {
  int w = 0, h = 0;
  auto b = bmp(800, 480);
  auto bad = b;
  bad[0] = 'P';
  CHECK(!valid(bad, w, h));                                   // not a BMP
  CHECK(!valid(bmp(640, 480), w, h));                         // not one of our screens
  CHECK(!valid(bmp(800, 480, 24), w, h));                     // colour
  bad = b;
  put32(bad, 30, 1);
  CHECK(!valid(bad, w, h));                                   // compressed
  bad = b;
  bad.pop_back();
  CHECK(!pictureLooksValid(bad.data(), bad.size(), bad.size(), w, h));   // short
  CHECK(!pictureLooksValid(b.data(), b.size(), b.size() + 1, w, h));     // longer than its header says
  CHECK(!pictureLooksValid(b.data(), 40, b.size(), w, h));               // header cut off
  CHECK(!pictureLooksValid(nullptr, 0, 0, w, h));
  CHECK(!pictureLooksValid(b.data(), b.size(), kMaxPictureBytes + 1, w, h));
  bad = b;
  put32(bad, 2, 12345);
  CHECK(!valid(bad, w, h));                                   // size field disagrees
  bad = b;
  put32(bad, 2, 0);
  CHECK(valid(bad, w, h));                                    // 0 is allowed
  bad = b;
  put16(bad, 26, 2);
  CHECK(!valid(bad, w, h));                                   // planes
}

static void testNames() {
  CHECK(nextPictureName({}) == "sleep-001.bmp");
  CHECK(nextPictureName({"sleep-001.bmp", "sleep-002.bmp"}) == "sleep-003.bmp");
  CHECK(nextPictureName({"sleep-007.bmp", "sleep-002.bmp"}) == "sleep-008.bmp");   // never reuses a gap below the top
  CHECK(nextPictureName({"holiday.bmp", "sleep-x.bmp", "sleep-.bmp"}) == "sleep-001.bmp");
  CHECK(nextPictureName({"sleep-999.bmp"}) == "sleep-1000.bmp");
  CHECK(pictureLabel("sleep-003.bmp") == "Picture 3");
  CHECK(pictureLabel("sleep-1000.bmp") == "Picture 1000");
  CHECK(pictureLabel("holiday.bmp") == "holiday");
  CHECK(pictureLabel("noext") == "noext");
}

static void testPick() {
  CHECK(pickRandom(0, -1, 5) == -1);
  CHECK(pickRandom(1, 0, 7) == 0);
  for (uint32_t r = 0; r < 200; r++) {
    for (int last = 0; last < 4; last++) {
      const int p = pickRandom(4, last, r);
      CHECK(p >= 0 && p < 4 && p != last);
    }
  }
  int seen[3] = {0, 0, 0};
  for (uint32_t r = 0; r < 300; r++) seen[pickRandom(3, 1, r)]++;
  CHECK(seen[1] == 0 && seen[0] == 150 && seen[2] == 150);   // the others evenly
  CHECK(pickRandom(5, 9, 3) == 3);                            // a stale "last" is ignored
}

static void testSettle() {
  CHECK(settleChoice(Choice::Stock, false, 0) == Choice::Stock);
  CHECK(settleChoice(Choice::Stock, false, 5) == Choice::Stock);          // adding pictures never changes Stock here
  CHECK(settleChoice(Choice::Picture, true, 3) == Choice::Picture);
  CHECK(settleChoice(Choice::Picture, false, 3) == Choice::Random);       // the chosen one deleted
  CHECK(settleChoice(Choice::Picture, false, 1) == Choice::Picture);      // the only one left
  CHECK(settleChoice(Choice::Picture, false, 0) == Choice::Stock);
  CHECK(settleChoice(Choice::Random, false, 2) == Choice::Random);
  CHECK(settleChoice(Choice::Random, false, 1) == Choice::Picture);
  CHECK(settleChoice(Choice::Random, false, 0) == Choice::Stock);
}

int main() {
  testSettle();
  testValidPictures();
  testRefused();
  testNames();
  testPick();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
