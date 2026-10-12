#pragma once

#include "config.h"

void fileManagerSetup();
void refreshFileList();
// Reconcile leftover .tmp files from a crash mid-save: delete a .tmp whose real
// file still exists (stale partial), or promote a .tmp whose real file is missing
// (the verified new content that never got renamed in). Run once at boot.
void recoverTempFiles();
int getFileCount();
FileInfo* getFileList();

void loadFile(const char* filename);
void saveCurrentFile(bool refreshList = true);
void createNewFile();
void deriveUniqueFilename(const char* title, char* out, int maxLen);
void updateFileTitle(const char* filename, const char* newTitle);
void deleteFile(const char* filename);
