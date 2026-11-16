// Host tests for the editor line layout. Run with: sh test/editor/run-on-hinton.sh
//
// The main check: after any edit, update() must leave exactly the table a full rebuild() gives. Random texts (words,
// spaces, newlines, long words, 2- to 4-byte UTF-8, cut-off UTF-8 at the end) and random inserts, deletes and
// replacements, in pixel and character wrapping, with narrow and wide lines, a small scratch area (so the fall-back to
// a full layout runs) and a table that cannot grow past a ceiling.
//
// Why update() may restart two lines before the change: a line's break is decided at its overflow character i; the next
// line starts at or before i and fits everything before i, so the line after that starts at or after i. The decision
// that ended line L-3 therefore lies before line L-1 starts, which is before a change in line L.
#include "editor_layout.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace edlayout;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                      \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }       \
  } while (0)

static int g_ceiling = 1 << 30;   // largest table the grow callback allows

static bool growTable(LineTable& t, int need) {
  if (need > g_ceiling) return false;
  int cap = t.cap ? t.cap : 4;
  while (cap < need) cap *= 2;
  if (cap > g_ceiling) cap = g_ceiling;
  int* p = (int*)realloc(t.pos, (size_t)cap * sizeof(int));
  if (!p) return false;
  t.pos = p;
  t.cap = cap;
  return true;
}

struct Table {
  LineTable t;
  Table() { t.grow = growTable; }
  ~Table() { free(t.pos); }
  std::vector<int> v() const { return std::vector<int>(t.pos, t.pos + t.count); }
};

static uint16_t g_w[128];

static WrapParams params(bool px, int width) {
  WrapParams p;
  if (px) {
    p.pxMax = width;
    p.pxW = g_w;
    p.pxFallback = 9;
  } else {
    p.charsPerLine = width;
  }
  return p;
}

static std::string randomPiece(std::mt19937& r, int maxLen) {
  static const char* bits[] = {"the", "a", "of", "writing", "MicroSlate", "x", "  ", " ", " ", " ", "\n", "\n\n",
                               "caf\xC3\xA9", "\xE2\x80\x94", "\xF0\x9F\x99\x82", "supercalifragilisticexpialidocious",
                               "WWWWWWWWWWWWWWWWWWWWWWWWWWWWWW", "iiiiiiiiii", ".", ",", "don't", "\t", "\n\t"};
  std::string s;
  const int n = (int)(r() % (unsigned)maxLen);
  while ((int)s.size() < n) s += bits[r() % (sizeof(bits) / sizeof(bits[0]))];
  return s;
}

// Plain wrapping facts on a short text.
static void testBasics() {
  Table a;
  const WrapParams p = params(false, 10);
  const char* s = "hello world this is\nshort";
  rebuild(a.t, s, (int)strlen(s), p);
  // "hello " (6) + "world " -> wraps after the space before "world"? 10 columns: "hello worl" overflows at 'l'.
  CHECK(a.t.count >= 3);
  CHECK(a.t.pos[0] == 0);
  CHECK(a.t.pos[1] == 6);                         // "world" starts the second line
  for (int i = 1; i < a.t.count; i++) CHECK(a.t.pos[i] > a.t.pos[i - 1]);
  CHECK(lineOf(a.t, 0) == 0);
  CHECK(lineOf(a.t, 5) == 0);
  CHECK(lineOf(a.t, 6) == 1);
  CHECK(lineOf(a.t, (int)strlen(s)) == a.t.count - 1);

  Table e;
  rebuild(e.t, "", 0, p);
  CHECK(e.t.count == 1 && e.t.pos[0] == 0);
  rebuild(e.t, "\n", 1, p);
  CHECK(e.t.count == 2 && e.t.pos[1] == 1);       // the empty line after a final newline exists
}

// A tab is one byte with its own fixed width (the device gives it four spaces): an indent pushes the line's words along.
static void testTabs() {
  Table a;
  const WrapParams p = params(true, 50);   // tab 20, letters 9, space 5
  rebuild(a.t, "\taa b", 5, p);            // 20 + 18 + 5 + 9 = 52: "b" wraps (as a space it would fit)
  CHECK(a.t.count == 2 && a.t.pos[1] == 4);
  rebuild(a.t, "aa b", 4, p);              // the same words with no indent fit on one line
  CHECK(a.t.count == 1);
  rebuild(a.t, "\t\taaaa", 6, p);          // 40 + 9 + 9: no space on the line, so it breaks before the letter past 50
  CHECK(a.t.count == 2 && a.t.pos[1] == 3);
}

// The note-full check, done before a selection is deleted (cap 16 holds at most 15 bytes).
static void testReplacementFits() {
  CHECK(replacementFits(14, 0, 1, 16));     // the 15th byte fits
  CHECK(!replacementFits(15, 0, 1, 16));    // the 16th does not
  CHECK(replacementFits(15, 3, 3, 16));     // typing over a selection the same size: still 15
  CHECK(!replacementFits(15, 2, 3, 16));    // a paste one byte longer than the selection it replaces
  CHECK(replacementFits(15, 15, 15, 16));   // select all, paste the same amount
  CHECK(replacementFits(0, 0, 0, 1));
  CHECK(!replacementFits(524287, 0, 1, 524288));   // the S3 limit: a full 512 KB note takes no more
}

