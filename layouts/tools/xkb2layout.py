"""Generates MicroSlate layout files (layouts/*.txt) from the Linux keyboard definitions
(xkeyboard-config symbols + xorgproto keysymdef.h).

  python -I layouts/tools/xkb2layout.py <cache-dir> <out-dir>

<cache-dir> holds the downloaded definitions (fetched once from gitlab.freedesktop.org into
<cache-dir>/symbols and <cache-dir>/keysymdef.h). Only text is parsed; nothing downloaded
is run. Characters the built-in fonts cannot show are left out of the files.

What it reads from xkb: the four levels of group 1 of each key (normal, Shift, AltGr,
Shift+AltGr), following `include` lines; dead_* keysyms become dead keys. What it ignores:
other groups, key types, modifier maps, keys outside the main block (F-keys, arrows).
"""
import os
import re
import sys
import urllib.request

RAW = "https://gitlab.freedesktop.org/xkeyboard-config/xkeyboard-config/-/raw/master/symbols/"
KEYSYMDEF = "https://gitlab.freedesktop.org/xorg/proto/xorgproto/-/raw/master/include/X11/keysymdef.h"

# (output file, xkb file, xkb section or None for the default, name, short)
LAYOUTS = [
    ("us-intl.txt", "us", "intl", "US International (accent keys)", "USI"),
    ("uk.txt", "gb", None, "English (UK)", "UK"),
    ("de.txt", "de", None, "Deutsch (QWERTZ)", "DE"),
    ("ch.txt", "ch", None, "Schweiz (QWERTZ)", "CH"),
    ("fr.txt", "fr", None, "Français (AZERTY)", "FR"),
    ("be.txt", "be", None, "Belgique (AZERTY)", "BE"),
    ("ca-fr.txt", "ca", None, "Français (Canada)", "CA"),
    ("es.txt", "es", None, "Español", "ES"),
    ("latam.txt", "latam", None, "Español (Latinoamérica)", "LA"),
    ("it.txt", "it", None, "Italiano", "IT"),
    ("pt.txt", "pt", None, "Português", "PT"),
    ("br.txt", "br", None, "Português (Brasil, ABNT2)", "BR"),
    ("nl.txt", "nl", None, "Nederlands", "NL"),
    ("se.txt", "se", None, "Svenska", "SE"),
    ("fi.txt", "fi", None, "Suomi", "FI"),
    ("no.txt", "no", None, "Norsk", "NO"),
    ("dk.txt", "dk", None, "Dansk", "DK"),
    ("is.txt", "is", None, "Íslenska", "IS"),
    ("pl.txt", "pl", None, "Polski (programisty)", "PL"),
    ("cz.txt", "cz", None, "Čeština (QWERTZ)", "CZ"),
    ("sk.txt", "sk", None, "Slovenčina (QWERTZ)", "SK"),
    ("hu.txt", "hu", None, "Magyar (QWERTZ)", "HU"),
    ("hr.txt", "hr", None, "Hrvatski", "HR"),
    ("si.txt", "si", None, "Slovenščina", "SI"),
    ("tr.txt", "tr", None, "Türkçe (Q)", "TR"),
    ("ro.txt", "ro", "std", "Română (standard)", "RO"),
    ("ru.txt", "ru", None, "Русский (ЙЦУКЕН)", "RU"),
    ("ua.txt", "ua", None, "Українська", "UA"),
    ("by.txt", "by", None, "Беларуская", "BY"),
    ("bg.txt", "bg", None, "Български", "BG"),
    ("dvorak.txt", "us", "dvorak", "Dvorak (US)", "DV"),
    ("colemak.txt", "us", "colemak", "Colemak (US)", "CM"),
]

# xkb key name -> (MicroSlate key name, HID usage)
KEYS = {
    "TLDE": "GRAVE", "AE01": "1", "AE02": "2", "AE03": "3", "AE04": "4", "AE05": "5", "AE06": "6",
    "AE07": "7", "AE08": "8", "AE09": "9", "AE10": "0", "AE11": "MINUS", "AE12": "EQUAL", "AE13": "INTL3",
    "AD01": "Q", "AD02": "W", "AD03": "E", "AD04": "R", "AD05": "T", "AD06": "Y", "AD07": "U",
    "AD08": "I", "AD09": "O", "AD10": "P", "AD11": "LBRACKET", "AD12": "RBRACKET",
    "AC01": "A", "AC02": "S", "AC03": "D", "AC04": "F", "AC05": "G", "AC06": "H", "AC07": "J",
    "AC08": "K", "AC09": "L", "AC10": "SEMICOLON", "AC11": "APOSTROPHE", "BKSL": "BACKSLASH",
    "LSGT": "ISO_BACKSLASH", "AB01": "Z", "AB02": "X", "AB03": "C", "AB04": "V", "AB05": "B",
    "AB06": "N", "AB07": "M", "AB08": "COMMA", "AB09": "DOT", "AB10": "SLASH", "AB11": "INTL1",
    "KPDL": "KP_DOT",
}
ORDER = list(KEYS)

