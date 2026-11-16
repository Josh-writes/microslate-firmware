#pragma once
// How a note's text is stored on the card versus how the editor holds it (2026-11-02).
//
// The editor works with plain '\n' line breaks only. Notes written on Windows (Notepad, Scrivener's plain-text
// External Folder Sync) use "\r\n" and may start with a UTF-8 byte-order mark. Opening such a note turns every
// "\r\n" (and a stray '\r') into '\n' and drops the mark; saving puts them back the way the file had them, so a
// Windows note stays a Windows note and an LF note gets no '\r' added. Notes written on the device stay LF.
#include <stddef.h>
#include <stdint.h>

struct NoteStyle {
  bool crlf = false;   // write "\r\n" for every line break
  bool bom = false;    // write the UTF-8 byte-order mark first
};

// Turns stored bytes into editor text, chunk by chunk (a "\r\n" or a mark split across two chunks is handled).
// Feed every chunk in order into one output, then call finish(). The output is never longer than the input.
class NoteDecoder {
 public:
  // Writes the decoded form of `in` to `out` and returns how many bytes it wrote (with `out` null: only counts them).
  size_t feed(const uint8_t* in, size_t n, char* out);
  // Writes what is still held back (the start of a file shorter than a mark) and returns how many bytes.
  size_t finish(char* out);
  // The style to save with; valid after finish().
  NoteStyle style() const;

 private:
  int bomAt_ = 0;          // bytes of the mark matched so far (0-3), -1 once past the start
  bool pendingCr_ = false; // the last byte was '\r' (already written as '\n')
  size_t crlf_ = 0, lf_ = 0;
  bool bom_ = false;
};

// Turns editor text back into stored bytes, chunk by chunk, for the save and for its read-back check.
class NoteEncoder {
 public:
  NoteEncoder(const char* text, size_t len, NoteStyle style) : text_(text), len_(len), style_(style) {}
  // Fills `out` with up to `cap` (at least 3) stored bytes; returns how many, 0 when everything has been produced.
  size_t next(uint8_t* out, size_t cap);
  // The stored size of the whole text.
  static size_t storedSize(const char* text, size_t len, NoteStyle style);

 private:
  const char* text_;
  size_t len_;
  NoteStyle style_;
  size_t at_ = 0;
  bool bomDone_ = false;
};
