#include "note_format.h"

static const uint8_t kBom[3] = {0xEF, 0xBB, 0xBF};

static inline void put(char* out, size_t& o, char c) {
  if (out) out[o] = c;
  o++;
}

size_t NoteDecoder::feed(const uint8_t* in, size_t n, char* out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    const uint8_t c = in[i];
    if (bomAt_ >= 0) {
      if (c == kBom[bomAt_]) {
        if (++bomAt_ == 3) { bom_ = true; bomAt_ = -1; }
        continue;
      }
      // Not a mark after all: the bytes held back are text.
      for (int k = 0; k < bomAt_; k++) put(out, o, (char)kBom[k]);
      bomAt_ = -1;
    }
    if (c == '\r') {
      put(out, o, '\n');
      pendingCr_ = true;
      continue;
    }
    if (c == '\n') {
      if (pendingCr_) crlf_++;   // the '\n' of a "\r\n": already written
      else { lf_++; put(out, o, '\n'); }
      pendingCr_ = false;
      continue;
    }
    pendingCr_ = false;
    put(out, o, (char)c);
  }
  return o;
}

size_t NoteDecoder::finish(char* out) {
  size_t o = 0;
  if (bomAt_ > 0) for (int k = 0; k < bomAt_; k++) put(out, o, (char)kBom[k]);
  bomAt_ = -1;
  return o;
}

NoteStyle NoteDecoder::style() const {
  NoteStyle s;
  s.crlf = crlf_ > lf_;   // a mixed file is saved in the style most of its lines had
  s.bom = bom_;
  return s;
}

size_t NoteEncoder::next(uint8_t* out, size_t cap) {
  size_t o = 0;
  if (style_.bom && !bomDone_) {
    for (int k = 0; k < 3; k++) out[o++] = kBom[k];
    bomDone_ = true;
  }
  while (at_ < len_) {
    const char c = text_[at_];
    if (c == '\n' && style_.crlf) {
      if (o + 2 > cap) break;
      out[o++] = '\r';
      out[o++] = '\n';
    } else {
      if (o + 1 > cap) break;
      out[o++] = (uint8_t)c;
    }
    at_++;
  }
  return o;
}

size_t NoteEncoder::storedSize(const char* text, size_t len, NoteStyle style) {
  size_t n = len + (style.bom ? 3 : 0);
  if (style.crlf)
    for (size_t i = 0; i < len; i++)
      if (text[i] == '\n') n++;
  return n;
}