# Dead keysyms the MicroSlate keymap understands, as the accent a dead key shows.
DEAD = {
    "dead_grave": "`", "dead_acute": "´", "dead_circumflex": "^", "dead_tilde": "~",
    "dead_macron": "¯", "dead_breve": "˘", "dead_abovedot": "˙", "dead_diaeresis": "¨",
    "dead_abovering": "°", "dead_doubleacute": "˝", "dead_caron": "ˇ",
    "dead_cedilla": "¸", "dead_ogonek": "˛",
}

# Characters the built-in fonts can show (lib/EpdFont/builtinFonts/notosans_14_regular.h).
FONT = [(0x20, 0x7E), (0xA0, 0x17F), (0x218, 0x21B), (0x300, 0x36F), (0x400, 0x4FF), (0x2000, 0x2064), (0x2066, 0x2071),
        (0x2074, 0x208E), (0x2090, 0x209C), (0x20A0, 0x20C0), (0x2212, 0x2212)]


def shown(ch):
    return all(any(a <= ord(c) <= b for a, b in FONT) for c in ch)


def fetch(url, path):
    if not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with urllib.request.urlopen(url, timeout=60) as r:
            data = r.read()
        with open(path, "wb") as f:
            f.write(data)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def load_keysyms(cache):
    text = fetch(KEYSYMDEF, os.path.join(cache, "keysymdef.h"))
    syms = {}
    for m in re.finditer(r"#define XK_(\w+)\s+0x([0-9a-fA-F]+)\s*(?:/\*\(?\s*U\+([0-9A-Fa-f]{4,6}))?", text):
        name, val, uni = m.group(1), int(m.group(2), 16), m.group(3)
        if uni:
            syms[name] = chr(int(uni, 16))
        elif 0x20 <= val <= 0x7E or 0xA0 <= val <= 0xFF:
            syms[name] = chr(val)
    return syms


def keysym_char(sym, syms):
    """(text, dead) for an xkb keysym, None for nothing, False for keep-what-was-there."""
    if sym in ("NoSymbol",):
        return False
    if sym == "VoidSymbol":
        return None
    if sym in DEAD:
        return (DEAD[sym], True)
    if sym.startswith("dead_"):
        return None                                     # an accent the keymap has no pair table for
    m = re.fullmatch(r"U([0-9A-Fa-f]{4,6})", sym)
    if m:
        return (chr(int(m.group(1), 16)), False)
    m = re.fullmatch(r"0x0*1([0-9A-Fa-f]{6})", sym)
    if m:
        return (chr(int(m.group(1), 16)), False)
    if len(sym) == 1:
        return (sym, False)
    if sym in syms:
        return (syms[sym], False)
    return None                                         # function keysyms (BackSpace, ISO_Level3_Shift ...)


