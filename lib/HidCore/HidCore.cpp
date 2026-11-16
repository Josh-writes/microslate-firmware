#include "HidCore.h"

#include <cstring>

namespace hidcore {

namespace {

constexpr uint16_t kPageKeyboard = 0x07;
constexpr uint16_t kPageConsumer = 0x0C;
constexpr uint16_t kPageVendorFirst = 0xFF00;
constexpr int kMaxUsages = 16;
constexpr int kStackDepth = 4;
constexpr int kMaxReportBits = 64 * 8;  // larger than any keyboard report seen

struct Globals {
  uint16_t usagePage;
  int32_t logicalMin;
  uint32_t reportSize;
  uint32_t reportCount;
  uint8_t reportId;
};

struct Locals {
  uint32_t usages[kMaxUsages];  // extended usages: page << 16 | id
  int usageCount;
  uint32_t usageMin, usageMax;
  bool hasMin, hasMax;
};

uint32_t extend(uint32_t usage, int size, uint16_t page) {
  return size == 4 ? usage : (uint32_t(page) << 16) | (usage & 0xFFFF);
}

ReportLayout* reportFor(ReportMap& map, uint8_t id) {
  int i = map.find(id);
  if (i >= 0) return &map.reports[i];
  if (map.count >= kMaxReports) return nullptr;
  ReportLayout& r = map.reports[map.count++];
  memset(&r, 0, sizeof(r));
  r.id = id;
  return &r;
}

// One Input main item: records keyboard fields and the report's kind, then advances.
bool addInput(ReportMap& map, const Globals& g, const Locals& l, uint32_t flags) {
  ReportLayout* r = reportFor(map, g.reportId);
  if (!r) return true;  // too many reports: ignore the rest, not an error
  const uint32_t bits = g.reportSize * g.reportCount;
  if (g.reportSize == 0 || g.reportSize > 32 || r->bits + bits > kMaxReportBits) return false;
  const bool constant = flags & 0x01;
  const bool variable = flags & 0x02;
  if (!constant) {
    uint32_t first = l.usageCount > 0 ? l.usages[0] : l.hasMin ? l.usageMin : (uint32_t(g.usagePage) << 16);
    uint16_t page = uint16_t(first >> 16);
    if (page == kPageKeyboard && r->fieldCount < kMaxFields) {
      KeyField f{};
      f.bitOffset = r->bits;
      f.bitSize = uint8_t(g.reportSize);
      f.count = uint16_t(g.reportCount);
      f.logicalMin = g.logicalMin;
      bool usable = true;
      if (variable) {
        f.type = FieldType::Bitmap;
        if (g.reportSize != 1) usable = false;
        if (l.hasMin) {
          f.usageMin = uint8_t(l.usageMin & 0xFF);
        } else if (l.usageCount > 0) {
          // An explicit usage list must be a plain run (E0, E1 ... E7).
          for (int i = 1; i < l.usageCount; i++) {
            if (l.usages[i] != l.usages[0] + uint32_t(i)) usable = false;
          }
          f.usageMin = uint8_t(l.usages[0] & 0xFF);
        } else {
          usable = false;
        }
        if (uint32_t(f.usageMin) + f.count > 0x100) f.count = uint16_t(0x100 - f.usageMin);
      } else {
        f.type = FieldType::Array;
        if (g.reportSize > 16) usable = false;
        f.usageMin = uint8_t(l.hasMin ? (l.usageMin & 0xFF) : 0);
      }
      if (usable) {
        r->fields[r->fieldCount++] = f;
        r->kind = ReportKind::Keyboard;
      }
    } else if (r->kind == ReportKind::Other || r->kind == ReportKind::Vendor) {
      if (page == kPageConsumer) r->kind = ReportKind::Consumer;
      else if (page >= kPageVendorFirst && r->kind == ReportKind::Other) r->kind = ReportKind::Vendor;
    }
  }
  r->bits = uint16_t(r->bits + bits);
  return true;
}

uint32_t readBits(const uint8_t* data, size_t len, uint32_t offset, uint32_t size) {
  uint32_t v = 0;
  for (uint32_t i = 0; i < size; i++) {
    uint32_t bit = offset + i;
    if (bit / 8 >= len) break;
    if (data[bit / 8] & (1u << (bit % 8))) v |= 1u << i;
  }
  return v;
}

void addKey(uint8_t usage, uint8_t& mods, uint8_t* keys, int& n) {
  if (usage >= 0xE0 && usage <= 0xE7) { mods |= uint8_t(1u << (usage - 0xE0)); return; }
  if (usage < 0x04 || usage > 0xDD) return;  // none, error codes, reserved
  for (int i = 0; i < n; i++) if (keys[i] == usage) return;
  if (n < kMaxKeys) keys[n++] = usage;
}

// Compares the new key set with the state and writes presses then releases.
int diff(uint8_t mods, const uint8_t* keys, int n, KeyState& state, KeyEvent* out, int maxEvents) {
  int e = 0;
  for (int i = 0; i < n && e < maxEvents; i++) {
    bool held = false;
    for (int j = 0; j < state.keyCount; j++) if (state.keys[j] == keys[i]) { held = true; break; }
    if (!held) out[e++] = {keys[i], mods, true};
  }
  for (int j = 0; j < state.keyCount && e < maxEvents; j++) {
    bool still = false;
    for (int i = 0; i < n; i++) if (keys[i] == state.keys[j]) { still = true; break; }
    if (!still) out[e++] = {state.keys[j], mods, false};
  }
  state.mods = mods;
  state.keyCount = uint8_t(n);
  memcpy(state.keys, keys, size_t(n));
  return e;
}

}  // namespace

int ReportMap::find(uint8_t id) const {
  for (int i = 0; i < count; i++) if (reports[i].id == id) return i;
  return -1;
}

int ReportMap::keyboardReports() const {
  int n = 0;
  for (int i = 0; i < count; i++) if (reports[i].kind == ReportKind::Keyboard) n++;
  return n;
}

const char* parseResultName(ParseResult r) {
  switch (r) {
    case ParseResult::Ok: return "ok";
    case ParseResult::Empty: return "empty";
    case ParseResult::Malformed: return "malformed";
    case ParseResult::NoKeyboard: return "no keyboard report";
  }
  return "?";
}

ParseResult parseReportMap(const uint8_t* data, size_t len, ReportMap& out) {
  memset(&out, 0, sizeof(out));
  if (!data || len == 0) return ParseResult::Empty;
  Globals g{};
  Globals stack[kStackDepth];
  int depth = 0;
  Locals l{};
  int collections = 0;
  size_t i = 0;
  while (i < len) {
    const uint8_t prefix = data[i];
    if (prefix == 0xFE) {  // long item: skipped
      if (i + 2 >= len) { out.count = 0; return ParseResult::Malformed; }
      i += 3 + size_t(data[i + 1]);
      continue;
    }
    int size = prefix & 0x03;
    if (size == 3) size = 4;
    if (i + 1 + size_t(size) > len) { out.count = 0; return ParseResult::Malformed; }
    uint32_t u = 0;
    for (int b = 0; b < size; b++) u |= uint32_t(data[i + 1 + b]) << (8 * b);
    int32_t s = int32_t(u);
    if (size == 1) s = int8_t(u);
    else if (size == 2) s = int16_t(u);
    const uint8_t type = (prefix >> 2) & 0x03;
    const uint8_t tag = prefix >> 4;
    i += 1 + size_t(size);

    if (type == 0) {  // main
      if (tag == 0x8) {  // Input
        if (!addInput(out, g, l, u)) { out.count = 0; return ParseResult::Malformed; }
      } else if (tag == 0xA) {
        collections++;
      } else if (tag == 0xC) {
        if (--collections < 0) { out.count = 0; return ParseResult::Malformed; }
      }
      l = Locals{};
    } else if (type == 1) {  // global
      switch (tag) {
        case 0x0: g.usagePage = uint16_t(u); break;
        case 0x1: g.logicalMin = s; break;
        case 0x7: g.reportSize = u; break;
        case 0x8:
          if (u == 0 || u > 0xFF) { out.count = 0; return ParseResult::Malformed; }
          g.reportId = uint8_t(u);
          break;
        case 0x9: g.reportCount = u; break;
        case 0xA:
          if (depth >= kStackDepth) { out.count = 0; return ParseResult::Malformed; }
          stack[depth++] = g;
          break;
        case 0xB:
          if (depth == 0) { out.count = 0; return ParseResult::Malformed; }
          g = stack[--depth];
          break;
        default: break;
      }
    } else if (type == 2) {  // local
      switch (tag) {
        case 0x0:
          if (l.usageCount < kMaxUsages) l.usages[l.usageCount++] = extend(u, size, g.usagePage);
          break;
        case 0x1: l.usageMin = extend(u, size, g.usagePage); l.hasMin = true; break;
        case 0x2: l.usageMax = extend(u, size, g.usagePage); l.hasMax = true; break;
        default: break;
      }
    }
    // type 3 (reserved) is skipped
  }
  // Reports that only ever had padding carry nothing; drop them so find() misses them.
  int kept = 0;
  for (int r = 0; r < out.count; r++) {
    if (out.reports[r].bits == 0) continue;
    if (kept != r) out.reports[kept] = out.reports[r];
    kept++;
  }
  out.count = uint8_t(kept);
  return out.keyboardReports() > 0 ? ParseResult::Ok : ParseResult::NoKeyboard;
}

int decodeReport(const ReportLayout& layout, const uint8_t* data, size_t len, KeyState& state,
                 KeyEvent* out, int maxEvents) {
  if (layout.kind != ReportKind::Keyboard || len * 8 < layout.bits) return 0;
  uint8_t mods = 0;
  uint8_t keys[kMaxKeys];
  int n = 0;
  for (int f = 0; f < layout.fieldCount; f++) {
    const KeyField& k = layout.fields[f];
    if (k.type == FieldType::Bitmap) {
      for (uint16_t b = 0; b < k.count; b++) {
        uint32_t bit = k.bitOffset + b;
        if (data[bit / 8] & (1u << (bit % 8))) addKey(uint8_t(k.usageMin + b), mods, keys, n);
      }
    } else {
      for (uint16_t e = 0; e < k.count; e++) {
        int32_t v = int32_t(readBits(data, len, k.bitOffset + uint32_t(e) * k.bitSize, k.bitSize));
        int32_t usage = int32_t(k.usageMin) + (v - k.logicalMin);
        if (usage == 0x01) return 0;  // ErrorRollOver: too many keys down, keep the last state
        if (usage > 0 && usage <= 0xFF) addKey(uint8_t(usage), mods, keys, n);
      }
    }
  }
  return diff(mods, keys, n, state, out, maxEvents);
}

int decodeLegacy(const uint8_t* data, size_t len, KeyState& state, KeyEvent* out, int maxEvents) {
  if (len != 7 && len != 8) return 0;
  const uint8_t* k = data + (len == 8 ? 2 : 1);
  uint8_t mods = data[0];
  uint8_t keys[kMaxKeys];
  int n = 0;
  for (int i = 0; i < 6; i++) {
    if (k[i] == 0x01) return 0;
    addKey(k[i], mods, keys, n);
  }
  return diff(mods, keys, n, state, out, maxEvents);
}

// --- KeyRepeat ---

bool KeyRepeat::repeatable(uint8_t u, uint8_t mods) {
  if (mods & 0x88) return false;                                  // Win/Cmd: a shortcut
  const bool edit = u == 0x2A || u == 0x4C || (u >= 0x4F && u <= 0x52);   // Backspace, Delete, arrows
  if (mods & 0x11) return edit;                                   // Ctrl: word jumps and deletes only
  if (edit) return true;
  if (u == 0x4A || u == 0x4B || u == 0x4D || u == 0x4E) return true;      // Home, Page Up, End, Page Down
  if (u >= 0x04 && u <= 0x27) return true;                        // letters, digits
  if (u >= 0x2C && u <= 0x38) return true;                        // Space, punctuation
  if (u >= 0x54 && u <= 0x63 && u != 0x58) return true;           // keypad, not keypad Enter
  return u == 0x64 || (u >= 0x87 && u <= 0x89);                   // ISO and Japanese extra keys
}

void KeyRepeat::onEvent(uint8_t usage, uint8_t mods, bool pressed, uint32_t now) {
  if (pressed) {
    // A new key takes over; one that does not repeat stops the old one, as on a PC.
    key_ = repeatable(usage, mods) ? usage : 0;
    next_ = now + kDelayMs;
    fastAt_ = next_ + kFastAfterMs;
    unshown_ = 0;
  } else if (usage == key_) {
    key_ = 0;
  }
}

uint8_t KeyRepeat::tick(uint32_t now, uint8_t mods, bool screenShown) {
  if (!key_) return 0;
  if (!repeatable(key_, mods)) { key_ = 0; return 0; }   // Ctrl or Win pressed while holding
  if (screenShown) unshown_ = 0;
  if (int32_t(now - next_) < 0) return 0;
  const int allowed = int32_t(now - fastAt_) >= 0 ? kBurst : 1;
  if (unshown_ >= allowed) return 0;   // wait for the screen
  unshown_++;
  next_ = now + kRateMs;   // from now, so a stalled loop sends one, not a burst
  return key_;
}

}  // namespace hidcore
