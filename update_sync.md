# Plan: Sync UX Overhaul

## Problem

1. The device shows total `.txt` files on device (23) instead of files actually being synced (5). See "Show Accurate File Count During Sync" section.
2. The SYNCING screen shows a raw activity log ("Sent: note.txt" lines) with no clear step-by-step progress UI. See "Stepped Checkbox UI" section below.

---

## Part 1: Show Accurate File Count During Sync

### Problem

The device shows the total number of files on the device, not the number that will actually be synced. The device has zero visibility into the PC's sync plan — it just serves whatever the PC asks for. The PC already knows exactly which files need syncing.

Example: Device has 23 files, but only 5 are new/changed. The UI shows "Sent: 5 / 23" when it should show "Sent: 5 / 5".

### Approach

The PC tells the device the total upfront via a new HTTP endpoint. The device stores and displays the number.

### Changes

#### `sync/microslate_sync.py`

**Add `signal_sync_total()` function** (after `signal_sync_complete()`, ~line 113):
```python
def signal_sync_total(count):
    """Tell the device how many files will be synced."""
    try:
        r = requests.put(f"{DEVICE_URL}/api/sync-info",
                       json={"total": count}, timeout=3)
        r.raise_for_status()
        log.info("Notified device: %d file(s) to sync", count)
    except Exception as e:
        log.warning("Could not signal sync total: %s", e)
```

**Modify `main()`** (line 134) — call `signal_sync_total()` before downloading starts:
```python
# After device_files is fetched and downloaded is computed:
downloaded = sync_once(device_files)
signal_sync_total(len(downloaded))  # ← ADD THIS LINE
```

#### `src/wifi_sync.cpp`

**A. `resetSyncTracking()` — REMOVE `getFileCount()` call** (~line 83):
```cpp
// Remove: totalFilesToSync = getFileCount();
```
`totalFilesToSync` stays at `0` from static declaration. Set only when PC calls the new endpoint.

**B. Add `handleSyncInfo()` handler** (after `handleSyncComplete()`, ~line 475):
```cpp
static void handleSyncInfo() {
  lastHttpActivityMs = millis();
  totalFilesToSync = 0;

  // Parse body: expected format is {"total": N}
  String body = server->arg("plain");
  int idx = body.indexOf("\"total\":");
  if (idx >= 0) {
    int val = body.substring(idx + 7).toInt();
    if (val > 0) totalFilesToSync = val;
  }

  server->send(200, "text/plain", "OK");
  DBG_PRINTF("[SYNC] PC says %d file(s) to sync\n", totalFilesToSync);
}
```

**C. Register endpoint in `startHttpServer()`** (~line 497):
```cpp
server->on("/api/sync-info", HTTP_PUT, handleSyncInfo);
```

#### `src/wifi_sync.h`
No changes needed.

#### Summary of file count changes

| File | Lines | Change |
|------|-------|--------|
| `sync/microslate_sync.py` | ~113 (new function), 134 (call) | Add `signal_sync_total()`, call it before `sync_once()` in `main()` |
| `src/wifi_sync.cpp` | ~83 | Remove `getFileCount()` from `resetSyncTracking()` |
| `src/wifi_sync.cpp` | ~475 (new function) | Add `handleSyncInfo()` handler |
| `src/wifi_sync.cpp` | ~497 (register) | Add `server->on("/api/sync-info"...` in `startHttpServer()` |

---

## Part 2: Stepped Checkbox UI

### Current state

The SYNCING screen shows:
- IP address at top
- "Waiting for PC..." / "Run microslate_sync.py on PC" text (initial)
- Activity log: "Sent: filename.txt" lines (during transfer)
- Footer: "Sent: X / Y  Recv: Z  Esc: Cancel"

### New stepped checkbox design

The screen shows 6 checkboxes in a vertical list. Items are labeled `[ ]` (unchecked) or `[x]` (checked). A filled box `☑` means step is complete. Unchecked `☐` means pending.

