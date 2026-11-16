#pragma once

#include "config.h"

void fileManagerSetup();
void fileManagerPoll();     // notices a pulled card (every 3 s, outside the editor; the editor notices when a save fails)
bool sdCardMissing();       // no usable card right now
bool sdLost();              // the card is gone (or was never there): everything stops until it is back
bool sdProbe();             // key pressed while parked: true when a card answers (caller restarts the device)
bool sdResume();            // editor only: mount again, save the open note, carry on without a restart
void refreshFileList();
// Reconcile leftover .tmp files from a crash mid-save: delete a .tmp whose real
// file still exists (stale partial), or promote a .tmp whose real file is missing
// (the verified new content that never got renamed in). Run once at boot.
void recoverTempFiles();
int getFileCount();
FileInfo* getFileList();

void loadFile(const char* filename);
// Set when loadFile refused a note over the editor limit: its size in bytes, 0 when none. The notes list shows a
// notice until a key or tap clears it. The file itself is never touched.
size_t loadRefusedBytes();
void loadRefusedClear();
bool saveCurrentFile(bool refreshList = true);
void createNewFile();
// Notes are .txt or .md (one format per note; work order 1.10). ext is ".txt" or ".md".
bool isNoteFileName(const char* name);
const char* noteExtOf(const char* filename);   // ".md" for an .md note, else ".txt"
void deriveUniqueFilename(const char* title, const char* ext, char* out, int maxLen);
void updateFileTitle(const char* filename, const char* newTitle, const char* ext);
void deleteFile(const char* filename);
