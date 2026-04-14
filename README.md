# MicroSlate

A dedicated writing firmware for the **Xteink X4** e-paper device. Pairs with any Bluetooth keyboard and saves notes to MicroSD.

## Features

- **Bluetooth Keyboard** — BLE HID host, connects to any standard wireless keyboard. Stores up to 4 keyboards; auto-cycles through them on reconnect. Tested with Logitech Keys-To-Go 2 and Keychron K3.
- **Note Management** — browse, create, rename, and delete notes from an SD card
- **Named Notes** — each note has a title stored in the file; shown in the browser and editable without touching body text
- **Text Editor** — cursor navigation, word-wrap, fast e-paper refresh
- **Writing Modes** — three display modes to suit different writing styles:
  - *Scroll* — standard scrolling editor (default)
  - *Typewriter* — shows only the current line centered on a blank screen. Focused, distraction-free single-line writing
  - *Pagination* — page-based display instead of scrolling. Clean page flips instead of per-line scroll refreshes
- **Auto-Save** — content is silently saved to SD card after 10 seconds of idle or every 2 minutes during continuous typing; no manual save required. Every exit path (back button, Esc, power button, sleep, restart) also saves automatically
- **Safe Writes** — saves use a write-verify + `.bak` rotation pattern; a failed or interrupted write never destroys the previous version. Orphaned files from a crash are recovered automatically on next boot
- **Clean Mode** — hides all UI chrome while editing so only your text is on screen (Ctrl+Z to toggle)
- **Dark Mode** — inverted display
- **Display Orientation** — portrait, landscape, and inverted variants
- **Power Management** — ESP-IDF light sleep between loop iterations (CPU drops to 10MHz), BLE modem sleep keeps the radio alive, SD card sleeps between accesses, display analog circuits power down after each refresh, and the device enters deep sleep after 5 minutes of inactivity
- **WiFi Sync** — one-button backup of all notes to your PC over WiFi. Saves network credentials for instant reconnect. Read-only server — nothing on the device can be modified over the network
- **Standalone Build** — all libraries are bundled in the repo; no sibling projects required

## Hardware Requirements

- Xteink X4 e-paper device (ESP32-C3, 800x480 display, physical buttons, SD slot)
- MicroSD card formatted as FAT32
- A Bluetooth HID keyboard

## Installation

### Prerequisites

