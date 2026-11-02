# ROM patching: plan

Status: planned (2026-11-02), nothing implemented. Goal: hacks and translations distributed as
patches (no ROM inside) become playable without a PC tool. The player picks a game they own and a
patch made for it; the PS3 builds the patched ROM once, which then appears as a normal game.

## Behavior

1. **Patches on the PS3**: `/dev_hdd0/ps3gba/patches/` (created at startup like `roms/`), filled
   over FTP or from the phone page (which accepts `.ips`, `.ups`, `.bps` and saves them there).
2. **In the game carousel**, a new button (Square, shown in the legend as PATCH) on the selected
   game opens the list of patches for it:
   - UPS and BPS patches whose recorded base checksum is this ROM's: listed.
   - UPS and BPS patches made for another ROM: not listed for this game (or listed greyed with
     "made for another version", if the other ROM's code matches: to decide while building it).
   - IPS patches: always listed (the format has no checksum), with a note that the base can't
     be checked.
   With no patch for the game, the list says where to put patches.
3. **Only while nobody plays**: if any player is in a game (playing or paused), the list shows
   "All players must quit their games to patch a ROM" and nothing else. While a patch is being
   built, no one can start a game (the carousel's PLAY is refused with the same kind of message).
4. **Confirmation**: "Create <name> from <game> + <patch>?", NO preselected (like deleting a
   save).
5. **Building** off the main thread, "PATCHING..." on screen (the UI keeps animating):
   - read the base ROM, compute the output size, apply the patch into a new buffer;
   - UPS/BPS: check the result's checksum against the patch's;
   - write `roms/<patch file name without extension>.<base's extension>` to a temporary file in
     `ps3gba/tmp/`, then rename it into place; never overwrite an existing game (refuse with a
     message);
   - rescan the library (like a phone upload) and move the player's carousel onto the new game.
6. **The new game** is an ordinary ROM file: its own saves (its own name), its cover from
   `covers/<name>.png` or, through its header code, the base game's (existing fallback).
7. **Errors** say what to do: unknown or damaged patch, wrong base version (UPS/BPS checksum),
   result checksum mismatch, not enough memory, drive full, a game of that name already exists.

Not covered: xdelta/VCDIFF patches (mGBA has no decoder for them). IPS can't change bytes past
16 MB (24-bit offsets: a format limit, relevant for 32 MB ROMs).

## Implementation notes

- **mGBA's patch code** (already in our libmgba: `src/util/patch.c`, `patch-ips.c`,
  `patch-ups.c`, `include/mgba-util/patch.h`): `loadPatch(vf, &patch)` recognizes IPS, UPS and
  BPS; `patch.outputSize(&patch, inSize)` gives the result's size; `patch.applyPatch(&patch, in,
  inSize, out, outSize)` writes it. UPS checks the result's checksum itself; BPS checks the
  input's. To list the patches that fit a ROM, read the UPS/BPS footer ourselves: the last 12
  bytes are the base's CRC32, the result's CRC32 and the patch's own CRC32, little-endian; compare
  the first with the ROM's CRC32 (`doCrc32` in `mgba-util/crc32.h`, computed once per ROM and
  cached in the library, off the main thread).
- **Where**: `source/library.c` scans `patches/` (with the games, on its worker thread) and runs
  the build job like the save operations (one request at a time, result polled by the main
  loop); `source/main.c` adds the player states for the patch list, the confirmation and the
  building screen, and the "nobody playing" checks (`players[i].started`); `source/ui.c` draws
  them with the existing list, confirmation and legend helpers; `source/web.c` accepts the three
  extensions into `patches/`.
- **Memory**: no game runs while patching, so ~178 MB are free: the base (up to 32 MB) and the
  result (up to 32 MB) fit in memory.
- **Dev builds**: a scripted-input command to pick a patch by part of its file name (scripts
  must not count presses in lists), log lines for each step (`patch: <base> + <patch> -> <out>
  done|failed: <reason>`).

## Tests

- **Without any game**: a small script (Python, in `tools/`) makes a synthetic ROM and IPS and UPS
  patches for it (UPS's format is simple to write); a host-side check builds the result with
  mGBA's patch code and compares it with the expected bytes. Run in Docker like
  `tools/mgba_exact.sh`. Nothing copyrighted involved, so it can live in the repo.
- **With real files** (kept out of the repo): a ROM and a BPS hack or translation patch for it,
  in RPCS3 then on the console: list, wrong-version refusal, build, play, save, rescan; the
  "nobody playing" refusal with a game running; a phone upload of a patch.
- **A chain**: a hack built from a ROM by a UPS patch, then a translation BPS made for that hack:
  the translation must show up on the newly built game (its checksum is the hack's result
  checksum), not on the original ROM. Large outputs too: a 16 MB ROM patched into 32 MB.
- UI at 1080p and 720p (`RES=1280x720`), in a single slot and in 2x2.

## Done means

The flow above works on the console with a real patch, the docs (README for players,
CONTRIBUTING code map and dev command, notes if the console teaches something) are updated, and
CHANGELOG.md gets a 0.1 step (its number decided when merging: another feature may land first).
