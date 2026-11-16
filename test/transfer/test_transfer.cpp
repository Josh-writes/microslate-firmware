// Host tests for the Transfer rules (src/transfer/transfer_rules.*): addresses, the access code, download names,
// JSON text and the Wi-Fi QR text. Build and run:  sh test/transfer/run-on-hinton.sh
#include "transfer_rules.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace transfer;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

static uint32_t g_seed = 1;
static uint32_t lcg() { g_seed = g_seed * 1103515245u + 12345u; return g_seed >> 8; }
static uint32_t zero() { return 0; }

static void testCodes() {
  const std::string c = makeCode(kCodeLen, lcg);
  CHECK(c.size() == 4);
  CHECK(codeLooksValid(c, kCodeLen));
  CHECK(makeCode(kCodeLen, zero) == "2222");
  CHECK(makeCode(kHotspotPassLen, lcg).size() == 8);
  for (int i = 0; i < 200; i++) {
    const std::string p = makeCode(kHotspotPassLen, lcg);
    CHECK(p.find_first_of("01ilo") == std::string::npos);
  }
  CHECK(!codeLooksValid("", 4));
  CHECK(!codeLooksValid("abc", 4));
  CHECK(!codeLooksValid("abcde", 4));
  CHECK(!codeLooksValid("ab1d", 4));   // 1 is not in the alphabet
  CHECK(!codeLooksValid("ABCD", 4));
  CHECK(codeLooksValid("k7m2", 4));
}

static void testParse() {
  const std::string code = "k7m2";
  CHECK(parse("/k7m2/", code).route == Route::Page);
  CHECK(parse("/k7m2/index.html", code).route == Route::Page);
  CHECK(parse("/k7m2", code).route == Route::NoCode);
  CHECK(parse("/k7m2/list", code).route == Route::List);
  Request r = parse("/k7m2/n/0", code);
  CHECK(r.route == Route::Note && r.index == 0);
  r = parse("/k7m2/n/49", code);
  CHECK(r.route == Route::Note && r.index == 49);
  // wrong or missing code
  CHECK(parse("/", code).route == Route::Unknown);
  CHECK(parse("", code).route == Route::Unknown);
  CHECK(parse("/k7m3/", code).route == Route::Unknown);
  CHECK(parse("/k7m2x/", code).route == Route::Unknown);
  CHECK(parse("/K7M2/", code).route == Route::Unknown);
  CHECK(parse("/k7m2/", "").route == Route::Unknown);
  CHECK(parse("/hotspot-detect.html", code).route == Route::Unknown);
  CHECK(parse("/generate_204", code).route == Route::Unknown);
  // notes only by list position, never by name or path
  CHECK(parse("/k7m2/n/", code).route == Route::Unknown);
  CHECK(parse("/k7m2/n/-1", code).route == Route::Unknown);
  CHECK(parse("/k7m2/n/1a", code).route == Route::Unknown);
  CHECK(parse("/k7m2/n/../../secrets", code).route == Route::Unknown);
  CHECK(parse("/k7m2/n/notes.txt", code).route == Route::Unknown);
  CHECK(parse("/k7m2/n/99999", code).route == Route::Unknown);
  CHECK(parse("/k7m2/list/", code).route == Route::Unknown);
  CHECK(parse("/k7m2/../microslate/wifi.json", code).route == Route::Unknown);
}

static void testNames() {
  CHECK(downloadName("Chapter 7.txt") == "Chapter 7.txt");
  CHECK(downloadName("Chapter 7.md") == "Chapter 7.md");   // .md notes keep their format (1.10)
  CHECK(downloadName("NOTE.TXT") == "NOTE.txt");           // any letter case, as on the device
  CHECK(downloadName("Ideas.MD") == "Ideas.md");
  CHECK(downloadName("a/b.md") == "a_b.md");
  CHECK(downloadName("notes") == "notes.txt");
  CHECK(downloadName("") == "note.txt");
  CHECK(downloadName(".txt") == "note.txt");
  CHECK(downloadName("a\"b.txt") == "a_b.txt");
  CHECK(downloadName("a/b\\c.txt") == "a_b_c.txt");
  CHECK(downloadName("x;y.txt") == "x_y.txt");
  CHECK(downloadName("..hidden.txt") == "hidden.txt");
  CHECK(downloadName("line\nbreak.txt") == "line_break.txt");
  CHECK(downloadName("Caf\xC3\xA9.txt") == "Caf\xC3\xA9.txt");

  CHECK(contentDisposition("Chapter 7.txt") == "attachment; filename=\"Chapter 7.txt\"");
  CHECK(contentDisposition("Caf\xC3\xA9.txt") ==
        "attachment; filename=\"Caf__.txt\"; filename*=UTF-8''Caf%C3%A9.txt");
  CHECK(contentDisposition("a\"b.txt") == "attachment; filename=\"a_b.txt\"");
}

static void testJson() {
  CHECK(jsonString("plain") == "\"plain\"");
  CHECK(jsonString("a\"b") == "\"a\\\"b\"");
  CHECK(jsonString("a\\b") == "\"a\\\\b\"");
  CHECK(jsonString("l1\nl2\t") == "\"l1\\nl2\\t\"");
  CHECK(jsonString(std::string("x\x01y", 3)) == "\"x\\u0001y\"");
  CHECK(jsonString("</script>") == "\"\\u003c/script>\"");
  CHECK(jsonString("Caf\xC3\xA9") == "\"Caf\xC3\xA9\"");
}

static void testWifiQr() {
  CHECK(wifiQrText("MicroSlate-1A2B", "abcd2345") == "WIFI:T:WPA;S:MicroSlate-1A2B;P:abcd2345;;");
  CHECK(wifiQrText("My;Net", "p:a,s\"s\\") == "WIFI:T:WPA;S:My\\;Net;P:p\\:a\\,s\\\"s\\\\;;");
}

int main() {
  testCodes();
  testParse();
  testNames();
  testJson();
  testWifiQr();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
