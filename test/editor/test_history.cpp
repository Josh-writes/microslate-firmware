// Host tests for the undo/redo history (src/edit_history.h). Run with: sh test/editor/run-on-hinton.sh
//
// A small model of the editor records each change the way text_editor.cpp does (just before making it) and applies
// undo/redo the same way. The main checks, over random typing, Backspace, Delete, pastes, selection replaces and cursor
// moves, with a roomy history and with tiny ones that must drop old steps:
// - every undo lands on a text that really existed, each one older than the last (never a mix of states);
// - with nothing dropped, undoing everything gives the starting text, and redoing everything the final text;
// - a new change after some undos drops the redo steps.
#include "edit_history.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace edhist;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                      \
  do {                                                                                \
    g_checks++;                                                                       \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); }       \
  } while (0)

struct Ed {
  std::string t;
  int cur = 0;
  std::vector<std::string> states;   // every text the editor showed, oldest first (for the undo checks)

  void snap() { states.push_back(t); }
  static bool oneChar(const std::string& s) {
    const unsigned char c = (unsigned char)s[0];
    const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    return n == s.size();
  }
  void type(const std::string& s) {
    record(cur, t.data() + cur, 0, s.data(), (int)s.size(), oneChar(s) ? Kind::Typing : Kind::Other, cur);
    t.insert((size_t)cur, s);
    cur += (int)s.size();
    snap();
  }
  void replace(int lo, int hi, const std::string& s) {   // typing or pasting over a selection
    record(lo, t.data() + lo, hi - lo, s.data(), (int)s.size(), Kind::Other, cur);
    t.replace((size_t)lo, (size_t)(hi - lo), s);
    cur = lo + (int)s.size();
    snap();
  }
  static bool cont(unsigned char c) { return (c & 0xC0) == 0x80; }
  void backspace() {
    if (cur == 0) return;
    int s = cur - 1;
    while (s > 0 && cont((unsigned char)t[(size_t)s])) s--;
    record(s, t.data() + s, cur - s, nullptr, 0, Kind::Backspace, cur);
    t.erase((size_t)s, (size_t)(cur - s));
    cur = s;
    snap();
  }
  void del() {
    if (cur >= (int)t.size()) return;
    int e = cur + 1;
    while (e < (int)t.size() && cont((unsigned char)t[(size_t)e])) e++;
    record(cur, t.data() + cur, e - cur, nullptr, 0, Kind::DeleteFwd, cur);
    t.erase((size_t)cur, (size_t)(e - cur));
    snap();
  }
  bool apply(bool (*step)(Change*)) {
    Change ch;
    if (!step(&ch)) return false;
    t.replace((size_t)ch.at, (size_t)ch.oldLen, std::string(ch.newText, (size_t)ch.newLen));
    cur = ch.cursor;
    return true;
  }
  bool u() { return apply(undo); }
  bool r() { return apply(redo); }
};

static std::vector<uint8_t> g_buf;
static void fresh(size_t cap) {
  g_buf.assign(cap, 0);
  init(g_buf.data(), cap);
}

static void typeAll(Ed& e, const char* s) {
  for (const char* p = s; *p; p++) e.type(std::string(1, *p));
}

// Typing goes back a word (with its spaces) at a time.
static void testWords() {
  fresh(1 << 16);
  Ed e;
  typeAll(e, "hello world  again\nnext");
  CHECK(stepCount() == 4);   // "hello ", "world  ", "again\n", "next"
  CHECK(e.u() && e.t == "hello world  again\n" && e.cur == 19);
  CHECK(e.u() && e.t == "hello world  ");
  CHECK(e.u() && e.t == "hello ");
  CHECK(e.u() && e.t == "" && e.cur == 0);
  CHECK(!e.u());
  CHECK(e.r() && e.t == "hello " && e.cur == 6);
  CHECK(e.r() && e.r() && e.r() && e.t == "hello world  again\nnext");
  CHECK(!e.r());
}

// Backspace and Delete runs are one step per word; a paste or a selection replace is one step.
static void testDeletesAndPaste() {
  fresh(1 << 16);
  Ed e;
  e.type("one two three");   // a run of text (a paste): one step
  CHECK(stepCount() == 1);
  for (int i = 0; i < 9; i++) e.backspace();   // "three", then " " and "two" (a word with its spaces) -> "one "
  CHECK(e.t == "one ");
  CHECK(e.u() && e.t == "one two ");         // "two " came back
  CHECK(e.u() && e.t == "one two three" && e.cur == 13);
  e.cur = 0;
  for (int i = 0; i < 3; i++) e.del();        // "one" forwards
  CHECK(e.t == " two three");
  CHECK(e.u() && e.t == "one two three" && e.cur == 0);
  e.replace(4, 7, "2");                       // select "two", type "2"
  CHECK(e.t == "one 2 three");
  CHECK(e.u() && e.t == "one two three");
  CHECK(e.r() && e.t == "one 2 three");
  // A new change after an undo drops the redo step.
  CHECK(e.u() && e.t == "one two three");
  e.cur = 13;
  e.type("!");
  CHECK(!canRedo());
}