**Checklist items (top to bottom):**
1. `[ ]` Wi-Fi Connected
2. `[ ]` PC Connected
3. `[ ]` Python Script Active
4. `[ ]` X Files to Sync *(only visible when step 3 is complete)*
5. `[x]` Syncing... *(only visible during file transfer — replaces step 4 when transfer starts)*
6. `[x]` Sync Complete *(only visible after sync done — replaces step 5)*

**Step transitions (driven by events in wifi_sync.cpp):**

| Event | Checkbox 3 | Checkbox 4 | Checkbox 5 | Checkbox 6 |
|-------|----------- |----------- |----------- |-----------|
| PC calls `PUT /api/sync-info` | checked | shows "X Files to Sync" | hidden | hidden |
| First file download begins | — | hidden | checked, shows "1/X" | hidden |
| All files done, `/api/sync-complete` called | — | hidden | hidden | checked |

**Footer:** Shows "Esc: Cancel" during steps 1-5. Hidden (or shows "Returning to menu...") on step 6.

### Changes

#### `src/wifi_sync.cpp`

**A. Add sync phase tracking variable** (near `filesSent`, line ~56):
```cpp
static int syncPhase = 0;  // 0=initial, 1=PC connected, 2=file count known, 3=syncing, 4=complete
```

**B. Modify `resetSyncTracking()`** to reset phase:
```cpp
syncPhase = 0;
```

**C. In `handleSyncInfo()`** — when PC sets total, advance phase:
```cpp
syncPhase = 1;  // PC connected and told us total
```

**D. In `handleFileDownload()`** — when first file transfer begins, advance phase:
```cpp
if (syncPhase == 1) syncPhase = 2;  // Syncing begins
```

**E. In `handleSyncComplete()`** — when PC signals done, advance phase:
```cpp
syncPhase = 4;  // Complete
```

**F. Expose getters in wifi_sync.h** (add declarations):
```cpp
int getSyncPhase();
int getSyncTotalFiles();
```

**G. Expose getters in wifi_sync.cpp** (add implementations):
```cpp
int getSyncPhase() { return syncPhase; }
int getSyncTotalFiles() { return totalFilesToSync; }
```

**H. Expose log lines** — keep existing log for detailed activity, but suppress display during checkbox UI (only show step 4 count).

#### `src/wifi_sync.h`

**Add declarations** (after `getSyncFilesReceived()`):
```cpp
int getSyncPhase();
int getSyncTotalFiles();
```

#### `src/ui_renderer.cpp`

**Replace the entire `SyncState::SYNCING` body** (lines 851-877).

Helper function — draw one checkbox row:
```cpp
static void drawSyncCheckbox(Renderer& r, int x, int y, bool checked, const char* label) {
  // Checkbox: 12x12 empty rect, then checkmark + label inside
  r.drawRect(x, y, 12, 12, tc);          // Box outline
  if (checked) {
    // Draw checkmark: small X inside the box (two diagonal lines)
    r.drawLine(x + 2, y + 2, x + 9, y + 9, tc);
    r.drawLine(x + 9, y + 2, x + 2, y + 9, tc);
  }
  drawClippedText(r, FONT_SMALL, x + 18, y + 1, label, sw - x - 18, tc);
}
```

