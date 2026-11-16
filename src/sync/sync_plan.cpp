#include "sync_plan.h"

#include <algorithm>

namespace msync {

namespace {

// One side's entries sorted by name; for a name listed twice the last one wins (as the earlier map assignment did).
// Pointer lists, not maps: a map node per name is a small block, and blocks that small always come from internal RAM
// (the PSRAM threshold), which a sync of 60 notes drove down to 2.5 KB free (safety audit 1.5, 2026-11-01).
template <typename T>
std::vector<const T*> byName(const std::vector<T>& items) {
  std::vector<const T*> v;
  v.reserve(items.size());
  for (const T& it : items) v.push_back(&it);
  std::stable_sort(v.begin(), v.end(), [](const T* a, const T* b) { return a->name < b->name; });
  std::vector<const T*> out;
  out.reserve(v.size());
  for (size_t i = 0; i < v.size(); i++) {
    if (i + 1 < v.size() && v[i + 1]->name == v[i]->name) continue;
    out.push_back(v[i]);
  }
  return out;
}

}  // namespace

std::vector<Action> planSync(const std::vector<LocalFile>& local, const std::vector<RemoteFile>& remote,
                             const std::vector<StateEntry>& state, bool pushOnly) {
  const std::vector<const LocalFile*> L = byName(local);
  const std::vector<const RemoteFile*> R = byName(remote);
  const std::vector<const StateEntry*> S = byName(state);

  std::vector<Action> out;
  size_t li = 0, ri = 0, si = 0;
  while (li < L.size() || ri < R.size() || si < S.size()) {
    // The smallest name still waiting on any side; all three lists move past it together.
    const std::string* next = nullptr;
    if (li < L.size()) next = &L[li]->name;
    if (ri < R.size() && (!next || R[ri]->name < *next)) next = &R[ri]->name;
    if (si < S.size() && (!next || S[si]->name < *next)) next = &S[si]->name;
    const std::string& name = *next;
    const LocalFile* l = li < L.size() && L[li]->name == name ? L[li++] : nullptr;
    const RemoteFile* r = ri < R.size() && R[ri]->name == name ? R[ri++] : nullptr;
    const StateEntry* s = si < S.size() && S[si]->name == name ? S[si++] : nullptr;

    const bool localChanged = l && (!s || l->crc != s->syncedCrc);
    const bool remoteChanged = r && (!s || r->rev != s->remoteRev);

    ActionType t = ActionType::Skip;
    if (l && r) {
      if (!s) t = ActionType::Compare;  // first sight of this name on both sides
      else if (localChanged && remoteChanged) t = ActionType::Conflict;
      else if (localChanged) t = ActionType::Upload;
      else if (remoteChanged) t = ActionType::Download;
    } else if (l) {  // local only
      if (!s) t = ActionType::Upload;                 // new local note
      else if (localChanged) t = ActionType::Upload;  // deleted remotely but edited here: the change wins
      else t = ActionType::DeleteLocal;               // deleted remotely, untouched here
    } else if (r) {  // remote only
      if (!s) t = ActionType::Download;                  // new remote note
      else if (remoteChanged) t = ActionType::Download;  // deleted here but changed remotely: the change wins
      else t = ActionType::DeleteRemote;                 // deleted here, untouched remotely
    } else if (s) {
      t = ActionType::Forget;
    }

    if (pushOnly) {
      switch (t) {
        case ActionType::Compare:
        case ActionType::Conflict: t = ActionType::Conflict; break;  // keep the cloud copy, upload ours as new
        case ActionType::Download:
        case ActionType::DeleteLocal:
        case ActionType::DeleteRemote: t = ActionType::Skip; break;
        default: break;
      }
    }
    if (t != ActionType::Skip) out.push_back({t, name});
  }
  return out;
}

}  // namespace msync