// Moving the cursor away ends the typed word: typing elsewhere is its own step.
static void testCursorMoveSplits() {
  fresh(1 << 16);
  Ed e;
  typeAll(e, "abc");
  e.cur = 1;
  typeAll(e, "X");
  CHECK(e.t == "aXbc");
  CHECK(e.u() && e.t == "abc");
  CHECK(e.u() && e.t == "");
}

// A full history drops its oldest steps; a step bigger than the whole history clears it.
static void testFull() {
  fresh(400);
  Ed e;
  for (int i = 0; i < 100; i++) typeAll(e, "word ");
  CHECK(usedBytes() <= 400);
  int n = 0;
  while (e.u()) n++;
  CHECK(n > 3 && n < 100);
  CHECK(e.t.size() > 0 && e.t.size() % 5 == 0);   // whole words came back, not part of one
  std::string big(600, 'x');
  e.type(big);
  CHECK(!canUndo() && usedBytes() == 0);
  fresh(0);   // no buffer: undo is off and nothing breaks
  Ed f;
  typeAll(f, "abc");
  CHECK(!f.u() && f.t == "abc");
}

static std::string piece(std::mt19937& r) {
  static const char* bits[] = {"a", "b", " ", "\n", "\t", "caf\xC3\xA9", "\xE2\x80\x94", "x", "word ", "  "};
  return bits[r() % (sizeof(bits) / sizeof(bits[0]))];
}

static long g_rounds = 0;

static void fuzz(unsigned seed, size_t cap, int edits) {
  std::mt19937 r(seed);
  fresh(cap);
  Ed e;
  e.snap();
  for (int k = 0; k < edits; k++) {
    const int len = (int)e.t.size();
    switch (r() % 7) {
      case 0: case 1: case 2: {   // typing one character
        const std::string p = piece(r);
        if (Ed::oneChar(p)) e.type(p);
        else typeAll(e, p.c_str());
        break;
      }
      case 3: e.backspace(); break;
      case 4: e.del(); break;
      case 5: {   // select a range and paste over it
        int lo = len ? (int)(r() % (unsigned)(len + 1)) : 0;
        int hi = lo + (len - lo ? (int)(r() % (unsigned)(len - lo + 1)) : 0);
        while (lo > 0 && Ed::cont((unsigned char)e.t[(size_t)lo])) lo--;
        while (hi < len && Ed::cont((unsigned char)e.t[(size_t)hi])) hi++;
        e.replace(lo, hi, r() % 3 ? piece(r) + piece(r) : std::string());
        if (e.states.size() >= 2 && e.states.back() == e.states[e.states.size() - 2]) e.states.pop_back();
        break;
      }
      default: {   // move the cursor
        int c = len ? (int)(r() % (unsigned)(len + 1)) : 0;
        while (c > 0 && c < len && Ed::cont((unsigned char)e.t[(size_t)c])) c--;
        e.cur = c;
        breakGroup();
        break;
      }
    }
    // Now and then undo a few steps and redo some, then carry on editing from there.
    if (r() % 25 == 0) {
      const int n = 1 + (int)(r() % 4);
      for (int i = 0; i < n && e.u(); i++) {}
      const int m = (int)(r() % (unsigned)(n + 1));
      for (int i = 0; i < m && e.r(); i++) {}
      e.snap();   // the text after undo/redo is an earlier state; recorded again so the edits that follow start from it
      breakGroup();
    }
  }
  // A run can end with redo steps still waiting (undone just before the end, then only cursor moves): the newest text
  // the history knows is at the end of them.
  while (e.r()) e.snap();
  const std::string final = e.t;
  // Undo all the way: each result is an earlier state, older each time (or the same one, for a step that replaced
  // text by the same text).
  int pos = (int)e.states.size() - 1;
  bool inOrder = true;
  while (e.u()) {
    int p = pos;
    while (p >= 0 && e.states[(size_t)p] != e.t) p--;
    if (p < 0) { inOrder = false; break; }
    pos = p;
  }
  CHECK(inOrder);
  if (!inOrder) printf("  fuzz seed=%u cap=%zu: an undo gave a text that never existed\n", seed, cap);
  const bool nothingDropped = cap >= (1u << 22);
  if (nothingDropped) CHECK(e.t == e.states.front());
  while (e.r()) {}
  CHECK(e.t == final);
  if (e.t != final) printf("  fuzz seed=%u cap=%zu: redo all did not give the final text\n", seed, cap);
  g_rounds++;
}

int main() {
  testWords();
  testDeletesAndPaste();
  testCursorMoveSplits();
  testFull();
  unsigned seed = 100;
  for (int i = 0; i < 40; i++) fuzz(seed++, 1u << 22, 400);
  for (size_t cap : {64, 200, 1000, 5000}) for (int i = 0; i < 40; i++) fuzz(seed++, cap, 400);
  printf("%ld random edit runs checked\n", g_rounds);
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
