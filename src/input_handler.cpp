#include "input_handler.h"
#include "text_editor.h"
#include "file_manager.h"
#include "ble_keyboard.h"
#include "wifi_sync.h"
#include "keyboard_layout.h"

#include <Arduino.h>
#include <SDCardManager.h>

// External variables
extern bool autoReconnectEnabled;
extern bool darkMode;
extern bool cleanMode;
extern bool deleteConfirmPending;
extern WritingMode writingMode;
extern FontSize fontSize;
extern bool showWordCount;
extern KeyboardLayout currentKeyboardLayout;

// External functions
void storePairedDevice(const std::string& address, const std::string& name);
bool getStoredDevice(std::string& address, std::string& name);
void clearStoredDevice();
uint32_t getCurrentPasskey();
bool isDeviceScanning();
void refreshScanNow();
void clearAllBluetoothBonds();

// --- Input Queue ---
static KeyEvent inputQueue[INPUT_QUEUE_SIZE];
static int queueHead = 0;
static int queueTail = 0;
static volatile bool queueFull = false;

// --- CapsLock state ---
static bool capsLockOn = false;

// Where to return after title edit is confirmed or cancelled
static UIState renameReturnState = UIState::FILE_BROWSER;

// Forward declaration
static void openTitleEdit(const char* currentTitle, UIState returnTo);

// OTA app detection (defined in main.cpp)
extern OtaAppEntry otaApps[];
extern int otaAppCount;
void switchToOtaApp(int index);

// --- Shared UI state (defined in main.cpp) ---
extern UIState currentState;
extern int mainMenuSelection;
extern int selectedFileIndex;
extern int settingsSelection;
extern int bluetoothDeviceSelection;
extern bool btShowAllDevices;
extern int pairedKeyboardSelection;
extern bool pairedForgetPending;
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
  const char* text = keyboardLayoutQwertyText(hid, modifiers, capsLockOn);
  if (!text || text[0] == '\0' || text[1] != '\0') return 0;
  return text[0];
}

// Handle text editor input
static void handleEditorKey(uint8_t keyCode, uint8_t modifiers) {
  // Ctrl shortcuts
  if (isCtrl(modifiers) && !isAltGr(modifiers)) {
    if (keyCode == HID_KEY_S) {
      saveCurrentFile();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_Z) {
      cleanMode = !cleanMode;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_N) {
      openTitleEdit(editorGetCurrentTitle(), UIState::TEXT_EDITOR);
      return;
    }
    if (keyCode == HID_KEY_T) {
      writingMode = (writingMode == WritingMode::TYPEWRITER) ? WritingMode::NORMAL : WritingMode::TYPEWRITER;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_F) {
      int v = static_cast<int>(fontSize);
      fontSize = static_cast<FontSize>((v + 1) % 3);
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_W) {
      showWordCount = !showWordCount;
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_K) {
      currentKeyboardLayout = keyboardLayoutNext(currentKeyboardLayout);
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_P) {
      writingMode = (writingMode == WritingMode::PAGINATION) ? WritingMode::NORMAL : WritingMode::PAGINATION;
      screenDirty = true;
      return;
    }
    // Clipboard: Ctrl+A select all, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste
    if (keyCode == HID_KEY_A) {
      editorSelectAll();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_C) {
      editorCopy();
      return;
    }
    if (keyCode == HID_KEY_X) {
      editorCut();
      screenDirty = true;
      return;
    }
    if (keyCode == HID_KEY_V) {
      editorPaste();
      screenDirty = true;
      return;
    }
    // Ctrl+Left/Right: jump pages in pagination mode
    if (writingMode == WritingMode::PAGINATION) {
      int pageSize = editorGetStoredVisibleLines();
      if (keyCode == HID_KEY_LEFT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorUp();
        screenDirty = true;
        return;
      }
      if (keyCode == HID_KEY_RIGHT) {
        for (int i = 0; i < pageSize; i++) editorMoveCursorDown();
        screenDirty = true;
        return;
      }
    }
    return;
  }

  // ESC = save and return to file browser
  if (keyCode == HID_KEY_ESCAPE) {
    if (editorHasUnsavedChanges()) saveCurrentFile();
    currentState = UIState::FILE_BROWSER;
    screenDirty = true;
    return;
  }

  // Tab cycles writing modes
  if (keyCode == HID_KEY_TAB) {
    int v = static_cast<int>(writingMode);
    writingMode = static_cast<WritingMode>((v + 1) % 3);
    screenDirty = true;
    return;
  }

  // Navigation keys. Shift+arrow/Home/End extends the selection (Ctrl already
  // returned above, so it keeps its own meaning). Backspace/Delete handle an
  // active selection internally.
  const bool extend = isShift(modifiers);
  switch (keyCode) {
    case HID_KEY_LEFT:      editorMoveCursorLeft(extend);  screenDirty = true; return;
    case HID_KEY_RIGHT:     editorMoveCursorRight(extend); screenDirty = true; return;
    case HID_KEY_UP:        editorMoveCursorUp(extend);    screenDirty = true; return;
    case HID_KEY_DOWN:      editorMoveCursorDown(extend);  screenDirty = true; return;
    case HID_KEY_HOME:      editorMoveCursorHome(extend);  screenDirty = true; return;
    case HID_KEY_END:       editorMoveCursorEnd(extend);   screenDirty = true; return;
    case HID_KEY_BACKSPACE: editorDeleteChar();      screenDirty = true; return;
    case HID_KEY_DELETE:    editorDeleteForward();   screenDirty = true; return;
  }

  // CapsLock toggle
  if (keyCode == HID_KEY_CAPSLOCK) {
    capsLockOn = !capsLockOn;
    return;
  }

  // Printable character
  const char* text = keyboardLayoutText(currentKeyboardLayout, keyCode, modifiers, capsLockOn);
  if (text != nullptr) {
    editorInsertText(text);
    screenDirty = true;
  }
}

