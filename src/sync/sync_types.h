#pragma once
// Cloud sync core types. Pure C++ (no Arduino/ESP headers) so the logic builds and is tested on a PC.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace msync {

struct LocalFile {
  std::string name;
  uint32_t size = 0;
  uint32_t crc = 0;  // CRC32 of the content
};

struct RemoteFile {
  std::string name;
  std::string id;
  std::string rev;   // provider content revision (OneDrive cTag)
  uint32_t size = 0;
};

// What the last successful sync left behind for one note.
struct StateEntry {
  std::string name;
  std::string remoteId;
  std::string remoteRev;
  uint32_t syncedCrc = 0;
  uint32_t syncedSize = 0;
  // Deleted on this device by the user (the notes list), not yet removed from the cloud: the mass-deletion guard
  // lets these through. Any new record for the name (upload, download) starts without it.
  bool deletedHere = false;
};

enum class ActionType : uint8_t {
  Skip,          // nothing to do
  Upload,        // local -> cloud (new or changed)
  Download,      // cloud -> local (new or changed)
  Conflict,      // both changed: keep both
  Compare,       // first sync, same name on both sides: download and compare before deciding
  DeleteLocal,   // deleted in the cloud, unchanged here: move to trash
  DeleteRemote,  // deleted here, unchanged in the cloud: delete in the cloud
  Forget,        // gone on both sides: drop the state entry
};

struct Action {
  ActionType type = ActionType::Skip;
  std::string name;
};

}  // namespace msync
