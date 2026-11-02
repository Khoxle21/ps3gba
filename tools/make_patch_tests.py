#!/usr/bin/env python3
"""Synthetic ROMs and IPS/UPS/BPS patches for testing ROM patching without any real game.

    tools/make_patch_tests.py <dir>

Writes to <dir>: ROMs (*.gba), patches (*.ips, *.ups, *.bps), the results the patches must produce
(*.expected), a 16 MB ROM and its patch (*.bin), and cases.txt, one case per line: "<rom> <patch> <expected result, or - when it must be refused>",
for tests/patch/harness.c (tools/patch_test.sh). The encoders follow the formats' specs and
compute each expected result on their own, so mGBA's decoders are checked against them.

The same files serve the app's own flow (RPCS3 or the console): copy the *.gba files to roms/
and the *.ips, *.ups and *.bps files to patches/ (two of them are damaged on purpose). "Patch Test" and its hack chain look like a game and its versions
to the library (game codes ZPTE and ZPTF):
  Patch Test (ZPTE) + Patch Test Hack.ups -> Patch Test Hack (ZPTE, twice the size)
  Patch Test Hack + Patch Test Hack FR.bps -> Patch Test Hack FR
  Patch Test (ZPTE) + Patch Test Fix.ips -> Patch Test Fix
  Patch Test (France) (ZPTF): the other version, which the UPS and BPS patches must refuse.
"""
import os
import random
import sys
import zlib

KB = 1024


def crc(data):
    return zlib.crc32(data) & 0xFFFFFFFF


def le32(n):
    return n.to_bytes(4, "little")


def vlq(n):
    """The number encoding of UPS and BPS."""
    out = bytearray()
    while True:
        x = n & 0x7F
        n >>= 7
        if n == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        n -= 1


def rom(size, code, title, seed):
    """A GBA ROM: random bytes under a header mGBA accepts, with a game code and a title."""
    r = random.Random(seed)
    data = bytearray(r.randbytes(size))
    data[0xA0:0xAC] = title.encode().ljust(12, b"\0")[:12]
    data[0xAC:0xB0] = code.encode()
    data[0xB2] = 0x96  # GBA_ROM_MAGIC
    return data


def ips(records):
    """records: (offset, bytes) or (offset, count, byte) for a run (RLE)."""
    out = bytearray(b"PATCH")
    for rec in records:
        out += rec[0].to_bytes(3, "big")
        if len(rec) == 2:
            out += len(rec[1]).to_bytes(2, "big") + rec[1]
        else:
            out += b"\0\0" + rec[1].to_bytes(2, "big") + bytes([rec[2]])
    return bytes(out + b"EOF")


def ips_apply(source, records):
    """What an IPS patch makes: the source grown to its furthest record."""
    end = max([len(source)] + [r[0] + (len(r[1]) if len(r) == 2 else r[1]) for r in records])
    out = bytearray(source) + bytes(end - len(source))
    for rec in records:
        if len(rec) == 2:
            out[rec[0]:rec[0] + len(rec[1])] = rec[1]
        else:
            out[rec[0]:rec[0] + rec[1]] = bytes([rec[2]]) * rec[1]
    return out


def ups(source, target):
    """Runs of XORed bytes, each ended by a 0 (the first unchanged byte), the source counting as
    zeros past its end."""
    out = bytearray(b"UPS1" + vlq(len(source)) + vlq(len(target)))
    x = lambda i: (source[i] if i < len(source) else 0) ^ target[i]
    pos = i = 0
    while i < len(target):
        if not x(i):
            i += 1
            continue
        out += vlq(i - pos)
        while i < len(target) and x(i):
            out.append(x(i))
            i += 1
        out.append(0)
        i += 1
        pos = i
    out += le32(crc(source)) + le32(crc(target))
    return bytes(out + le32(crc(out)))


