#!/usr/bin/env python3
"""Source of truth for the pixel art: composes sprites and writes source/art.c, plus the XMB
icon ICON0.PNG and the sprite sheet of its animated version (the welcome screen).

Usage: python3 tools/gen_art.py && python3 tools/preview_art.py out.png
Each character is one art pixel; the app maps characters to colors per use.
"""
import os

def grid(w, h):
    return [['.'] * w for _ in range(h)]

def rect(g, x, y, w, h, c):
    for j in range(y, y + h):
        for i in range(x, x + w):
            g[j][i] = c

def put(g, x, y, rows):
    for j, r in enumerate(rows):
        for i, c in enumerate(r):
            if c != ' ':
                g[y + j][x + i] = c

def rounded_box(g, x, y, w, h, r=2):
    """Body 'b' with outline 'o', top/left highlight 'h' and corners cut by r pixels."""
    rect(g, x, y, w, h, 'b')
    for i in range(x, x + w):
        g[y][i] = g[y + h - 1][i] = 'o'
    for j in range(y, y + h):
        g[j][x] = g[j][x + w - 1] = 'o'
    for i in range(x + 1, x + w - 1):
        g[y + 1][i] = 'h'
    for j in range(y + 1, y + h - 1):
        g[j][x + 1] = 'h'
    for k in range(r):
        for cx, cy, dx, dy in [(x, y, 1, 1), (x + w - 1, y, -1, 1), (x, y + h - 1, 1, -1), (x + w - 1, y + h - 1, -1, -1)]:
            for t in range(r - k):
                g[cy + dy * k][cx + dx * t] = '.'
            g[cy + dy * k][cx + dx * (r - k)] = 'o'

def screen(g, x, y, w, h):
    rect(g, x, y, w, h, 'k')
    rect(g, x + 1, y + 1, w - 2, h - 2, 's')
    g[y + 1][x + 1] = g[y + 1][x + 2] = g[y + 2][x + 1] = 'w'

DPAD = [" d ", "ddd", " d "]

def gba():
    g = grid(32, 17)
    rounded_box(g, 0, 0, 32, 17, r=3)
    screen(g, 9, 3, 14, 10)
    put(g, 3, 6, DPAD)
    put(g, 27, 5, ["dd", "dd"])
    put(g, 24, 8, ["dd", "dd"])
    put(g, 13, 14, ["dd dd"])
    return g

def gba_sp():
    g = grid(20, 27)
    rounded_box(g, 0, 0, 20, 14, r=2)
    screen(g, 3, 2, 14, 10)
    rect(g, 3, 14, 14, 2, 'm')
    rounded_box(g, 0, 16, 20, 11, r=2)
    put(g, 3, 19, DPAD)
    put(g, 15, 18, ["dd", "dd"])
    put(g, 12, 21, ["dd", "dd"])
    put(g, 7, 24, ["dd dd"])
    return g

def shell(mask):
    """Outline 'o' around a silhouette, body 'b', lit top/left edge 'h', shaded bottom/right 's'."""
    H, W = len(mask), len(mask[0])
    inside = lambda i, j: 0 <= i < W and 0 <= j < H and mask[j][i]
    g = grid(W, H)
    for j in range(H):
        for i in range(W):
            if mask[j][i]:
                edge = not all(inside(i + di, j + dj) for di, dj in [(1, 0), (-1, 0), (0, 1), (0, -1)])
                g[j][i] = 'o' if edge else 'b'
    for j in range(H):
        for i in range(W):
            if g[j][i] != 'b':
                continue
            if g[j - 1][i] == 'o' or g[j][i - 1] == 'o':
                g[j][i] = 'h'
            elif g[j + 1][i] == 'o' or g[j][i + 1] == 'o':
                g[j][i] = 's'
    return g

def cart_label(g, x, y, w, h):
    """Recessed sticker: rim 'k', paper 'l', printed band 'L' and artwork 'A'."""
    rect(g, x, y, w, h, 'k')
    rect(g, x + 1, y + 1, w - 2, h - 2, 'l')
    rect(g, x + 2, y + 2, w - 4, 2, 'L')
    rect(g, x + 2, y + 5, w - 4, h - 7, 'A')

def cart_plate(g, x, y, w):
    """Embossed logo plate: a groove 's' around a line of raised 'h' letters."""
    rect(g, x, y, w, 4, 's')
    rect(g, x + 1, y + 1, w - 2, 2, 'b')
    for i in range(x + 2, x + w - 2):
        if (i - x) % 4 != 1:
            g[y + 1][i] = 'h'

