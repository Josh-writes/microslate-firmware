#include "file_manager.h"
#include "text_editor.h"
#include <Arduino.h>
#include <SDCardManager.h>
#include <cstring>

// --- File list ---
static FileInfo fileList[MAX_FILES];
static int fileCount = 0;

// Shared state
extern UIState currentState;

// Read the first non-empty line as a title
static void readTitleFromFile(const char* path, char* titleOut, int maxLen) {
  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    strncpy(titleOut, "Untitled", maxLen - 1);
    titleOut[maxLen - 1] = '\0';
    return;
  }

  // Read a small chunk to find the first non-empty line
  char chunk[256];
  int bytesRead = file.read(chunk, sizeof(chunk) - 1);
  if (bytesRead > 0) {
    chunk[bytesRead] = '\0';

    // Scan for first non-empty line
    int start = 0;
    while (start < bytesRead && (chunk[start] == '\n' || chunk[start] == '\r'))
      start++;

    if (start < bytesRead) {
      // Find end of line
      int end = start;
      while (end < bytesRead && chunk[end] != '\n' && chunk[end] != '\r')
        end++;

      int len = end - start;
      // Trim trailing spaces
      while (len > 0 && chunk[start + len - 1] == ' ')
        len--;

      if (len > 0) {
        if (len > maxLen - 4) {
          strncpy(titleOut, chunk + start, maxLen - 4);
          titleOut[maxLen - 4] = '\0';
          strcat(titleOut, "...");
        } else {
          strncpy(titleOut, chunk + start, len);
          titleOut[len] = '\0';
        }
        file.close();
        return;
      }
    }
  }

  file.close();
  strncpy(titleOut, "Untitled", maxLen - 1);
  titleOut[maxLen - 1] = '\0';
}

void fileManagerSetup() {
  if (!SdMan.begin()) {
    Serial.println("SD Card mount failed!");
    return;
  }

  if (!SdMan.exists("/notes")) {
    SdMan.mkdir("/notes");
  }

  Serial.println("SD Card initialized");
  refreshFileList();
}

void refreshFileList() {
  fileCount = 0;

  auto root = SdMan.open("/notes");
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();
  char name[256];

  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || fileCount >= MAX_FILES) {
      file.close();
      if (fileCount >= MAX_FILES) break;
      continue;
    }

    int nameLen = strlen(name);
    if (nameLen > 4 && strcmp(name + nameLen - 4, ".txt") == 0) {
      strncpy(fileList[fileCount].filename, name, MAX_FILENAME_LEN - 1);
      fileList[fileCount].filename[MAX_FILENAME_LEN - 1] = '\0';

      char fullPath[320];
      snprintf(fullPath, sizeof(fullPath), "/notes/%s", name);
      readTitleFromFile(fullPath, fileList[fileCount].title, MAX_TITLE_LEN);
      fileList[fileCount].modTime = 0;
      fileCount++;
    }
    file.close();
  }
  root.close();

  Serial.printf("File listing: %d files found\n", fileCount);
}

int getFileCount() { return fileCount; }
FileInfo* getFileList() { return fileList; }

void loadFile(const char* filename) {
  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename);

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    Serial.printf("Could not open: %s\n", path);
    return;
  }

  char* buf = editorGetBuffer();
  int readResult = file.read(buf, TEXT_BUFFER_SIZE - 1);
  size_t bytesRead = (readResult > 0) ? (size_t)readResult : 0;
  buf[bytesRead] = '\0';
  file.close();

  editorLoadBuffer(bytesRead);
  editorSetCurrentFile(filename);

  // Extract title from first line
  char title[MAX_TITLE_LEN];
  char* newline = strchr(buf, '\n');
  if (newline && (newline - buf) < MAX_TITLE_LEN - 1 && (newline - buf) > 0) {
    int tLen = newline - buf;
    strncpy(title, buf, tLen);
    title[tLen] = '\0';
  } else {
    strncpy(title, filename, MAX_TITLE_LEN - 1);
    title[MAX_TITLE_LEN - 1] = '\0';
  }
  editorSetCurrentTitle(title);
  editorSetUnsavedChanges(false);

  currentState = UIState::TEXT_EDITOR;
  Serial.printf("Loaded: %s (%d bytes)\n", filename, (int)bytesRead);
}

void saveCurrentFile() {
  const char* filename = editorGetCurrentFile();
  if (filename[0] == '\0') return;

  char path[320], tmpPath[330];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

  auto file = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    Serial.printf("Could not write: %s\n", tmpPath);
    return;
  }

  file.write((const uint8_t*)editorGetBuffer(), editorGetLength());
  file.close();

  SdMan.remove(path);
  SdMan.rename(tmpPath, path);

  editorSetUnsavedChanges(false);
  refreshFileList();
  Serial.printf("Saved: %s\n", filename);
}

void createNewFile() {
  // Read counter
  int counter = 0;
  auto cf = SdMan.open("/notes/.counter", O_RDONLY);
  if (cf) {
    char buf[16];
    int len = cf.read(buf, sizeof(buf) - 1);
    if (len > 0) {
      buf[len] = '\0';
      counter = atoi(buf);
    }
    cf.close();
  }
  counter++;

  // Save counter
  auto wf = SdMan.open("/notes/.counter", O_WRONLY | O_CREAT | O_TRUNC);
  if (wf) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", counter);
    wf.write(buf, strlen(buf));
    wf.close();
  }

  char filename[MAX_FILENAME_LEN];
  snprintf(filename, sizeof(filename), "note_%d_%lu.txt", counter, millis());

  editorClear();
  editorSetCurrentFile(filename);
  editorSetCurrentTitle("Untitled");
  editorSetUnsavedChanges(true);

  currentState = UIState::TEXT_EDITOR;
  Serial.printf("New file: %s\n", filename);
}
