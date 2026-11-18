#include "sync_state.h"

#include "sync_json.h"

#include <cstdio>

namespace msync {

const StateEntry* SyncState::find(const std::string& name) const {
  for (const auto& e : entries)
    if (e.name == name) return &e;
  return nullptr;
}

void SyncState::set(const StateEntry& e) {
  for (auto& x : entries) {
    if (x.name == e.name) { x = e; return; }
  }
  entries.push_back(e);
}

void SyncState::erase(const std::string& name) {
  for (size_t i = 0; i < entries.size(); i++) {
    if (entries[i].name == name) { entries.erase(entries.begin() + i); return; }
  }
}

std::string SyncState::toJson() const {
  std::string out = "{\"v\":1,\"last\":";
  out += std::to_string(lastSyncEpoch);
  out += ",\"e\":[";
  for (size_t i = 0; i < entries.size(); i++) {
    const StateEntry& e = entries[i];
    if (i) out += ",";
    out += "{\"n\":\"" + jsonEscape(e.name) + "\",\"id\":\"" + jsonEscape(e.remoteId) + "\",\"rev\":\"" +
           jsonEscape(e.remoteRev) + "\",\"crc\":" + std::to_string(e.syncedCrc) +
           ",\"sz\":" + std::to_string(e.syncedSize) + (e.deletedHere ? ",\"del\":1}" : "}");
  }
  out += "]}\n";
  return out;
}

bool SyncState::fromJson(const std::string& text, SyncState& out) {
  out.clear();
  Json j;
  if (!jsonParse(text, j) || !j.isObject()) return false;
  if (j.num("v") != 1) return false;
  const Json* arr = j.get("e");
  if (!arr || !arr->isArray()) return false;
  SyncState tmp;
  tmp.lastSyncEpoch = (uint32_t)j.num("last");
  for (const Json& item : arr->a) {
    if (!item.isObject()) return false;
    StateEntry e;
    e.name = item.str("n");
    if (e.name.empty()) return false;
    e.remoteId = item.str("id");
    e.remoteRev = item.str("rev");
    e.syncedCrc = (uint32_t)item.num("crc");
    e.syncedSize = (uint32_t)item.num("sz");
    e.deletedHere = item.num("del") != 0;
    tmp.entries.push_back(e);
  }
  out = tmp;
  return true;
}

}  // namespace msync