def cart_gb(color):
    """Game Boy Game Pak (57x65 mm), top-right corner cut. color: the Game Boy Color's smoked
    translucent pak, its contacts 'g' and inner edge 'p' showing through."""
    W, H = 24, 28
    mask = [[True] * W for _ in range(H)]
    for j in range(5):
        for i in range(W - 5 + j, W):
            mask[j][i] = False
    for i, j in [(0, 0), (0, H - 1), (W - 1, H - 1)]:
        mask[j][i] = False
    g = shell(mask)
    cart_plate(g, 3, 2, 14)
    cart_label(g, 3, 8, 18, 17)
    if color:
        for i in range(4, 20, 2):
            g[26][i] = 'g'
        for j, i in [(3, 19), (4, 20), (5, 21)]:
            g[j][i] = 'p'
    return g

def cart_gba():
    """Game Boy Advance Game Pak (57x35 mm), notched top corners."""
    W, H = 30, 19
    mask = [[True] * W for _ in range(H)]
    for j in range(2):
        for i in list(range(2)) + list(range(W - 2, W)):
            mask[j][i] = False
    for i, j in [(0, H - 1), (W - 1, H - 1)]:
        mask[j][i] = False
    g = shell(mask)
    cart_plate(g, 5, 2, 20)
    cart_label(g, 3, 7, 24, 10)
    return g

