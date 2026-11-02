# PS3 homebrew notes

Hard-won knowledge from building and debugging ps3gba on a real PS3 (CFW Evilnat 4.93 CEX,
Cobra, webMAN MOD 1.47.48) and in RPCS3 on Apple Silicon. Update it whenever something new is
learned by trial and error.

## Golden rules

- **RPCS3 is not proof.** It tolerates misaligned stores to GPU memory, repeat-wrapped linear
  textures, unaligned texture pitches and CPU/GPU races that hang or corrupt real hardware.
  Every change must also run on the console.
- **Never cut power while the PS3 writes to its drive.** A hard power-off during a frozen pkg
  install corrupted the internal drive's file system ("Restore File System" failed). Recover a
  hung app with webMAN instead (see below). Hold the power button only as the very last resort.
- **Don't reinstall over an existing app with Package Manager.** A reinstall over an existing
  copy froze the console (cause not identified). On CFW, copy the app folder over FTP instead
  (see Deploying). Fresh installs work: upload the pkg to `/dev_hdd0/packages/`, then
  `http://<ps3>/install.ps3/dev_hdd0/packages/<file>.pkg` (webMAN): done in 2-5 s, XMB
  responsive (CFW twice on 2026-11-01, HEN on 2026-11-02). The installed EBOOT.BIN is ours plus
  16-32 zero bytes of pkg padding.

## Toolchain

- Docker image built from `docker/Dockerfile`: ps3toolchain (GCC 13 for PPU/SPU + PSL1GHT) and
  ps3libraries, pinned commits. Builds natively on arm64. Full build ~15 min.
- ps3libraries: `polarssl` download is dead; skip `017-polarssl` (mbedTLS replaces it).
- Mount the project at `/src`, **not `/build`**: PSL1GHT's Makefile treats a folder named
  `build` as its object dir and silently does nothing.
- Docker Desktop's file sharing can show a container a folder the host just deleted: after
  `rm -rf build`, the next build skipped its `mkdir` and failed with "can't create
  pressstart2p.ttf.o" (twice out of two). `build.sh` creates `build/` on the host first.
- Available portlibs worth knowing: Tiny3D (2D/3D with shaders), Cairo (+FreeType, PNG),
  libpng, zlib, SDL2, libcurl, mbedTLS, `pthread.h` in `ppu/include`.
- `make pkg` signs `build/<name>.elf` (the stripped copy of the linked ELF) into
  `build/pkg/USRDIR/EBOOT.BIN`, next to `PARAM.SFO` and `ICON0.PNG`.

### GCC 13 vs old libraries (real-hardware crash)

GCC 13 merges adjacent float stores into 8-byte `std`. RSX (GPU) memory is cache-inhibited and
a misaligned 8-byte store to it raises an **alignment exception** on Cell: the thread dies and
the screen stays black. RPCS3 just does the store. Tiny3D writes vertices into RSX memory this
way. Fix: build such code with `-mstrict-align -fno-store-merging` (done for Tiny3D in the
Dockerfile). Verify with `ppu-objdump -d` that no `std`/`stdx` remains in hot paths.

## Deploying and remote control

- **Title ID must be 4 letters + 5 digits** (e.g. `PGBA00001`). `PSGBA0001` installs but the
  XMB silently never shows it.
- Install by copying `build/pkg/` contents to `/dev_hdd0/game/<TITLEID>/` over FTP (webMAN FTP,
  anonymous, port 21). New folders appear in the XMB after a restart.
- webMAN MOD web commands (HTTP on port 80), all verified working:
  - `/play.ps3?<TITLEID>` launch an app. Ignored if sent while the XMB is still settling
    after an exit, while another app runs, or (silently, HTTP 200) for a folder the XMB hasn't
    registered yet: restart first. `/play.ps3/<folder path>` doesn't launch homebrew either.
  - **PS3HEN 3.5.0 (on 4.93)**: the XMB never lists a folder copied over FTP, even after a
    restart: install a pkg there (see above, and "HEN" below). A restart turns HEN off until
    re-enabled on the console, so never `/restart.ps3` a HEN console. `/play.ps3?<id>` there just
    presses X on the item under the XMB cursor. `/cpursx.ps3` shows `(XMB)` at the XMB and the
    app's title while it runs.
  - `/xmb.ps3$exit` clean exit to XMB (only works if the app's main loop still runs and
    handles `SYSUTIL_EXIT_GAME`).
  - `/restart.ps3` clean reboot, works even with a hung app. Use this instead of the power
    button.
  - `/pad.ps3?start|1|cross` virtual controller (shows up as the next free pad port).
  - `/cpursx.ps3` status page: running title, temperatures, uptime.
  - `/xmb.ps3$screenshot/<path>` did not produce a file on this build; use the app's own.
- Don't trust EBOOT size to tell builds apart; compare checksums.

## Debugging on the console

- Release builds can't be observed remotely: no log, and webMAN's `/cpursx.ps3` only shows the
  running title and temperatures (its `MEM:` figure isn't the app's memory). Checked on
  2026-11-02 that the release build boots and stays up (`RELEASE=1 tools/ps3.sh deploy`); gameplay
  needs someone at the TV.
- Dev builds (`make DEV=1`):
  - `stdout` reopened to `/dev_hdd0/<app>/log.txt` (line-buffered), read over FTP.
    Note: `freopen` only affects the calling thread; newlib gives each thread its own `stdout`,
    so other threads' `printf` goes to the TTY (visible in RPCS3's log, not on the console).
  - Scripted input file polled every 15 frames (button presses, virtual controllers, `SHOT`).
    Upload it under another name and rename it over FTP (`RNFR`/`RNTO`, done by
    `tools/ps3.sh cmd`): the app can catch a half-written file, read nothing and delete it.
  - `SHOT` dumps the displayed framebuffer (`Video_buffer[Video_currentBuffer ^ 1]`) as BMP.
    Reading RSX memory from the PPU is slow (~0.5 s for 1080p), fine for debugging.
    RPCS3 needs "Write Color Buffers: true" or the dump is black.
  - Breadcrumb logging of the first frames located crashes to the exact call.
  - **A crash (e.g. a NULL read) kills only the faulting PPU thread**: the app keeps running, the
    watchdog reports "no frame" at the last `emu_where` breadcrumb. If that spot can't block, it
    crashed there.
  - A watchdog thread logs where the main loop is stuck when no frame completes for 3 s.
  - Per-phase timings and fps once per second: essential for performance work.

## Graphics (Tiny3D on RSX)