// Open the title edit screen, returning to `returnTo` on confirm/cancel
static void openTitleEdit(const char* currentTitle, UIState returnTo) {
  strncpy(renameBuffer, currentTitle, MAX_TITLE_LEN - 1);
  renameBuffer[MAX_TITLE_LEN - 1] = '\0';
  renameBufferLen = strlen(renameBuffer);
  renameReturnState = returnTo;
  currentState = UIState::RENAME_FILE;
  screenDirty = true;
}

// Handle title edit input
static void handleRenameKey(uint8_t keyCode, uint8_t modifiers) {
  if (keyCode == HID_KEY_ENTER) {
    if (renameBufferLen > 0) {
      if (renameReturnState == UIState::TEXT_EDITOR) {
        editorSetCurrentTitle(renameBuffer);
        if (editorGetCurrentFile()[0] == '\0') {
          // New file — derive filename from title
          char filename[MAX_FILENAME_LEN];
          deriveUniqueFilename(renameBuffer, filename, MAX_FILENAME_LEN);
          editorSetCurrentFile(filename);
        } else {
          // Existing file — rename on disk to match new title
          updateFileTitle(editorGetCurrentFile(), renameBuffer);
        }
        editorSetUnsavedChanges(true);
        saveCurrentFile();
      } else {
        // Updating title of a file selected in the browser
        FileInfo* files = getFileList();
        updateFileTitle(files[selectedFileIndex].filename, renameBuffer);
      }
    }
    currentState = renameReturnState;
    screenDirty = true;
    return;
  }

  if (keyCode == HID_KEY_ESCAPE) {
    currentState = renameReturnState;
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

  // Allow all printable characters in a title (including spaces)
  char c = hidToAscii(keyCode, modifiers);
  if (c != 0 && c >= ' ' && renameBufferLen < MAX_TITLE_LEN - 1) {
    renameBuffer[renameBufferLen++] = c;
    renameBuffer[renameBufferLen] = '\0';
    screenDirty = true;
  }
}

static void dispatchEvent(const KeyEvent& event) {
  if (!event.pressed) return;

  switch (currentState) {
    case UIState::MAIN_MENU: {
      int menuCount = 4 + otaAppCount;
      if (event.keyCode == HID_KEY_DOWN) {
        mainMenuSelection = (mainMenuSelection + 1) % menuCount;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        mainMenuSelection = (mainMenuSelection - 1 + menuCount) % menuCount;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (mainMenuSelection == 0) {
          refreshFileList();
          currentState = UIState::FILE_BROWSER;
          screenDirty = true;
        } else if (mainMenuSelection == 1) {
          createNewFile();
          openTitleEdit("Untitled", UIState::TEXT_EDITOR);
        } else if (mainMenuSelection == 2) {
          currentState = UIState::SETTINGS;
          screenDirty = true;
        } else if (mainMenuSelection == 3) {
          wifiSyncStart();
          currentState = UIState::WIFI_SYNC;
          screenDirty = true;
        } else if (mainMenuSelection >= 4) {
          switchToOtaApp(mainMenuSelection - 4);
        }
      }
      break;
    }

    case UIState::FILE_BROWSER: {
      int fc = getFileCount();

      // Delete confirmation pending — Enter confirms, anything else cancels
      if (deleteConfirmPending) {
        if (event.keyCode == HID_KEY_ENTER && fc > 0) {
          FileInfo* files = getFileList();
          deleteFile(files[selectedFileIndex].filename);
          int newFc = getFileCount();
          if (selectedFileIndex >= newFc) selectedFileIndex = newFc - 1;
          if (selectedFileIndex < 0) selectedFileIndex = 0;
        }
        deleteConfirmPending = false;
        screenDirty = true;
        break;
      }

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
      } else if (isCtrl(event.modifiers) && !isAltGr(event.modifiers) && event.keyCode == HID_KEY_N) {
        if (fc > 0) {
          FileInfo* files = getFileList();
          openTitleEdit(files[selectedFileIndex].title, UIState::FILE_BROWSER);
        }
      } else if (isCtrl(event.modifiers) && !isAltGr(event.modifiers) && event.keyCode == HID_KEY_D) {
        if (fc > 0) {
          deleteConfirmPending = true;
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
      const int SETTINGS_COUNT = 8;  // Orientation, Dark Mode, Writing Mode, Font Size, Layout, BLE Power, Bluetooth, Paired Keyboards

      // Up/Down: navigate settings list (physical buttons also map here)
      if (event.keyCode == HID_KEY_DOWN) {
        settingsSelection = (settingsSelection + 1) % SETTINGS_COUNT;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_UP) {
        settingsSelection = (settingsSelection - 1 + SETTINGS_COUNT) % SETTINGS_COUNT;
        screenDirty = true;

      // Enter or Right: cycle setting forward
      } else if (event.keyCode == HID_KEY_ENTER || event.keyCode == HID_KEY_RIGHT) {
        if (settingsSelection == 0) {
          int v = static_cast<int>(currentOrientation);
          currentOrientation = static_cast<Orientation>((v + 1) % 4);
        } else if (settingsSelection == 1) {
          darkMode = !darkMode;
        } else if (settingsSelection == 2) {
          int v = static_cast<int>(writingMode);
          writingMode = static_cast<WritingMode>((v + 1) % 3);
        } else if (settingsSelection == 3) {
          int v = static_cast<int>(fontSize);
          fontSize = static_cast<FontSize>((v + 1) % 3);
        } else if (settingsSelection == 4) {
          currentKeyboardLayout = keyboardLayoutNext(currentKeyboardLayout);
        } else if (settingsSelection == 5) {
          cycleBleTxPower(+1);   // more power (adjustable in place, like Font Size)
        } else if (settingsSelection == 6) {
          bluetoothDeviceSelection = 0;
          btShowAllDevices = true;   // default to showing all (see note at definition)
          currentState = UIState::BLUETOOTH_SETTINGS;
        } else if (settingsSelection == 7) {
          pairedKeyboardSelection = 0;
          pairedForgetPending = false;
          currentState = UIState::PAIRED_KEYBOARDS;
        }
        screenDirty = true;

      // Left: cycle setting backward (keyboard only — physical L/R map to Up/Down)
      } else if (event.keyCode == HID_KEY_LEFT) {
        if (settingsSelection == 0) {
          int v = static_cast<int>(currentOrientation);
          currentOrientation = static_cast<Orientation>((v - 1 + 4) % 4);
        } else if (settingsSelection == 1) {
          darkMode = !darkMode;
        } else if (settingsSelection == 2) {
          int v = static_cast<int>(writingMode);
          writingMode = static_cast<WritingMode>((v - 1 + 3) % 3);
        } else if (settingsSelection == 3) {
          int v = static_cast<int>(fontSize);
          fontSize = static_cast<FontSize>((v - 1 + 3) % 3);
        } else if (settingsSelection == 4) {
          currentKeyboardLayout = keyboardLayoutPrev(currentKeyboardLayout);
        } else if (settingsSelection == 5) {
          cycleBleTxPower(-1);   // less power
        }
        screenDirty = true;

      } else if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::MAIN_MENU;
        screenDirty = true;
      }
      break;
    }

    case UIState::BLUETOOTH_SETTINGS: {
      // Build the visible list (keyboards first; others only when "show all").
      // The last selectable row may be the "Show other devices" toggle.
      int visIdx[32];
      int total = 0, kbd = 0;
      int visCount = getVisibleDeviceIndices(btShowAllDevices, visIdx, 32, &total, &kbd);
      bool toggleShown = (kbd > 0 && total > kbd);   // a real filter choice exists
      int itemCount = visCount + (toggleShown ? 1 : 0);

      // Keep selection in range
      if (itemCount == 0) bluetoothDeviceSelection = 0;
      else if (bluetoothDeviceSelection >= itemCount) bluetoothDeviceSelection = itemCount - 1;
      if (bluetoothDeviceSelection < 0) bluetoothDeviceSelection = 0;

      bool onToggle = (toggleShown && bluetoothDeviceSelection == visCount);

      if (event.keyCode == HID_KEY_ESCAPE) {
        currentState = UIState::SETTINGS;
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_DOWN) {
        if (itemCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection + 1) % itemCount;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_UP) {
        if (itemCount > 0) {
          bluetoothDeviceSelection = (bluetoothDeviceSelection - 1 + itemCount) % itemCount;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (onToggle) {
          btShowAllDevices = !btShowAllDevices;   // flip filter, reset to top
          bluetoothDeviceSelection = 0;
        } else if (visCount > 0 && !isDeviceScanning()) {
          connectToDevice(visIdx[bluetoothDeviceSelection]);
        } else if (!isDeviceScanning()) {
          startDeviceScan();   // nothing to connect to — scan instead
        }
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_RIGHT) {
        // Right button = re-scan for devices
        if (!isDeviceScanning()) startDeviceScan();
        screenDirty = true;
      } else if (event.keyCode == HID_KEY_LEFT) {
        if (isKeyboardConnected()) {
          disconnectCurrentDevice();
          screenDirty = true;
        }
      }
      break;
    }

    case UIState::PAIRED_KEYBOARDS: {
      int count = getPairedKeyboardCount();

      if (event.keyCode == HID_KEY_ESCAPE) {
        if (pairedForgetPending) {
          pairedForgetPending = false;   // cancel the confirmation, stay on screen
          screenDirty = true;
        } else {
          currentState = UIState::SETTINGS;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_DOWN) {
        pairedForgetPending = false;     // moving the selection cancels a pending forget
        if (count > 0) {
          pairedKeyboardSelection = (pairedKeyboardSelection + 1) % count;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_UP) {
        pairedForgetPending = false;
        if (count > 0) {
          pairedKeyboardSelection = (pairedKeyboardSelection - 1 + count) % count;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_ENTER) {
        if (count > 0) {
          if (pairedForgetPending) {
            // Enter confirms the forget
            removePairedKeyboard(pairedKeyboardSelection);
            pairedForgetPending = false;
            int newCount = getPairedKeyboardCount();
            if (newCount == 0) pairedKeyboardSelection = 0;
            else if (pairedKeyboardSelection >= newCount)
              pairedKeyboardSelection = newCount - 1;
            screenDirty = true;
          } else {
            connectToPairedKeyboard(pairedKeyboardSelection);
            currentState = UIState::SETTINGS;
            screenDirty = true;
          }
        }
      } else if (event.keyCode == HID_KEY_D) {
        // Arm the forget confirmation (Right button on-device, or 'D' on a keyboard)
        if (count > 0) {
          pairedForgetPending = true;
          screenDirty = true;
        }
      } else if (event.keyCode == HID_KEY_LEFT) {
        // Disconnect if this keyboard is the currently connected one
        if (count > 0 && isKeyboardConnected()) {
          std::string addr, name; uint8_t addrType;
          getPairedKeyboard(pairedKeyboardSelection, addr, name, addrType);
          if (getCurrentDeviceAddress() == addr) {
            disconnectCurrentDevice();
            screenDirty = true;
          }
        }
      }
      break;
    }

    case UIState::WIFI_SYNC: {
      syncHandleKey(event.keyCode, event.modifiers);
      break;
    }

    default:
      break;
  }
}

int processAllInput() {
  int processedCount = 0;
  while (!isQueueEmpty()) {
    KeyEvent event = dequeueKeyEvent();
    dispatchEvent(event);
    processedCount++;
  }
  return processedCount;
}
