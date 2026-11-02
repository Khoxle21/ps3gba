#!/usr/bin/env python3
"""Renders the ASCII pixel art in source/art.c to a PNG for review.

Usage: python3 tools/preview_art.py [out.png]
Checks that every row of a sprite has the same width.
"""
import re
import sys
from PIL import Image

PALETTE = {  # preview colors; the app uses its own palettes
    'o': (58, 49, 112), 'h': (90, 80, 160), 'b': (44, 37, 88), 'k': (30, 25, 62),
    's': (66, 140, 120), 'w': (200, 230, 210), 'd': (30, 25, 62), 'm': (36, 30, 72),
    'l': (200, 90, 110), 't': (90, 200, 140), 'q': (230, 120, 200), 'c': (240, 90, 90), 'x': (110, 150, 255),
    'g': (28, 27, 38), 'r': (86, 85, 122), 'P': (70, 69, 96), '1': (74, 168, 255), '2': (59, 58, 74),
    '3': (59, 58, 74), '4': (59, 58, 74),
    'A': (200, 200, 220), 'W': (255, 255, 255), 'G': (120, 120, 140), 'L': (255, 210, 60), 'K': (20, 16, 40),
}

src = open(__file__.rsplit('/tools/', 1)[0] + '/source/art.c').read()
sprites = re.findall(r'const char\* const (\w+)\[\] = \{(.*?)NULL', src, re.S)
# The welcome screen's sheet carries its own colors.
INTRO = {e[0]: tuple(int(e[k:k + 2], 16) for k in (4, 6, 8))
         for e in re.search(r'art_intro_palette\[\] = "([^"]*)"', src).group(1).split()}
scale, gap = 8, 4
images = []
for name, body in sprites:
    rows = re.findall(r'"([^"]*)"', body)
    widths = {len(r) for r in rows}
    if len(widths) != 1:
        sys.exit(f'{name}: inconsistent row widths {sorted(widths)}: ' +
                 str([(i, len(r)) for i, r in enumerate(rows) if len(r) != max(widths)]))
    w, h = widths.pop(), len(rows)
    img = Image.new('RGB', (w, h), (16, 12, 36))
    palette = INTRO if name == 'art_intro' else PALETTE
    for y, row in enumerate(rows):
        for x, c in enumerate(row):
            if c in palette:
                img.putpixel((x, y), palette[c])
    images.append((name, img.resize((w * scale, h * scale), Image.NEAREST)))
    print(f'{name}: {w}x{h}')

out = Image.new('RGB', (sum(i.width for _, i in images) + gap * scale * len(images),
                        max(i.height for _, i in images)), (16, 12, 36))
x = 0
for _, img in images:
    out.paste(img, (x, 0))
    x += img.width + gap * scale
out.save(sys.argv[1] if len(sys.argv) > 1 else 'art_preview.png')
