// Host tests for the sync core: names, CRC, JSON, state file, planner.
// Build and run (any g++ with C++17):  g++ -std=c++17 -I src/sync test/sync/test_core.cpp src/sync/sync_*.cpp -o /tmp/t && /tmp/t
#include "sync_crc32.h"
#include "sync_json.h"
#include "sync_names.h"
#include "sync_plan.h"
#include "sync_state.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace msync;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                       \
  do {                                                                 \
    g_checks++;                                                        \
    if (!(c)) { g_fail++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } \
  } while (0)

static void testCrc() {
  const char* s = "123456789";
  CHECK(crc32Update(0, (const uint8_t*)s, 9) == 0xCBF43926u);
  uint32_t a = crc32Update(0, (const uint8_t*)"1234", 4);
  CHECK(crc32Update(a, (const uint8_t*)"56789", 5) == 0xCBF43926u);  // chainable
  CHECK(crc32Update(0, nullptr, 0) == 0);
}

static void testNames() {
  CHECK(noteNameValid("hello.txt"));
  CHECK(noteNameValid("My Note 2.TXT"));
  CHECK(noteNameValid("Привет.txt"));
  CHECK(!noteNameValid(".txt"));
  CHECK(noteNameValid("a.md"));          // .md notes (work order 1.10)
  CHECK(noteNameValid("Chapter 1.MD"));
  CHECK(!noteNameValid(".md"));
  CHECK(!noteNameValid("a.mdx"));
  CHECK(!noteNameValid("a.markdown"));
  CHECK(!noteNameValid("con.md"));
  CHECK(conflictName("a.md", "sticky") == "a_sticky.md");
  CHECK(conflictName("Chapter.TXT", "e1001") == "Chapter_e1001.TXT");
  CHECK(!noteNameValid("../x.txt"));
  CHECK(!noteNameValid("a/b.txt"));
  CHECK(!noteNameValid("a\\b.txt"));
  CHECK(!noteNameValid("a:b.txt"));
  CHECK(!noteNameValid(".hidden.txt"));
  CHECK(!noteNameValid("a..b.txt"));
  CHECK(!noteNameValid("CON.txt"));
  CHECK(!noteNameValid("com1.txt"));
  CHECK(noteNameValid("com10.txt"));
  CHECK(!noteNameValid("trail .txt"));
  CHECK(!noteNameValid(std::string(60, 'a') + ".txt"));  // 64 chars
  CHECK(noteNameValid(std::string(59, 'a') + ".txt"));   // 63 chars
  CHECK(!noteNameValid(std::string("a\x01") + "b.txt"));

  std::string c = conflictName("Chapter one.txt", "sticky");
  CHECK(c == "Chapter one_sticky.txt");
  CHECK(conflictName("chapter_1.txt", "e1001_2") == "chapter_1_e1001_2.txt");
  std::string longName = std::string(59, 'x') + ".txt";
  std::string c2 = conflictName(longName, "x4classic");
  CHECK(c2.size() <= kMaxNoteNameLen);
  CHECK(noteNameValid(c2));
  // UTF-8 boundary: 2-byte characters must not be cut in half
  std::string ru;
  for (int i = 0; i < 30; i++) ru += "\xD0\xB0";  // 60 bytes
  std::string c3 = conflictName(ru + ".txt", "x4classic");
  CHECK(c3.size() <= kMaxNoteNameLen);
  CHECK(noteNameValid(c3));
  size_t mark = c3.find("_x4classic");
  CHECK(mark != std::string::npos && mark % 2 == 0);  // whole 2-byte characters only
}

static void testJson() {
  Json j;
  CHECK(jsonParse("{\"a\":1,\"b\":\"x\",\"c\":[1,2,{\"d\":true}],\"e\":null}", j));
  CHECK(j.num("a") == 1);
  CHECK(j.str("b") == "x");
  CHECK(j.get("c") && j.get("c")->a.size() == 3);
  CHECK(j.get("c")->a[2].get("d")->b);
  CHECK(j.str("a") == "1");  // numbers format as strings
  CHECK(jsonParse("{\"s\":\"a\\u00e9\\n\\\"q\\\"\"}", j) && j.str("s") == "a\xC3\xA9\n\"q\"");
  CHECK(jsonParse("{\"s\":\"\\ud83d\\ude00\"}", j) && j.str("s") == "\xF0\x9F\x98\x80");  // surrogate pair
  CHECK(!jsonParse("{\"a\":", j));
  CHECK(!jsonParse("{\"a\":1} x", j));
  CHECK(!jsonParse("{\"a\":\"\\ud83d\"}", j));  // lone surrogate
  std::string deep(40, '[');
  CHECK(!jsonParse(deep, j));
  CHECK(jsonEscape("a\"b\\c\n\x01") == "a\\\"b\\\\c\\n\\u0001");
  Json roundtrip;
  CHECK(jsonParse("{\"n\":\"" + jsonEscape("tab\there \"q\"") + "\"}", roundtrip) && roundtrip.str("n") == "tab\there \"q\"");
}

