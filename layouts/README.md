# MicroSlate keyboard layouts

MicroSlate has US QWERTY built in. Every other keyboard layout is a small text file that you
copy to the SD card. This folder holds a ready-made file for most languages, and this page
explains how to change one or write your own.

## Install a layout

1. Pick the file for your keyboard from the list below and download it.
2. Put the SD card in a computer and copy the file into the folder `microslate/layouts/`
   (create the `layouts` folder if it is not there).
3. Put the card back. In **Settings > Keyboard layout**, step to your layout. In the editor,
   **Ctrl+K** or **Win+Space** switches between US and your layouts. A short tag such as `DE` at
   the top shows which one is active.

Copy only the layouts you use: every file on the card is in the switching order.

| File | Layout | File | Layout |
|---|---|---|---|
| `us-intl.txt` | US International (accent keys) | `se.txt` | Swedish |
| `uk.txt` | English (UK) | `fi.txt` | Finnish |
| `de.txt` | German (QWERTZ) | `no.txt` | Norwegian |
| `ch.txt` | Swiss (QWERTZ) | `dk.txt` | Danish |
| `fr.txt` | French (AZERTY) | `is.txt` | Icelandic |
| `be.txt` | Belgian (AZERTY) | `pl.txt` | Polish (programmers) |
| `ca-fr.txt` | Canadian French | `cz.txt` | Czech (QWERTZ) |
| `es.txt` | Spanish | `sk.txt` | Slovak (QWERTZ) |
| `latam.txt` | Latin American Spanish | `hu.txt` | Hungarian (QWERTZ) |
| `it.txt` | Italian | `hr.txt` | Croatian |
| `pt.txt` | Portuguese | `si.txt` | Slovenian |
| `br.txt` | Brazilian (ABNT2) | `tr.txt` | Turkish (Q) |
| `nl.txt` | Dutch | `ru.txt` | Russian |
| `dvorak.txt` | Dvorak (US) | `ua.txt` | Ukrainian |
| `colemak.txt` | Colemak (US) | `by.txt` | Belarusian |
| `ro.txt` | Romanian (standard, ș ț) | `bg.txt` | Bulgarian |

The files are made from the Linux keyboard definitions (xkeyboard-config) by
`tools/xkb2layout.py`, so they type what the same layout types on a Linux PC. Characters the
MicroSlate fonts cannot show (Greek, arrows, some symbols) are left out. Languages whose letters
the fonts do not have yet (Greek, Hebrew, Arabic, Asian scripts) cannot be typed yet.

## Change a key on the MicroSlate

**Settings > Edit keyboard layout**, then press the key you want to change. You see what it types
on its own and with **Shift** (and with **AltGr**, the right Alt key, on layouts that use it).
- **Change**: pick a row with Up/Down, type what it should type, then **Save**.
- **Reset**: the key types what the layout says again.
- **Reset all** (on the first screen, press it twice): undoes every change to this layout.

Pressing another key switches to that key. A key you changed is marked "changed by you".

Your changes never touch the layout file. They are kept in their own small file,
`microslate/layouts/changes/<layout file>` (`built-in-us.txt` for US), so a newer copy of a layout
file keeps your fixes, and deleting the changes file undoes them all.

To enter a character your keyboard cannot type, type its code: `\u0142` is ł. Codes are listed
at [unicode.org/charts](https://www.unicode.org/charts/) or by searching the character's name.

## The file format

A layout file is plain UTF-8 text, one line per key. Lines starting with `#` are comments.

```
name = Deutsch (QWERTZ)
short = DE
base = us

Y = z Z
Q = q Q @
EQUAL = dead:´ dead:`
SEMICOLON = ö Ö
```

**Settings at the top**

| Line | Meaning |
|---|---|
| `name = ...` | The name shown in Settings. |
| `short = DE` | The tag shown while typing (up to 7 letters). |
| `base = us` | Start from US and list only the keys that differ (the default). `base = none` starts empty. Must come before the keys. |
| `altgr = any-alt` | Either Alt key works as AltGr, like Option on a Mac keyboard. The default `right-alt` uses only the right one (Ctrl+Alt also works). |
| `latin = yes` / `no` | Whether Ctrl shortcuts follow the letters of this layout (worked out by itself; rarely needed). |

**Key lines**

`KEY = normal shift altgr shift+altgr`, with as many values as you need:

- **KEY** is where the key sits on a US keyboard: a letter (`Q`), a digit (`1`), or one of
  `GRAVE MINUS EQUAL LBRACKET RBRACKET BACKSLASH HASH SEMICOLON APOSTROPHE COMMA DOT SLASH`,
  `ISO_BACKSLASH` (the extra key next to left Shift on European keyboards),
  `INTL1` (the extra key next to right Shift on Brazilian keyboards), `INTL3`,
  keypad keys `KP_0`..`KP_9 KP_DOT KP_SLASH KP_STAR KP_MINUS KP_PLUS KP_COMMA KP_EQUAL`, or a
  HID code such as `0x2D`.
- A value is the character (or a few characters) the key types. `none` leaves that one empty.
- `dead:´` makes it an accent key: it types nothing until the next key, then the accented letter
  (´ then e = é). Accent then Space types the accent itself. The usual accents
  (´ ` ^ ~ ¨ ° ¸ ˇ ˘ ˛ ˝ ¯ ˙) combine with every letter that has an accented form.
- Escapes: `\s` is a space, `\\` a backslash, `\u00E9` (é) a character by its code (exactly four hex
  digits), `\u{1F600}` a longer code.
- Caps Lock acts like Shift on letters by itself. Add `+caps` or `-caps` at the end of a line to
  change that for one key.

**Extra accent pairs**

`compose ´ = c ć C Ć` adds pairs to an accent key: ´ then c types ć. Use it for pairs the
built-in table does not have.

## When something is wrong

A line with a mistake is skipped and the rest of the layout still works. Settings shows
"(see problems)" next to the layout, and `microslate/layout-problems.txt` on the card lists each
skipped line with its line number. Characters the fonts cannot show are listed there too. They
are kept, but they appear as a blank or a box when typed.

**Ctrl shortcuts** (Ctrl+S save and the others) follow the letter on the key in Latin layouts, so
on German the key labelled Z is Ctrl+Z. In Cyrillic layouts they stay where they are on a US
keyboard.
