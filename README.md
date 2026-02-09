# MicroSlate - Dedicated Writing Device for Xteink X4

MicroSlate is a specialized firmware for the Xteink X4 e-paper device that transforms it into a dedicated writing device with Bluetooth keyboard support.

## Features

- **Bluetooth Keyboard Support**: Full HID keyboard functionality via BLE
- **Simple Menu System**: Main menu with Browse Files, New Note, and Settings
- **Text Editor**: Dedicated editor optimized for e-paper displays
- **File Title Management**: View and edit titles of text files
- **File Content Editing**: Full text editing capabilities
- **Display Orientation**: Adjustable screen orientation (Portrait, Landscape, Inverted)
- **Text Formatting**: Configurable line widths for optimal text display
- **MicroSD Storage**: Automatic saving to SD card in `/notes/` directory
- **E-Paper Optimized**: Efficient display updates minimizing refresh flicker
- **Low Memory Footprint**: Designed to run efficiently on ESP32-C3's limited RAM

## Hardware Requirements

- Xteink X4 e-paper device
- MicroSD card (FAT32 formatted)
- External Bluetooth keyboard (physical or smartphone app)

## BLE HID Host Functionality

MicroSlate works by acting as a BLE HID Host, connecting to your external Bluetooth keyboard as a client. When powered on, it will:

1. Scan for nearby Bluetooth HID devices (keyboards)
2. Automatically connect to the first compatible keyboard found
3. Receive key press events and route them to the appropriate application mode
4. Provide a responsive typing experience with full keyboard support

No pairing from the keyboard side is required - the device will automatically attempt to connect to compatible keyboards when in range.

## Building and Installation

### Prerequisites

1. Install [PlatformIO](https://platformio.org/install/)
2. Clone or download this repository

### Build Instructions

```bash
cd xteink-writer-firmware
pio run
```

### Upload Instructions

```bash
# Build and upload in one command
pio run --target upload

# Or just upload if already built
pio run --target upload --upload-port <PORT>
```

## Usage Instructions

### Initial Setup

1. Power on your Xteink X4 with MicroSlate firmware installed
2. Pair your Bluetooth keyboard with the device (it will appear as "MicroSlate")
3. Once paired, you can start typing immediately

### Main Menu

MicroSlate presents a simple menu system:
- **Browse Files**: View and select existing notes
- **New Note**: Create a new text file
- **Settings**: Access configuration options (future enhancement)

Navigate using arrow keys and press Enter to select.

### File Browser Mode

- Shows all `.txt` files in the `/notes/` directory
- Displays both the file title and filename
- Use arrow keys to navigate
- Press Enter to open a file for editing
- Press F2 or Ctrl+R to rename a selected file
- Press Ctrl+N to create a new file

### Text Editor Mode

- Full keyboard support for text entry and editing
- Use arrow keys to move cursor
- Backspace/Delete to remove characters
- Press Ctrl+S to save the current file
- Press Ctrl+Q to return to file browser

### File Renaming Mode

- Access by pressing F2 or Ctrl+R in file browser
- Type the new filename using your connected keyboard
- Press Enter to confirm or Esc to cancel

### Supported Key Mappings

- Arrow keys: Menu navigation/cursor movement
- Enter: Select/open files
- Ctrl+S: Save file
- Ctrl+N: New file
- Ctrl+Q: Return to file browser
- Ctrl+R: Rename file (in file browser)
- F2: Rename file (in file browser)
- Escape: Cancel operation
- Settings Menu Navigation:
  - Arrow keys: Navigation between settings
  - Left/Right: Adjust orientation and line width settings
  - Enter: Select options (Back option)

## Memory and Performance

- Total application RAM: Under 200KB
- Text buffer: 16KB capacity for larger documents
- BLE keyboard stack: Optimized for minimal overhead
- Display updates: Partial refresh for cursor movement

## File Structure

- Files are saved in the `/notes/` directory on the SD card
- Files named with timestamps by default
- Compatible with other text editors and file managers
- Title information extracted from file content

## Troubleshooting

### BLE Keyboard Pairing Issues

- Reset the device if the keyboard stops responding
- Try pairing again with the device powered off/on
- Some keyboards may require entering pairing mode before connecting

### Display Issues

- E-paper displays are slow by design - full refresh takes several seconds
- Partial updates are used where possible for faster response
- Wait for refresh to complete before expecting screen changes

## Development

This firmware is organized in a minimal fashion:
- `src/main.cpp`: Main application logic
- `platformio.ini`: Build configuration
- `partitions.csv`: ESP32 flash partition map

## Limitations

- No EPUB or PDF reading capabilities (focus is on text editing)
- Limited to text files only (no rich formatting)
- BLE keyboard only (no on-screen keyboard)

## Future Enhancements

- Advanced settings and customization
- More sophisticated file organization
- Enhanced text formatting
- Backup/sync features