// The parsed tree takes every piece of memory through the hook (on the device: PSRAM), and gives all of it back.
static long g_hookAllocs = 0, g_hookLive = 0;
static void* countingAlloc(size_t n) { g_hookAllocs++; g_hookLive++; return std::malloc(n); }
static void countingFree(void* p) { if (p) g_hookLive--; std::free(p); }

static void testJsonAllocator() {
  jsonSetAllocator(countingAlloc, countingFree);
  {
    // A listing shaped like a Graph reply, with strings well past the small-string buffer.
    std::string body = "{\"value\":[";
    for (int i = 0; i < 40; i++) {
      if (i) body += ",";
      body += "{\"name\":\"a note with a long enough name " + std::to_string(i) + ".txt\",\"size\":" +
              std::to_string(100 + i) + ",\"eTag\":\"\\\"{0123456789ABCDEF-0123456789ABCDEF},3\\\"\",\"file\":{\"mimeType\":\"text/plain\"}}";
    }
    body += "],\"@odata.nextLink\":\"https://graph.microsoft.com/v1.0/me/drive/next-page-token-0123456789\"}";
    Json j;
    CHECK(jsonParse(body, j));
    CHECK(j.get("value") && j.get("value")->a.size() == 40);
    CHECK(j.get("value")->a[7].str("name") == "a note with a long enough name 7.txt");
    CHECK(j.str("@odata.nextLink").find("next-page-token") != std::string::npos);
    CHECK(g_hookAllocs > 40);  // the nodes, member lists and long strings all came through the hook
    CHECK(g_hookLive > 0);
  }
  CHECK(g_hookLive == 0);  // all of it freed through the hook
  jsonSetAllocator(nullptr, nullptr);  // back to malloc/free
}

static void testState() {
  SyncState s;
  s.lastSyncEpoch = 1790000000;
  StateEntry e;
  e.name = "A \"quoted\".txt";
  e.remoteId = "8DDAA33A4FDF4C57!s1";
  e.remoteRev = "\"c:{ABC},3\"";
  e.syncedCrc = 0xDEADBEEF;
  e.syncedSize = 1234;
  s.set(e);
  StateEntry e2;
  e2.name = "b.txt";
  s.set(e2);
  SyncState r;
  CHECK(SyncState::fromJson(s.toJson(), r));
  CHECK(r.entries.size() == 2 && r.lastSyncEpoch == 1790000000);
  const StateEntry* f = r.find("A \"quoted\".txt");
  CHECK(f && f->remoteRev == "\"c:{ABC},3\"" && f->syncedCrc == 0xDEADBEEFu && f->syncedSize == 1234);
  e.syncedSize = 9;
  r.set(e);
  CHECK(r.entries.size() == 2 && r.find(e.name)->syncedSize == 9);
  r.erase("b.txt");
  CHECK(r.entries.size() == 1 && !r.find("b.txt"));
  SyncState bad;
  CHECK(!SyncState::fromJson("not json", bad) && bad.entries.empty());
  CHECK(!SyncState::fromJson("{\"v\":2,\"e\":[]}", bad));
  CHECK(!SyncState::fromJson("{\"v\":1,\"e\":[{\"n\":\"\"}]}", bad));
  CHECK(!SyncState::fromJson("", bad));
}

static LocalFile L(const char* n, uint32_t crc) { LocalFile f; f.name = n; f.crc = crc; f.size = 1; return f; }
static RemoteFile R(const char* n, const char* rev) { RemoteFile f; f.name = n; f.id = std::string("id-") + n; f.rev = rev; f.size = 1; return f; }
static StateEntry S(const char* n, uint32_t crc, const char* rev) {
  StateEntry e; e.name = n; e.syncedCrc = crc; e.remoteRev = rev; e.remoteId = std::string("id-") + n; return e;
}

