#!/usr/bin/env python3
"""Build the built-in English spellcheck word list (data/spell/en_words.dat) from SCOWL.

Source: SCOWL 2020.12.07, http://wordlist.aspell.net/ (the release's final/ lists; plain data, nothing of SCOWL's
is run). Size 60 is SCOWL's standard spellchecker size. Categories: words, upper, contractions, proper names,
abbreviations. Sections:
  common  english-*       (US and UK alike)
  us      american-*
  uk      british-* and british_z-*  (both -ise and -ize, so "organise" and "organize" pass)
The lists are ISO-8859-1; the output is UTF-8.

Output format (read by src/spell/spell_dict.h):
  8 bytes   magic "MSWL2\\0\\0\\0"
  6 x u32   offset and length of the common, us and uk sections (little-endian, from the start of the file)
  sections  words sorted by their UTF-8 bytes, each followed by a rank byte and '\\n'
The rank says how common the word is, from the smallest SCOWL size that lists it (RANKS): 1 (size 10) to 5 (55-60).
Suggestions use it to put "the" before "thew".

Usage: make_wordlist.py <scowl-final-dir> <out.dat>
"""
import os
import struct
import sys

SIZE = 60
CATEGORIES = ["words", "upper", "contractions", "proper-names", "abbreviations"]
SECTIONS = [("common", ["english"]), ("us", ["american"]), ("uk", ["british", "british_z"])]
RANKS = [(10, 1), (20, 2), (35, 3), (50, 4), (60, 5)]   # (largest SCOWL size, rank)


def rank_of(level):
    return next(r for top, r in RANKS if level <= top)


def read_lists(final_dir, spellings):
    words = {}   # word -> rank
    for name in sorted(os.listdir(final_dir)):
        stem, _, level = name.rpartition(".")
        if not level.isdigit() or int(level) > SIZE:
            continue
        spelling, _, category = stem.partition("-")
        if spelling not in spellings or category not in CATEGORIES:
            continue
        with open(os.path.join(final_dir, name), "rb") as f:
            for raw in f.read().decode("iso-8859-1").splitlines():
                w = raw.strip()
                if w and " " not in w:
                    words[w] = min(words.get(w, 9), rank_of(int(level)))
    return words


def main():
    final_dir, out = sys.argv[1], sys.argv[2]
    sections = [(name, read_lists(final_dir, sp)) for name, sp in SECTIONS]
    common = sections[0][1]
    # A word in both regional lists belongs to neither only: keep regional sections free of common words.
    blobs = []
    for name, words in sections:
        if name != "common":
            words = {w: r for w, r in words.items() if w not in common}
        data = b"".join(w.encode("utf-8") + bytes([words[w]]) + b"\n"
                        for w in sorted(words, key=lambda w: w.encode("utf-8")))
        blobs.append((name, len(words), data))
    header = 8 + 6 * 4
    offsets = []
    pos = header
    for _, _, data in blobs:
        offsets += [pos, len(data)]
        pos += len(data)
    with open(out, "wb") as f:
        f.write(b"MSWL2\0\0\0")
        f.write(struct.pack("<6I", *offsets))
        for _, _, data in blobs:
            f.write(data)
    for name, n, data in blobs:
        print(f"{name}: {n} words, {len(data)} bytes")
    print(f"total {pos} bytes -> {out}")


if __name__ == "__main__":
    main()
