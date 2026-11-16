#pragma once
#include "sync_types.h"

namespace msync {

// Decide what to do for every note name, from three views: the local folder, the cloud folder and the state left
// by the last successful sync. Pure function. Without state a name is never deleted (spec section 6, rule 3):
// a name seen on only one side is copied to the other, and a name on both sides is compared first.
//
// `pushOnly` (X4 tier): only uploads are produced. A cloud copy that also changed becomes a conflict, which the
// engine resolves by uploading the local note under a new name; downloads and deletions are suppressed.
std::vector<Action> planSync(const std::vector<LocalFile>& local, const std::vector<RemoteFile>& remote,
                             const std::vector<StateEntry>& state, bool pushOnly = false);

}  // namespace msync