static ActionType planFor(const std::vector<LocalFile>& l, const std::vector<RemoteFile>& r,
                          const std::vector<StateEntry>& s, bool push = false) {
  auto a = planSync(l, r, s, push);
  return a.empty() ? ActionType::Skip : a[0].type;
}

static void testPlan() {
  // synced and untouched
  CHECK(planFor({L("a.txt", 1)}, {R("a.txt", "r1")}, {S("a.txt", 1, "r1")}) == ActionType::Skip);
  // local changed only
  CHECK(planFor({L("a.txt", 2)}, {R("a.txt", "r1")}, {S("a.txt", 1, "r1")}) == ActionType::Upload);
  // remote changed only
  CHECK(planFor({L("a.txt", 1)}, {R("a.txt", "r2")}, {S("a.txt", 1, "r1")}) == ActionType::Download);
  // both changed
  CHECK(planFor({L("a.txt", 2)}, {R("a.txt", "r2")}, {S("a.txt", 1, "r1")}) == ActionType::Conflict);
  // new local, new remote
  CHECK(planFor({L("a.txt", 1)}, {}, {}) == ActionType::Upload);
  CHECK(planFor({}, {R("a.txt", "r1")}, {}) == ActionType::Download);
  // first sync, same name both sides: compare, never delete
  CHECK(planFor({L("a.txt", 1)}, {R("a.txt", "r1")}, {}) == ActionType::Compare);
  // deleted remotely, untouched locally -> delete local; edited locally -> change wins (upload)
  CHECK(planFor({L("a.txt", 1)}, {}, {S("a.txt", 1, "r1")}) == ActionType::DeleteLocal);
  CHECK(planFor({L("a.txt", 2)}, {}, {S("a.txt", 1, "r1")}) == ActionType::Upload);
  // deleted locally, untouched remotely -> delete remote; changed remotely -> change wins (download)
  CHECK(planFor({}, {R("a.txt", "r1")}, {S("a.txt", 1, "r1")}) == ActionType::DeleteRemote);
  CHECK(planFor({}, {R("a.txt", "r2")}, {S("a.txt", 1, "r1")}) == ActionType::Download);
  // gone both sides
  CHECK(planFor({}, {}, {S("a.txt", 1, "r1")}) == ActionType::Forget);
  // LOST STATE must never delete anything: every note on one side is copied, never removed
  {
    auto a = planSync({L("only-local.txt", 1)}, {R("only-remote.txt", "r")}, {});
    CHECK(a.size() == 2);
    for (const auto& x : a) CHECK(x.type == ActionType::Upload || x.type == ActionType::Download);
  }
  // several names, sorted and independent
  {
    auto a = planSync({L("a.txt", 1), L("b.txt", 5)}, {R("a.txt", "r1"), R("c.txt", "r9")},
                      {S("a.txt", 1, "r1"), S("b.txt", 5, "rb")});
    // a: skip, b: local only, state exists, unchanged -> delete local; c: new remote
    CHECK(a.size() == 2);
    CHECK(a[0].name == "b.txt" && a[0].type == ActionType::DeleteLocal);
    CHECK(a[1].name == "c.txt" && a[1].type == ActionType::Download);
  }
  // push-only tier: uploads only; remote changes never downloaded or deleted
  CHECK(planFor({L("a.txt", 2)}, {R("a.txt", "r1")}, {S("a.txt", 1, "r1")}, true) == ActionType::Upload);
  CHECK(planFor({L("a.txt", 1)}, {R("a.txt", "r2")}, {S("a.txt", 1, "r1")}, true) == ActionType::Skip);
  CHECK(planFor({}, {R("a.txt", "r1")}, {}, true) == ActionType::Skip);
  CHECK(planFor({L("a.txt", 1)}, {}, {S("a.txt", 1, "r1")}, true) == ActionType::Skip);
  CHECK(planFor({}, {R("a.txt", "r1")}, {S("a.txt", 1, "r1")}, true) == ActionType::Skip);
  CHECK(planFor({L("a.txt", 2)}, {R("a.txt", "r2")}, {S("a.txt", 1, "r1")}, true) == ActionType::Conflict);
  CHECK(planFor({L("a.txt", 1)}, {R("a.txt", "r1")}, {}, true) == ActionType::Conflict);
}

int main() {
  testCrc();
  testNames();
  testJson();
  testJsonAllocator();
  testState();
  testPlan();
  printf("%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