static long g_edits = 0;

static void fuzz(unsigned seed, bool px, int width, int scratchCap, int ceiling, int rounds) {
  std::mt19937 r(seed);
  g_ceiling = ceiling;
  const WrapParams p = params(px, width);
  std::string text = randomPiece(r, 400);
  Table inc;
  rebuild(inc.t, text.data(), (int)text.size(), p);
  std::vector<int> scratch((size_t)scratchCap + 1);
  for (int k = 0; k < rounds; k++) {
    const int len = (int)text.size();
    const int at = len ? (int)(r() % (unsigned)(len + 1)) : 0;
    const int maxDel = len - at;
    int del = 0;
    std::string ins;
    switch (r() % 4) {
      case 0: ins = randomPiece(r, 3) + (r() % 2 ? "a" : ""); break;   // typing
      case 1: del = maxDel ? 1 + (int)(r() % (unsigned)std::min(maxDel, 3)) : 0; break;   // backspace / delete
      case 2: del = maxDel ? (int)(r() % (unsigned)(maxDel + 1)) : 0; ins = randomPiece(r, 60); break;   // replace
      default: ins = randomPiece(r, 300); break;   // paste
    }
    // Edits may land inside a UTF-8 character: the layout must still agree with a full rebuild.
    text.replace((size_t)at, (size_t)del, ins);
    if (text.size() > 6000) text.resize(3000);
    const bool trimmed = (int)text.size() != len - del + (int)ins.size();
    if (trimmed) {
      rebuild(inc.t, text.data(), (int)text.size(), p);
    } else {
      update(inc.t, text.data(), (int)text.size(), at, del, (int)ins.size(), p, scratch.data(), scratchCap);
    }
    Table full;
    rebuild(full.t, text.data(), (int)text.size(), p);
    g_edits++;
    if (inc.v() != full.v()) {
      g_fail++;
      printf("FAIL fuzz seed=%u px=%d width=%d round=%d at=%d del=%d ins=%d: %d lines vs %d\n", seed, (int)px, width,
             k, at, del, (int)ins.size(), inc.t.count, full.t.count);
      return;
    }
    // Starts are increasing and inside the text.
    bool ok = inc.t.count >= 1 && inc.t.pos[0] == 0;
    for (int i = 1; i < inc.t.count; i++) ok = ok && inc.t.pos[i] > inc.t.pos[i - 1] && inc.t.pos[i] <= (int)text.size();
    CHECK(ok);
  }
  g_ceiling = 1 << 30;
}

// A long paragraph of ordinary prose: one keystroke in the middle touches only a few lines (the point of the change).
// Lines that are all exactly full (the same word over and over) cannot absorb a change, so there a keystroke pushes a
// word down every following line to the end of the paragraph; the result must still be right.
static void testWorkStaysLocal() {
  static const char* words[] = {"the", "quiet", "house", "on", "a", "hill", "where", "nobody", "had", "written",
                                "anything", "for", "years", "and", "then", "she", "came", "back", "with", "letters"};
  std::mt19937 r(7);
  std::string text;
  while (text.size() < 100000) text += std::string(words[r() % 20]) + " ";
  const WrapParams p = params(false, 40);
  Table t;
  rebuild(t.t, text.data(), (int)text.size(), p);
  CHECK(t.t.count > 2000);
  int scratch[8];
  const int at = 50000;
  text.insert((size_t)at, "x");
  CHECK(update(t.t, text.data(), (int)text.size(), at, 0, 1, p, scratch, 8));   // stayed local
  Table full;
  rebuild(full.t, text.data(), (int)text.size(), p);
  CHECK(t.v() == full.v());

  std::string same;
  for (int i = 0; i < 20000; i++) same += "word ";
  Table u;
  rebuild(u.t, same.data(), (int)same.size(), p);
  same.insert(50000, "x");
  update(u.t, same.data(), (int)same.size(), 50000, 0, 1, p, scratch, 8);   // falls back to a full layout
  Table full2;
  rebuild(full2.t, same.data(), (int)same.size(), p);
  CHECK(u.v() == full2.v());
}

int main() {
  for (int c = 0; c < 128; c++) g_w[c] = (uint16_t)(c == 'W' ? 17 : c == 'i' || c == '.' || c == ',' ? 4 : c == ' ' ? 5 : 9);
  g_w['\t'] = 20;
  testBasics();
  testTabs();
  testReplacementFits();
  testWorkStaysLocal();
  unsigned seed = 1;
  for (int width : {1, 2, 5, 9, 40, 100}) {
    fuzz(seed++, false, width, 512, 1 << 30, 1500);
    fuzz(seed++, false, width, 3, 1 << 30, 600);
  }
  for (int width : {3, 9, 30, 120, 400, 774}) {
    fuzz(seed++, true, width, 512, 1 << 30, 1500);
    fuzz(seed++, true, width, 3, 1 << 30, 600);
    fuzz(seed++, true, width, 512, 40, 600);   // the table cannot grow past 40 lines
  }
  g_checks++;   // the fuzz rounds, counted once
  printf("%ld random edits compared with a full layout\n", g_edits);
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