def bps(source, actions, metadata=b""):
    """actions: ("source read", length), ("target read", bytes), ("source copy", offset, length),
    ("target copy", offset, length) with absolute offsets. Returns the patch and its target."""
    target = bytearray()
    body = bytearray()
    source_at = target_at = 0  # the decoder's relative offsets

    def relative(delta):
        return vlq(abs(delta) << 1 | (delta < 0))

    for a in actions:
        if a[0] == "source read":
            body += vlq((a[1] - 1) << 2 | 0)
            target += source[len(target):len(target) + a[1]]
        elif a[0] == "target read":
            body += vlq((len(a[1]) - 1) << 2 | 1) + a[1]
            target += a[1]
        elif a[0] == "source copy":
            body += vlq((a[2] - 1) << 2 | 2) + relative(a[1] - source_at)
            target += source[a[1]:a[1] + a[2]]
            source_at = a[1] + a[2]
        else:  # target copy: may overlap what it writes, byte by byte
            body += vlq((a[2] - 1) << 2 | 3) + relative(a[1] - target_at)
            for k in range(a[2]):
                target.append(target[a[1] + k])
            target_at = a[1] + a[2]
    out = bytearray(b"BPS1" + vlq(len(source)) + vlq(len(target)) + vlq(len(metadata)) + metadata)
    out += body + le32(crc(source)) + le32(crc(target))
    return bytes(out + le32(crc(out))), bytes(target)


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    files, cases = {}, []

    base = rom(64 * KB, "ZPTE", "PATCH TEST", 1)
    other = rom(64 * KB, "ZPTF", "PATCH TEST", 2)  # same game, another version
    files["Patch Test.gba"] = base
    files["Patch Test (France).gba"] = other

    # UPS: grows the ROM to twice its size (like a 16 MB game made 32 MB) and changes some bytes,
    # including one at the very end.
    hack = bytearray(base) + random.Random(3).randbytes(64 * KB)
    hack[0xA0:0xAC] = b"PATCH HACK\0\0"
    for at in (0x200, 0x201, 0x1234, 0xFFFF, 0x10000, 0x1FFFF):
        hack[at] ^= 0x5A
    hack[0x3000:0x3100] = bytes(0x100)
    files["Patch Test Hack.ups"] = ups(base, hack)
    files["Patch Test Hack.gba.expected"] = hack
    cases += [("Patch Test.gba", "Patch Test Hack.ups", "Patch Test Hack.gba.expected"),
              ("Patch Test (France).gba", "Patch Test Hack.ups", "-")]  # result checksum

    # BPS made for the hack (a translation of it, as a chain): every action, metadata, shrinks.
    fr, fr_target = bps(hack, [
        ("source read", 0xA0), ("target read", b"PATCH HACKFR"), ("source read", 0x100),
        ("source copy", 0x8000, 0x300), ("source copy", 0x400, 0x20), ("target read", b"AB"),
        ("target copy", 0x1AC, 0x81), ("source copy", 0x1C000, 0x3000), ("target copy", 0x10, 0x200),
        ("source read", 0x1000),
    ], metadata=b"<patch>test</patch>")
    files["Patch Test Hack FR.bps"] = fr
    files["Patch Test Hack FR.gba.expected"] = fr_target
    cases += [("Patch Test Hack.gba.expected", "Patch Test Hack FR.bps", "Patch Test Hack FR.gba.expected"),
              ("Patch Test.gba", "Patch Test Hack FR.bps", "-")]  # made for the hack, not the original

    # IPS: data and run records, one past the end (the ROM grows), offsets above 0xFFFF.
    recs = [(0x100, b"IPS!"), (0x10000 - 2, b"edge"), (0x8000, 0x40, 0xEE), (0x10010, b"grown")]
    files["Patch Test Fix.ips"] = ips(recs)
    files["Patch Test Fix.gba.expected"] = ips_apply(base, recs)
    cases.append(("Patch Test.gba", "Patch Test Fix.ips", "Patch Test Fix.gba.expected"))

    # IPS on a ROM past 16 MB: offsets stop there, so it keeps its size. Harness only (other
    # extensions: the app would list them).
    big = bytes(16 * 1024 * KB + 64 * KB)
    big_recs = [(0xFFFFF0, b"near the 16 MB end")]
    files["big.bin"] = big
    files["big-fix.bin"] = ips(big_recs)
    files["big.expected"] = ips_apply(big, big_recs)
    cases.append(("big.bin", "big-fix.bin", "big.expected"))

    # Damaged (one byte changed: the patch's own checksum fails) and not a patch at all.
    damaged = bytearray(files["Patch Test Hack.ups"])
    damaged[20] ^= 1
    files["Damaged.ups"] = damaged
    files["Not A Patch.bps"] = b"BPS2 this is not a patch"
    cases += [("Patch Test.gba", "Damaged.ups", "-"), ("Patch Test.gba", "Not A Patch.bps", "-")]

    for name, data in files.items():
        with open(os.path.join(d, name), "wb") as f:
            f.write(data)
    with open(os.path.join(d, "cases.txt"), "w") as f:
        f.writelines(f"{r}\t{p}\t{e}\n" for r, p, e in cases)
    print(f"{len(files)} files, {len(cases)} cases in {d}")


if __name__ == "__main__":
    main()
