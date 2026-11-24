# /// script
# requires-python = ">=3.9"
# dependencies = ["fonttools>=4.40"]
# ///
# Copyright 2026 Ian Karlsson
"""quickdom's Japanese fallback, after Noto Sans and DejaVu Sans: Noto Sans CJK JP Regular's outlines (CFF, which
stb_truetype reads), cut to the characters Shift JIS encodes as Windows code page 932 has it: JIS X 0208's kana,
kanji and symbols, the half-width katakana, NEC's special characters and the IBM extension kanji, about 7,400.  Its
private-use characters (U+E000-F8FF) are left out.  Bold and the other faces are left out too: bold text is drawn
in the regular weight.

    uv run fonts/cjk.py NotoSansCJK-Regular.ttc

The collection's Japanese face is found by its name; the font (SIL OFL 1.1) is written beside this script.
"""
import os
import sys

from fontTools import subset
from fontTools.ttLib import TTCollection

FAMILY = 'Noto Sans CJK JP'
OUTPUT = 'NotoSansCJKjp-Regular.otf'


def code_page_932():
    chars = set()
    leads = list(range(0x81, 0xa0)) + list(range(0xe0, 0xfd))
    trails = list(range(0x40, 0x7f)) + list(range(0x80, 0xfd))
    for b in range(0xa1, 0xe0):
        chars.add(ord(bytes([b]).decode('cp932')))
    for lead in leads:
        for trail in trails:
            try:
                chars.add(ord(bytes([lead, trail]).decode('cp932')))
            except UnicodeDecodeError:
                pass
    return sorted(c for c in chars if not 0xe000 <= c <= 0xf8ff)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    collection = TTCollection(sys.argv[1])
    font = next((f for f in collection.fonts if f['name'].getDebugName(1) == FAMILY), None)
    if font is None:
        sys.exit(f'{sys.argv[1]}: no {FAMILY}')
    options = subset.Options()
    options.layout_features = ['kern']
    options.hinting = False
    options.name_IDs = ['*']
    options.notdef_outline = True
    subsetter = subset.Subsetter(options)
    subsetter.populate(unicodes=code_page_932())
    subsetter.subset(font)
    font.save(os.path.join(os.path.dirname(os.path.abspath(__file__)), OUTPUT))


if __name__ == '__main__':
    main()
