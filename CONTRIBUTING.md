# Contributing to ps3gba

How to build, run, test and change ps3gba: for developers and coding agents. What it is and how
to play: [README.md](README.md).

**Before any work on the console, read [docs/ps3-homebrew-notes.md](docs/ps3-homebrew-notes.md)
and add what you learn to it.** Real hardware differs a lot from the RPCS3 emulator, and a mistake
can cost a console's drive.

## Where things are

- [README.md](README.md): what ps3gba is, install and play.
- This file: setup, workflow, tools, code map, rules.
- [CHANGELOG.md](CHANGELOG.md): the version history; its newest entry is the version the app
  shows.
- [docs/ps3-homebrew-notes.md](docs/ps3-homebrew-notes.md): PS3 hardware and homebrew findings
  (RSX, SPUs, audio, threads, executable memory, HEN, recovery).
- [docs/jit.md](docs/jit.md): the ARM/Thumb to PowerPC JIT's design, exactness and self-check.
- [docs/performance-plan.md](docs/performance-plan.md): how 4 players reached 60 fps, with every
  measurement.
- [docs/wireless-adapter.md](docs/wireless-adapter.md): the GBA Wireless Adapter's emulation
  (protocol, design, tests).
- [docs/rom-patching.md](docs/rom-patching.md): ROM patching's design, decisions and tests.
- Coding agents: link this file as the agent's instructions (e.g. `ln -s CONTRIBUTING.md
  CLAUDE.md` for Claude Code; `CLAUDE.md` is gitignored) and keep personal notes (your consoles,
  preferences) in `CLAUDE.local.md`, also gitignored.

## Setup

1. Clone with the mGBA submodule: `git clone --recursive <repo url>` (or
   `git submodule update --init`).
2. Build the toolchain image once (~15 min): `docker build -t ps3gba-toolchain docker/`
   (ps3toolchain, PSL1GHT, ps3libraries and a patched Tiny3D, pinned commits).
3. `cp .env.example .env` and set your PS3's IP: every `tools/*.sh` reads it (`PS3=<ip>` on the
   command line wins). `.env` stays out of git.
4. `./build.sh DEV=1 pkg`, then install `ps3gba.pkg` once on the console (README, step 1;
   webMAN can do it remotely: copy it to `/dev_hdd0/packages/`, then open
   `http://<ps3 ip>/install.ps3/dev_hdd0/packages/ps3gba.pkg`).
5. From then on `tools/ps3.sh deploy` builds, copies the new EBOOT over FTP and relaunches the
   app (`tools/ps3.sh log` shows its log).

You need a PS3 with CFW or PS3HEN and webMAN MOD, macOS or Linux with Docker, `curl`, Python 3
with Pillow. [RPCS3](https://rpcs3.net) is optional for quick checks (useful, never proof).

## Build

- `./build.sh` release build, `./build.sh DEV=1` dev build (log file, scripted input,
  screenshots, watchdog, startup trace); `pkg` also stages `build/pkg/` (PARAM.SFO, ICON0.PNG,
  USRDIR/EBOOT.BIN) and builds `ps3gba.pkg`.
- Everything builds in the `ps3gba-toolchain` container. mGBA is the `third_party/mgba` submodule,
  built into `mgba-build/` (incrementally, every build); `./build.sh clean` rebuilds everything.
- Our changes to mGBA are `patches/mgba/*.patch` (each described at its top, e.g.
  `skip-sound-samples.patch`: unheard games skip their sound samples), applied by `build.sh` to
  the submodule's working tree, which then rebuilds libmgba incrementally (and relinks the app).
  To change one, edit the submodule's files and regenerate it with `git -C third_party/mgba diff`.
- `sfo.xml`: PARAM.SFO's template (PSL1GHT's, but stereo sound only: claiming more formats left
  the app silent on the TV, see the notes' Audio section).
- Objects rebuild by themselves when the compile flags change (DEV, TIMES, LINK_CABLES,
  JIT_CACHE_MB; `build/cflags.txt`). After changing link-time libs other than libmgba, delete
  `ps3gba.elf` so it relinks.
- Pixel art is generated: edit `tools/gen_art.py`, run it, preview with `tools/preview_art.py`.
  It also draws the XMB icon `ICON0.PNG` (a fat PS3 juggling four linked GBAs), the sprite sheet
  of its animated version, the welcome screen (`art_intro`, colors and layout shared), and the
  button glyphs of the legends (`pill()` for labeled buttons such as START, L3/R3, L1/R1).
- Version: `CHANGELOG.md`'s newest entry (`## x.y.z - date`), shown on the welcome screen (the
  Makefile reads it; a missing one stops the build).

## On the console

- `tools/ps3.sh deploy|cmd|shot|log|exit|restart`. `deploy [VAR=value...]` passes build options
  (e.g. `TIMES=1`); `RELEASE=1 tools/ps3.sh deploy` installs the release build.
  - Update an installed app by FTP (`deploy`). Never reinstall over it with Package Manager: that
    froze a console once.
  - Hung app: `tools/ps3.sh restart` (clean reboot through webMAN). On HEN that turns HEN off:
    power-cycle there instead. Never cut power while the PS3 writes to its drive.
- Parallel sessions on one machine (e.g. one worktree per feature) share RPCS3 and the consoles:
  `tools/rpcs3.sh` and `tools/ps3.sh` claim the device for the current git branch and refuse while
  another branch holds it (`tools/lock.sh status`; `tools/lock.sh release <device>` when done; a
  lock unused for 20 minutes expires).
- On HEN, change one thing per launch, and send no request to webMAN (FTP or HTTP) while the
  XMB loads the app: ~30 s after a launch (`LAUNCH_WAIT=30` for `deploy`). A hang there takes
  the console off the network. See the notes' HEN section.

## Tests

- JIT: `tools/jitcheck.sh` (RPCS3) runs FireRed with the JIT self-check under random input for
  `MINUTES` and prints the first mismatch (`MODE=chain`, `SEED=n`, `GAME=<part of a file name>`;
  `PROFILE=1`: instruction profile instead). RPCS3 can't time JIT code: performance needs the
  console. On the console, put a `jitcheck` file in `/dev_hdd0/ps3gba/` (dev build).
- Skipped sound samples: `SOUND=1 tools/jitcheck.sh` also toggles sampling on and off at random
  frames (the JIT's reference core follows); `SOUND=twin` runs an interpreted twin with the
  opposite setting instead, compared every frame (game-visible state) and at each toggle (whole
  audio state); works for GB/GBC games too (`GAME=Crystal`). Both print `soundcheck P1` lines.
- mGBA changes: `tools/mgba_exact.sh` runs pristine mGBA and ours (or `PATCH=...`) on every GBA
  ROM in RPCS3's folder with random input, ~30 s, and prints the first differing frame and value
  per game. Expected: Emerald, FireRed and Unbound differ in sound only (the sample clock fix in
  `skip-sound-samples.patch`).
- ROM patching, no game needed: `tools/patch_test.sh` makes synthetic ROMs and IPS, UPS and BPS
  patches (`tools/make_patch_tests.py`: a hack chain, a 16 MB+ ROM, a damaged patch, another
  version) and applies them with mGBA's patch code, natively and on a big-endian CPU (s390x under
  qemu, in a small Docker image built on first use). The same files test the app's own flow:
  copy the `.gba` files to `roms/` and the patches to `patches/` (RPCS3's drive or the console's),
  then `<player> PATCH <text>` picks one in the list.
- Phone server: `NET=1 tools/rpcs3.sh run` (network on for that run only), then
  `tools/webtest.sh` runs uploads (games, saves, covers, patches), downloads, names and every
  refusal (bad token, traversal, oversized, cut and stalled uploads) with curl against
  `127.0.0.1:8000` and removes what it added. Check the page in a real browser too: a script
  error leaves every button dead.
- RPCS3 (macOS): `tools/rpcs3.sh run|cmd|shot|log` (`PPU=interp|llvm` forces the PPU decoder,
  `RES=1280x720` another output resolution for layout checks). It runs `ps3gba.self`, which has
  an RWX code segment for the JIT; the pkg's EBOOT.BIN for real hardware keeps R-X. Configure
  RPCS3 for 1080p, nearest scaling and "Write Color Buffers" (needed for in-app screenshots).
- Benchmarks: `tools/bench.sh <players>` right after a deploy brings N FireReds, plugged into the
  cable, to the cable club desk (~90 s, check with a screenshot: presses during fades get lost),
  then `tools/ps3.sh log | tools/perfsum.py` averages the last 10 reports.
  `tools/bench_unbound.sh` does the same with 4 Pokemon Unbound players on `BENCH 1-4` saves (the
  hardest case; it first restores them from pristine copies in `ps3gba/bench/`, since Unbound
  saves by itself now and then). Build options for measurements, given to the deploy:
  `LINK_CABLES=0|1|2` (no cable, one, two 2-player cables), `TIMES=1` (a `time P<n>` line: the
  JIT run loop's split into translated code, mGBA events, interpreter and dispatch; costs ~1%).
- UI changes: check them at 1080p and 720p (`RES=1280x720`), with screenshots.
- Wireless Adapter: `tools/rfu_test.sh <script> <rom> <save> [<rom> <save>...]` runs 2-4 mGBA
  cores with `source/rfu.c`'s adapter on the Mac (toolchain container), from a script
  (`tests/rfu/harness.c`), with screenshots and each game's save at the end;
  `tests/rfu/trade.txt` (two FireReds trade in the Union Room), `trade4.txt` (four, two trades),
  `leave.txt` and `leave3.txt` (a player leaves mid-trade, everyone recovers and trades).
  `tools/save_trainer.py` copies a Gen 3 save as another trainer (the Union Room needs distinct
  ones). See [docs/wireless-adapter.md](docs/wireless-adapter.md).

## Dev builds

- Scripted input (`tools/ps3.sh cmd` or `tools/rpcs3.sh cmd`, one command per line):
  `<player 0-3> <UP|DOWN|LEFT|RIGHT|CROSS|CIRCLE|SQUARE|TRIANGLE|L1|R1|L2|R2|L3|R3|START|SELECT>`,
  `<player> CONNECT|DISCONNECT` (virtual controllers), `<player> TYPE <text>` (on the rename
  keyboard), `WAIT <frames>`, `WAITGBA <frames>` (frames emulated by player 1: scripts stay in
  step with the game on slow hosts), `SHOT`, `RENDER PPU|SPU|CHECK` (renderer for games started
  afterwards; CHECK runs both and logs frames that differ), `<player> GAME <text>` (moves the
  player's carousel to the first game whose file name contains the text, switching to its
  system's catalog: scripts must not count presses in the carousel, its order changes with the
  library), `<player> PATCH <text>` (selects the first patch whose file name contains the text in
  the player's patch list, waiting until it is read), `<player> HOLD <button> <frames>` (a longer
  press, e.g. to walk), `<player> LINK ON|OFF` (the pause menu's cable switch, for a running GBA
  game), `<player> WIRELESS ON|OFF` (its Wireless Adapter switch),
  `INTRO <frame> [<frames since Start>]` (holds one frame of the welcome screen, for
  screenshots), `FRAME <n>` (draws the UI's animations as at frame n, -1 for live: pixel diffs of
  two builds), `MENU` (opens the main menu; player commands then drive the menus instead of
  skipping them), `AUDIO <seconds>` (records what goes to the audio port to
  `/dev_hdd0/ps3gba/audio.wav`), `SAMPLEALL 1|0` (unheard games compute their sound anyway, to
  measure the difference). Each press is held 2 frames, then released for 6 before the next
  command; up to 4095 commands are queued, the rest of a longer file is dropped (logged). Any
  player command skips the welcome screen and the main menu at once (unless `MENU` was given), so
  scripts send input right after `ready:`.
- Menus: Cross confirms, Circle goes back; L3/R3 opens the pause menu; Triangle on a save opens
  its options (rename, copy, delete; on the keyboard Square deletes a character, Start is OK),
  Triangle on the game carousel shows the player's phone QR code, Square its patches; L1/R1
  switch catalogs. Games
  start unplugged; 2+ plugged GBAs on a cable are linked, the first plugged in is the parent.
- The log (`/dev_hdd0/ps3gba/log.txt`) prints every second: fps, per-phase timings, per-player
  emulation ms, the menu music's decoding cost, the heard game's audio (silence padded, queue
  depth, rate stretch), the drawing per frame (`gfx`: draw calls, binds, quads, Mpx filled,
  command buffer, RSX time, waits), JIT and SPU renderer stats, free memory. Also each screen
  change (`menu: a -> b`), each player's state change and save operation. Output of non-main
  threads goes to the TTY (RPCS3's log), not the log file.
- `/dev_hdd0/ps3gba_boot.txt`: one line per startup step from the static constructors to the
  first frame, each written and closed at once through lv2 (survives a hang or a crash): where a
  build stops before its log exists.

## Code map

- `source/main.c` input, the main menu's state (shared screens: menu, About, Help, then the
  slots; an iris between the menu and the join screen, where Circle goes back while no one has
  joined), per-player state machine (join, games, saves, save options and rename keyboard,
  playing, paused, phone, patches: list, confirmation, building, cleanup question, error), main
  loop, dev tools. No game starts while a patch job runs (`rompatch_busy`, a refusal toast).
- `source/emu.c` mGBA cores: one lv2 thread per player (it also loads the game), frame
  handshake, saves (atomic writes), ROM cache (one copy per game, shared), link cable
  (`emu_link_set` from the pause menu; the player's own thread plugs/unplugs between slices;
  `LINK_CABLES` build option). GB/GBC games: mGBA's GB core (picked from the ROM header),
  interpreter and software renderer on the PPU, no JIT/SPU/cable; original GB games run on a GBC
  (its built-in colorization), SGB games as a Super Game Boy without border, 160x144 in the
  top-left of the player's 240x160 frames. The MBC3 clock is saved after the battery RAM. newlib
  gets the console's time zone (`emu_init`), so GBA cartridge clocks read local time; GBA clocks
  saved on UTC by 0.17.8 and older move to local time on load (`rtc_utc_to_local`). Only
  the heard game makes sound samples (the others skip them, invisibly to the game); it and the
  games on its cable, or in a wireless session with it ("favored"), run at thread priority 1199
  instead of 1200 and are the only ones the main loop waits for; frames are triple-buffered so the others can run late.
  `source/soundcheck.c`: the dev self-check of skipped samples.
- `source/rfu.c` + `include/rfu.h`: the GBA Wireless Adapter, one per player (`emu_wireless_set`
  from the pause menu, plugged in on the player's thread like the cable, exclusive with it): an
  mGBA SIO driver timing its own Normal 32-bit transfers, and a hub shared by the emulation
  threads as the radio (packet queues, never blocking). Protocol and design:
  `docs/wireless-adapter.md`.
- `source/spu_render.c` + `spu/render.c` + `include/spu_render.h`: mGBA's renderer on one SPU per
  player, fed by its proxy renderer's packet stream (falls back to the PPU renderer).
- `source/jit/` ARM/Thumb to PowerPC JIT (design: `docs/jit.md`): on unless
  `/dev_hdd0/ps3gba/nojit` exists (all games) or `nojit_games.txt` lists the game's file; dev
  builds self-check it against the interpreter when `/dev_hdd0/ps3gba/jitcheck` exists (first
  mismatch goes to the log), `jitprof` logs where GBA instructions run.
- `source/ui.c` all drawing (background, main menu, About credits, Help with the manual's QR
  code, join screen, carousel with its catalog tabs, saves list, save options and keyboard, phone
  QR code, patch list and its boxes (`menu_box` with a wrapped text), game frame, pause). Every
  menu ends with a legend of the buttons that do something there (`hint_row`), one row even in a
  2x2 slot at 720p (`hint_row_in` tightens the gaps when it must).
- `source/intro.c` welcome screen: the XMB icon animated (the PS3 juggles the GBAs until Start or
  Cross, then an iris closes on its wink and opens on the main menu). Under PRESS START, retro
  quotes (`quotes[]`) type in and erase in turn; bottom right, barely visible, the credit and the
  version. The same scene without its sky is the main menu's logo (`intro_draw_logo`), and its
  iris serves the menu's transitions (`intro_iris`).
- `source/gfx.c` Tiny3D wrapper: textures, batched quads, font atlas, flip.
- `source/library.c` games/covers/saves on `/dev_hdd0/ps3gba/{roms,covers,saves/<game>/<save>.sav}`:
  `.gba/.gb/.gbc` in `roms/` and its direct subfolders; a game is named by its file name (`.gba`
  dropped, `.gb/.gbc` kept, so systems never share saves). Covers: `covers/<game>.png`, else
  `covers/<header code>.png` (GBA game code e.g. `BPRE`, GB/GBC header title), else the cover of
  another game with the same code (GBA: region letter ignored, so hacks show the original's),
  else the UI draws a blank Game Pak of the system. Save lists and save rename/copy/delete run on
  its worker thread. Never overwrites a save; a save someone plays or another player is renaming,
  copying or deleting is refused (`save_taken` in main.c).
- `source/rompatch.c` ROM patching (`docs/rom-patching.md`) on a thread of its own (1250, below the
  emulators): lists the `patches/` that fit a game (UPS/BPS footer checksum against the ROM's
  CRC32, cached per file version; greyed when made for another ROM with the game's header code;
  every IPS), builds `roms/<patch name>.<base's extension>` with mGBA's patch code (temporary file
  in `tmp/`, renamed, never over a game), records it in `patches/built-games.txt`, and deletes
  the patch and a chain's intermediate ROM (recorded, same checksum, no saves) on request.
- `source/web.c` phone web server (port 8000): the player's page (`data/phone.html`, embedded)
  adds games (each checked by mGBA's header test), covers, saves (always as a new save) and
  patches (`.ips/.ups/.bps`, checked by their first bytes, into `patches/`),
  downloads saves and sets the player's name. URLs carry a per-boot token per player. Uploads
  stream to `ps3gba/tmp/` and are renamed into place whole; new games come in through
  `library_rescan()` (off-thread, swapped in between frames). Games without a cover get their box
  art from libretro-thumbnails through the phone, so the PS3 needs no internet.
- `source/share.c` state shared with the web server threads; the main loop only ever try-locks it.
- `source/audio.c` PS3 audio port feeding (the heard player's game) and the menu music
  (`data/music.ogg`, decoded with `third_party/stb_vorbis`). `source/art.c` generated sprites.
- `tests/jitprobe/` standalone app probing which memory is executable (`tools/jitprobe.sh`).
- `tests/patch/harness.c` the ROM patching check run by `tools/patch_test.sh`.

## Rules

- **Keep the repository clean**: no personal or console-specific data in anything tracked, code,
  docs, commit messages or file names:
  - no IP addresses (they go in `.env`), user names, home paths or emails;
  - no console keys or IDs (ERK, PSID, IDPS...), no instructions to dump them;
  - no ROMs, BIOS, saves, or the file names of anyone's ROM and save files (use generic titles,
    e.g. "FireRed", in docs and tool defaults);
  - personal notes go in `CLAUDE.local.md` or `.env` (not in git).
- Every user-visible change adds a `CHANGELOG.md` entry, newest first: a display change or a fix
  is a 0.0.1 bump, a major step or optimization a 0.1 step; 1.0.0 will be the first stable
  release.
- Every version bump is published: once its commit is pushed, `tools/release.sh` builds the
  release package, tags `vX.Y.Z` and creates the GitHub release with the changelog entry and
  `ps3gba-vX.Y.Z.pkg` (needs `gh` logged into the repo owner's account; before 1.0.0 they are
  pre-releases).
- Commit messages describe the change; no AI attribution lines (`Co-Authored-By` etc.).
- Update the docs with what you learn: hardware findings in `docs/ps3-homebrew-notes.md`, JIT
  changes in `docs/jit.md`, measurements in `docs/performance-plan.md`, workflow and code map
  here, the overview in `README.md`.
- No em dashes in text; plain dashes.

## Status (0.18.0)

- Runs on CFW and on PS3HEN (installed from its pkg). UI at 60 fps, saves, link cable (trades
  verified on hardware). The 0.17.8 release package was played on HEN (welcome screen, music, a
  game with sound at 60 fps).
- **4 Pokemon Unbound players at a steady 60 fps** (CFW, `tools/bench_unbound.sh`): every game at
  60 frames every second over 2 minutes, ~5.4 ms per emulator per frame; the same with all 4 on
  one cable. 4 linked FireReds: 60 each. Not measured with 4 players: battles, the Trade Center,
  a full JIT code cache. Numbers, method and history: `docs/performance-plan.md`.
- Startup: 17-18 s from pressing X on HEN, like any app there (a retail game ~15 s); ours is
  0.9 s of it, to the first frame (covers load after it). See the notes' HEN section.
- GB/GBC games: run in RPCS3 next to GBA games; not linked, not yet run on the console.
- Phone web server: works in RPCS3 (`tools/webtest.sh`), not yet tried on the console.
- Save management, link cable switch, UI drawing batching: checked in RPCS3; the link cable
  switch and the drawing counters still to check on the console.
- GBA Wireless Adapter: FireRed Union Room trades on the CFW console, 2 and 4 players at 60 fps;
  Unbound closed its Union Room (no wireless trades there). Battles, chat and LeafGreen not tried
  ([docs/wireless-adapter.md](docs/wireless-adapter.md)).
- Todo: the manual's URL for Help's QR code (`MANUAL_URL`, top of `source/ui.c`) is a placeholder.
- Next: 3-4 player UI polish, trades and battles tested with several cables.
- Planned: ROM patching ([docs/rom-patching.md](docs/rom-patching.md)), in its own branch.
