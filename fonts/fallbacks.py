# /// script
# requires-python = ">=3.9"
# dependencies = ["fonttools>=4.40", "regex>=2023.0"]
# ///
# Copyright 2026 Ian Karlsson
"""quickdom's fallback fonts after Noto Sans and DejaVu Sans, cut from the fonts Firefox finds through fontconfig on
Linux for the characters neither has:

- NotoColorEmoji.ttf: Noto Color Emoji's colour bitmaps (its CBDT/CBLC strike, PNGs at 109 pixels an em) and its
  advances, for the characters shown as emoji by default (Unicode's Emoji_Presentation) below U+2C00, in the
  blocks the other subsets take, that Noto Sans lacks: Firefox's fallback takes such a character from the emoji font
  before DejaVu Sans (the ⚡ and ☕ DejaVu has too). stb_truetype reads only fonts with outlines,
  so the subset gets an empty glyf table beside its bitmaps; quickdom draws the bitmaps (src/text/fonts.cpp).
- NotoSansSymbols2-Regular.ttf: Noto Sans Symbols 2's outlines, drawn in the text's colour as Firefox draws them,
  for the symbols shown as text by default (not Emoji_Presentation) in Miscellaneous Symbols and Pictographs
  (U+1F300-1F5FF: wastebasket, files, folders, pens), Ornamental Dingbats, Geometric Shapes
  Extended and Supplemental Arrows-C, that Noto Sans and DejaVu Sans lack.

The emoji of the other planes (most of them, about 1,200 pictures and 3 MB) are left out.

    uv run fonts/fallbacks.py NOTO_DIR DEJAVU_DIR

NOTO_DIR holds NotoColorEmoji.ttf, NotoSansSymbols2-Regular.ttf and NotoSans-Regular.ttf, DEJAVU_DIR DejaVuSans.ttf
(all SIL OFL 1.1, but DejaVu's own licence); the fonts are written beside this script.
"""
import os
import sys

import regex
from fontTools import subset
from fontTools.ttLib import TTFont, newTable
from fontTools.ttLib.tables._g_l_y_f import Glyph

EMOJI_RANGES = [(0x0020, 0x2BFF)]
SYMBOL_RANGES = [(0x1F300, 0x1F5FF), (0x1F650, 0x1F67F), (0x1F780, 0x1F7FF), (0x1F800, 0x1F8FF)]


def emoji_presentation(c):
    return regex.match(r'\p{Emoji_Presentation}', chr(c)) is not None


def chars_of(ranges, cmap, have, emoji):
    return [c for a, b in ranges for c in range(a, b + 1)
            if c in cmap and c not in have and emoji_presentation(c) == emoji]


def options(**kw):
    o = subset.Options()
    o.name_IDs = ['*']
    o.hinting = False
    for k, v in kw.items():
        setattr(o, k, v)
    return o


def emoji_font(src, chars, out):
    font = TTFont(src)
    s = subset.Subsetter(options(layout_features=[], drop_tables=subset.Options().drop_tables + ['GSUB', 'vhea', 'vmtx']))
    s.populate(unicodes=chars)
    s.subset(font)
    # an empty outline for every glyph, so that stb_truetype takes the font; the pictures are in CBDT
    order = font.getGlyphOrder()
    glyf = newTable('glyf')
    glyf.glyphOrder = order
    glyf.glyphs = {name: Glyph() for name in order}
    font['glyf'] = glyf
    font['loca'] = newTable('loca')
    font['head'].indexToLocFormat = 0
    font['head'].glyphDataFormat = 0
    maxp = font['maxp']
    if maxp.tableVersion < 0x10000:
        maxp.tableVersion = 0x10000
        for k in ('maxPoints', 'maxContours', 'maxCompositePoints', 'maxCompositeContours', 'maxTwilightPoints',
                  'maxStorage', 'maxFunctionDefs', 'maxInstructionDefs', 'maxStackElements', 'maxSizeOfInstructions',
                  'maxComponentElements', 'maxComponentDepth'):
            setattr(maxp, k, 0)
        maxp.maxZones = 2
    font.save(out)


def symbols_font(src, chars, out):
    font = TTFont(src)
    s = subset.Subsetter(options(layout_features=['kern'], notdef_outline=True))
    s.populate(unicodes=chars)
    s.subset(font)
    font.save(out)


def main():
    noto, dejavu = sys.argv[1], sys.argv[2]
    here = os.path.dirname(os.path.abspath(__file__))
    # what Firefox finds before these: the system's Noto Sans (and DejaVu Sans, for a symbol), and the subsets
    cmap = lambda *path: set(TTFont(os.path.join(*path)).getBestCmap())
    have_noto = cmap(noto, 'NotoSans-Regular.ttf') | cmap(here, 'NotoSans-Regular.ttf')
    have = have_noto | cmap(dejavu, 'DejaVuSans.ttf') | cmap(here, 'DejaVuSans.ttf')
    emoji_src = os.path.join(noto, 'NotoColorEmoji.ttf')
    symbols_src = os.path.join(noto, 'NotoSansSymbols2-Regular.ttf')
    emoji = chars_of(EMOJI_RANGES, TTFont(emoji_src).getBestCmap(), have_noto, True)
    symbols = chars_of(SYMBOL_RANGES, TTFont(symbols_src).getBestCmap(), have, False)
    for name, make, src, chars in (('NotoColorEmoji.ttf', emoji_font, emoji_src, emoji),
                                   ('NotoSansSymbols2-Regular.ttf', symbols_font, symbols_src, symbols)):
        out = os.path.join(here, name)
        make(src, chars, out)
        print(f'{out}: {len(chars)} characters, {os.path.getsize(out)} bytes')


if __name__ == '__main__':
    main()