**New `SyncState::SYNCING` body**:
```cpp
case SyncState::SYNCING: {
  const char* ip = getSyncStatusText();
  drawClippedText(renderer, FONT_SMALL, 20, 42, ip, sw - 40, tc, EpdFontFamily::BOLD);

  int phase = getSyncPhase();
  int totalFiles = getSyncTotalFiles();
  int filesSent = getSyncFilesSent();
  int yPos = 75;

  // 1. Wi-Fi Connected (always checked once we're in SYNCING state)
  drawSyncCheckbox(renderer, 20, yPos, true, "Wi-Fi Connected");
  yPos += 24;

  // 2. PC Connected (checked when syncPhase >= 1)
  drawSyncCheckbox(renderer, 20, yPos, phase >= 1, "PC Connected");
  yPos += 24;

  // 3. Python Script Active (checked when syncPhase >= 1)
  drawSyncCheckbox(renderer, 20, yPos, phase >= 1, "Python Script Active");
  yPos += 24;

  // 4. X Files to Sync — only when total known and not yet transferring
  if (phase == 1 && totalFiles > 0) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%d File%s to Sync", totalFiles, totalFiles == 1 ? "" : "s");
    drawSyncCheckbox(renderer, 20, yPos, false, buf);
    yPos += 24;
  }

  // 5. Syncing... (shown while files are being transferred)
  if (phase == 2) {
    char buf[32];
    snprintf(buf, sizeof(buf), "Syncing %d/%d", filesSent, totalFiles);
    drawSyncCheckbox(renderer, 20, yPos, false, buf);
    yPos += 24;
  }

  // 6. Sync Complete (shown after sync-complete signal)
  if (phase >= 4) {
    drawSyncCheckbox(renderer, 20, yPos, true, "Sync Complete");
    yPos += 24;
  }

  // Footer
  constexpr int bm = 28;
  clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
  if (phase < 4) {
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4, "Esc: Cancel", sw - 20, tc);
  } else {
    drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4, "Returning to menu...", sw - 20, tc);
  }
  break;
}
```

**Also update the DONE state** (lines 879-884) to show checkboxes instead of text:
```cpp
case SyncState::DONE: {
  const char* ip = getSyncStatusText();
  drawClippedText(renderer, FONT_SMALL, 20, 42, ip, sw - 40, tc, EpdFontFamily::BOLD);

  int yPos = 75;
  drawSyncCheckbox(renderer, 20, yPos, true, "Wi-Fi Connected");
  yPos += 24;
  drawSyncCheckbox(renderer, 20, yPos, true, "PC Connected");
  yPos += 24;
  drawSyncCheckbox(renderer, 20, yPos, true, "Python Script Active");
  yPos += 24;
  drawSyncCheckbox(renderer, 20, yPos, true, "Sync Complete");
  yPos += 24;

  const char* summary = getSyncStatusText();
  if (summary && summary[0]) {
    drawClippedText(renderer, FONT_SMALL, 20, yPos + 8, summary, sw - 40, tc);
  }

  constexpr int bm = 28;
  clippedLine(renderer, 10, sh - bm - 2, sw - 10, sh - bm - 2, tc);
  drawClippedText(renderer, FONT_SMALL, 10, sh - bm + 4, "Returning to menu...", sw - 20, tc);
  break;
}
```

---

## Summary of all changes

| File | Lines | Change |
|------|-------|--------|
| `sync/microslate_sync.py` | ~113 (new), 134 (call) | Add `signal_sync_total()`, call it before `sync_once()` |
| `src/wifi_sync.cpp` | ~56 (var) | Add `syncPhase` tracking |
| `src/wifi_sync.cpp` | ~83 (reset) | Reset `syncPhase`; remove `getFileCount()` |
| `src/wifi_sync.cpp` | ~475 (new) | Add `handleSyncInfo()` handler |
| `src/wifi_sync.cpp` | ~497 (register) | Register `PUT /api/sync-info` |
| `src/wifi_sync.cpp` | ~479 (advance) | `syncPhase = 1` in `handleSyncInfo()` |
| `src/wifi_sync.cpp` | ~469 (advance) | `syncPhase = 2` on first `handleFileDownload()` |
| `src/wifi_sync.cpp` | ~481 (advance) | `syncPhase = 4` in `handleSyncComplete()` |
| `src/wifi_sync.cpp` | ~88 (getter) | Add `getSyncPhase()` + fix `getSyncTotalFiles()` |
| `src/wifi_sync.h` | ~39 (decl) | Add `getSyncPhase()` + `getSyncTotalFiles()` |
| `src/ui_renderer.cpp` | 851-877 (replace) | New checkbox-based SYNCING body |
| `src/ui_renderer.cpp` | 879-884 (replace) | New checkbox-based DONE body |