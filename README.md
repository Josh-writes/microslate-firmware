# MicroSlate

A dedicated writing firmware for the **Xteink X4** e-paper device. Pairs with any Bluetooth keyboard and saves notes to MicroSD — no companion app, no cloud, no distractions.

## Features

- **Bluetooth Keyboard** — BLE HID host, connects to any standard wireless keyboard. Tested with Logitech Keys-To-Go 2.
- **Note Management** — browse, create, rename, and delete notes from an SD card
- **Named Notes** — each note has a title stored in the file; shown in the browser and editable without touching body text
- **Text Editor** — cursor navigation, word-wrap, fast e-paper refresh
- **Dark Mode** — inverted display for low-light writing
- **Clean Mode** — hides UI chrome while editing so only your text is on screen
- **Display Orientation** — portrait, landscape, and inverted variants
- **Power Management** — CPU light-sleeps between events to extend battery life
- **Standalone Build** — all libraries are bundled in the repo; no sibling projects required

## Hardware Requirements

- Xteink X4 e-paper device (ESP32-C3, 800×480 display, physical buttons, SD slot)
- MicroSD card formatted as FAT32
- A Bluetooth HID keyboard

## Installation

### Prerequisites

- [PlatformIO](https://platformio.org/install/) (CLI or VS Code extension)
- USB cable to connect to the Xteink X4

### Build and Flash

```bash
# Clone the repository
git clone <repo-url>
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

All libraries are included in the `lib/` directory. The only external dependency fetched automatically by PlatformIO is **NimBLE-Arduino** (BLE stack).

### First Boot

1. Insert a FAT32-formatted MicroSD card
2. Power on the device — it boots to the main menu
3. Go to **Settings → Bluetooth Settings** and scan for your keyboard
4. Select your keyboard from the list and press Enter to pair
5. Return to the main menu and start writing

The device remembers the paired keyboard and reconnects automatically on subsequent boots.

## Usage

### Main Menu

| Key | Action |
|-----|--------|
| Up / Down | Navigate |
| Enter | Select |

Options: **Browse Notes**, **New Note**, **Settings**

### File Browser

| Key | Action |
|-----|--------|
| Up / Down | Navigate list |
| Enter | Open note |
| Ctrl+T | Edit title of selected note |
| Ctrl+D | Delete selected note (confirmation required) |
| Esc | Back to main menu |

When delete is pending, the footer shows `Delete? Enter:Yes  Esc:No`. Press Enter to confirm or any other key to cancel.

### Text Editor

| Key | Action |
|-----|--------|
| Arrow keys | Move cursor |
| Home / End | Start / end of line |
| Backspace / Delete | Remove characters |
| Ctrl+S | Save |
| Ctrl+T | Edit note title |
| Ctrl+Z | Toggle clean mode (hides UI chrome) |
| Esc | Save and return to file browser |

### Title Edit

Accessed via Ctrl+T from the file browser or editor.

| Key | Action |
|-----|--------|
| Type | Enter title text |
| Backspace | Delete last character |
| Enter | Confirm |
| Esc | Cancel |

### Settings

| Key | Action |
|-----|--------|
| Up / Down | Navigate |
| Enter or Right | Adjust / enter submenu |
| Left | Adjust (orientation, dark mode) |
| Esc | Back to main menu |

Options:
- **Orientation** — cycles through portrait/landscape/inverted variants
- **Dark Mode** — toggle inverted display
- **Bluetooth Settings** — submenu to scan and connect keyboards
- **Clear Paired Device** — removes stored pairing from device memory

### Bluetooth Settings

| Key | Action |
|-----|--------|
| Up / Down | Navigate device list |
| Enter | Connect to selected device (or start scan if list is empty) |
| Right | Re-scan for devices |
| Left | Disconnect current keyboard |
| Esc | Back to Settings |

A scan runs for 5 seconds and then stops. Up to 10 nearby devices are shown with name, address, and signal strength.

## File Format

Notes are plain `.txt` files stored in `/notes/` on the SD card. The first line is the title, followed by a blank line separator, then the body:

```
My Note Title

This is the body of the note.
It continues here...
```

Files are named `note_N_TIMESTAMP.txt` and are fully compatible with any text editor on a computer.

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
│   └── config.h          — hardware pins, buffer sizes, constants
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
- Hold the BACK button for 5 seconds to restart the device

**Display appears frozen**
- E-paper refresh takes ~430ms — wait for it to complete before pressing more keys

**Serial monitor shows nothing on startup**
- The ESP32-C3 USB-CDC port re-enumerates after reset; startup logs are sent before the monitor reconnects. This is normal — the device is working correctly.
