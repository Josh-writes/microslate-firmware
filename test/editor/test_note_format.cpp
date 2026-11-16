// Host tests for note_format: Windows line endings and the byte-order mark are dropped on open and put back on save.
#include <cstdio>
#include <random>
#include <string>

#include "note_format.h"

static int g_checks = 0, g_failed = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    g_checks++;                                                     \
    if (!(c)) { g_failed++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

// Decodes `stored` in chunks of `chunk` bytes (0: one chunk).
static std::string decode(const std::string& stored, size_t chunk, NoteStyle* style) {
  std::string out(stored.size() + 4, '\0');
  NoteDecoder d;
  size_t o = 0;
  const size_t step = chunk ? chunk : (stored.empty() ? 1 : stored.size());
  for (size_t at = 0; at < stored.size(); at += step) {
    const size_t n = stored.size() - at < step ? stored.size() - at : step;
    o += d.feed((const uint8_t*)stored.data() + at, n, &out[o]);
  }
  o += d.finish(&out[o]);
  out.resize(o);
  if (style) *style = d.style();
  return out;
}

static std::string encode(const std::string& text, NoteStyle s, size_t cap) {
  std::string out;
  NoteEncoder e(text.data(), text.size(), s);
  uint8_t buf[4096];
  for (;;) {
    const size_t n = e.next(buf, cap);
    if (n == 0) break;
    out.append((const char*)buf, n);
  }
  return out;
}

static const std::string kBom = "\xEF\xBB\xBF";

// A stored file that is all LF or all CR LF (with or without a mark) comes back byte for byte, in any chunking.
static void testRoundTrip(const std::string& stored, bool crlf, bool bom) {
  for (size_t chunk : {0, 1, 2, 3, 5, 1024}) {
    NoteStyle s;
    const std::string text = decode(stored, chunk, &s);
    CHECK(text.find('\r') == std::string::npos);
    CHECK(text.compare(0, 3, kBom) != 0 || !bom);
    CHECK(s.crlf == crlf);
    CHECK(s.bom == bom);
    for (size_t cap : {3, 4, 7, 1024})
      CHECK(encode(text, s, cap) == stored);
    CHECK(NoteEncoder::storedSize(text.data(), text.size(), s) == stored.size());
  }
}

int main() {
  // Device notes: plain LF, nothing changes.
  testRoundTrip("", false, false);
  testRoundTrip("one line", false, false);
  testRoundTrip("a\nb\n\nc\n", false, false);
  {
    NoteStyle s;
    CHECK(decode("a\nb\n", 0, &s) == "a\nb\n");
    CHECK(encode("a\nb\n", s, 1024) == "a\nb\n");
  }

  // Scrivener for Windows: CR LF, each paragraph followed by a blank line, curly quotes.
  const std::string scriv = "\xE2\x80\x9CMr. Harper, could you please focus?\xE2\x80\x9D\r\n\r\nI sat up in class.\r\n\r\n";
  testRoundTrip(scriv, true, false);
  CHECK(decode(scriv, 0, nullptr) == "\xE2\x80\x9CMr. Harper, could you please focus?\xE2\x80\x9D\n\nI sat up in class.\n\n");
  testRoundTrip("x\r\n", true, false);
  testRoundTrip("\r\n\r\n\r\n", true, false);

  // Notepad with a mark, LF and CR LF.
  testRoundTrip(kBom + "hello\r\nworld", true, true);
  testRoundTrip(kBom + "hello\nworld", false, true);
  testRoundTrip(kBom, false, true);

  // Text typed on the device into a CR LF note is saved with CR LF everywhere.
  {
    NoteStyle s;
    std::string text = decode("a\r\nb", 0, &s);
    text += "\nnew line";
    CHECK(encode(text, s, 1024) == "a\r\nb\r\nnew line");
  }

  // A stray CR (old Mac) becomes a line break; mixed files keep the style most lines had.
  {
    NoteStyle s;
    CHECK(decode("a\rb", 0, &s) == "a\nb");
    CHECK(!s.crlf);
    CHECK(decode("a\r\nb\r\nc\nd", 0, &s) == "a\nb\nc\nd");
    CHECK(s.crlf);
    CHECK(decode("a\r\nb\nc\nd", 0, &s) == "a\nb\nc\nd");
    CHECK(!s.crlf);
    CHECK(decode("a\r\r\nb", 0, &s) == "a\n\nb");   // CR then CR LF: two breaks
  }

  // Bytes that only look like the start of a mark are text, also at the very end of a file.
  {
    NoteStyle s;
    CHECK(decode("\xEF", 0, &s) == "\xEF" && !s.bom);
    CHECK(decode("\xEF\xBB", 1, &s) == "\xEF\xBB" && !s.bom);
    CHECK(decode("\xEF" "A", 1, &s) == "\xEF" "A" && !s.bom);
    CHECK(decode("A" + kBom, 0, &s) == "A" + kBom && !s.bom);   // a mark only counts at the start
  }

  // Random files: decoding never grows the text, the text never holds CR, and a file written by the encoder decodes
  // back to the same text and style in any chunking.
  std::mt19937 rng(11);
  const char alphabet[] = {'a', ' ', '\n', '\r', '\t', (char)0xE2, (char)0x80, (char)0x99, (char)0xEF};
  for (int it = 0; it < 20000; it++) {
    std::string stored;
    if (rng() % 4 == 0) stored = kBom;
    const int n = rng() % 40;
    for (int i = 0; i < n; i++) stored += alphabet[rng() % sizeof(alphabet)];
    NoteStyle s;
    const std::string text = decode(stored, 1 + rng() % 6, &s);
    CHECK(text.size() <= stored.size());
    CHECK(text.find('\r') == std::string::npos);
    CHECK(decode(stored, 0, nullptr) == text);
    const std::string again = encode(text, s, 3 + rng() % 8);
    NoteStyle s2;
    CHECK(decode(again, 1 + rng() % 6, &s2) == text);
    CHECK(encode(text, s2, 1024) == again);
  }

  printf("%d checks, %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
