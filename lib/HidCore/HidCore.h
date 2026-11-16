#pragma once
// HID core: reads a keyboard's HID Report Map and turns its input reports into key press
// and release events. Pure C++ (no Arduino, no NimBLE) so it is shared by every device
// build and tested on a PC (test/hid). Only keyboard keys (usage page 0x07) produce
// events; consumer, system, vendor (Logitech HID++) and other reports are recognised so
// the caller can leave them alone.
//
// Report data passed in never includes the report ID byte: over Bluetooth each report has
// its own characteristic and its ID comes from the Report Reference descriptor.

#include <cstddef>
#include <cstdint>

namespace hidcore {

constexpr int kMaxReports = 16;  // input reports kept per Report Map
constexpr int kMaxFields = 4;    // key fields per report (modifiers, key array, bitmap ...)
constexpr int kMaxKeys = 16;     // keys tracked as held per report

enum class ReportKind : uint8_t { Other = 0, Keyboard, Consumer, Vendor };
enum class FieldType : uint8_t { Bitmap, Array };

// One run of keyboard usages inside a report.
//   Bitmap: count one-bit fields; bit i set = usage (usageMin + i) is down (modifiers, NKRO).
//   Array:  count entries of bitSize bits; each holds a pressed usage
//           (usageMin + value - logicalMin), 0 = empty slot (6-key rollover).
struct KeyField {
  FieldType type;
  uint8_t bitSize;
  uint16_t bitOffset;  // from the first data byte of the report
  uint16_t count;
  uint8_t usageMin;
  int32_t logicalMin;
};

struct ReportLayout {
  uint8_t id;          // 0 when the map uses no report IDs
  ReportKind kind;
  uint16_t bits;       // input data length in bits
  uint8_t fieldCount;
  KeyField fields[kMaxFields];
};

struct ReportMap {
  uint8_t count;
  ReportLayout reports[kMaxReports];
  // Index of the input report with this ID, or -1.
  int find(uint8_t id) const;
  int keyboardReports() const;
};

enum class ParseResult : uint8_t { Ok, Empty, Malformed, NoKeyboard };
const char* parseResultName(ParseResult r);

// Parses a Report Map. Ok means at least one keyboard input report was found; otherwise
// the caller falls back to the fixed boot-style format (decodeLegacy).
ParseResult parseReportMap(const uint8_t* data, size_t len, ReportMap& out);

// What a report said last time, per report: modifier bits (E0-E7) and the keys held.
struct KeyState {
  uint8_t mods;
  uint8_t keyCount;
  uint8_t keys[kMaxKeys];
};

struct KeyEvent {
  uint8_t usage;   // HID keyboard usage (0x04-0xDD), never a modifier
  uint8_t mods;    // modifier bits after this report
  bool pressed;
};

// Decodes one report against its previous state and writes press events (in report
// order), then release events. Returns the number written. A report that signals
// rollover (too many keys) or is too short for its layout is ignored.
int decodeReport(const ReportLayout& layout, const uint8_t* data, size_t len, KeyState& state,
                 KeyEvent* out, int maxEvents);

// Fallback without a usable map: 8 bytes (modifiers, reserved, 6 keys) or 7 bytes
// (modifiers, 6 keys). Other lengths are ignored.
int decodeLegacy(const uint8_t* data, size_t len, KeyState& state, KeyEvent* out, int maxEvents);

// Key repeat for a held key. Keyboards send a report on press and on release; repeating is
// the host's job. The last key pressed repeats (as on a PC) after kDelayMs. The pace follows
// the screen: for the first kFastAfterMs of repeating, the next step waits until the screen
// shows the last one (every step visible, so the writer can count them); after that up to
// kBurst steps may go out per refresh. kRateMs is the shortest gap either way. tick() is
// called from the main loop; after a stall it gives one repeat, never a backlog.
class KeyRepeat {
 public:
  static constexpr uint32_t kDelayMs = 500;
  static constexpr uint32_t kRateMs = 70;
  static constexpr uint32_t kFastAfterMs = 1500;   // of repeating, then faster
  static constexpr int kBurst = 3;                 // steps per refresh once faster

  // Keys that repeat with these modifiers: text keys, Space, Backspace, Delete, arrows,
  // Home/End, Page Up/Down. Never Enter, Esc, Tab, F-keys, media keys or Win/Cmd combos;
  // with Ctrl held only the arrows, Backspace and Delete (word jumps and deletes).
  static bool repeatable(uint8_t usage, uint8_t mods);

  void onEvent(uint8_t usage, uint8_t mods, bool pressed, uint32_t now);
  // The key to send again now, or 0. screenShown: the screen shows everything typed so far
  // (no redraw waiting, no refresh running).
  uint8_t tick(uint32_t now, uint8_t mods, bool screenShown);
  void cancel() { key_ = 0; }
  uint8_t key() const { return key_; }

 private:
  uint8_t key_ = 0;
  uint32_t next_ = 0;
  uint32_t fastAt_ = 0;   // repeating faster from this time
  int unshown_ = 0;       // steps sent since the screen last caught up
};

}  // namespace hidcore
