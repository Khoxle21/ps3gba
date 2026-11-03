#!/usr/bin/env python3
"""Copy a Gen 3 Pokemon save (FireRed, LeafGreen, Emerald...) as another trainer: a new name and
trainer ID in both save slots, sector checksums fixed. Several players can then use copies of one
save in the Union Room, which takes trainers with the same name and ID for one player.

    tools/save_trainer.py <in.sav> <out.sav> <name: letters, up to 7> <trainer id, e.g. 0x12345678>
"""
import struct
import sys

SIGNATURE = 0x08012025
SECTOR, SLOT = 0x1000, 0xE000
TRAINER_SIZE = 3884  # section 0, the trainer info: name at 0, trainer ID at 10


def encode(name):
    # The games' character set: A-Z from 0xBB, a-z from 0xD5, 0xFF ends a name.
    if not (0 < len(name) <= 7 and name.isascii() and name.isalpha()):
        sys.exit("name: 1 to 7 letters")
    out = bytes(0xBB + ord(c) - 65 if c.isupper() else 0xD5 + ord(c) - 97 for c in name)
    return out.ljust(7, b"\xff")


def main():
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    src, dst, name, trainer_id = sys.argv[1], sys.argv[2], encode(sys.argv[3]), int(sys.argv[4], 0)
    data = bytearray(open(src, "rb").read())
    patched = 0
    for off in range(0, min(len(data), 2 * SLOT), SECTOR):
        sig, = struct.unpack_from("<I", data, off + 0xFF8)
        section, = struct.unpack_from("<H", data, off + 0xFF4)
        if sig != SIGNATURE or section != 0:
            continue
        data[off:off + 7] = name
        struct.pack_into("<I", data, off + 10, trainer_id)
        total = sum(struct.unpack_from("<%dI" % (TRAINER_SIZE // 4), data, off)) & 0xFFFFFFFF
        struct.pack_into("<H", data, off + 0xFF6, ((total >> 16) + total) & 0xFFFF)
        patched += 1
    if not patched:
        sys.exit("no Gen 3 trainer section found: not a Gen 3 save?")
    open(dst, "wb").write(data)
    print("%s: %d save slot(s) changed" % (dst, patched))


main()
