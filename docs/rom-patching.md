# ROM patching

Status: implemented (2026-11-03, `source/rompatch.c`), done on the CFW console and in RPCS3
(synthetic files and a real FireRed -> Unbound -> French Unbound chain); results below. Goal: hacks and translations
distributed as patches (no ROM inside) become playable without a PC tool. The player picks a game
they own and a patch made for it; the PS3 builds the patched ROM once, which then appears as a
normal game. The plan follows as written, then what was decided while building it.

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
8. **Cleaning up** (patches are big: a hack's UPS can weigh 26 MB, and a chain leaves its
   intermediate ROMs behind): after every successful build, before going back to the carousel,
   ask "<game> is ready. Delete the patch file? You won't be able to rebuild it without it.",
   KEEP preselected (the safe choice, like NO for deleting a save).
   - When the base is itself a ROM ps3gba built (a chain: FireRed -> a hack -> its translation),
     and no one has saves for it, the same screen also offers to delete that intermediate ROM.
   - Never offered: an original ROM (the player's own dump), nor any game with saves.
   - ps3gba knows which ROMs it built from a small record in `patches/` (one line per build:
     output, base, patch, checksums), written with the ROM and updated on deletion.
   - Deleting runs on the library's worker thread like a save deletion, then the list refreshes.

Not covered: xdelta/VCDIFF patches (mGBA has no decoder for them). IPS can't change bytes past
16 MB (24-bit offsets: a format limit, relevant for 32 MB ROMs).

## Decisions taken while building it

- **Listing**: UPS/BPS made for this ROM: listed ("FOR THIS VERSION"). Made for another ROM of the
  library with this game's header code (`library_same_game`: GBA code without its region letter,
  which hacks keep): greyed, "FOR ANOTHER VERSION", and picking it shows the refusal. Made for a
  ROM not in the library: not listed. Every IPS, and files with a patch extension but not a patch
  format: "VERSION NOT CHECKED" (building an unrecognized one says it is damaged). A patch whose
  game already exists: greyed, "ALREADY BUILT". The checksums of the other versions are only
  computed when a patch needs them, then cached per file version (name, size, date) for the
  session.
- **Nobody plays**: the list shows the message instead of the patches; a build or cleanup job
  refuses Cross in the carousel and in the saves list with a toast in place of the legend ("WAIT:
  A GAME IS BEING PATCHED"). One job for everyone at a time; a second player's confirmation is
  answered "ANOTHER PLAYER IS PATCHING A GAME".
- **Building** shows its step (reading, applying, writing). Before writing, the drive's free space
  is read with lv2's `sys_fs_disk_free` (syscall 840; skipped if it fails). The output name drops
  a ROM extension the patch's name may carry ("Hack.gba.ups" -> "Hack").
- **Cleanup** rows: KEEP, DELETE (the patch), and in a chain DELETE BOTH (the patch and the
  intermediate ROM). The record is `patches/built-games.txt`; a ROM counts as built only if a line
  names it with its current checksum, and DELETE BOTH checks that again, with no saves, right
  before deleting (its empty saves folder goes too). The questions show over the carousel, on
  the new game.
- **mGBA's patch code** needed `patches/mgba/patch-formats-big-endian.patch`: it read numbers as
  little-endian (every UPS/BPS refused on the PS3), and sized IPS results 16 MB always.

## Console results (CFW, 2026-11-03, dev build)

- With FireRed US, FireRed FR and an Unbound in the library: Square on FireRed FR listed the
  Unbound UPS greyed (made for FireRed US) and refused it; on FireRed US it was "FOR THIS VERSION".
- FireRed US (16 MB) + a 26 MB UPS -> Unbound, 32 MB, CRC32 as published: 5.9-6.4 s (read 1.0 s,
  apply 1.4-1.9 s, write 3.5 s, ~9 MB/s). Unbound + the 1.8 MB French BPS -> 32 MB: 4.7-4.9 s
  (apply 0.6 s). The UI stays at 60 fps once files move in 256 KB pieces (see the notes).
- The French BPS was listed on the built Unbound (its checksum), greyed on FireRed. KEEP kept
  everything; DELETE removed the UPS only; DELETE BOTH removed the BPS and the intermediate
  Unbound, never FireRed US nor the player's own Unbound.
- The French game ran (a copy of an Unbound bench save loaded in it) and saved from its own menu:
  the save file was rewritten. While it ran, another player's Square showed the "must quit"
  message. The phone server accepted the BPS (then refused it as already there).
- lv2's `sys_fs_disk_free` (syscall 840) returned the free bytes there (and in RPCS3).

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
  Done: `tools/patch_test.sh` (`tools/make_patch_tests.py`, `tests/patch/harness.c`), BPS too,
  natively and big-endian; `tools/webtest.sh` covers patch uploads.
- **With real files** (kept out of the repo): a ROM and a BPS hack or translation patch for it,
  in RPCS3 then on the console: list, wrong-version refusal, build, play, save, rescan; the
  "nobody playing" refusal with a game running; a phone upload of a patch; the cleanup question
  (KEEP leaves everything, DELETE removes the patch, and in a chain the intermediate ROM without
  saves, never the original ROM).
- **A chain**: a hack built from a ROM by a UPS patch, then a translation BPS made for that hack:
  the translation must show up on the newly built game (its checksum is the hack's result
  checksum), not on the original ROM. Large outputs too: a 16 MB ROM patched into 32 MB.
- UI at 1080p and 720p (`RES=1280x720`), in a single slot and in 2x2.

## Done means

The flow above works on the console with a real patch, the docs (README for players,
CONTRIBUTING code map and dev command, notes if the console teaches something) are updated, and
CHANGELOG.md gets a 0.1 step (its number decided when merging: another feature may land first).