class Symbols:
    def __init__(self, cache):
        self.cache = cache
        self.files = {}

    def sections(self, name):
        if name not in self.files:
            text = fetch(RAW + name, os.path.join(self.cache, "symbols", name))
            text = re.sub(r"//[^\n]*", "", text)
            text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
            secs, first, marked = {}, None, None
            for m in re.finditer(r"((?:\w+\s+)*)xkb_symbols\s+\"([^\"]+)\"\s*\{", text):
                flags, sec = m.group(1).split(), m.group(2)
                depth, i = 1, m.end()
                while depth and i < len(text):
                    depth += {"{": 1, "}": -1}.get(text[i], 0)
                    i += 1
                secs[sec] = text[m.end():i - 1]
                first = first or sec
                if "default" in flags and not marked:
                    marked = sec
            # xkb's default section: the one marked default, else the first one
            self.files[name] = (secs, marked or first)
        return self.files[name]

    def resolve(self, name, sec, keys, syms, depth=0):
        if depth > 12:
            return
        secs, default = self.sections(name)
        body = secs.get(sec or default)
        if body is None:
            return
        for st in re.finditer(r"(include\s+\"([^\"]+)\")|((?:replace\s+|override\s+|augment\s+)?key\s+<(\w+)>\s*\{(.*?)\}\s*;)",
                              body, flags=re.S):
            if st.group(1):
                for part in re.split(r"[+|]", st.group(2)):
                    m = re.fullmatch(r"\s*(\w[\w-]*)(?:\(([^)]*)\))?(?::\d)?\s*", part)
                    if m and m.group(1) != "pc":
                        self.resolve(m.group(1), m.group(2), keys, syms, depth + 1)
                continue
            kname, kbody = st.group(4), st.group(5)
            if kname not in KEYS:
                continue
            # Group 1's keysyms: "symbols[Group1] = [...]", else the first plain [...] list
            # (after dropping type[...] = "..." and actions[...] = [...] entries).
            plain = re.sub(r"type\s*\[[^\]]*\]\s*=\s*\"[^\"]*\"", "", kbody, flags=re.I)
            plain = re.sub(r"actions\s*\[[^\]]*\]\s*=\s*\[[^\]]*\]", "", plain, flags=re.I)
            plain = re.sub(r"symbols\s*\[\s*(?:Group)?[2-4]\s*\]\s*=\s*\[[^\]]*\]", "", plain, flags=re.I)
            m = re.search(r"symbols\s*\[\s*(?:Group)?1\s*\]\s*=\s*\[([^\]]*)\]", plain, flags=re.I) or \
                re.search(r"\[([^\]]*)\]", plain)
            if not m:
                continue
            levels = [s.strip() for s in m.group(1).split(",")]
            replace = st.group(3).lstrip().startswith("replace")
            old = [None] * 4 if replace else keys.get(kname, [None] * 4)
            new = list(old)
            for i, s in enumerate(levels[:4]):
                v = keysym_char(s, syms)
                if v is not False:
                    new[i] = v
            keys[kname] = new


def esc(text, dead):
    out = []
    for i, c in enumerate(text):
        if c == " ":
            out.append("\\s")
        elif c == "\\":
            out.append("\\\\")
        else:
            out.append(c)
    s = "".join(out)
    if not dead and (s.lower() == "none" or s.startswith("dead:") or s in ("+caps", "-caps")):
        s = "\\u%04X" % ord(text[0]) + s[1:]
    return ("dead:" if dead else "") + s


US_BASE = None


def us_levels(sym, syms):
    keys = {}
    sym.resolve("us", None, keys, syms)
    return keys


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    cache, out = sys.argv[1], sys.argv[2]
    syms = load_keysyms(cache)
    sym = Symbols(cache)
    us = us_levels(sym, syms)
    os.makedirs(out, exist_ok=True)
    for fname, xfile, xsec, name, short in LAYOUTS:
        keys = {}
        sym.resolve(xfile, xsec, keys, syms)
        dropped = 0
        lines = []
        for k in ORDER:
            lv = keys.get(k)
            if not lv:
                continue
            vals = []
            for v in lv:
                # An accent key stays even when the accent mark itself has no glyph (ˇ ˘ ˛):
                # what it makes (č ă ą) is in the font.
                if v and not v[1] and not shown(v[0]):
                    dropped += 1
                    v = None
                vals.append(v)
            if all(v is None for v in vals):
                continue
            # base = us: write only keys that differ from US
            usv = [v if v and shown(v[0]) else None for v in us.get(k, [None] * 4)]
            if vals == usv:
                continue
            while vals and vals[-1] is None:
                vals.pop()
            toks = ["none" if v is None else esc(v[0], v[1]) for v in vals]
            lines.append("%s = %s" % (KEYS[k], " ".join(toks)))
            if k == "BKSL":   # ISO keyboards send this key as HASH (0x32), ANSI ones as BACKSLASH
                lines.append("HASH = %s" % " ".join(toks))
        src = "%s(%s)" % (xfile, xsec) if xsec else xfile
        head = [
            "# MicroSlate keyboard layout: %s" % name,
            "# Generated from the Linux keyboard definitions (xkeyboard-config, symbols/%s, MIT licence)." % src,
            "# Copy it to /microslate/layouts/ on the SD card, then pick it in Settings > Keyboard layout.",
        ]
        if dropped:
            head.append("# %d character(s) the MicroSlate fonts cannot show were left out." % dropped)
        head += ["name = %s" % name, "short = %s" % short, "base = us", ""]
        with open(os.path.join(out, fname), "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(head + lines) + "\n")
        print("%-12s %3d keys, %d left out" % (fname, len(lines), dropped))


if __name__ == "__main__":
    main()
