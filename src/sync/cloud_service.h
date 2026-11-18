#pragma once
// Cloud sync service for the device: owns the engine, brings Wi-Fi up when a background sync needs it, decides
// WHEN to sync (idle save, note closed, browsing, Sync now) and exposes a small model for the Cloud screen.
// All of it runs from the Arduino loop(); the only other task is the HTTPS worker in device_cloud.cpp.

#include <cstdint>

namespace cloud {

enum class Glyph : uint8_t { Off, Synced, Syncing, Pending, Offline, Error };

struct Settings {
  bool autoPause = true;    // push after the note has been idle (the autosave idle trigger, 10 s)
  bool autoClose = true;    // push when a note is closed or the device goes to sleep
  bool autoBrowse = true;   // check the cloud when the notes list is opened
  uint8_t writeMinutes = 5; // while writing without a pause: push unsynced changes every N minutes (0 = never)
};

void init();                      // after the SD card is mounted
void loop();                      // every loop() iteration; cheap when idle
void saveSettings();
bool signedIn();
Glyph glyph();
const char* providerName();       // the active cloud ("OneDrive")

// Triggers (all ignored when not signed in or switched off in Settings)
void onAutosave();                // any autosave of the open note (paragraph, 1-minute cap or idle)
// Editor idle tick: pushes a saved, unsynced note once the writer pauses. The pause is autoPause's 10 s, or 1 s once
// the timed backup interval has passed.
void editorTick(uint32_t idleMs, bool hasUnsaved);
uint32_t pauseMs();               // idle time after which the editor autosaves (10 s, or 1 s when a backup is overdue)
int noteMark();                   // editor cloud mark: 0 = none (synced or not signed in), 1 = waiting to be pushed, 2 = last sync failed
void onNoteClosed();              // the editor was left
// The notes list opened: a sync starts in the background; the list shows at once from the SD card (decided by Josh,
// 2026-10-28). listStatus() is the corner text: "OneDrive syncing", then "OneDrive synced" for a few seconds.
void onBrowse();
const char* listStatus();
// A note is being opened while a sync runs: true = wait. The note's own transfer goes next, the UI shows
// holdNotice() ("Syncing with OneDrive...") and, once that note is done (about 8 s at most), takeHeldNote() hands it
// back to be opened; the rest of the sync carries on. False = open it now.
bool holdNoteOpen(const char* file);
bool takeHeldNote(char* out, unsigned size);
bool browseHoldActive();          // while true the UI shows holdNotice() instead of the list
const char* holdNotice();
void browseHoldCancel();          // left the notes list: the note is not opened
void onSdLost();                  // the SD card was pulled: stop any sync and start none until it is back
void onSdBack();
// The user deleted these notes from the notes list (n names): every cloud's sync record marks them, so the next sync
// removes them there even when they are most of the notes.
void onNotesDeleted(const char* const* names, int n);
void flushBeforeSleep();          // bounded wait (about 10 s) so the last edit reaches the cloud before sleep
// Transfer: wifiInUse() = a sync is connecting, running or keeping Wi-Fi up for a moment after one. radioTakenOver()
// stops a sync in progress because the radio becomes a hotspot; a push that was waiting runs again later.
bool wifiInUse();
void radioTakenOver();

// Cloud screen (the CLOUD substate of the Sync screen). The UI only draws this model.
struct ScreenModel {
  char title[24];
  char lines[9][60];
  uint8_t lineCount;
  bool bigLine;                   // lines[bigIndex] is the sign-in code: draw it large
  uint8_t bigIndex;
  const char* bar[4];             // button labels (nullptr = none); bar[0] is Back/Cancel
  // The cloud list: when rowCount > 0 the UI draws one main-menu style card per row instead of text lines.
  uint8_t rowCount;
  int8_t rowSel;                  // highlighted row
  char rowName[4][24];            // "OneDrive"
  bool rowSigned[4];              // signed in: the UI draws a check mark
  // Code entry sign-in (Dropbox): the UI draws qrText as a QR code, lines[] beside it, and codeInput in a text box.
  bool codeEntry;
  bool codeBox;                   // false while the relay collects the code: no text box
  char qrText[256];
  char codeInput[96];
  char codeNote[64];              // "Checking...", or why the last code did not work
};
void screenOpen();                // Wi-Fi is up and the Cloud screen is shown
void screenClose();
const ScreenModel& screenModel();
// Key from the input handler (HID code, or HID_KEY_BAR0 + n for the on-screen buttons 2 and 3).
// Returns true when the screen should be closed.
bool screenKey(uint8_t key, uint8_t mods);

}  // namespace cloud
