#!/bin/sh
# Copyright 2026 Ian Karlsson
# quickdom's default fonts, cut to a set of characters: Latin, punctuation, arrows, shapes, symbols,
# dingbats and braille. From Noto Sans (SIL Open Font License 1.1, OFL.txt) and DejaVu (its own licence,
# DEJAVU_LICENSE), and the fallbacks after them that fallbacks.py cuts from Noto Color Emoji (its colour bitmaps) and
# Noto Sans Symbols 2 (OFL.txt), and the Japanese fallback cjk.py cuts from Noto Sans CJK JP (OFL.txt):
#   ./subset.sh /usr/share/fonts/noto /usr/share/fonts/TTF [NotoSansCJK-Regular.ttc]
# Ranges: Basic Latin to Latin Extended-A, punctuation, arrows, mathematical operators, technical symbols, shapes,
# miscellaneous symbols, dingbats (U+2700-27BF) and braille (U+2800-28FF).
set -e
noto=$1 dejavu=$2
cd "$(dirname "$0")"
ranges=U+0020-007E,U+00A0-017F,U+2010-205E,U+2190-21FF,U+2200-22FF,U+2300-23FF,U+25A0-25FF,U+2600-26FF,U+2700-27BF,U+27F0-27FF,U+2800-28FF,U+FFFD
for f in "$noto/NotoSans-Regular.ttf" "$noto/NotoSans-Bold.ttf" "$noto/NotoSans-Italic.ttf" "$noto/NotoSans-BoldItalic.ttf" \
         "$dejavu/DejaVuSans.ttf" "$dejavu/DejaVuSans-Bold.ttf" "$dejavu/DejaVuSansMono.ttf" "$dejavu/DejaVuSansMono-Bold.ttf"; do
	uv run -q --with fonttools pyftsubset "$f" --unicodes="$ranges" --layout-features='kern' --no-hinting \
		--name-IDs='*' --notdef-outline --output-file="$(basename "$f")"
done
uv run -q fallbacks.py "$noto" "$dejavu"
uv run -q cjk.py "${3:-/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc}"