- [PlatformIO](https://platformio.org/install/) (CLI or VS Code extension)
- USB cable to connect to the Xteink X4

### Build and Flash

```bash
# Clone the repository
git clone https://github.com/Josh-writes/microslate-firmware
cd xteink-writer-firmware

# Build
pio run

# Build and upload (adjust port if needed)
pio run --target upload
```

The upload port defaults to `COM5` in `platformio.ini`. To override:

```bash
pio run --target upload --upload-port /dev/ttyUSB0
```

All libraries are included in the `lib/` directory. The only external dependency fetched automatically by PlatformIO is **esp-nimble-cpp** (BLE stack).

### First Boot

1. Insert a FAT32-formatted MicroSD card
2. Power on the device — it boots to the main menu
3. Go to **Settings → Bluetooth** and scan for your keyboard
4. Select your keyboard from the list and press Enter to pair
5. Return to the main menu and start writing

The device remembers paired keyboards (up to 4) and reconnects automatically on subsequent boots. If multiple keyboards are stored, it cycles through them until one responds.

## Usage

### Main Menu

| Key | Action |
|-----|--------|
| Up / Down | Navigate |
| Left / Right | Also navigate (convenient in landscape) |
| Enter | Select |

Options: **Browse Notes**, **New Note**, **Settings**, **Sync**

### File Browser

| Key | Action |
|-----|--------|
| Up / Down | Navigate list |
| Left / Right | Also navigate (convenient in landscape) |
| Enter | Open note |
| Ctrl+N | Edit title of selected note |
| Ctrl+D | Delete selected note (confirmation required) |
| Esc | Back to main menu |

When delete is pending, the footer shows `Delete? Enter:Yes  Esc:No`. Press Enter to confirm or any other key to cancel.

### Text Editor

| Key | Action |
|-----|--------|
| Arrow keys | Move cursor |
| Home / End | Start / end of line |
| Backspace / Delete | Remove characters |
| Tab | Cycle writing mode (Scroll → Typewriter → Pagination) |
| Ctrl+S | Save manually |
| Ctrl+N | Edit note title |
| Ctrl+Z | Toggle clean mode (hides UI chrome) |
| Ctrl+T | Toggle Typewriter mode |
| Ctrl+P | Toggle Pagination mode |
| Ctrl+Left / Right | Jump pages (Pagination mode only) |
| Esc / Back button | Save and return to file browser |

The current writing mode is shown in the header: **[S]** Scroll, **[T]** Typewriter, **[P]** Pagination.

Auto-save runs silently after 10 seconds of idle or every 2 minutes during continuous typing — Ctrl+S is only needed if you want to save immediately.

### Writing Modes

**Scroll [S]** — Standard scrolling editor. Text scrolls as the cursor moves down the page.

**Typewriter [T]** — Only the current line is shown, centered vertically on a blank screen. When you press Enter, the previous line disappears and a fresh line appears. Text is still saved to the buffer normally. Combine with Clean Mode (Ctrl+Z) for a completely minimal writing experience.

**Pagination [P]** — Instead of scrolling when text fills the screen, the display flips to a new blank page. The current page is shown in the header (e.g. "Pg 1/3"). Use Ctrl+Left and Ctrl+Right to jump between pages. Eliminates per-line scroll refreshes — only one refresh per page transition.

### Title Edit

Accessed via Ctrl+N from the file browser or editor.

| Key | Action |
|-----|--------|
| Type | Enter title text |
| Backspace | Delete last character |
| Enter | Confirm |
| Esc | Cancel |

### Settings

Navigate with all four direction buttons (or Up/Down on keyboard). Press Enter (or confirm button) to cycle through a setting's values. On a keyboard, Left/Right also cycle values backward/forward.

| Setting | Values |
|---------|--------|
| Orientation | Portrait, Landscape CW, Inverted, Landscape CCW |
| Dark Mode | Light / Dark |
| Writing Mode | Normal, Typewriter, Pagination |
| Bluetooth | Opens Bluetooth scan to pair a new keyboard |
| Paired Keyboards | Manage saved keyboards (connect, forget, disconnect) |

All settings persist across reboots.

### Paired Keyboards

Shows all keyboards saved on the device (up to 4). The currently active keyboard is labelled **active**; the last used keyboard when none is connected is labelled **last**.

| Key | Action |
|-----|--------|
| Up / Down | Navigate list |
| Enter | Switch to selected keyboard |
| D | Forget selected keyboard (removes pairing) |
| Left | Disconnect selected keyboard (if currently active) |
| Esc | Back to Settings |

To pair a second keyboard, go to **Settings → Bluetooth**, scan, and connect. Both keyboards will then appear in the Paired Keyboards list. On each boot the device tries the last-used keyboard first, then works through the rest of the list until one connects.

### Bluetooth Settings

| Key | Action |
|-----|--------|
| Up / Down | Navigate device list |
| Enter | Connect to selected device (or start scan if list is empty) |
| Right | Re-scan for devices |
| Left | Disconnect current keyboard |
| Esc | Back to Settings |

A scan runs for 5 seconds and then stops. Up to 10 nearby devices are shown with name, address, and signal strength.

### WiFi Sync

Back up all notes from the device to your PC over WiFi. The device and PC must be on the **same WiFi network**.

#### One-time PC setup

1. Install [Python 3](https://www.python.org/downloads/) if you don't have it
2. Install the required library:
   ```bash
   pip install requests
   ```
3. Double-click **`sync\install_sync.bat`** to install and start the sync script

That's it. The script starts immediately and will run silently in the background on every login — no window, no tray icon. When a sync completes, a Windows notification pops up listing the files that were downloaded. Notes are saved to `Documents\MicroSlate Notes\` by default (edit `LOCAL_DIR` in `microslate_sync.py` to change).

To stop auto-start later, double-click **`sync\uninstall_sync.bat`**.

#### Syncing

1. Select **Sync** from the main menu on the device
2. **First time:** pick your WiFi network and enter the password. The device asks to save credentials.
3. **After that:** the device auto-connects — just press Sync and wait
4. The device syncs automatically once connected — a progress log is shown on screen
5. When done, the device shows a summary and turns WiFi off automatically. A Windows notification appears listing the downloaded files

If the sync script isn't running, you can start it manually:
```bash
python sync/microslate_sync.py
```

#### How sync works

- One-way backup: device → PC. Nothing is ever uploaded or deleted.
- Files already on the PC with the same name and size are skipped
- Files deleted from the device are **not** deleted from the PC — they stay as a backup
- The device HTTP server is **read-only** — no one on the network can modify or delete files
- WiFi turns off automatically after sync completes or after 60 seconds of no activity

#### Sync controls

| Key | Action |
|-----|--------|
| Up / Down | Navigate network list |
| Enter | Select network / confirm |
| Esc | Cancel / back |

## File Format

Notes are plain `.txt` files stored in `/notes/` on the SD card. Filenames are derived from the note title — spaces become underscores, everything is lowercased, and `.txt` is appended. For example, a note titled "My Note" becomes `my_note.txt`.

Files are fully compatible with any text editor on a computer. To add notes manually, drop `.txt` files into the `/notes/` folder on the SD card — the title shown on the device is derived from the filename.

## Project Structure

```
xteink-writer-firmware/
├── src/
│   ├── main.cpp          — setup, main loop, shared UI state
│   ├── ble_keyboard.cpp  — BLE scanning, pairing, HID report handling
│   ├── input_handler.cpp — keyboard event queue and UI state dispatch
│   ├── text_editor.cpp   — text buffer and cursor management
│   ├── file_manager.cpp  — SD card file operations
│   ├── ui_renderer.cpp   — screen rendering for all UI modes
│   ├── wifi_sync.cpp     — WiFi sync server and state machine
│   └── config.h          — enums, buffer sizes, constants
├── sync/
│   ├── microslate_sync.py   — PC sync script (Python)
│   ├── install_sync.bat     — register auto-start on Windows login
│   └── uninstall_sync.bat   — remove auto-start task
├── lib/                  — all hardware/display libraries (bundled)
│   ├── GfxRenderer/
│   ├── EpdFont/
│   ├── EInkDisplay/
│   ├── hal/
│   ├── BatteryMonitor/
│   ├── InputManager/
│   ├── SDCardManager/
│   └── Utf8/
└── platformio.ini
```

## Troubleshooting

**Keyboard not showing in scan**
- Make sure the keyboard is in pairing mode and not connected to another device
- Press Right to re-scan after switching the keyboard to pairing mode

**Physical buttons not responding**
- BLE scanning can occasionally interfere with the ADC button reads
- Hold the BACK button for 3 seconds to restart the device

**Display appears frozen**
- E-paper refresh takes ~430ms — wait for it to complete before pressing more keys

**Serial monitor shows nothing on startup**
- The ESP32-C3 USB-CDC port re-enumerates after reset; startup logs are sent before the monitor reconnects. This is normal — the device is working correctly.

---

## More from TypeSlate

MicroSlate is the hardware companion to **TypeSlate** — a free, full-screen distraction-free writing app for Windows. Same idea, different form factor: open it, write, close it.

- **TypeSlate for Windows** — free on the [Microsoft Store](https://apps.microsoft.com/detail/9PM3J9SQB0TV?hl=en-us&gl=US&ocid=pdpshare)
- **Website** — [typeslate.com](https://typeslate.com)

If MicroSlate is useful to you and you'd like to say thanks, you can support the project at [ko-fi.com/typeslate](https://ko-fi.com/typeslate).