def dualshock():
    """DualShock 3 seen from above. Built from a silhouette: outline 'o' and top-left
    highlight 'h' are derived, then details are placed. LEDs are '1'-'4' so the app can
    light the one matching the player."""
    W, H = 40, 22
    left = [4, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4, 5, 7]
    inner = {15: 13, 16: 12, 17: 12, 18: 11, 19: 11, 20: 10, 21: 9}  # handles split below row 15
    mask = [[False] * W for _ in range(H)]
    for j in range(H):
        for i in range(left[j], W - left[j]):
            if j not in inner or i <= inner[j] or i >= W - 1 - inner[j]:
                mask[j][i] = True
    g = grid(W, H)
    inside = lambda i, j: 0 <= i < W and 0 <= j < H and mask[j][i]
    for j in range(H):
        for i in range(W):
            if not mask[j][i]:
                continue
            edge = not all(inside(i + di, j + dj) for di, dj in [(1, 0), (-1, 0), (0, 1), (0, -1)])
            g[j][i] = 'o' if edge else 'b'
    for j in range(H):
        for i in range(W):
            if g[j][i] == 'b' and (g[j - 1][i] == 'o' or (i < W // 2 and g[j][i - 1] == 'o')):
                g[j][i] = 'h'
    for x, y in [(7, 5), (7, 6), (5, 7), (6, 7), (7, 7), (8, 7), (9, 7), (7, 8), (7, 9)]:
        g[y][x] = 'd'
    g[5][32], g[7][30], g[7][34], g[9][32] = 't', 'q', 'c', 'x'
    g[8][14] = g[8][15] = g[8][24] = g[8][25] = 'g'  # select, start
    g[10][19] = g[10][20] = 'P'                      # PS button
    for cx in (13, 26):                              # sticks
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                g[12 + dy][cx + dx] = 'r'
        g[12][cx] = 's'
    for n, x in enumerate((16, 18, 21, 23)):
        g[2][x] = str(n + 1)
    return g

# --- XMB icon (ICON0.PNG, 320x176): a fat PS3 sweating as it juggles four linked GBAs ---

ICON_W, ICON_H, ICON_SCALE = 80, 44, 4
PS3_X, PS3_Y = 23, 26                       # the PS3 on the icon's canvas
SHOULDERS = [(PS3_X, PS3_Y + 8), (PS3_X + 33, PS3_Y + 8)]

def shade(c, f):
    """f < 1 darkens towards black, f > 1 lightens towards white."""
    return tuple(round(v * f) if f <= 1 else round(v + (255 - v) * (f - 1)) for v in c)

def mix(a, b, t):
    return tuple(round(x + (y - x) * t) for x, y in zip(a, b))

def gba_mini(mouth):
    """Small original GBA with a face on its screen."""
    g = grid(19, 10)
    rounded_box(g, 0, 0, 19, 10, r=3)
    rect(g, 5, 2, 9, 6, 'k')
    rect(g, 7, 3, 5, 4, 's')
    put(g, 7, 3, ["seses", "sssss", mouth.replace(".", "s"), "sssss"])
    g[3][7] = 'w'
    put(g, 1, 4, DPAD)
    g[3][16] = g[5][15] = 'd'
    return g

def ps3_fat():
    """Fat PS3 lying flat, seen from the front: arched glossy top, the disc slot is the mouth."""
    W, H = 34, 16
    inset = [9, 5, 3, 2, 1, 1] + [0] * (H - 6)
    g = grid(W, H)
    inside = lambda i, j: 0 <= j < H and inset[j] <= i < W - inset[j]
    for j in range(H):
        for i in range(W):
            if inside(i, j):
                edge = not all(inside(i + di, j + dj) for di, dj in [(1, 0), (-1, 0), (0, 1), (0, -1)])
                g[j][i] = 'o' if edge else 'b'
    for j in range(H):  # gloss along the arch
        for i in range(W):
            if g[j][i] == 'b' and g[j - 1][i] == 'o':
                g[j][i] = 'h'
    for i, j in [(8, 2), (9, 2), (6, 3), (7, 3), (5, 4)]:
        g[j][i] = 'H'
    eye = [".ww.", "wppw", "wppw", "wwww", ".ww."]
    put(g, 11, 4, eye)
    put(g, 19, 4, eye)
    for i, j in [(10, 3), (11, 3), (12, 2), (13, 2), (20, 2), (21, 2), (22, 3), (23, 3)]:
        g[j][i] = 'S'  # worried brows
    put(g, 8, 10, ["kkkkkkkkkkkkkkkkkk", "kwwkwwkwwkwwkwwkwk", "kkkkkkkkkkkkkkkkkk"])
    g[10][8] = g[12][8] = g[10][25] = g[12][25] = 'b'
    rect(g, 3, 13, 5, 1, 'S')       # memory card flap
    g[11][29] = g[11][31] = 'S'      # power and eject
    return g

DROP = ["..D..", ".DLD.", "DLDDD", "DDDDD", ".DDD."]
FONT3X5 = {
    'P': ["###", "#.#", "###", "#..", "#.."], 'S': ["###", "#..", "###", "..#", "###"],
    '3': ["###", "..#", "###", "..#", "###"], 'G': ["###", "#..", "#.#", "#.#", "###"],
    'B': ["##.", "#.#", "##.", "#.#", "##."], 'A': [".#.", "#.#", "###", "#.#", "#.#"],
    'T': ["###", ".#.", ".#.", ".#.", ".#."], 'R': ["##.", "#.#", "##.", "#.#", "#.#"],
    'L': ["#..", "#..", "#..", "#..", "###"], '/': ["..#", "..#", ".#.", "#..", "#.."],
    '1': [".#.", "##.", ".#.", ".#.", "###"],
}

def pill(text):
    """A labeled button (START, L3/R3) for the hints: 7 pixels tall like the face buttons, the
    letters cut out of a filled pill."""
    w = 4 * len(text) + 3
    g = grid(w, 7)
    rect(g, 0, 0, w, 7, 'w')
    for x, y in [(0, 0), (w - 1, 0), (0, 6), (w - 1, 6)]:
        g[y][x] = '.'
    for k, ch in enumerate(text):
        put(g, 2 + 4 * k, 1, [row.replace('.', ' ').replace('#', '.') for row in FONT3X5[ch]])
    return g
GLOVE = [".ooo.", "owwwo", "owwwo", ".ooo."]

# Colors and layout shared by the icon and the welcome screen (exported to include/art.h).
SKY_TOP, SKY_BOTTOM, SPOT, STAR = (0x1d, 0x15, 0x46), (0x0b, 0x09, 0x20), (0x6a, 0x58, 0xc8), (0x8a, 0x80, 0xd0)
STARS = [(3, 3), (76, 4), (74, 31), (5, 33), (66, 40), (13, 41), (39, 1)]
PLAYERS = [(0x4a, 0xa8, 0xff), (0xff, 0x5a, 0x6e), (0x4a, 0xd6, 0x6d), (0xff, 0xcf, 0x3f)]
INK = (0x05, 0x05, 0x0a)
SILVER, SILVER_HI = (0xaa, 0xae, 0xba), (0xe1, 0xe4, 0xec)
WHITE = (0xf4, 0xf2, 0xff)
PS3_PAL = {'o': INK, 'b': (0x1c, 0x1b, 0x24), 'h': (0x48, 0x47, 0x58), 'H': SILVER_HI, 'S': SILVER,
           'w': WHITE, 'p': INK, 'k': (0x00, 0x00, 0x00)}
FEET = ["oooo", "obbo"]
GLOVE_PAL = {'w': WHITE, 'o': INK}
DROP_PAL = {'D': (0x5a, 0xb4, 0xff), 'L': (0xd8, 0xf0, 0xff)}
LABELS = [("PS3", SILVER_HI), ("GBA", (0xb4, 0xa8, 0xff))]

def gba_pal(c):
    return {'o': shade(c, 0.35), 'b': shade(c, 0.75), 'h': c, 'k': (0x24, 0x20, 0x38),
            's': (0xb4, 0xe6, 0xc8), 'w': (0xf0, 0xff, 0xf4), 'e': (0x18, 0x14, 0x30), 'd': shade(c, 0.35)}

def label(word, c):
    """3x5 letters with a hard shadow, as on the icon."""
    g = grid(4 * len(word), 6)
    for k, ch in enumerate(word):
        for dx, dy, col in [(1, 1, 'z'), (0, 0, '#')]:
            for j, r in enumerate(FONT3X5[ch]):
                for i, v in enumerate(r):
                    if v == '#':
                        g[dy + j][4 * k + dx + i] = col
    return g, {'z': INK, '#': c}

def bezier(p0, p1, p2, n=60):
    return [(round((1 - t) ** 2 * p0[0] + 2 * (1 - t) * t * p1[0] + t * t * p2[0]),
             round((1 - t) ** 2 * p0[1] + 2 * (1 - t) * t * p1[1] + t * t * p2[1]))
            for t in (k / n for k in range(n + 1))]

def icon():
    from PIL import Image
    img = Image.new('RGB', (ICON_W, ICON_H))
    px = img.load()
    def plot(x, y, c):
        if 0 <= x < ICON_W and 0 <= y < ICON_H:
            px[x, y] = c
    def blit(rows, x, y, pal):
        for j, r in enumerate(rows):
            for i, c in enumerate(r):
                if c in pal:
                    plot(x + i, y + j, pal[c])
    for y in range(ICON_H):  # source/intro.c draws the same sky, cone and spot
        base = mix(SKY_TOP, SKY_BOTTOM, round(y / (ICON_H - 1) * 4) / 4)  # banded like pixel art
        half = 8 + y * 0.45  # spotlight cone from the top
        for x in range(ICON_W):
            lit = abs(x + 0.5 - ICON_W / 2) < half
            px[x, y] = mix(base, SPOT, 0.18) if lit else base
    for y in range(41, 44):  # lit spot on the floor
        for x in range(ICON_W):
            if ((x + 0.5 - 40) / 26) ** 2 + ((y + 0.5 - 43) / 2.6) ** 2 < 1:
                px[x, y] = mix(SKY_BOTTOM, SPOT, 0.35)
    for x, y in STARS:
        plot(x, y, STAR)

    gbas = [(1, 15, "..e.."), (17, 2, ".eee."), (44, 2, "..e.."), (60, 13, ".eee.")]
    cable, cable_shadow = (0xb4, 0xae, 0xd8), (0x2a, 0x24, 0x58)
    tops = [(x + 9, y) for x, y, _ in gbas]
    for (a, b, sag) in [(0, 1, -8), (1, 2, 16), (2, 3, -8)]:
        (x0, y0), (x2, y2) = tops[a], tops[b]
        pts = bezier((x0, y0), ((x0 + x2) / 2, min(y0, y2) + sag), (x2, y2))
        for x, y in pts:
            plot(x, y + 1, cable_shadow)
        for x, y in pts:
            plot(x, y, cable)

    ps3 = ps3_fat()
    bx, by = PS3_X, PS3_Y
    for (sx, sy), (hx, hy) in [(SHOULDERS[0], (10, 26)), (SHOULDERS[1], (69, 26))]:
        for x, y in bezier((sx, sy), ((sx + hx) / 2, sy + 3), (hx, hy), 40):
            plot(x, y, SILVER)
            plot(x, y + 1, INK)
        blit(GLOVE, hx - 2, hy - 1, GLOVE_PAL)
    blit(ps3, bx, by, PS3_PAL)
    for fx in (bx + 5, bx + 24):
        blit(FEET, fx, by + 15, PS3_PAL)
    blit(DROP, bx + 30, by - 4, DROP_PAL)
    blit(DROP, bx + 35, by + 1, DROP_PAL)

    for (x, y, mouth), c in zip(gbas, PLAYERS):
        blit(gba_mini(mouth), x, y, gba_pal(c))
    for (word, c), x in zip(LABELS, (3, 66)):
        g, pal = label(word, c)
        blit(g, x, 37, pal)
    return img.resize((ICON_W * ICON_SCALE, ICON_H * ICON_SCALE), Image.NEAREST)

# --- Welcome screen (source/intro.c): the icon's parts, animated by the app ---

def intro_sprites():
    """(name, grid, palette) of every sprite the animation needs, in the icon's colors."""
    ps3 = ps3_fat() + [['.'] * 34]
    for ex in (11, 19):  # the app draws the pupils; also fill the holes at the eyes' corners
        for dx, dy in [(0, 0), (3, 0), (0, 4), (3, 4)]:
            ps3[4 + dy][ex + dx] = 'b'
        for dx in (1, 2):
            for dy in (1, 2):
                ps3[4 + dy][ex + dx] = 'w'
    for fx in (5, 24):
        put(ps3, fx, 15, FEET)
    sprites = [("PS3", ps3, PS3_PAL),
               ("EYE_BLINK", ["bbbb", "bbbb", "wwww", "bbbb", "bbbb"], PS3_PAL),
               ("EYE_WINK", ["bbbb", "bwwb", "wbbw", "bbbb", "bbbb"], PS3_PAL)]
    for n, c in enumerate(PLAYERS):  # the icon's two faces: grinning up, gasping "o" down
        sprites.append((f"GBA{n + 1}_UP", gba_mini(".eee."), gba_pal(c)))
        sprites.append((f"GBA{n + 1}_DOWN", gba_mini("..e.."), gba_pal(c)))
    sprites += [("GLOVE", GLOVE, GLOVE_PAL), ("DROP", DROP, DROP_PAL)]
    for word, c in LABELS:
        g, pal = label(word, c)
        sprites.append(("LABEL_" + word, g, pal))
    white = {'w': (255, 255, 255)}  # tinted by the app
    sprites += [("CROSS", ART["art_btn_cross"][1], white), ("PIXEL", ["w"], white)]
    return sprites

def intro_sheet(width=64):
    """Packs the sprites into one texture (shelves, 1 pixel apart so nothing bleeds), so the
    whole scene is one draw call. Returns names, rows of palette characters, palette, rects."""
    sprites = intro_sprites()
    rects, x, y, shelf = [None] * len(sprites), 0, 0, 0
    for k in sorted(range(len(sprites)), key=lambda k: -len(sprites[k][1])):
        g = sprites[k][1]
        w, h = len(g[0]), len(g)
        if x + w > width:
            x, y, shelf = 0, y + shelf + 1, 0
        rects[k] = (x, y, w, h)
        x, shelf = x + w + 1, max(shelf, h)
    sheet = grid(width, y + shelf)
    chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
    colors = {}
    for (_, g, pal), (rx, ry, w, h) in zip(sprites, rects):
        for j in range(h):
            for i in range(w):
                if g[j][i] in pal:
                    sheet[ry + j][rx + i] = colors.setdefault(pal[g[j][i]], chars[len(colors)])
    palette = ' '.join(f"{ch}=ff{r:02x}{g:02x}{b:02x}" for (r, g, b), ch in colors.items())
    return [n for n, _, _ in sprites], sheet, palette, rects

ART = {
    "art_gba": ("Original Game Boy Advance.", gba()),
    "art_gba_sp": ("Game Boy Advance SP, opened.", gba_sp()),
    "art_cart_gb": ("Game Boy Game Pak, shown when a game has no cover art.", cart_gb(False)),
    "art_cart_gbc": ("Game Boy Color Game Pak (smoked translucent), same use.", cart_gb(True)),
    "art_cart_gba": ("Game Boy Advance Game Pak, same use.", cart_gba()),
    "art_dualshock": ("PS3 controller, for the join screen.", dualshock()),
    "art_sparkle": ("Twinkle.", ["..w..", "..w..", "wwhww", "..w..", "..w.."]),
    "art_cursor": ("Menu cursor.", ["w....", "ww...", "www..", "wwww.", "www..", "ww...", "w...."]),
    "art_lock": ("Save in use.", ["..www..", ".w...w.", ".w...w.", "wwwwwww", "www.www", "www.www", "wwwwwww"]),
    "art_btn_cross": ("Cross button.", ["w.....w", ".w...w.", "..w.w..", "...w...", "..w.w..", ".w...w.", "w.....w"]),
    "art_btn_circle": ("Circle button.", ["..www..", ".w...w.", "w.....w", "w.....w", "w.....w", ".w...w.", "..www.."]),
    "art_btn_triangle": ("Triangle button.", ["...w...", "..w.w..", "..w.w..", ".w...w.", ".w...w.", "w.....w", "wwwwwww"]),
    "art_btn_square": ("Square button.", ["wwwwwww", "w.....w", "w.....w", "w.....w", "w.....w", "w.....w", "wwwwwww"]),
    "art_btn_start": ("Start button.", pill("START")),
    "art_btn_sticks": ("Left or right stick click.", pill("L3/R3")),
    "art_btn_shoulders": ("L1 or R1 (the GBA's L and R).", pill("L1/R1")),
    "art_arrow": ("More to scroll to, pointing down (flipped for up).", ["wwwwwww", ".wwwww.", "..www..", "...w..."]),
    "art_key_space": ("Space bar label on the rename keyboard (a font-sized glyph).", ["w.....w", "w.....w", "wwwwwww"]),
}

def emit(name, comment, g):
    rows = [''.join(r) if isinstance(r, list) else r for r in g]
    widths = {len(r) for r in rows}
    assert len(widths) == 1, f"{name}: row widths {widths}"
    return (f"// {comment}\nconst char* const {name}[] = {{\n" +
            ''.join(f'\t"{r}",\n' for r in rows) + "\tNULL,\n};\n")

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
intro_names, intro_rows, intro_palette, intro_rects = intro_sheet()
with open(os.path.join(root, "source/art.c"), "w") as f:
    f.write("// Generated by tools/gen_art.py - edit the art there, not here.\n")
    f.write("#include <stddef.h>\n\n#include \"art.h\"\n")
    for name, (comment, g) in ART.items():
        f.write("\n" + emit(name, comment, g))
    f.write("\n" + emit("art_intro", "Welcome screen sprite sheet, colors in art_intro_palette.", intro_rows))
    f.write(f'\nconst char art_intro_palette[] = "{intro_palette}";\n')
    f.write("\nconst unsigned char art_intro_rects[INTRO_SPRITES][4] = {\n" +
            ''.join(f"\t{{ {x}, {y}, {w}, {h} }}, // {n}\n" for n, (x, y, w, h) in zip(intro_names, intro_rects)) +
            "};\n")
    f.write("\nconst unsigned char art_icon_stars[ICON_STARS][2] = {" +
            ', '.join(f"{{ {x}, {y} }}" for x, y in STARS) + "};\n")
with open(os.path.join(root, "include/art.h"), "w") as f:
    f.write("// Generated by tools/gen_art.py\n#ifndef ART_H\n#define ART_H\n\n")
    for name in ART:
        f.write(f"extern const char* const {name}[];\n")
    f.write("\n// Welcome screen (source/intro.c): one sheet, sprite rects as x, y, w, h.\n")
    f.write("enum {\n" + ''.join(f"\tINTRO_{n},\n" for n in intro_names) + "\tINTRO_SPRITES,\n};\n")
    f.write("extern const char* const art_intro[];\nextern const char art_intro_palette[];\n")
    f.write("extern const unsigned char art_intro_rects[INTRO_SPRITES][4];\n")
    hexrgb = lambda c: "0x%02x%02x%02x" % c
    f.write("\n// The icon's canvas, layout and colors (0xRRGGBB), which the welcome screen extends.\n")
    for name, v in [("ICON_W", ICON_W), ("ICON_H", ICON_H), ("ICON_PS3_X", PS3_X), ("ICON_PS3_Y", PS3_Y),
                    ("ICON_SKY_TOP", hexrgb(SKY_TOP)), ("ICON_SKY_BOTTOM", hexrgb(SKY_BOTTOM)),
                    ("ICON_SPOT", hexrgb(SPOT)), ("ICON_STAR", hexrgb(STAR)), ("ICON_SILVER", hexrgb(SILVER)),
                    ("ICON_INK", hexrgb(INK)), ("ICON_STARS", len(STARS))]:
        f.write(f"#define {name} {v}\n")
    f.write("extern const unsigned char art_icon_stars[ICON_STARS][2];\n")
    f.write("\n#endif\n")
icon().save(os.path.join(root, "ICON0.PNG"))
print("wrote source/art.c, include/art.h and ICON0.PNG")
