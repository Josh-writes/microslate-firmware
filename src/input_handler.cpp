#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"

#include <Arduino.h>
#include <SDCardManager.h>

// External variables
extern bool autoReconnectEnabled;

// External functions
void storePairedDevice(const std::string& address, const std::string& name);
bool getStoredDevice(std::string& address, std::string& name);
void clearStoredDevice();
uint32_t getCurrentPasskey();

// --- Input Queue ---
static KeyEvent inputQueue[INPUT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;
static volatile bool queueFull = false;

// --- CapsLock state ---
static bool capsLockOn = false;

// --- Shared UI state (defined in main.cpp) ---
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int bluetoothDeviceSelection;
extern Orientation currentOrientation;
extern int charsPerLine;
extern bool screenDirty;
extern char renameBuffer[];
extern int renameBufferLen;

void inputSetup() {
  queueHead = 0;
  queueTail = 0;
  queueFull = false;
  capsLockOn = false;
}

static bool isQueueEmpty() {
  return (queueHead == queueTail) && !queueFull;
}

void enqueueKeyEvent(uint8_t keyCode, uint8_t modifiers, bool pressed) {
  noInterrupts();
  if (!queueFull) {
    inputQueue[queueHead].keyCode = keyCode;
    inputQueue[queueHead].modifiers = modifiers;
    inputQueue[queueHead].pressed = pressed;
    queueHead = (queueHead + 1) % INPUT_QUEUE_SIZE;
    if (queueHead == queueTail) queueFull = true;
  }
  interrupts();
}

static KeyEvent dequeueKeyEvent() {
  KeyEvent event = {0, 0, false};
  noInterrupts();
  if (!isQueueEmpty()) {
    event = inputQueue[queueTail];
    queueTail = (queueTail + 1) % INPUT_QUEUE_SIZE;
    queueFull = false;
  }
  interrupts();
  return event;
}

char hidToAscii(uint8_t hid, uint8_t modifiers) {
  bool shifted = isShift(modifiers) ^ capsLockOn;

  // Letters a-z (HID 0x04-0x1D)
  if (hid >= 0x04 && hid <= 0x1D) {
    char base = 'a' + (hid - 0x04);
    return shifted ? (base - 32) : base;
  }

  // Number row (HID 0x1E-0x27)
  static const char unshifted[] = "1234567890";
  static const char shiftedNum[] = "!@#$%^&*()";
  if (hid >= 0x1E && hid <= 0x27) {
    int idx = hid - 0x1E;
    return isShift(modifiers) ? shiftedNum[idx] : unshifted[idx];
  }

  // Special keys
  switch (hid) {
    case 0x28: return '\n';  // Enter
    case 0x2B: return '\t';  // Tab
    case 0x2C: return ' ';   // Space

    // Symbol keys
    case 0x2D: return isShift(modifiers) ? '_' : '-';
    case 0x2E: return isShift(modifiers) ? '+' : '=';
    case 0x2F: return isShift(modifiers) ? '{' : '[';
    case 0x30: return isShift(modifiers) ? '}' : ']';
    case 0x31: return isShift(modifiers) ? '|' : '\\';
    case 0x33: return isShift(modifiers) ? ':' : ';';
    case 0x34: return isShift(modifiers) ? '"' : '\'';
    case 0x35: return isShift(modifiers) ? '~' : '`';
    case 0x36: return isShift(modifiers) ? '<' : ',';
    case 0x37: return isShift(modifiers) ? '>' : '.';
    case 0x38: return isShift(modifiers) ? '?' : '/';

    default: return 0;
  }
}

// Handle text editor input
static void handleEditorKey(uint8_t keyCode, uint8_t modifiers) {
  // Ctrl shortcuts
  if (isCtrl(modifiers)) {
    if (keyCode == HID_KEY_S) {
      saveCurrentFile();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_Q) {
      if (editorHasUnsavedChanges()) saveCurrentFile();
      currentState = UIState::FILE_BROWSER;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_N) {
      createNewFile();
      screenDirty = true;
      return;
    }
    return;
  }

  // Navigation keys
  switch (keyCode) {
    case HID_KEY_LEFT:      editorMoveCursorLeft();  screenDirty = true; return;
    case HID_KEY_RIGHT:     editorMoveCursorRight(); screenDirty = true; return;
    case HID_KEY_UP:        editorMoveCursorUp();    screenDirty = true; return;
    case HID_KEY_DOWN:      editorMoveCursorDown();  screenDirty = true; return;
    case HID_KEY_HOME:      editorMoveCursorHome();  screenDirty = true; return;
    case HID_KEY_END:       editorMoveCursorEnd();   screenDirty = true; return;
    case HID_KEY_BACKSPACE: editorDeleteChar();      screenDirty = true; return;
    case HID_KEY_DELETE:    editorDeleteForward();   screenDirty = true; return;
  }

  // CapsLock toggle
  if (keyCode == HID_KEY_CAPSLOCK) {
    capsLockOn = !capsLockOn;
    return;
  }

  // Printable character
  char c = hidToAscii(keyCode, modifiers);
  if (c != 0) {
    editorInsertChar(c);
    screenDirty = true;
  }
}

// Handle rename input
static void handleRenameKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ENTER) {
    if (renameBufferLen > 0) {
      char oldPath[128], newPath[128];
      FileInfo* files = getFileList();
      snprintf(oldPath, sizeof(oldPath), "/notes/%s", files[selectedFileIndex].filename);
      snprintf(newPath, sizeof(newPath), "/notes/%s.txt", renameBuffer);
      if (!SdMan.exists(newPath)) {
        SdMan.rename(oldPath, newPath);
        refreshFileList();
        currentState = UIState::FILE_BROWSER;
      }
    }
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_ESCAPE) {
    currentState = UIState::FILE_BROWSER;
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_BACKSPACE) {
    if (renameBufferLen > 0) {
      renameBufferLen--;
      renameBuffer[renameBufferLen] = '\0';
      screenDirty = true;
    }
    return;
  }

  char c = hidToAscii(keyCode, modifiers);
  if (c != 0 && renameBufferLen < MAX_FILENAME_LEN - 5) {
    // Only allow safe filename characters
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.') {
      renameBuffer[renameBufferLen++] = c;
      renameBuffer[renameBufferLen] = '\0';
      screenDirty = true;
    } else if (c == ' ') {
      renameBuffer[renameBufferLen++] = '_';
      renameBuffer[renameBufferLen] = '\0';
      screenDirty = true;
    }
  }
}

