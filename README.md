# MicroSlate

Distraction-free writing firmware for e-paper devices. Pair a **Bluetooth LE keyboard**, write, and your notes are
saved to the SD card as plain text files, with optional sync to OneDrive, Dropbox or Google Drive.

MicroSlate is a [partner firmware](https://www.seeedstudio.com/sticky/playground/firmware/) on Seeed Studio's
reTerminal Sticky Firmware Playground, where Sticky owners can also flash it.

## Which firmware for my device

| Device | Firmware |
|---|---|
| Seeed reTerminal Sticky | **v3**, this branch |
| Seeed reTerminal E1001 | **v3**, this branch |
| Xteink X4 Pro | v3, this branch. *Experimental*: built on the FreeInk SDK's X4 Pro support, not yet tested on hardware by us. Feedback welcome. |
| Xteink X4 Classic ("X4 v2") | v3, this branch. *Experimental*: built on the FreeInk SDK's X4 Classic support, not yet tested on hardware by us. Feedback welcome. |
| Xteink X4 (original) | **v2.1** (see below). A simplified v3 for it is coming. |
| Xteink X3 | Not supported yet. The simplified v3 for the X4 will also cover the X3. |

> **Original Xteink X4? Use MicroSlate v2.1.** It is still available and supported:
> - Install it from your browser at [typeslate.com/tools/microslate](https://typeslate.com/tools/microslate/#x4)
>   (MicroSlate on its own, or the dual-boot edition with the CrossPoint e-reader).
> - Release files: [v2.1.0](https://github.com/Josh-writes/microslate-firmware/releases/tag/v2.1.0).
> - Source and its own README: the [`v2.1` branch](https://github.com/Josh-writes/microslate-firmware/tree/v2.1).
>
> The v3 files on this page are for the Sticky, E1001, X4 Pro and X4 Classic only. They do not run on the original X4.

**Keyboards:** only Bluetooth LE keyboards work. None of these devices has a Classic Bluetooth radio, so a
Classic-only keyboard can never connect, whatever the firmware does. "Bluetooth 5.0" on the box is not enough: look for
BLE, Bluetooth Low Energy, or "works with iPad". Tested on v3 with the Logitech Keys-To-Go 2 and MX Keys Mini.

## Features

- **Editor**: word wrap, selection with Shift+arrows, copy/cut/paste, find, undo and redo, Tab indents, word count.
  Notes up to 512 KB (about 75,000 words).
- **Writing modes**: Normal (scrolling), Typewriter (only the current line, centred) and Pagination (page flips instead
  of scrolling), plus Focus mode, which hides everything but your text.
- **Autosave**: after 10 seconds without typing, every minute while you keep typing, and on every way out of a note.
  A note is never left half-saved: after a power cut it is either the old or the new version.
- **Cloud sync**: OneDrive, Dropbox or Google Drive. Sign in by scanning a QR code with your phone. Notes sync in the
  background; when a note changed on both sides, both copies are kept.
- **Transfer**: get notes onto a phone or computer with no account and no app. The device joins your Wi-Fi (or opens
  its own hotspot) and shows a QR code for a page where you download or copy notes.
- **Spellcheck and autocorrect** (US or UK English, both off by default): a light zigzag under misspelled words,
  suggestions with Ctrl+J, your own dictionary on the card. Autocorrect fixes common typos (teh, dont, i) and leaves
  names and your own words alone.
- **Keyboard layouts**: US built in, 32 more as small files on the SD card (see [layouts/](layouts/README.md)), and a
  layout editor on the device.
- **Keyboards**: up to 4 remembered; the device reconnects to whichever one is switched on. Keyboards are read from
  their own report descriptor, so most work without a device list. Held keys repeat.
- **E-reader next to MicroSlate** (Sticky, X4 Pro, X4 Classic): install CrossPoint or CrossInk from Settings and switch
  between writing and reading from the main menu.
- **Sleep screen**: the built-in screen, or your own pictures sent from a phone.
- **Touch** on the Sticky and X4 Pro: lists, buttons, and tapping into the text.
- **Files**: `.txt` or `.md` notes in `/notes/` on the card, readable by any text editor. Notes written on Windows keep
  their line endings.

## Install

These steps are for the Sticky, E1001, X4 Pro and X4 Classic. For the original Xteink X4, see v2.1 above.

The easiest way is the browser installer at [typeslate.com/tools/microslate](https://typeslate.com/tools/microslate/#install)
(Chrome or Edge): pick your device and click Install. Settings, keyboards and sign-ins are kept.

To flash by hand instead: back up your device first. Flash **the app only, at 0x10000**, so settings, keyboards and cloud sign-ins survive, and
erase `otadata` so the device starts the app you flashed (after an update over Wi-Fi it may be running from its second
app slot):

```bash
esptool.py --chip esp32s3 --port <PORT> erase_region 0xE000 0x2000
esptool.py --chip esp32s3 --port <PORT> --baud 921600 write_flash 0x10000 microslate-<device>-<version>.bin
```

The release files are on the [Releases](https://github.com/Josh-writes/microslate-firmware/releases) page:
`microslate-sticky-*.bin`, `microslate-e1001-*.bin`, `microslate-x4pro-*.bin` and `microslate-x4c-*.bin`.

Updates after that: **Settings > Wi-Fi > Firmware update** checks the latest release and installs it over Wi-Fi.

### First steps

1. Insert a FAT32 microSD card and turn the device on.
2. **Settings > Bluetooth**: put the keyboard in pairing mode and pick it from the list. If a number appears, type it
   on the keyboard and press Enter.
3. **Notes** on the main menu: open a note or create one.
4. Optional: **Sync** on the main menu to connect a cloud account.

## Using MicroSlate

The main menu has **Notes**, **Settings**, **Transfer** and **Sync**. Every screen has a button bar at the bottom.

**Device buttons**
- **Sticky**: tap lists, buttons and text; its button selects. A keyboard does everything else.
- **E1001**: Up and Down move, the green key selects, Up and Down together go back.
- **X4 Pro**: the Home key below the screen is Back; Left and Right move; Power opens the menu.
- **X4 Classic**: the side keys and the front Left and Right move, Confirm selects, Back goes back. The buttons follow
  the screen when you turn it.
- Hold **Power** for 3 seconds to sleep. On devices with a Back button, hold it for 5 seconds to restart.

### Editor shortcuts

Ctrl+M opens the editor menu (writing mode, font size, word count, focus mode, keyboard layout, spellcheck). Its
Shortcuts page lists these keys too.

| Keys | Action |
|---|---|
| Ctrl+M | Editor menu |
| Ctrl+S | Save now (autosave makes this optional) |
| Ctrl+Z / Ctrl+Y (or Ctrl+Shift+Z) | Undo / redo |
| Ctrl+A | Select all |
| Ctrl+C, Ctrl+X, Ctrl+V | Copy, cut, paste |
| Shift+arrows, Shift+Home/End | Select text |
| Ctrl+F | Find (Enter: next match, Esc: close) |
| Ctrl+N | Rename the note |
| Ctrl+T / Ctrl+P | Typewriter / Pagination mode |
| Ctrl+H | Focus mode |
| Ctrl+W | Word count |
| Ctrl+K | Next keyboard layout |
| Ctrl+J | Spelling suggestions |
| Ctrl+B | Frontlight (X4 Pro) |
| Tab | Indent |
| Esc | Transfer, Exit or Back to the note (Esc twice leaves at once) |

Undo goes back a word of typing at a time; a paste, a cut or typing over a selection is one step. The history survives
autosave and is cleared when another note opens.

### Settings

| Tab | Rows |
|---|---|
| Editor | Writing mode, Font size, New notes (.txt or .md), Spellcheck, Autocorrect, My words |
| Wi-Fi | Saved networks, Add a network |
| Bluetooth | Bluetooth (pair a keyboard), Paired keyboards, BLE power |
| Keyboard | Keyboard layout, Edit keyboard layout |
| Display | Orientation, Dark mode, Sleep screen, Frontlight (X4 Pro), Refresh (E1001: Clear or Fast) |

**BLE power** sets the radio's transmit power. Leave it at the lowest step (best battery life) unless a keyboard drops
out at a distance.

### Cloud sync

**Sync** on the main menu connects to Wi-Fi and opens the cloud screen. Choose a cloud, scan the QR code with your
phone and approve MicroSlate; the device picks up the sign-in by itself (or type the code shown). After that, notes
sync in the background: when you pause writing, every few minutes while you keep writing, and when you open the notes
list. Sign-ins are stored on the device only, never on the SD card.

### Transfer

**Transfer** on the main menu (or on a note) starts a small page on the device. Scan the QR code with a phone, or open
`microslate.local/<code>` on a computer on the same network (the code stays the same, so it can be bookmarked). On the
page, tick notes and download them (one `.txt`, or a `.zip` for several), or copy a note's text. With no saved Wi-Fi
in range, the device opens its own hotspot: join it with the first QR code, then open the page with the second.

### E-reader (dual boot)

On the Sticky, X4 Pro and X4 Classic, **Settings > Wi-Fi > Reader** installs an e-reader next to MicroSlate:
[CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader) or [CrossInk](https://github.com/uxjulia/CrossInk).
It goes into the device's second app slot, so notes, settings and sign-ins stay as they are. The reader then appears on
MicroSlate's main menu, and its home screen has MicroSlate to come back. MicroSlate keeps a copy of the reader on the SD
card: swapping back, or the reader returning after a MicroSlate update, needs no download. **Update** on the installed
reader's card fetches a newer release; Remove asks whether to keep the copy on the card.

The readers come from MicroSlate's forks, changed only for dual boot:
[crosspoint-microslate](https://github.com/Josh-writes/crosspoint-microslate) and
[crossink-microslate](https://github.com/Josh-writes/crossink-microslate). Reader issues belong with the original projects.

### Files on the SD card

| Path | What it holds |
|---|---|
| `/notes/` | Your notes, `.txt` or `.md`. Text files copied here from a computer appear on the device. |
| `/microslate/layouts/` | Keyboard layout files |
| `/microslate/dictionary.txt` | Your spellcheck words, one per line |
| `/microslate/readers/` | The e-reader's saved copy (dual boot) |
| `/microslate/` | Settings backups (Wi-Fi network names only; passwords stay on the device) |

Take the SD card out only when the device is off, or on a menu with no save or sync running. If the card is pulled
while you write, the note stays in memory and is saved when the card is back.

## Building from source

MicroSlate builds with [PlatformIO](https://platformio.org/) on the
[pioarduino](https://github.com/pioarduino/platform-espressif32) ESP32 platform (pinned in `platformio.ini`), with the
[FreeInk SDK](https://github.com/Free-Ink/freeink-sdk) as a submodule, so clone with `--recursive`:

```bash
git clone --recursive https://github.com/Josh-writes/microslate-firmware
cd microslate-firmware
pio run -e sticky_release        # firmware: .pio/build/sticky_release/firmware.bin
```

| Environment | Device |
|---|---|
| `sticky_release` / `sticky` | Seeed reTerminal Sticky (`sticky` keeps debug output on serial) |
| `e1001_release` / `e1001` | Seeed reTerminal E1001 (`e1001` keeps debug output on serial) |
| `xteink_x4pro` | Xteink X4 Pro (experimental) |
| `xteink_x4c` | Xteink X4 Classic (experimental) |
| `xteink_x4` | Xteink X4 (builds; v3 is not yet tested on it) |
| `sticky_bledump` | Test devices only: prints raw Bluetooth reports, which include everything typed. Never for daily use. |

Google Drive needs a device client ID and secret in `src/sync/secrets.local.h` (git-ignored; see
`scripts/make_google_secrets.py`). Without it the build has no Google Drive.

Host tests (plain C++, no device) are in `test/`: one folder per area, each with a script that builds and runs its
tests with `g++`.

### Source layout

```
src/
  main.cpp            setup, main loop, device buttons, sleep
  text_editor.*       text buffer, cursor, selection; edit_history.* undo/redo; editor_layout.* line wrapping
  file_manager.*      notes on the SD card; sd_safe.* safe writes; note_format.* line endings and BOM
  input_handler.*     keyboard input for every screen; touch_input.* and touch_targets.* touch
  ui_renderer.*       drawing every screen
  ble_keyboard.*      Bluetooth LE keyboard host (NimBLE): scan, pairing, reconnect, HID reports
  keyboard_layout.*   layout files and the layout editor
  wifi_sync.*         Wi-Fi networks and the Sync screen
  sync/               cloud sync: engine, OneDrive, Dropbox, Google Drive, device storage
  transfer/           the Transfer page and hotspot
  spell/              spellcheck and autocorrect
  sleep/              sleep screen and pictures
lib/                  display, fonts, HID and keymap libraries
freeink-sdk/          FreeInk SDK (submodule): board profiles and panel drivers, see docs/freeink-sdk.md
layouts/              keyboard layout files
test/                 host tests
scripts/              build helpers, word list and sign-in relay tools
```

## Credits

MicroSlate includes work from the community. Thank you to:

- **[m-danya](https://github.com/m-danya)**: the first selectable keyboard layouts.
  [m-danya/microslate-firmware](https://github.com/m-danya/microslate-firmware)
- **[nonexistent9](https://github.com/nonexistent9)**: stability work (task watchdog, Bluetooth reconnect hardening)
  and the fork-friendly CI build. [nonexistent9/microslate-firmware](https://github.com/nonexistent9/microslate-firmware)
- **[lucidstack](https://github.com/lucidstack)**: the fix for the reboot when pairing a keyboard (SD and display
  sharing the SPI bus). [lucidstack/microslate-firmware](https://github.com/lucidstack/microslate-firmware)
- **[Leticia-NdO](https://github.com/Leticia-NdO)**: text selection and clipboard, and the LE Secure Connections and
  bond-race fixes for pairing, from the Ardosia firmware.
  [Leticia-NdO/ardosia-firmware](https://github.com/Leticia-NdO/ardosia-firmware)

Built on the [FreeInk SDK](https://github.com/Free-Ink/freeink-sdk), with libraries from
[CrossPoint](https://github.com/crosspoint-reader/crosspoint-reader), [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
and the [QR Code generator library](https://www.nayuki.io/page/qr-code-generator-library). The spellcheck word list
comes from [SCOWL](http://wordlist.aspell.net/) (licence: `data/spell/SCOWL-Copyright.txt`).

## More from TypeSlate

MicroSlate is the hardware companion to **TypeSlate**, a free full-screen writing app for Windows:
[Microsoft Store](https://apps.microsoft.com/detail/9PM3J9SQB0TV?hl=en-us&gl=US&ocid=pdpshare) ·
[typeslate.com](https://typeslate.com).
If MicroSlate is useful to you, you can support the project at [ko-fi.com/typeslate](https://ko-fi.com/typeslate).

MIT License.
