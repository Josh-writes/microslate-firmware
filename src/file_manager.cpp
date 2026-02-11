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
    DBG_PRINTLN("SD Card mount failed!");
    return;
  }

  if (!SdMan.exists("/notes")) {
    SdMan.mkdir("/notes");
  }

  DBG_PRINTLN("SD Card initialized");
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

  DBG_PRINTF("File listing: %d files found\n", fileCount);
}

int getFileCount() { return fileCount; }
FileInfo* getFileList() { return fileList; }

void loadFile(const char* filename) {
  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename);

  auto file = SdMan.open(path, O_RDONLY);
  if (!file) {
    DBG_PRINTF("Could not open: %s\n", path);
    return;
  }

  char* buf = editorGetBuffer();
  int readResult = file.read(buf, TEXT_BUFFER_SIZE - 1);
  size_t bytesRead = (readResult > 0) ? (size_t)readResult : 0;
  buf[bytesRead] = '\0';
  file.close();

  editorSetCurrentFile(filename);

  // Split: first line = title, remainder after blank line = body
  char title[MAX_TITLE_LEN];
  char* newline = strchr(buf, '\n');
  if (newline && newline != buf) {
    int tLen = (int)(newline - buf);
    if (tLen > MAX_TITLE_LEN - 1) tLen = MAX_TITLE_LEN - 1;
    strncpy(title, buf, tLen);
    // Strip any trailing \r
    while (tLen > 0 && title[tLen - 1] == '\r') tLen--;
    title[tLen] = '\0';

    // Skip past title line + blank separator line(s)
    char* body = newline + 1;
    while (*body == '\n' || *body == '\r') body++;

    size_t bodyLen = bytesRead - (size_t)(body - buf);
    memmove(buf, body, bodyLen);
    buf[bodyLen] = '\0';
    editorLoadBuffer(bodyLen);
  } else {
    // Old-format file or empty — treat entire content as body, no title
    strncpy(title, "Untitled", MAX_TITLE_LEN - 1);
    title[MAX_TITLE_LEN - 1] = '\0';
    editorLoadBuffer(bytesRead);
  }
  editorSetCurrentTitle(title);
  editorSetUnsavedChanges(false);

  currentState = UIState::TEXT_EDITOR;
  DBG_PRINTF("Loaded: %s (%d bytes)\n", filename, (int)bytesRead);
}

void saveCurrentFile() {
  const char* filename = editorGetCurrentFile();
  if (filename[0] == '\0') return;

  char path[320], tmpPath[330];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

  auto file = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    DBG_PRINTF("Could not write: %s\n", tmpPath);
    return;
  }

  const char* title = editorGetCurrentTitle();
  file.write((const uint8_t*)title, strlen(title));
  file.write((const uint8_t*)"\n\n", 2);
  file.write((const uint8_t*)editorGetBuffer(), editorGetLength());
  file.close();

  SdMan.remove(path);
  SdMan.rename(tmpPath, path);

  editorSetUnsavedChanges(false);
  refreshFileList();
  DBG_PRINTF("Saved: %s\n", filename);
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

  // State transition is handled by the caller (goes to title edit first)
  DBG_PRINTF("New file: %s\n", filename);
}

// Convert a title to a valid FAT filename (lowercase, spaces→underscores,
// non-alphanumeric stripped, ".txt" appended).
static void titleToFilename(const char* title, char* out, int maxLen) {
  int maxBase = maxLen - 5; // room for ".txt" + null
  int j = 0;
  for (int i = 0; title[i] != '\0' && j < maxBase; i++) {
    char c = title[i];
    if (c >= 'A' && c <= 'Z') c += 32;
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      out[j++] = c;
    } else if (c == ' ' || c == '_' || c == '-') {
      if (j > 0 && out[j - 1] != '_') out[j++] = '_';
    }
  }
  while (j > 0 && out[j - 1] == '_') j--;
  if (j == 0) { strncpy(out, "note", maxLen - 1); j = 4; }
  strcpy(out + j, ".txt");
}

// Update just the title of a file on disk without touching the body.
// Also renames the file on disk to match the new title.
// Uses the editor buffer temporarily — only call this from the file browser
// (no active edit session).
void updateFileTitle(const char* filename, const char* newTitle) {
  char path[320], tmpPath[330];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);

  // Read the existing file into the editor buffer
  auto rFile = SdMan.open(path, O_RDONLY);
  if (!rFile) return;
  char* buf = editorGetBuffer();
  int readResult = rFile.read(buf, TEXT_BUFFER_SIZE - 1);
  rFile.close();
  if (readResult < 0) readResult = 0;
  buf[readResult] = '\0';

  // Find start of body (skip first line + blank separator)
  char* body = strchr(buf, '\n');
  if (body) {
    while (*body == '\n' || *body == '\r') body++;
  } else {
    body = buf + readResult; // no body
  }

  // Write new file: new title + separator + existing body
  auto wFile = SdMan.open(tmpPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!wFile) return;
  wFile.write((const uint8_t*)newTitle, strlen(newTitle));
  wFile.write((const uint8_t*)"\n\n", 2);
  wFile.write((const uint8_t*)body, strlen(body));
  wFile.close();

  SdMan.remove(path);
  SdMan.rename(tmpPath, path);

  // Rename the file on disk to match the title
  char newFilename[MAX_FILENAME_LEN];
  titleToFilename(newTitle, newFilename, MAX_FILENAME_LEN);

  if (strcmp(newFilename, filename) != 0) {
    // Handle collisions: strip .txt suffix for base, append _2, _3 etc.
    char base[MAX_FILENAME_LEN];
    strncpy(base, newFilename, MAX_FILENAME_LEN - 1);
    base[strlen(base) - 4] = '\0'; // remove ".txt"

    char newPath[320];
    snprintf(newPath, sizeof(newPath), "/notes/%s", newFilename);
    int suffix = 2;
    while (SdMan.exists(newPath) && suffix <= 99) {
      snprintf(newFilename, MAX_FILENAME_LEN, "%s_%d.txt", base, suffix++);
      snprintf(newPath, sizeof(newPath), "/notes/%s", newFilename);
    }

    char oldPath[320];
    snprintf(oldPath, sizeof(oldPath), "/notes/%s", filename);
    SdMan.rename(oldPath, newPath);

    // Keep editor in sync if this file is currently open
    if (strcmp(editorGetCurrentFile(), filename) == 0) {
      editorSetCurrentFile(newFilename);
    }
  }

  refreshFileList();
}

void deleteFile(const char* filename) {
  char path[320];
  snprintf(path, sizeof(path), "/notes/%s", filename);
  SdMan.remove(path);
  refreshFileList();
  DBG_PRINTF("Deleted: %s\n", filename);
}
