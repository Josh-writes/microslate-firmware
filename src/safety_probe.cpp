// Safety audit probe (work order 1.5), only in timing builds (-DMS_EDITOR_PROF): serial commands that let a PC
// script save a test file over and over and restart the device mid-save (probes/safety_reset.py in the workspace).
// It works on /notes/safety-test.dat only: the .dat name keeps it out of the notes list and out of sync, and it
// refuses to run while a note is open in the editor, so the writer's text is never touched.
//
//   ms:fill <gen> <bytes>   the editor buffer becomes test content <gen> of <bytes> bytes (same generator as the
//                           PC script), for safety-test.dat
//   ms:save                 saves it ("[SAFE] save start" just before the first write)
//   ms:check                the file on the card: length, CRC32, its generation, and which .tmp/.bak exist
#include "safety_probe.h"

#if defined(MS_EDITOR_PROF)
#include <Arduino.h>
#include <SDCardManager.h>

#include <cstdio>
#include <cstring>

#include "config.h"
#include "file_manager.h"
#include "sync/sync_crc32.h"
#include "text_editor.h"

extern UIState currentState;

namespace {

constexpr char kName[] = "safety-test.dat";
constexpr char kPath[] = "/notes/safety-test.dat";
char g_line[64];
int g_lineLen = 0;

// "GEN=<gen>\n", then numbered lines, cut to exactly `bytes`. probes/safety_reset.py builds the same bytes.
size_t fill(unsigned gen, size_t bytes) {
  char* buf = editorGetBuffer();
  if (bytes > TEXT_BUFFER_SIZE - 1) bytes = TEXT_BUFFER_SIZE - 1;
  size_t at = (size_t)snprintf(buf, bytes + 1, "GEN=%u\n", gen);
  if (at > bytes) at = bytes;
  char line[96];
  for (unsigned i = 0; at < bytes; i++) {
    const int n = snprintf(line, sizeof(line), "%06u line %07u the quick brown fox jumps over the lazy dog\n", gen, i);
    const size_t take = bytes - at < (size_t)n ? bytes - at : (size_t)n;
    memcpy(buf + at, line, take);
    at += take;
  }
  buf[at] = '\0';
  editorLoadBuffer(at);
  editorSetCurrentFile(kName);
  editorSetUnsavedChanges(true);
  return at;
}

void check() {
  char tmp[48], bak[48];
  snprintf(tmp, sizeof(tmp), "%s.tmp", kPath);
  snprintf(bak, sizeof(bak), "%s.bak", kPath);
  unsigned long len = 0;
  uint32_t crc = 0;
  unsigned gen = 0;
  bool have = false;
  auto f = SdMan.open(kPath, O_RDONLY);
  if (f) {
    have = true;
    len = (unsigned long)f.size();
    static uint8_t chunk[2048];
    bool first = true;
    for (;;) {
      const int n = f.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      if (first) {
        first = false;
        char head[24] = {};
        memcpy(head, chunk, n < 23 ? n : 23);
        if (sscanf(head, "GEN=%u", &gen) != 1) gen = 0;
      }
      crc = msync::crc32Update(crc, chunk, (size_t)n);
    }
    f.close();
  }
  Serial.printf("[SAFE] check file=%d len=%lu crc=%08lx gen=%u tmp=%d bak=%d\n", have ? 1 : 0, len,
                (unsigned long)crc, gen, SdMan.exists(tmp) ? 1 : 0, SdMan.exists(bak) ? 1 : 0);
}

void run(const char* cmd) {
  if (currentState == UIState::TEXT_EDITOR) {
    Serial.println("[SAFE] refused: a note is open in the editor");
    return;
  }
  unsigned gen = 0;
  unsigned long bytes = 0;
  if (sscanf(cmd, "ms:fill %u %lu", &gen, &bytes) == 2) {
    const size_t n = fill(gen, bytes);
    Serial.printf("[SAFE] filled gen=%u len=%u crc=%08lx head=%.40s\n", gen, (unsigned)n,
                  (unsigned long)msync::crc32Update(0, (const uint8_t*)editorGetBuffer(), n), editorGetBuffer());
  } else if (strcmp(cmd, "ms:save") == 0) {
    if (strcmp(editorGetCurrentFile(), kName) != 0) {
      Serial.println("[SAFE] refused: fill first");
      return;
    }
    Serial.println("[SAFE] save start");
    Serial.flush();
    const unsigned long t0 = millis();
    const bool ok = saveCurrentFile(false);
    Serial.printf("[SAFE] save done ok=%d ms=%lu\n", ok ? 1 : 0, millis() - t0);
  } else if (strcmp(cmd, "ms:check") == 0) {
    check();
  }
}

}  // namespace

void safetyProbePoll() {
  while (Serial.available() > 0) {
    const int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || c == '\r') {
      if (g_lineLen > 0) {
        g_line[g_lineLen] = '\0';
        if (strncmp(g_line, "ms:", 3) == 0) run(g_line);
      }
      g_lineLen = 0;
    } else if (g_lineLen < (int)sizeof(g_line) - 1) {
      g_line[g_lineLen++] = (char)c;
    }
  }
}

#else
void safetyProbePoll() {}
#endif