- 2D coordinates in screen pixels: `tiny3d_UserViewport(1, 0, 0, 1, 1, w/1920, h/1080)`.
- **Linear (non-swizzled) textures can't use REPEAT wrap on real RSX**: it samples garbage
  (diagonal white flashes). Tile by drawing a grid of quads with CLAMP.
- **Texture row pitch must be aligned** (we use 64 bytes); RPCS3 accepts anything.
- **Tiny3D reuses its vertex buffer from offset 0 every frame.** When GPU-bound, the CPU
  overwrites vertices the RSX is still reading: garbage triangles and CPU/GPU stalls. Fixed
  with `docker/tiny3d-vertex-double-buffer.patch` (alternate halves per frame).
- **Each draw call costs ~0.45 ms of RSX time** (Tiny3D resets texture/shader state; an early
  hardware estimate, from the big quads of the time: probably fill more than the call). Batch
  quads sharing a texture into one polygon.
  - A whole scene can be one draw call: pack its sprites into one sheet with a white texel, and
    draw solid quads from that texel (all four UVs at its center) with the color as vertex tint.
    `gfx.c` does this for everything but game screens and covers (one 512x256 sheet: font,
    white texel, sprites, background tile, the welcome screen's sheet) and batches on its own:
    consecutive quads from the same texture are one draw call, so a menu is one call and each
    game slot two. Sprites in the sheet sample the same texels as standalone textures did
    (pixel-identical in RPCS3); never draw them smoothed (neighbors would bleed in).
  - Per draw call Tiny3D writes ~35 command words (texture, cache invalidation, vertex
    attribute bindings, draw), and a switch between solid and textured quads reloads its shader
    state (~64 words); with everything textured from the sheet there are no switches.
- **Tiny3D's command buffer: 64 KB, and the CPU waited for the RSX mid-frame** (found
  2026-11-01 by reading Tiny3D and RPCS3's `cellGcmSys`). `tiny3d_Init` sets gcm up with a 64 KB
  default command buffer, which gcm splits into 32 KB segments (the first one minus 4 KB). When
  a segment is full, gcm's callback jumps to the next one and **waits until the RSX has left
  it**. Tiny3D asks for 16 KB of room at every shader switch and 64 KB at every flip (so every
  flip jumps). With 2 segments, a frame whose commands spilled past ~12-16 KB jumped into the
  segment the RSX was still in, parked at the previous flip's wait-for-flip until the display
  refresh: the main thread blocked up to a refresh in the middle of drawing (likely the "~4 ms
  apparently waiting" of the join screen and part of the menus' `slots` time). Fix: call
  `gcmInitBody(&context, 1 MB, 4 MB, tiny_host_addr)` before `tiny3d_Init` (its init keeps
  Tiny3D's globals `context` and `tiny_host_addr` when already set): 32 segments. Dev builds
  wrap the callback (a 32-bit function descriptor {entry, TOC} in `context->callback`) to count
  the jumps and the time blocked in them (`gfx` log line). Measured in RPCS3 before the fix:
  one jump per frame blocking 16.4 ms in every scene with 2+ slots or ~16 KB of commands; after:
  none. Not yet run on hardware with the 1 MB buffer.
- **PSL1GHT's `rsxSetTimeStamp` is broken**: it stores its argument one word too far (at
  `current + 8`, then advances by 8), so the RSX reads a stale word as the report's type and
  offset (RPCS3 logged "NV4097_GET_REPORT: Bad type 0", then died on a report offset out of
  range; on hardware it could write anywhere in the report area). `gfx.c` writes the report
  itself: `NV4097_GET_REPORT` (`1 << 18 | 0x1800`), `1 << 24 | index << 4` (a zpass pixel
  count report: every report carries the RSX's timestamp, read with `gcmGetTimeStamp(index)`).
- **Never end an empty Tiny3D polygon** (`tiny3d_SetPolygon` then `tiny3d_End` with no vertex):
  everything drawn after it in the frame is lost (RPCS3, 2026-11-01: a blank line of the About
  page blanked the rest of the page): `tiny3d_End` leaves the polygon open when it has too few
  vertices, and every later `tiny3d_SetPolygon` returns busy. `gfx.c` opens a polygon only with
  its first quad. Not checked on hardware.
- **Fill is what the RSX's time goes into** at 1080p: the background alone is two full-screen
  blended layers (sky bands, the drifting tile grid: ~4.2 Mpx). Boxes draw only what shows
  (the shadow beside the opaque border, the border's ring around an opaque fill, and around a
  game screen or QR code the fill's ring), and the join screen's controller rim is one dilated
  silhouette instead of 8 shifted ones: same pixels. The dev log's `gfx` line counts the pixels
  filled and the RSX's time per frame (timestamps at the frame's first and last command).
- RPCS3's frames are pixel-identical to a plain software rasterizer that fills the pixels whose
  centers lie inside a quad and samples the nearest texel (checked on the welcome screen at
  1080p, 0 differing pixels): host-side previews of layouts and animations can be trusted for
  RPCS3, not for hardware quirks.
- **Disable depth test and depth writes for 2D.** Tiny3D enables them; the per-pixel depth
  traffic alone cost ~6 ms per 1080p frame (40 fps -> 60 fps when off).
- Tiny3D only kicks the GPU at flip; call `rsxFlushBuffer(tiny3d_Get_GCM_Context())` after
  drawing, then do CPU work (emulation) while the GPU renders.
- Pace with Tiny3D's flip only (vsync). An extra wait on `gcmGetVBlankCount()` fell out of step
  on real hardware (locked at 40 fps). In RPCS3, flip pacing alone is correct too (an earlier
  "too fast" problem belonged to a different, raw-RSX blit path).
- Raw RSX (no Tiny3D): mixing 3D-engine clears with 2D transfer blits left stale pixels in
  RPCS3; the system's command-buffer wrap hung after ~2600 frames (rewind manually).
- Fonts: a pixel font rendered once into an 8x8 glyph atlas (FreeType via Cairo, no
  antialiasing) and drawn as quads at integer scales stays crisp.
- Cairo ARGB32 is native-endian u32, which on the PPU matches RSX A8R8G8B8.

## Audio

- **Declare only the sound formats the app outputs in PARAM.SFO** (`SOUND_FORMAT`, our
  `sfo.xml`): 1 = stereo PCM. PSL1GHT's default template says 279 (also 5.1/7.1 PCM, Dolby
  Digital, DTS). With that, on the CFW console (2026-11-02) the XMB played its sounds but the app
  was silent on the TV, while the dev log showed the port consuming our blocks at real time and
  an `AUDIO` capture held normal-level audio; RPCS3 played it fine. Stereo only brought the sound
  back without a reboot. An `AUDIO` capture only proves what we write, not what reaches the TV.
- PS3 audio port: 48 kHz float stereo, 256-sample blocks, poll `readIndex` once per frame and
  keep ~6 blocks queued; pad with silence below 2. Nudge the resampling rate by up to 0.5% to
  absorb clock drift (GBA 59.73 Hz vs display 60 Hz).
- The port loops over its 16 blocks whatever we write: a block not rewritten in time plays again.
  Harmless while the ring holds silence, a stutter with music, so the menu music keeps 6 blocks
  (32 ms) queued like a game does, not just 2.
- Menu music: stb_vorbis (single file, public domain) works unchanged on the big-endian PPU with
  float output (`STB_VORBIS_NO_INTEGER_CONVERSION`, so no endian setting), ~190 KB of heap. A
  48 kHz stereo Ogg at ~170 kbps decodes in 0.09 ms per frame in RPCS3 and 0.02 ms natively on an
  M-series Mac; the PPU is likely 0.1-0.3 ms (the dev log prints it: not measured on hardware
  yet). Encode the track at 48 kHz so nothing needs resampling, and decode it block by block as
  the port asks: the hardware clock paces it, so it never drifts and loops sample-exact.
- A WAV written on the PPU needs every header field and sample byte-swapped (little-endian).
- RPCS3: the port's read index barely moves while the app only pads silence 2 blocks ahead (a
  paused game): 1.4 s of pause came out as 85 ms in an `AUDIO` capture, and a capture taken
  during a long pause never finished. With 6 blocks queued (game or music) it runs at real time.
  Real hardware not checked.

## mGBA on PS3

- Build as a static lib with a CMake toolchain (`cmake/ps3.cmake`): `-U__STRICT_ANSI__`
  (old newlib hides POSIX functions under `-std=c11`), `-DPATH_MAX=4096` (GCC's own limits.h
  shadows newlib's), and don't set `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY` (it makes
  every feature check pass).
- **Compile the app with exactly libmgba's defines** (read from its `flags.make`): struct
  layouts depend on them (`ENABLE_DIRECTORIES`, `PATH_MAX`...). A mismatch crashed at the
  first field access. **Also pass `-fwrapv`**: mGBA's cycle math assumes wrapping signed
  overflow. `lockstep.c` (compiled in the app) without it livelocked the link cable once the
  cycle counter passed 2^31 (~128 s of emulated time, ~7640 frames).
- Don't name a macro `GBA_H`: it is the include guard of mGBA's `gba.h`.
- The generic build has no file backend (POSIX one needs `mmap`): stub `VFileOpenFD`, `VDirOpen`
  and do file I/O yourself with memory VFiles.
- `COLOR_16_BIT` + `COLOR_5_6_5` output is standard RGB565 (red in the high bits).
- **Memory: lv2 gives the app ~209 MB of user memory** (syscall 352: 214016 KB total), 178 MB
  free at the main menu. On a failed `malloc` mGBA and our code just get NULL, with nothing in
  the log unless we print it. The dev log now prints free memory every second (`mem: now`) and
  per game-load step.
  - **mGBA copied the whole ROM per player** (32 MB, `_pristineCow`): at reset for any cart with
    GPIO (RTC: Emerald, Ruby, Unbound...), and when a 16-32 MB game touches AGB print, which
    Unbound does a second after starting. With 4 Unbound players the 4th JIT failed for lack of
    memory.
  - `patches/mgba/shared-rom.patch` keeps one read-only ROM shared by all players of a game:
    4 Unbound players now take 52 MB and leave 127 MB free.
- **Our mGBA changes are patch files** (`patches/mgba/`, applied to the submodule by
  `build.sh`, `ignore = dirty` in `.gitmodules`). `tools/mgba_exact.sh` proves a change exact:
  it runs pristine mGBA and ours side by side on every GBA ROM with random input and compares
  per-frame hashes of video, memory, state, sound, cycles and registers, in ~30 s. Test every
  mGBA patch with it.
- **mGBA's ROM patch code (IPS, UPS, BPS) was little-endian only** (found 2026-11-03): it read
  the UPS/BPS checksums and the IPS offsets, sizes and end marker straight into native integers,
  so on the PPU `loadPatch` refused every UPS and BPS (its own checksum never matched) and IPS
  records were misread. Fixed by `patches/mgba/patch-formats-big-endian.patch`, which also sizes
  an IPS result to its furthest record (mGBA always made 16 MB: 32 MB ROMs were cut). The host
  test can't see such bugs on a little-endian Mac or PC: `tools/patch_test.sh` also runs on s390x
  under qemu (Debian's arm64 hosts have no big-endian PowerPC cross compiler), with
  `-D__BIG_ENDIAN__`, which mGBA's byte order macros need and only PowerPC GCC defines.
- **Set the video buffer before `reset()`**, or no renderer is attached (black screen).
- Silence the default logger: it prints every DMA/BIOS call.
- Saves: hook the memory VFile's `sync` (mGBA calls it once a save has settled) and write
  atomically (temp file + rename; lv2 may refuse to rename over an existing file).
- **Game clocks: the PS3's clock runs on UTC and newlib has no time zone.** mGBA's GBA cartridge
  clock (RTC: Pokemon Ruby/Sapphire/Emerald, Unbound...) converts `time(0)` with `localtime_r`
  (`src/gba/cart/gpio.c`), so games read UTC: on the CFW console (2026-11-03, set to UTC+1)
  Unbound latched 23:08 on Nov 2 at 00:08 local time on Nov 3. The console's settings:
  `sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE)` gives minutes east of UTC (60
  there; RPCS3 gave 180), `SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME` the XMB's manual daylight saving
  time switch (0 there; read as +60 min when on, not yet seen on). **newlib takes a POSIX time
  zone**: `setenv("TZ", "LOC-1:00", 1); tzset();` at startup (the sign is west of UTC) and
  `localtime_r`/`mktime` work in local time, checked on the console: Unbound then latched the
  console's local time.
  - mGBA keeps the GBA clock in a 16-byte footer after the save (`GBASavedataRTCBuffer`): the date
    and time the game last read (BCD), the control byte and `lastLatch`, the `time(0)` it read
    them at. On load `lastLatch - mktime(date)` is the clock's offset: with the time zone set,
    footers are exact between ps3gba and mGBA on a computer in the same time zone (checked with a
    footer 2 h behind: offset 7200, the game read local time minus 2 h). Like a real cartridge, a
    saved clock keeps running across a change of the summer time switch (the switch only gives
    newlib a fixed offset, no rules).
  - Up to 0.17.8 the footers ps3gba wrote were dated UTC (at offset 0, the date equals `gmtime` of
    `lastLatch`): `rtc_utc_to_local` in `emu.c` re-dates such a footer in local time before mGBA
    loads it (player's core, the JIT check's reference core and the sound check's twin alike).
  - When a game quits, ps3gba writes `savedataClone`, which for GBA saves stops at the data: the
    footer is dropped, and that save's clock restarts on local time at its next start (offset 0).
    Only a save last written by the game itself, the app then stopped without quitting it, keeps
    its footer.
  - GB/GBC clocks (MBC3: Gold/Silver/Crystal; HuC-3, TAMA5) only add the seconds elapsed since
    the last latch to a clock the game sets, and never convert to a date: the time zone doesn't
    touch them. Their saves store `time(0)` as a Unix time for other emulators.
- Link cable (`GBASIOLockstep`, compiled from `src/gba/sio/lockstep.c`, left out of the minimal
  build; the Makefile compiles a copy with one fix, see below): **requires one thread per GBA
  whose `sleep` callback blocks.** Now done (one lv2 thread per player). Run cooperatively on
  one thread, a "sleeping" GBA keeps executing and mGBA's sync event re-fires forever inside
  `runLoop`. It looked fine for a trade, then froze two-player sessions on an in-game save
  (FireRed probes the link port in multiplayer mode in the background).
- **mGBA only sends a GBA's SIO mode to the others when it changes**: attaching a game already
  in multiplayer mode left the parent's "all ready" bit (SIOCNT bit 3) at 0 forever and both
  games waited at the cable club. We used to attach right after `reset()` for this. Now games
  plug in at any time from the pause menu: `link_plug` sets `sio->mode = -1` before
  `setPeripheral` and then calls `GBASIOWriteRCNT(sio, sio->rcnt)`, which re-reads the mode from
  the registers and announces it (and sets the child's SI bit). Verified in RPCS3: two FireReds
  plugged in at the desk see each other ("LINK 2J") and enter the Trade Center together.
- **Hot-plugging mGBA's lockstep deadlocked every GBA on the cable** (console, 2026-11-01, 3
  players starting Unbound at different times while the cable was plugged in at reset;
  reproduced in RPCS3 on the 10th staggered join): mGBA numbers the GBAs by our `requestedId`
  callback and tells no one when a number changes. By screen slot, P2 joining below the running
  P1 (parent, id 0) and P3 (id 1) moved P3 to id 2 while P1 was waiting in a hard sync for the ack
  of id 1 (`waiting=2`). P3 acked as id 2 (clears bit 2), P2 never got the sync event, so all
  three slept forever (log: `cable n=3 waiting=2`, P2 asleep at pc=4 with 0 frames done). Fix:
  link IDs follow plugging order (`link_requested_id` returns the GBA's current ID, a newcomer
  asks for the last one), so an attach never renumbers anyone; a detach only compacts the IDs,
  which mGBA handles (`waiting` cleared, DETACH events).
- **mGBA bug: a GBA left alone on its cable stalled the next one plugged in for up to 128 s.**
  `_reconfigPlayers` with one player sets the cable's clock to that GBA's raw time
  (`mTimingCurrentTime`) instead of its lockstep time (raw - `cycleOffset`), which differ for any
  GBA that wasn't plugged in first. The lone GBA's lockstep time then lags the clock by its
  offset (any value up to 2^31 cycles, raw times are per game since its reset), it no longer
  advances the clock, and a newcomer sleeps until it catches up (seen in RPCS3: ~1 s for games
  started 1.5 s apart). The Makefile builds `lockstep.c` with that line set to
  `GBASIOLockstepTime(player)` (and fails if mGBA changes it); first attach is unchanged (offset
  0). Upstream-worthy.
- Where plug/unplug may run: only on the player's own emulation thread between two `runLoop`
  slices (its core idle, like mGBA's Qt frontend interrupting a core thread), without our
  emulator lock: under the cable's mutex mGBA calls `link_wake`/`link_sleep`, which take it, so
  the lock order is cable mutex then emulator lock. Other cores keep running and may be inside
  lockstep calls: the cable's mutex orders them. Asleep or not, mGBA removes a GBA cleanly
  (`_removePlayer` wakes the new parent). A quitting game unplugs on its thread before it exits.
  A paused game keeps running while anyone plays (`is_running`), so plugged GBAs all run or all
  stop and none waits on a stopped peer. Unplugging during a link session gives the games' own
  "communication error" screen, like a real cable.
- **The GBA Wireless Adapter is an SIO driver that never blocks** (`source/rfu.c`,
  `docs/wireless-adapter.md`): no lockstep, packets queued between the emulation threads through
  a hub under an lv2 mutex, a waiting adapter polling a volatile stamp every ~1 ms of emulated
  time. On the CFW console (2026-11-03) it behaved exactly as in the desktop harness: 2 and 4
  FireReds traded in the Union Room at 60 fps, emulation cost unchanged. Its games are favored
  only while in a wireless session: favoring every plugged adapter cost the main loop ~5 ms per
  frame of waiting with 4 players, even with games that never use it.
- Pokemon keeps the link port in multiplayer mode from the title screen on, so its cable is
  effectively always active: ~70 sleep/wake syncs per frame between the emulator threads. Hence
  games start unplugged (pause menu: LINK CABLE).
- Proxy renderer (video stream for another thread/SPU, `core->videoLogger` set before
  `reset()`), quirks on our build:
  - Non-blocking logger (proxy also drives the real renderer): the core's reset initializes the
    wrapped software renderer before it was ever given VRAM/palette/OAM pointers (NULL read).
    Reset once without the logger first so mGBA attaches the renderer normally.
  - Big endian: OAM packets carry the halfword as stored (little-endian bytes read natively),
    palette packets the value. mGBA's own reader (`mVideoLoggerRendererRun`) reads packets with
    `LOAD_32LE`, so its threaded renderer can't work on the PS3 as is; we parse packets ourselves.
  - mGBA stores in `io[]` the value the renderer's `writeVideoRegister` returns. The proxy
    masked fewer registers than the software renderer (BLDCNT, BLDALPHA, BLDY, WININ, WINOUT
    kept bits the hardware drops): fixed by `patches/mgba/proxy-register-masks.patch`.

### GB/GBC games (mGBA's GB core)

- Built since 2026-11-01 (`-DM_CORE_GB=OFF` dropped from `build.sh`; an old `mgba-build/` must
  be deleted once). No struct layout of the GBA side depends on `M_CORE_GB`, so the SPU renderer
  (built with its own fixed defines) is unaffected.
- `mCoreFindVF` picks the core from the ROM header (GBA checked first). The GB core runs on its
  interpreter and software renderer on the PPU: the JIT is ARM-only and the SPU program is
  mGBA's GBA renderer.
- Model: mGBA's autodetection already runs GBC games in color and SGB-enhanced games (Pokemon
  Red/Blue) as a Super Game Boy, which colors them with the palettes the game itself sends,
  per screen; for original GB-only games we set `gb.model=CGB` and `gb.colors=GB_COLORS_CGB`
  (the GBC boot ROM's colorization table, by header CRC), like playing them on a real GBC.
- **SGB borders must be off** (`gb->video.sgbBorders = false` before `reset()`; mGBA defaults to
  on): with them the renderer writes 256x224, past the end of our 240x160 frames.
- **The MBC3 clock (Pokemon Gold/Silver/Crystal) lives after the battery RAM in the save file**,
  but mGBA's `sync` callback only passes the RAM: write the whole memory VFile (`vf->size`) or
  the clock resets. `savedataClone` (used at quit) already includes it. Checked in RPCS3
  (Crystal, new game): 32768 + 48 bytes written on the game's own save and at quit, the footer's
  timestamp the current time.
- Cost (RPCS3 only, 2026-11-01, title screens, same run): GB core 0.45-0.9 ms per frame
  (Red as SGB, Yellow and Crystal as CGB) against 3.2 ms for FireRed on mGBA's GBA interpreter
  and PPU renderer, which costs ~12.8 ms on the console: so roughly 2-3.5 ms per GB game on the
  console, not measured there yet. 4 players (3 GB/GBC + FireRed) held 60 fps in RPCS3.
- Link cable: this mGBA's GB lockstep (`src/gb/sio/lockstep.c`) is the old `mLockstep` API: the
  frontend must implement the cycle posting and waiting itself (7 callbacks, like Qt's
  `MultiplayerController`), including changing the sleeping emulator's `nextEvent` from the
  other thread and `abort()` if a wait fails; 2 GBs at most. Not done: GB/GBC games are
  unlinked (no Gen 1/2 trades yet). The GBA coordinator is the clean model to follow if mGBA
  ports GB to it.

## Threads on the PS3

- **PSL1GHT's pthread condition variables don't work**: `pthread_cond_broadcast` didn't wake
  waiters and `pthread_cond_timedwait` never timed out (deadlocks in RPCS3). Its pthread mutexes
  are fine (mGBA's lockstep only needs mutexes). Use native lv2 primitives for handshakes:
  `sysMutexCreate` + `sysCondCreate(&cond, mutex, ...)`, `sysCondWait(cond, timeout_usec)`,
  `sysCondBroadcast`. Include `<ppu-lv2.h>` before `<sys/cond.h>`.
- **lv2 schedules strictly by priority** (lower number = higher). A busy higher-priority thread
  starves lower ones completely, including a watchdog. Run worker threads *below* the main
  loop (main is 1001; workers 1200) and a watchdog above everything (900). Create workers with
  `sysThreadCreate(..., THREAD_JOINABLE, name)` and `sysThreadJoin`.
- `freopen(stdout)` is per thread with newlib: printf from other threads goes to the TTY.
- **Never do drive I/O on the main loop**: it also releases every emulator's frame, so a stall
  there stops all games. Measured: a 16 MB ROM read took 0.9-2.7 s, a cold save folder listing
  30-60 ms. Games now load on their own emulation thread, save lists on a background thread.
- **newlib's buffered `fread` is slow for big files**: a 16 MB ROM took 0.9-2.7 s; with
  `setvbuf(f, NULL, _IONBF, 0)` (one read straight into the buffer) 0.43 s.
- **Don't spin long on the PPU while waiting for an SPU**: with 3-4 emulator threads on 2
  hardware threads a spinning thread holds one the others need. Yielding (`sysThreadYield`)
  after 20 us of spinning took 4 booting games from 46-51 fps to 59-60.
- Don't hold your own lock while calling into a library that can call you back (mGBA's
  lockstep calls sleep/wake from `setPeripheral`): non-recursive mutex, instant self-deadlock.
- **Changing a thread's priority**: PSL1GHT's `sysThreadSetPriority(id, prio)` /
  `sysThreadGetPriority(id, &prio)` (lv2 syscalls 47/48, `sys/thread.h`); the calling thread's id
  from `sysThreadGetId`. RPCS3: the new priority reads back (dev log `emu: P<n> thread
  priority 1199 (wanted 1199)`); not yet checked on the console.
- **`sysThreadYield` only yields to threads of the same or a higher priority** (lv2's yield
  semantics, RPCS3's too; not measured on the console): a thread one step above the others that
  spins with yields keeps its hardware thread. The heard game's thread (1199) does that while
  waiting for its SPU at the frame end (`wait_pending`, ~0.15-0.9 ms per frame with 2-4 players,
  ~2.3 ms alone): accepted, it is the game we want first.
- Our lv2 mutex (`sysMutexAttrInitialize`) is `SYS_MUTEX_PROTOCOL_PRIO`: waiters wake by
  priority, **no priority inheritance**. A favored thread can wait on a lower one holding
  `emu_lock`, but only for a few-microsecond critical section, and only while main or another
  favored thread holds both hardware threads; every favored thread blocks at least once per frame
  (and ~70 times per frame when linked), so that wait stays short. mGBA's cable mutex is only
  shared by threads on the same cable, which all have the same priority.
- **The PPU is one core with 2 hardware threads (SMT).** Two emulator threads ran at ~14 ms per
  frame each in-game (vs ~10.5 ms alone): ~1.5x combined throughput, not 2x. Enough for 2
  players at 60 fps. **JIT code gains less**: 5.85 ms alone -> 9.3 ms each with 2 linked players
  (~1.25x combined; renderer 2.15 -> 3.6, translated code 3.7 -> 5.7). Probably because dense
  generated code leaves fewer stall cycles for the other thread to fill (not verified).
- PSL1GHT's pthread mutexes are pthreads-embedded on lwarx/stwcx atomics with **no memory
  barriers** (and `pte_osAtomicExchangeAdd` returns a stale value). Not proven to break
  anything yet (replacing them with lv2 mutexes did not change the link stall), but suspect.

## Performance on real hardware (PPU, one thread)

- **JIT (2026-11-01, CFW, FireRed at the cable club desk, same dev build and save, `nojit`
  file for the interpreter)**: 1 player 11.2 ms -> 5.85 ms per frame (renderer ~2.2 ms in both:
  CPU emulation 8.9 -> 3.7 ms, 2.4x). 2 linked players: interpreter 15.1 / 14.4 ms per player
  and 16.6 ms of frame work (borderline 60 fps); JIT 9.3 / 9.4 ms per player (renderer 3.6:
  SMT sharing), 60 fps with the main loop waiting only 0.25 ms. Run-to-run spread ~0.05 ms.
  Details: `docs/jit.md`.

- mGBA (FireRed): ~10.5 ms per emulated frame, peaks ~16 ms. One player: 60 fps.
- Two players on two threads (FireRed + Emerald, both in game): **60 fps**, ~14 ms per player
  per frame, main loop waits only ~0.25 ms. The earlier "~29.5 fps" was the link cable
  deadlocking (both games enter multiplayer mode at their title screens, the cable plugs in,
  then both emulators stall and the main loop hits its 33 ms timeout every frame).
- Frame split (dev log `renderer`, timed around mGBA's `drawScanline`): the software renderer
  is only ~1-1.5 ms of FireRed's ~10.5 ms (up to ~4 ms in busy scenes); the ARM interpreter
  dominates. Moving it to the SPUs still saved 2.3 ms per frame (12.76 -> 10.46 ms at the desk,
  see the SPU section and the performance plan); the CPU core stays the main target.
- Two FireReds both in multiplayer mode keep the cable plugged and sync every ~4096 cycles;
  in RPCS3 that alone dropped to ~29 fps (lv2 thread switches are slow there).
- Compiler options for libmgba barely matter (1 player, FireRed standing at the cable club
  desk, run-to-run noise ~0.05 ms): `-O3` 12.08-12.11 ms, `-O2` 12.54, `-O3 -funroll-loops`
  12.16, `-Os` failed to build. **PGO: 12.20 ms, no gain** (so it was removed again). The
  interpreter's cost is indirect calls and memory traffic, not branch layout.
- How PGO works here, if ever needed again: the PS3 `libgcov` is freestanding (no file I/O,
  no `__gcov_exit`, no value/indirect-call/time profilers). Build with `-fprofile-generate
  -fprofile-info-section=gcov_info -fprofile-update=single -fno-profile-values
  -fno-profile-reorder-functions`, link `-lgcov`, walk `__start_gcov_info..__stop_gcov_info`
  and write each `__gcov_info_to_gcda()` stream to `<dir>/<path with / as #>`; that is the
  name `-fprofile-use=<dir>` looks for. Clean objects when switching modes.
- 2 linked FireReds in game (standing at the desk): each emulator ~15.5-16.5 ms per frame
  (12.1 alone: SMT sharing), 54-58 fps; this benchmark varies ~±2 fps between runs. The link
  cable is cheaper than it looks: mGBA's lockstep only sleeps the secondary ~2.5 times per
  frame (0.5 ms blocked) and the primary once per hard sync (every ~1.9 frames, ~2 ms per
  frame blocked waiting for the secondary). Polling up to 0.3 ms before an lv2 sleep (with
  `or 1,1,1` low SMT priority) did not help: it steals issue slots from the other emulator.
  In the cable club rooms (active transfers) the two GBAs mostly take turns (~45-47 fps).
- So 2 players at a solid 60 need cheaper emulation per thread, not a cheaper link.
- **VBA-Next is slower than mGBA on the PPU**: 16.6 ms per frame vs 12.1 ms (FireRed, same
  scene, libretro core built with its psl1ght target, `-mcpu=cell`, ROM loaded from memory).
  Its reputation as the fast console GBA core doesn't hold here. Build notes if retried: its
  static build omits libretro-common (archive it separately without `rthreads` and the ARM
  `neon_memcpy.S`); libretro's VFS for `__PSL1GHT__` wants `lv2/sysfs.h`, which this PSL1GHT
  lacks, so use `-DLOAD_FROM_MEMORY`. All state is global: one instance per process.
- Budget for 4 players at 60 fps: 2 SMT threads give ~1.5x one thread, so ~25 ms of
  single-thread time per frame for everything: each emulator must cost ~6 ms (3 players:
  ~8 ms). mGBA is 12.1 ms (9.6 without its renderer; 10.5 with the SPU renderer, other save).
- UI (Tiny3D, after the fixes above): ~0.4 ms CPU per frame at 1080p for 2 game slots. Empty
  join slots make the UI phase ~4 ms, apparently waiting (not CPU), not yet investigated.
- PPU writes to RSX memory are fast; reads are very slow.
- **JIT + SPU renderer together (2026-11-01, merged main, FireRed at the cable club desk)**:
  1 player 3.86 ms of emulation per frame (12.76 with neither), 2 linked players 60 fps at
  6.0/6.5 ms each, **4 linked players 60 fps** at 7.3-9.5 ms each (wall time), the main loop
  waiting 2.5 ms per frame. Details in `docs/performance-plan.md`.
- With the JIT, a frame's emulation can finish before its SPU has drawn it: the SPU needs
  ~4.2 ms per frame (`-Os`) and the JIT skips idle loops, so the video stream arrives early. The
  emulation thread then spins at the frame end (~2.3 ms per frame with 1 player, 0.15-0.9 ms
  with 2-4 players), at low SMT priority (`or 1,1,1`) but still holding a hardware thread.

## Network (phone web server)

- **Ports taken by webMAN MOD** (it keeps running while an app runs): 80 (HTTP), 21 (FTP),
  7887 (PS3MAPI), 38008 (NETSRV), per its `main.c` (`WWWPORT`, `FTPPORT`, `PS3MAPIPORT`,
  `NETPORT`). The app serves on 8000.
- PSL1GHT: BSD sockets (`socket`/`bind`/`listen`/`accept`/`recv`/`send`, `close` works on
  sockets) from `-lnet`; `netInitialize()` loads `SYSMODULE_NET` itself (hence `-lsysmodule`).
  netctl (`-lnetctl`) needs `sysModuleLoad(SYSMODULE_NETCTL)` before `netCtlInit()`. The IP:
  `netCtlGetState()` == `NET_CTL_STATE_IPObtained`, then
  `netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info)` gives `info.ip_address` as a string.
  `struct sockaddr_in` has a BSD `sin_len`.
- Server design that worked in RPCS3 (2026-11-01; **hardware not tested yet**): one thread
  refreshes the IP every 2 s, one blocks in `accept()` on `INADDR_ANY:8000`, one detached thread
  per connection (at most 4; more wait in the listen backlog), all at priority 1300 (below the
  emulators). `SO_RCVTIMEO`/`SO_SNDTIMEO` with a `struct timeval` dropped a stalled upload after
  the 10 s set. No `poll()`/`select()` needed.
- **RPCS3 networking is off by default** ("Internet enabled: Disconnected": netctl never reports an
  IP). `NET=1 tools/rpcs3.sh run` writes a per-run config with "Connected" (the shared config.yml
  stays untouched). netctl then reports the Mac's LAN IP and the app's sockets are real host
  sockets: the server listens on the Mac's port 8000, so curl to `127.0.0.1:8000` works.
- RPCS3: a ROM uploaded over loopback (16 MB at ~350 MB/s, the low-priority thread never
  blocking) made one frame take 34 ms while a game ran; the same upload at 4 MB/s (Wi-Fi speed)
  caused no hitch at all, rescan included. Real lv2 preempts by priority, so this is likely
  RPCS3's scheduler; check on the console with a wired upload.
- **Close with a lingering close**: refusing an upload too big to drain and then `close()`-ing
  with its bytes unread resets the connection, and curl sometimes lost the 413 reply (status 000,
  `tools/webtest.sh` "rom too big (no Expect)"). Now `shutdown(SHUT_WR)`, then discard incoming
  data for up to 2 s (250 ms receive timeout) before `close()`: 3 runs out of 3 clean.

## SPU programs

- `spu-gcc` is GCC 9.5 (GCC dropped the SPU backend in 10). `spu_intrinsics.h`/`spu_mfcio.h`
  come with it; PSL1GHT adds `$PSL1GHT/spu/include` (`sys/spu_thread.h`: `spu_thread_exit`,
  link `-L$PSL1GHT/spu/lib -lsputhread`).
- **`spu-gcc` doesn't define `__BIG_ENDIAN__`** (only `__BYTE_ORDER__`). mGBA's `LOAD_16LE` and
  friends then take their little-endian path and read video memory unswapped: pass
  `-D__BIG_ENDIAN__`.
- PSL1GHT's SPU `crt1` passes the thread's `arg0`-`arg2` as `main`'s three `uint64_t` parameters.
- Embedding: build the SPU ELF from the Makefile, turn it into an object with the `%.bin.o`
  rule (`bin2o`, from `data_rules`), then `sysSpuInitialize(6, 0)`,
  `sysSpuImageImport(&image, elf, SPU_IMAGE_PROTECT)`, one thread group per SPU thread
  (`sysSpuThreadGroupCreate`, `sysSpuThreadInitialize`, `sysSpuThreadGroupStart`).
- Waking an SPU without polling: `mfc_getllar` the PPU-written 128-byte line, check it, then
  `spu_readch(SPU_RdEventStat)` with `MFC_LLR_LOST_EVENT` enabled blocks until a PPU store to that
  line cancels the reservation. Loop: wakeups can be stale.
- **RPCS3 compiles SPU code on first use** (LLVM, "Building function..." in its log), in bursts
  of up to ~0.22 s, again on every launch of a bare SELF. The renderer's old 200 ms "SPU dead"
  timeout fired at game start (the SPU had processed only the reset packet) and froze the
  player's screen in most runs; it is 2 s now. Making the PPU's wake-up store atomic
  (`lwarx`/`stwcx.`) did not help: the wake-up was never the problem.
- mGBA's software renderer in local store (256 KB): code 101 KB at `-Os` (120 KB at `-O2`: too
  big), VRAM copy 96 KB, renderer state 28 KB (18.5 KB of it its per-line register cache),
  ~238 KB in all, leaving ~23 KB of stack.
- MFC DMA: sizes of 1, 2, 4, 8 or a multiple of 16 bytes, at most 16 KB per transfer, both
  addresses 16-byte aligned for 16+ bytes (128 is fastest). Everything the SPU renderer touches in
  main memory (ring, control/status lines, frame buffers) is `memalign(128)`; a 480-byte line and
  the 16-byte packets keep every transfer 16-byte aligned.
- The SPU decrementer (`SPU_WrDec`/`SPU_RdDec`, counts down) ticks at the PPU timebase frequency
  (`sysGetTimebaseFrequency()`), so SPU and PPU timings convert the same way.
- Apps get 6 SPUs (`sysSpuInitialize(6, 0)`); the renderer uses one per player slot (4), all
  started at boot and sleeping when idle, so 2 are left.

## Executable memory (for a dynarec)

Probed with `tests/jitprobe` (`tools/jitprobe.sh`, one probe per launch). **Identical results on
Evilnat 4.93 CFW and PS3HEN 3.5.0** (4.93 CEX):
- **The app's own `.text` is writable and executable from user mode**: `memcpy` code into it,
  `dcbst`/`sync`/`icbi`/`isync`, jump: works. So a dynarec can keep its code cache in a region
  reserved inside `.text` (e.g. an asm `.rept`/`.space` block in `.section .text`).
- Not executable (the call kills the app): heap (`memalign`), `.bss`, `sysMemoryAllocate`
  64 KB and 1 MB pages. The console survives these crashes; the app just exits.
- `sys_memory_get_page_attribute` (syscall 351) reports the same `0x40000` (read-write) for
  `.text`, heap, stack and lv2 memory: it says nothing about execute rights.
- Call generated code with `mtctr`/`bctrl` (inline asm), not a C function pointer: PSL1GHT is
  64-bit ELFv1, where a function pointer points to a descriptor, not to code.
- Code cache size (`CAVE_MB=4|8|16|32 tools/jitprobe.sh ...`, probe 7 writes, runs, rewrites and
  reruns code at the middle and the end of the cave): the SELF is zlib-compressed, so a zero-filled
  cave costs almost nothing in the EBOOT (80 KB at 4 MB, 84 KB at 8 MB, 92 KB at 16 MB, 108 KB at
  32 MB); the ELF grows by the full size. In RPCS3 all sizes work (see below). **CFW console
  (2026-11-01): 4 MB and 16 MB caves with the plain R-X code segment both work** (code at the
  middle and the end runs, rewrites are seen; launch to results 11.0 s either way, so no
  visible load-time cost). The one HEN attempt (32 MB cave **and** an RWX code segment at once)
  hung that console, see "HEN: what killed it" below: so it was the RWX flag, the 32 MB size or
  HEN itself, still unknown (not retried: not needed).

### PowerPC pitfalls met writing the JIT

- `addi`/`addis` and every D-form or indexed load/store read rA = r0 as the constant 0:
  `addi r0, r0, 1` is `li r0, 1`. Use another scratch register.
- `lis` sign-extends: a constant >= 0x80000000 built with `lis`/`ori` is negative as a 64-bit
  value. Fine for 32-bit arithmetic and `stw`, wrong as an address: build addresses with
  `li 0; oris; ori` (our heap and stack are at 0x1xxxxxxx-0xDxxxxxxx).
- ELFv1 function pointers point to descriptors `{entry, TOC, env}`: call generated code with
  `mtctr`/`bctrl`, and from generated code call C with `bl` to `descriptor[0]` (r2 stays the
  TOC). GCC dropped static functions whose only use was a cast to `void*` (undefined `.opd`
  symbols at link time): make them global.
- GCC's clobber names for FP registers are `fr0`..`fr31` (not `f0`); vector registers `v0`...
- C callees may store LR at 16(r1) and CR at 8(r1) of the caller's frame: code called from C
  that calls C itself needs a real ABI frame (112-byte minimum + saves, 16-byte aligned).
- **Load-hit-store costs far more on the PPE than instruction counts say** (2026-11-02, CFW):
  storing registers to memory and reading them back soon after (syncing GBA registers through
  `cpu->gprs` and `cpu->cycles` around a C call) stalls the pipeline each time. Doing
  LDM/STM/PUSH/POP inline instead of through mGBA was estimated at 0.3-0.6 ms per emulator per
  frame from counts and gained ~2 ms on the console. RPCS3 shows none of it.

### RPCS3 and self-written code (0.0.43)

- **RPCS3 maps a segment without the ELF W flag read-only**: writing `.text` kills the app with
  `VM: Access violation writing location 0x812000 (read-only memory)`. Real hardware allows it.
- Marking the code segment RWX (patch `p_flags` of the first program header to 7 after linking:
  `printf '\000\000\000\007' | dd of=x.elf bs=1 seek=68 conv=notrunc`, see `WRITABLE_TEXT=1` in
  `tests/jitprobe/Makefile`) makes the writes work, and then **both PPU decoders run rewritten
  code correctly**: "Interpreter (static)" and "Recompiler (LLVM)" returned 42, then 43 after a
  rewrite + `dcbst`/`sync`/`icbi`/`isync`, at the middle and the end of a 16 MB cave.
- **Use the LLVM recompiler for the JIT, not the PPU interpreter.** With the real JIT (megabytes
  of code written into the RWX segment), RPCS3's "Interpreter (static)" crashed with jumps to
  address 0 in unrelated C code (a `malloc` on the emulation thread, then `emu_frame_end` on the
  main thread), differently each run; the same build runs correctly under LLVM, self-check
  included. The interpreter only survived the probe's two-word rewrite.
- PPU decoders in 0.0.43: only `Interpreter (static)` and `Recompiler (LLVM)` ("Interpreter
  (dynamic)" is rejected as an invalid enum for the PPU). `rpcs3 --config <file> <self>` forces a
  config for a CLI boot (`PPU=interp|llvm tools/rpcs3.sh run` writes one).
- The RPCS3 GUI empties `/dev_hdd0/tmp` at startup (global "Empty /dev_hdd0/tmp/: true"), before
  any `--config` override applies: test apps must keep their state elsewhere (the probe now uses
  `/dev_hdd0/jitprobe.txt`).

### HEN

- **Install apps from a pkg.** On 2026-11-01 test builds ran inside another installed app's
  folder (its EBOOT swapped in, since FTP-copied folders never show in HEN's XMB): there ps3gba
  never ran one instruction (no boot trace) and the console dropped off the network ~2 minutes
  later, every time. A bisection with `tests/jitprobe` pointed at importing cellAudio, but that
  was the borrowed folder: an app with the same cellAudio imports runs on HEN (Hermes' SPU
  soundlib imports the same 7 functions), and ps3gba installed from its own pkg runs (below).
- **ps3gba runs on HEN** (2026-11-02, dev build 0.17.6 installed from its pkg with webMAN, first
  launch with the controller after enabling HEN): every startup step, welcome screen at 59.9 fps,
  178 MB free as on CFW; FireRed on the interpreter 9.0 ms per frame with the SPU renderer and
  sound, then on the JIT 2.9 ms per frame (its 16 MB code area in the R-X segment works there).
- **No webMAN requests while HEN loads an app.** Two launches hung (webMAN listed the app with
  pid 0 and no process, our code never ran, the console dropped off the network ~1 minute later):
  once with FTP reads of the log during the load, once with only `/cpursx.ps3` polled every
  second. Launches left alone all worked (2026-11-02): from the controller after enabling HEN,
  after a power cycle and after `/xmb.ps3$exit`, and `/play.ps3` after an exit with 45 s of
  silence. The very first launch survived polling every 4 s: it is likely a race with whatever
  webMAN serves at some moment of the load. Wait ~30 s after a launch before any request.
- **Startup on HEN** (dev build, `ps3gba_boot.txt`, from the first instruction): first frame at
  2.57 s, of which the game library scan 1.76 s (5 games: decoding and scaling their cover PNGs
  with Cairo), graphics init 0.51 s, opening the dev log 0.15 s. With the covers decoded on a
  thread after the first frame (0.17.8): first frame at 0.90 s (library 0.22 s: the ROM headers;
  graphics 0.44 s, mostly rendering the font atlas with FreeType).
- **Launch times on HEN are the console's** (stopwatch, X to first screen, 2026-11-02): ps3gba
  17-18 s, a retail game (inFamous: Festival of Blood) ~15 s, multiMAN longer still. A ps3gba
  built without the JIT's 16 MB code area (code segment 3.9 MB instead of 20.7 MB) took the same
  17-18 s: the size of what the XMB loads isn't the cost, and our own part is under 1 s.
- A 32 MB code cave with an RWX code segment (`tests/jitprobe`, two changes at once) hung HEN;
  4 MB R-X runs and rewrites code fine. **On HEN change one thing per launch.**
- After a restart, HEN's webMAN can take minutes to answer.

## Phone page (data/phone.html)

- **A top-level `const top` (or any name of a non-configurable `window` property: `top`, `window`,
  `document`, `location`) is a SyntaxError in every browser**, so the whole script never runs:
  the file picker still opens (plain HTML) but nothing happens after choosing a file, the games
  list stays on LOADING (seen on the user's phone, 2026-11-01; `tools/webtest.sh` only uses curl
  and could not catch it). Test the page in a browser: RPCS3 with `NET=1` serves it at
  `http://127.0.0.1:8000/<token>/`; a fake file can be fed to an input with `DataTransfer`.
- libretro-thumbnails (`raw.githubusercontent.com/libretro-thumbnails/<system>/master/
  Named_Boxarts/<No-Intro name>.png`, `&*/:`<>?\|"` replaced by `_`) answers with
  `Access-Control-Allow-Origin: *`, so the page can fetch box art and send it to the PS3. Dual
  GB/GBC games are in the Game Boy Color collection, SGB/CGB-enhanced GB games in Game Boy.

## Recovery

- App hung: `/restart.ps3` via webMAN (CFW; on HEN it turns HEN off: power-cycle there instead).
- XMB freezing at boot after a bad install: Recovery Mode (hold power until the double beep,
  USB-connected controller) > Restore File System, then Rebuild Database.