static void dispatchEvent(const KeyEvent& event) {
  if (!event.pressed) return;

  switch (currentState) {
    case UIState::MAIN_MENU: {
      if (event.keyCode == HID_KEY_DOWN) {
        mainMenuSelection = (mainMenuSelection + 1) % 3;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        mainMenuSelection = (mainMenuSelection - 1 + 3) % 3;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (mainMenuSelection == 0) {
          currentState = UIState::FILE_BROWSER;
          screenDirty = true;
        } else if (mainMenuSelection == 1) {
          createNewFile();
          screenDirty = true;
        } else if (mainMenuSelection == 2) {
          currentState = UIState::SETTINGS;
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::FILE_BROWSER: {
      int fc = getFileCount();
      if (event.keyCode == HID_KEY_DOWN && fc > 0) {
        selectedFileIndex = (selectedFileIndex + 1) % fc;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP && fc > 0) {
        selectedFileIndex = (selectedFileIndex - 1 + fc) % fc;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_ENTER && fc > 0) {
        FileInfo* files = getFileList();
        loadFile(files[selectedFileIndex].filename);
        screenDirty = true;
      } else if (isCtrl(event.modifiers) && event.keyCode == HID_KEY_N) {
        createNewFile();
        screenDirty = true;
      } else if ((isCtrl(event.modifiers) && event.keyCode == HID_KEY_R) ||
                 event.keyCode == HID_KEY_F2) {
        if (fc > 0) {
          currentState = UIState::RENAME_FILE;
          FileInfo* files = getFileList();
          // Copy filename without .txt
          strncpy(renameBuffer, files[selectedFileIndex].filename, MAX_FILENAME_LEN - 1);
          renameBuffer[MAX_FILENAME_LEN - 1] = '\0';
          // Remove .txt extension
          int len = strlen(renameBuffer);
          if (len > 4 && strcmp(renameBuffer + len - 4, ".txt") == 0) {
            renameBuffer[len - 4] = '\0';
          }
          renameBufferLen = strlen(renameBuffer);
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
      break;
    }

    case UIState::TEXT_EDITOR:
      handleEditorKey(event.keyCode, event.modifiers);
      break;

    case UIState::RENAME_FILE:
      handleRenameKey(event.keyCode, event.modifiers);
      break;

    case UIState::SETTINGS: {
      if (event.keyCode == HID_KEY_DOWN) {
        settingsSelection = (settingsSelection + 1) % 5;  // Increased to 5 options
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        settingsSelection = (settingsSelection - 1 + 5) % 5;  // Increased to 5 options
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_RIGHT) {
        if (settingsSelection == 0) {
          int v = static_cast<int>(currentOrientation);
          currentOrientation = static_cast<Orientation>((v + 1) % 4);
          screenDirty = true;
        } else if (settingsSelection == 1 && charsPerLine < 60) {
          charsPerLine += 5;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_LEFT) {
        if (settingsSelection == 0) {
          int v = static_cast<int>(currentOrientation);
          currentOrientation = static_cast<Orientation>((v - 1 + 4) % 4);
          screenDirty = true;
        } else if (settingsSelection == 1 && charsPerLine > 20) {
          charsPerLine -= 5;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (settingsSelection == 3) {  // Bluetooth settings option
          currentState = UIState::BLUETOOTH_SETTINGS;
          screenDirty = true;
        } else if (settingsSelection == 4) {  // Clear paired device option
          clearStoredDevice();
          screenDirty = true;
        } else if (settingsSelection == 2) {  // Back to main menu
          currentState = UIState::MAIN_MENU;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
      break;
    }

    case UIState::BLUETOOTH_SETTINGS: {
      int deviceCount = getDiscoveredDeviceCount();
      
      // Ensure selection is within bounds
      if (bluetoothDeviceSelection >= deviceCount && deviceCount > 0) {
        bluetoothDeviceSelection = deviceCount - 1;
      } else if (deviceCount == 0) {
        bluetoothDeviceSelection = 0; // Reset to 0 when no devices
      }
      
      if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_DOWN) {
        if (deviceCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection + 1) % deviceCount;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_UP) {
        if (deviceCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection - 1 + deviceCount) % deviceCount;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (deviceCount > 0) {
          // Connect to the selected device
          connectToDevice(bluetoothDeviceSelection);
        } else {
          // If no devices found, start scanning
          startDeviceScan();
        }
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_LEFT) {
        // Option to disconnect current device
        if (isKeyboardConnected()) {
          disconnectCurrentDevice();
          screenDirty = true;
        }
      }
      break;
    }

    default:
      break;
  }
}

void processAllInput() {
  while (!isQueueEmpty()) {
    KeyEvent event = dequeueKeyEvent();
    dispatchEvent(event);
  }
}
