#pragma once
#include "sync_types.h"

namespace msync {

// Per-note record of the last successful sync, kept at /microslate/sync/state.json on the SD card.
// Never holds tokens. If it is lost or corrupt the engine treats the next run as a first sync: nothing is deleted.
class SyncState {
 public:
  std::vector<StateEntry> entries;
  uint32_t lastSyncEpoch = 0;  // 0 = unknown

  const StateEntry* find(const std::string& name) const;
  void set(const StateEntry& e);       // insert or replace by name
  void erase(const std::string& name);
  void clear() { entries.clear(); lastSyncEpoch = 0; }

  std::string toJson() const;
  // False (and an empty state) when the text is not a valid state file.
  static bool fromJson(const std::string& text, SyncState& out);
};

}  // namespace msync
