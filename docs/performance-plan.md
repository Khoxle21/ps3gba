# Performance plan: 4 players at 60 fps

Handover document for the performance work. Measurements and hardware facts live in
`docs/ps3-homebrew-notes.md`; this file holds the model, the decisions so far and the plan.
Update both as phases land.

## Where we are (2026-11-02, real PS3, 4 Pokemon Unbound)

**4 Unbound players hold a steady 60, unlinked or all on one cable**: every game at 60 frames
every second over 2 minutes, display 59.92 fps (min 59.9), ~5.4 ms per emulator per frame.
How it got there and the numbers: "4 players on Pokemon Unbound" below. FireRed (lighter) also
holds 60 with 4 linked players at the cable club desk.

## Where we were (2026-11-01, real PS3, FireRed)

**4 players linked run at 60 fps with both the JIT and the SPU renderer** (main after merging
both, CFW console, FireRed at the cable club desk, `tools/bench.sh`, dev build, same saves):

| Setup | Before (interpreter, PPU renderer) | Both (JIT + SPU renderer) |
|---|---|---|
| 1 player, emulation ms/frame | 12.76 | 3.86 (+ ~2.3 waiting for the SPU, see below) |
| 2 players linked, fps (ms per emulator) | 51.1 (17.1-18.0) | 60 (6.0 / 6.5) |
| 4 players on one cable, fps (ms per emulator, wall time) | 28.9 | 60 (7.3-9.5) |

- 4 players: the main loop still waits 2.5 ms per frame for the emulators (frame work 9.3 ms);
  link blocked 1.7-5.9 ms per frame per player; one dip to 57.1 fps in 40 reports, otherwise
  59.9-60.0.
- 1 player: the emulation thread now finishes its frame before the SPU has drawn it (the SPU
  needs 4.17 ms per frame, and the JIT skips the idle loops, so a frame's video stream arrives
  early) and spins ~2.3 ms per frame at low SMT priority in `wait_pending`. With 2-4 players the
  wait shrinks to 0.15-0.9 ms (the threads share the PPU and finish later). If it starts to
  matter: stop waiting at the frame end (a third frame buffer) or speed up the SPU code.
- Code cache: each player's 4 MB slice holds 3.8-4.0 MB at the desk, 0 flushes so far: near
  full (open item in `docs/jit.md`).
- Correctness of the merged build: `RENDER CHECK` + `jitcheck` (chain), 1 player, ~4 min (boot,
  title, desk, random walking): 0 of 15,077 frames differ, 0 JIT mismatches (~200K block
  comparisons per second). Saves byte-identical afterwards.
- Not measured with both on: the Trade Center scene (busy cable), 3 players.

### 4-player test run (2026-11-01, CFW, dev build, 4 FireReds unless noted)

Boot to the desk with `tools/bench.sh 4`, then all four into the cable club Colosseum (Multi
Battle room), plus the user's manual session (Emerald, FireRed, Ruby, Sapphire).

| Found | Cause | Status |
|---|---|---|
| UI and every game froze ~1 s when a player started a game | `emu_start` read the 16 MB ROM (0.9-2.7 s buffered) and built the core on the main loop | **Fixed**: loads on the player's own thread (`PS_LOADING`, "LOADING" on the save row); ROM read unbuffered: 0.43 s, whole load 0.55 s |
| 30-60 ms hitch opening a game's save list | first `readdir`/`stat` of the folder on the drive, main thread | **Fixed**: read by a background thread (`library_list_saves_async`) |
| 46-51 fps for ~10 s while 4 games boot (title screens) | each SPU drew ~8 ms per frame there and each emulator busy-waited 2-3 ms per frame for it, holding a PPU hardware thread the other emulators needed | **Fixed**: `wait_pending` yields after 20 us; boot now 59-60 fps, desk per emulator 8.6 -> 7.9 ms |
| 43-53 fps for 1-3 s when all four press Continue together | link lockstep: 500-2400 sleeps per second per player (normally 30-150) while the games probe the cable | Open (one-off, only when simultaneous) |
| ~40 fps for ~5 s while all four save in game | JIT: per player 15K blocks invalidated and retranslated per second, 700K instructions interpreted (14x normal): STM invalidated one word too many, killing the flash routine copied right above a push | Fixed in RPCS3 (15K -> 48 invalidations/s), not measured on hardware yet: see `docs/jit.md` |
| 55 fps in the Colosseum room with 4 linked players | link lockstep: 1100-2200 sleeps per second per player, 5-11 ms blocked per frame, emulators 7-12 ms (wall) | Open: the next 4-player bottleneck |
| P1's code cache full (4029 of 4096 KB) in the Colosseum | 4 MB slice per player | Open: see `docs/jit.md` |

- Translation itself is cheap: 6.5 us per block on average (2.5 us during the save storm).
- Menus on the console after the new UI (2026-11-01, CFW, no game running): `slots` 3.7 ms and
  `begin` 0.5 ms of main thread per frame (the old UI cost ~0.4 ms), menu music decoding 0.40 ms
  per frame (stb_vorbis on the PPU; estimated 0.1-0.3). 60 fps, but worth profiling before 4
  players play from the menus' screens.
- Phone upload over wired LAN: a 4 MB ROM in 0.67 s (6.3 MB/s), picked up without a restart,
  frames held 60 fps (max frame work 7.45 ms during the rescan).
- The in-game LOADING row and the save list were checked on screen.

### 3 players on Pokemon Unbound (2026-11-01, CFW, read-only from the dev log)

The user and friends playing Pokemon Unbound v2.1.1.1 (a 32 MB FireRed hack, code `BPRE`) on 3
linked slots, JIT + SPU renderer, dev build. 900 one-second reports analysed from `log.txt` over
FTP, nothing sent to the app.

| Measure | Value |
|---|---|
| fps | avg 56.9, median 58.0, 10th percentile 52.3, min 47.6; 60 in 38% of seconds |
| frames > 33 ms (visible hitch) | in 62 of 900 seconds (~one every 15 s); per-emulator worst frames 45-53 ms |
| per emulator | 7.9-8.6 ms per frame (90th percentile ~10.5) |
| main loop | 15.2 ms of work, 11.9 of it waiting for the emulators; `slots` 3.0 ms, `flip` 2.4 ms |
| JIT, per player per second | ~8.2-8.8K blocks translated (110-124 ms), ~3.6K invalidated, 1.5 full flushes, cache at 4 MB, 0 idle iterations skipped, ~25K interpreted |
| SPU renderer | 4.26 ms of drawing per frame; emulators waited 2.0-3.7 ms per frame for it |
| link cable | P1 2.35, P2 2.19, P3 6.73 ms blocked per frame (max 14); P2/P3 ~400 lockstep sleeps/s |

- **The JIT's code churn is what costs the frames**: seconds without a flush run at 59.8 fps,
  seconds with 3+ flushes (all players) at 56.0; seconds translating under 6K blocks at 59.8,
  over 30K at 54.7. Unbound invalidates ~3.6K blocks per second per player (FireRed at the desk:
  ~0), the dead code fills the 4 MB slice, and each full flush retranslates everything.
  **Cause and fix (2026-11-01, RPCS3)**: Unbound's sound mixer (ARM code in IWRAM) patches its
  own unrolled resampling loop (0x03006214-0x03006348) with one of three opcode templates
  whenever the next channel needs another one, ~100 times per second, and each rewrite killed
  ~50-80 blocks. Dead translations are now kept and revived when their code comes back
  (`docs/jit.md`, "Self-modifying code"). Same overworld spot, 1 player, per second:

  | Build | translated | invalidated | revived | flushes | idle skipped | interpreted |
  |---|---|---|---|---|---|---|
  | before | 11.4-17.6K | 4.1-8.3K | - | 2-3 | 0 | 44-50K |
  | after | 1-18 | 4.4-8.1K | = invalidated | 0 | 0 | 44-50K |

  Title screen before: 17.5-18.8K translated, 10.6-11.4K invalidated, 4 flushes per second.
  Cache after: ~14,600 live blocks in 3.7 MB (the three templates' blocks ~360 KB of it), so
  new areas may still fill the 4 MB slice now and then (a bigger cache helps there). Each
  rewrite still costs ~16 forced exits and ~50-80 kills + revivals (~3K page bit rebuilds per
  second, no translation). To measure on the console: the `jit P<n>` line (translated ms,
  flushes) and fps with 3 players on Unbound, against the table above.
- **No idle loop to skip in Unbound** (FireRed: ~43K instructions per frame skipped), and none
  needed: its `WaitForVBlank` halts the CPU with BIOS `Halt` (`swi 0x02`) until the VBlank flag
  is set, and mGBA jumps to the next event while halted.
- **The link cable costs up to ~7 ms per frame although nobody was trading**: all slots are
  plugged into one lockstep coordinator at reset and the games keep it busy. **Done (2026-11-01,
  RPCS3)**: games start unplugged (no lockstep, 0 sleeps in the log) and each player plugs in
  from the pause menu (LINK CABLE: ON/OFF). Not measured on the console yet: expect the ~3.5 ms
  blocked per frame per player to go to 0 while nobody is plugged in.
- **Hitches (> 33 ms frames) are not explained by flushes** (same flush rate in hitch seconds);
  needs per-frame timing to find (e.g. a dev ring buffer of the slowest frames with their phase
  times).
- **The UI costs 3.0 ms (`slots`) + 2.4 ms (`flip`) of main thread per frame with 3 games**,
  against ~0.4 ms before the new UI; profile the draw calls. **Found and fixed**: mostly
  waiting, not drawing (see "UI drawing" below).
- The SPU renderer is on the critical path at the frame end (2-3.7 ms waited per frame):
  a third frame buffer would let the emulator start the next frame instead of waiting.
- Unbound's first load took 865 ms (557 ms reading the 32 MB ROM); the two other players reused
  the cached ROM (0 ms). Its cover is FireRed's through the header code, but the phone page shows
  none (`GET cover` serves only `covers/<name>.png`).

- **After the JIT revival fix (same 3 players, same game, 5 minutes)**: 55.3-59.1 fps per minute
  (was 56.9 average, 47.6 at worst), 1-482 blocks translated per player per second (was ~8.2K),
  0.00-0.07 flushes (was 1.5), 3.8-7.8K kills and revivals per second (the mixer's rewrites),
  emulators 8.7-10.6 ms per frame, link cable 3.5 ms blocked per frame, no frame > 33 ms in 4 of
  5 minutes. What's left below 60: the kill/revive churn and the cable.
- **Kill/revive churn cut ~12x (2026-11-01, RPCS3)**: 95% of the blocks each rewrite killed were
  partial copies of the mixer's loop, translated where translated code resumed after an event
  (mid-loop). The dispatcher now enters the live block containing that pc at its next segment
  check instead (interpreting ~0.5 instructions on average to get there), and stores invalidate
  each page once (`docs/jit.md`, "Resuming mid-block"). 1 player standing on the overworld, per
  second:

  | Build | killed = revived | forced exits | code-bit halfwords rebuilt | links + unlinks (icache flushes) | interpreted | dispatches | cache |
  |---|---|---|---|---|---|---|---|
  | before | 5,037 | 1,111 | 1.02M | 10.9K | 51.7K | 108.6K | 3,742 KB |
  | after | 412 | 260 | 57K | 0.9K | 62.2K (19.1K resumes) | 103.3K | 3,371 KB |

  Random play: 5,350 -> 457 kills, 449 -> 312 translations per second. FireRed standing on the
  overworld (it rarely writes code): 1.3K resumes per second, interpreted 52.6K -> 53.1K,
  dispatches and idle skipping unchanged. To measure on the console with 4 players on Unbound
  (was 40-47 fps, emulators 8.7-9.4 ms per frame): fps, per-emulator ms, and in the `jit P<n>`
  lines invalidated/revived (expect ~0.4-0.5K per player per second instead of 3.3-7.8K),
  resumed and interpreted; then FireRed, 4 players at the desk, for regressions (59.5 fps
  before).
- **Audio cuts below 60 fps** (reported by the players): the heard game makes ~7% fewer samples
  at 56 fps while the output rate could only bend 0.5%, so the 32 ms queue ran dry and got padded
  with silence. Now the rate bends up to 5% (smoothed over ~0.3 s: a slowdown lowers the pitch a
  little instead of cutting the sound) and the queue targets 43 ms; the dev log reports the
  silence padded per second (`audio:`). RPCS3: queue settles at 8.6-10.9 blocks, 0 ms padded.
  Console, 3 players on Unbound: 0 ms padded every second, queue 8.0-9.2 blocks, rate bent
  0.1-0.7%.
- **Lockstep deadlock at game start (console, 2026-11-01 19:3x)**: three players starting Unbound
  at different moments, cable plugged at reset as always: P2's core slept at its first
  instruction (pc=4, 0 frames) while P1 and P3 slept waiting on it (`cable n=3 waiting=2
  active=0`), forever; the main loop stayed alive and `/xmb.ps3$exit` got out cleanly. Stopgap:
  a `LINK_CABLES=0` build on the console; the pause-menu cable toggle (cable off by default)
  must make joining a running lockstep safe.

### Sound only for the heard game, and the heard game first (2026-11-01)

**Unheard games skip their sound samples** (`patches/mgba/skip-sound-samples.patch`,
`GBAAudioSkipSamples` / `GBAudioSkipSamples`, called by `emu.c` every frame). Before, every core ran mGBA's sample event
(GBA: every 1024 cycles, ~274 per frame, each a forced exit from the JIT; GB: every 2048 cycles,
32 samples each), mixed and buffered samples that `emu.c` then threw away.

- **Why it can't change what the game does.** What the sample event touches: the PSG channels
  (`GBAudioRun` to each sample's time: duty/wave/noise positions, and on the GBA the wave RAM,
  which the playing channel rotates in place and the game can read), `lastSample`/`sampleIndex`,
  the mixed samples and the FIFO sample history (audio only), and the CPU's next-event time.
  The PSG is run lazily (to "now" on register and wave RAM accesses, and by the frame sequencer),
  so most of it doesn't depend on when samples are taken, **but not all**: a `SOUND3CNT_L` write
  (wave bank/size) or a sound power-off while channel 3 plays catches the PSG up only to the last
  sample time with the old bank, then the rest with the new one, so the wave RAM's contents
  depend on the sample grid; on GB/GBC, NR52 power-off doesn't run the PSG first, and
  PCM12/PCM34 expose channel samples. So simply descheduling the event would change game-visible
  state in corner cases. The patch therefore **defers** the skipped events: their `when` keeps
  the next sample time, and every function that reads or changes that state (GBA:
  `GBAAudioSample`, which all sound register writes and the frame sequencer call first, wave RAM
  accesses, `SOUNDCNT_H`, FIFO timer ticks; GB: `GBAudioSample`, which `GBAudioRun` calls,
  and NR52) first replays the due ones, in order, at their own times, minus the output. Nothing
  can observe the state between a sample's time and its replay, so it evolves exactly as with
  the events running on schedule; turning sampling back on resumes bit-identical audio state.
  The CPU side: events are processed at the first instruction boundary at or after their time
  whatever else is queued, a halted CPU skips from event to event, `gba->bus` is refreshed at each
  event check but only read right after a DMA (no check in between), and the JIT's idle-loop
  skip stops at the next event either way.
- **mGBA quirk found on the way**: `_sample` rescheduled itself relative to "now", which inside a
  tick is late by the cycles an earlier event of the same tick added (an IRQ entry), so the
  sample clock drifted by a few cycles now and then, for good. The patch keeps it on its own
  clock (replays can't know about such shifts). Audio-only in practice (the drift would have to
  reach a sample interval, hours, and then meet a channel-3 bank switch).
- **Proof by test**: `SOUND=twin tools/jitcheck.sh` (an interpreted twin core with the opposite
  setting, sampling toggled at random frame counts, compared every frame: CPU, time, all memory,
  wave RAM, timers, DMA, IO, save, event queue; whole audio state at every toggle) and
  `SOUND=1 tools/jitcheck.sh` (JIT self-check while toggling). RPCS3, random input:
  FireRed (JIT main, interpreted twin) 18,600 frames and 1,283 toggles, Crystal (GBC) 15,600
  frames and 1,077 toggles: game state and whole audio state identical throughout. JIT self-check
  while toggling: FireRed 5 min, Unbound 4 min, no mismatch. The first FireRed twin run is what
  found the clock drift above (`sampleEvent.when` off by a few cycles after 740 identical
  toggles); the first Crystal run differed in the noise LFSR at a toggle where the sampling core
  still had a sample due (a GB core can stop a frame with an event due; harmless, such toggles
  are no longer compared).
- **Measured (RPCS3, 4 GBA players: FireRed x2, Emerald, Unbound in game, idle; `SAMPLEALL 1`
  vs `0` alternated 3 x 25 s in one session)**: the heard P1 unchanged (2738 vs 2742 JIT
  dispatches per frame, 4.61 vs 4.63 ms). Unheard: FireRed 2659 -> 2099 dispatches per frame
  (-21%), 4.91 -> 4.42 ms per frame; Emerald 551 -> 463 (-16%), 3.90 -> 3.80 ms; Unbound 360 ->
  302 (-16%), 2.49 -> 2.36 ms (RPCS3 times are rough; Emerald and Unbound mostly wait halted,
  where events cost no dispatch). Not measured: GB/GBC cores (no JIT; they save the event and
  the buffer writes, the PSG work is still replayed). Heard-game switch (P1 pauses: Emerald is
  heard, then FireRed again, twice, `AUDIO 14`): 0 ms of silence padded, the queue dips to
  7.4-7.6 blocks for a second at each switch, no gap or click in the recording.

**The heard game and its cable partners first.** `p->favored` = the heard game plus the games
plugged into the same cable (lockstepped games, hard syncs included, run at the pace of the
slowest, so the heard one can't keep 60 fps unless they do). Their emulation threads run at
priority 1199 instead of 1200 (`sysThreadSetPriority` by the thread itself at its frame start),
and **the main loop only waits for their frames**: waiting for everyone (as before) made one
slow game drag every screen, the heard one included, below 60 fps. The others are released a
frame per display frame like before; when the PPU is short they get less CPU, run late and drop
frames on their own screen. Frames are now triple-buffered (`frame_shown`, claimed by the main
loop under the lock) so a late game can't draw into the buffer being copied (this also fixes the
torn lines a linked player one frame ahead could cause).

- No deadlock or starvation from the priorities: they add no wait. A favored thread that waits
  for a partner blocks (lockstep sleep, frame handshake: lv2 condition variables) and so gives
  up its hardware thread; partners have the same priority anyway. An unfavored thread can only
  be held off while favored threads and the main loop keep both hardware threads busy, which
  they don't for long (each favored thread blocks at the end of every frame).
- Inversion: `emu_lock` has no priority inheritance (`SYS_MUTEX_PROTOCOL_PRIO`), but it is held
  for microseconds; see the notes (Threads). The SPU wait at the frame end spins with yields,
  which don't hand the hardware thread to lower priorities: the heard game keeps it (wanted).
- With everyone on one cable (the default build plugs every GBA game in), everyone is favored:
  same as before. It matters with GB/GBC games, unplugged games (the pause-menu cable toggle) or
  two cables.
- Wireless Adapter games are favored like cable partners, but only while their adapter and the
  heard game's are in a session (`docs/wireless-adapter.md`). Favoring every plugged adapter
  made the main loop wait for all four Unbound at the bench spot: 14.0 ms of its 16.7 instead of
  9.1, frames over 16.7 ms in 24 of 66 s (none dropped); unused adapters now leave it at 8.9.
- To measure on the console: 4 players with the heard one unlinked (e.g. FireRed + 3 GB/GBC, or
  the cable toggled off) in a busy scene: `perf P<n>` of the heard game should stay at 60 frames
  per second while the others drop, `audio:` 0 ms padded; the `emu: P<n> thread priority` log
  lines confirm the switch. And with sampling: `jit P<n>` dispatches per frame and `perf P<n>`
  ms for unheard players, `SAMPLEALL 1` vs `0` in the same session.

Phase 1 alone, same build and saves (`SAVE 2`/`SAVE 3`, see "How to measure"), PPU renderer ->
SPU renderer:

| Setup | Per emulator, ms/frame | Display |
|---|---|---|
| 1 player, standing at the cable club desk | 12.76 -> 10.46 | 60 fps |
| 2 players linked, at the desk | 17.1-18.0 -> 14.4-14.8 (sharing the PPU) | 51.1 -> 59.9 fps |
| 2 players in the cable club Trade Center (active transfers) | 16.4-16.7 -> 13.4-14.0 | 44.3 -> 52.6 fps |
| 4 players on one cable, at the desk | (wall time, oversubscribed) | 28.9 -> 34.7 fps |

- With rendering on the SPUs, the PPU's per-frame cost is the ARM interpreter (plus ~0.1 ms to
  queue the video stream). The UI costs ~0.4 ms of main thread per frame.
- Tried, no gain: `-O2`/`-O3`/`-funroll-loops`, PGO (12.20 vs 12.08 ms), polling instead of lv2
  sleeps for link waits. VBA-Next is slower than mGBA here (16.6 ms). Details in the notes.
- The link cable costs ~2 ms per frame of waiting with 2 players (the primary waits at each hard
  sync for the secondary), not sync overhead.
- The app can write PowerPC code into a block reserved inside its own `.text` and run it, on
  both CFW and HEN (`tests/jitprobe`). Heap and lv2 memory are not executable.

### 4 players on Pokemon Unbound (2026-11-01 evening, CFW, dev build, `BENCH` saves)

- **The 4th player ran out of memory, and that froze everyone.** Each Unbound player took
  ~37 MB of the 214 MB lv2 gives the app (178 MB free at the main menu), so the 4th
  `jit_create` failed silently. That player fell back to the interpreter, ran at ~12 frames,
  and the main loop's 33 ms wait per emulator dragged all four to 29-40 fps.
  Cause: mGBA copies the whole ROM (32 MB) for each player:
  - when a cart has GPIO (RTC), at reset, so the GPIO registers can be written into the ROM;
  - when a 16-32 MB game touches AGB print (dev-kit debug output, which Unbound does a second
    after starting), so the print buffer can be mapped over the ROM.

  Fixed in `patches/mgba/shared-rom.patch`:
  - GPIO registers read from the cartridge's state.
  - AGB print never maps into a ROM it overlaps. Such ROMs read as on a retail cart, where those
    writes do nothing either.

  Players on the same game now share one read-only ROM.

  | Unbound players | Free memory after |
  |---|---|
  | 0 | 178 MB |
  | 1 | 141 MB (ROM cache 32 MB, JIT ~3 MB, core ~1 MB) |
  | 4 | 127 MB (was: the 4th failed) |

  The dev log shows free memory each second (`mem: now`), and per load step in the
  `emu: P<n> ... took` line.
- **Baseline, all 4 on the JIT, in the overworld:**
  - 57.0 fps (min 53.0 over 10 s);
  - 6.5-7.5 ms per emulator;
  - per player per second: 0-8.4K blocks invalidated (all revived), ~43-45K instructions
    interpreted, 0 translated, 0 flushes;
  - cable unplugged; audio 0 ms padded.

  This is the reference for the next fixes (JIT mid-block resume, unheard players' sound off).
- **Comparing builds**: `BENCH` saves, the same scripted presses
  (`tools/bench_unbound.sh`, ends with P1/P2 in a dialog), checked with a screenshot. A
  different scene moves the result by more than most fixes.

  | Build | fps (10 s) | P1 ms/frame |
  |---|---|---|
  | before the JIT mid-block resume | 53.9 (min 49.2) | 7.47 |
  | mid-block resume | 54.4 (min 51.5) | 7.64 |
  | + DMA fast path | 56.6 (min 54.2) | 7.46 |

  Mid-block resume cut the mixer's kills and revivals ~20x (7.7K to 0.4K per player per
  second), but each emulator's time didn't move: that churn wasn't what cost the frames on
  hardware.
- **Where an emulator's frame goes** (dev log `time P<n>`, wall time, 4 players):

  | Part | ms/frame |
  |---|---|
  | translated code | 3.8 |
  | mGBA's events | 3.45 |
  | C dispatcher | 1.1 |
  | interpreter | 0.2 |

  A temporary per-event profile (each callback timed in `mTimingTick`), per frame:

  | Event | Count | ms |
  |---|---|---|
  | DMA | ~2000 | 1.7 |
  | audio sample | 274 | 0.85 |
  | timer 0 (sound FIFO) | 448 | 0.37 |
  | video | 456 | 0.25 |
  | IRQ, GB audio | | ~0.03 |

  mGBA runs a DMA one unit (16 or 32 bits) per event.
  - Batching those events alone (running the next unit while the DMA's event is due) gained
    nothing: the cost is in each unit's own bookkeeping (`GBADMAUpdate` re-picks the DMA,
    deschedules and reschedules the event).
  - **DMA fast path** (`patches/mgba/dma-fast-path.patch`): runs the middle units of a DMA
    between plain memory back to back while no other event is due and no other DMA is pending.
    Loads, stores and their order, cycle counts and event times are unchanged; the event is
    rescheduled once at the end. Events went from 3.45 to 2.65 ms per frame per player.
    `tools/mgba_exact.sh` (pristine mGBA vs ours, per-frame hashes of video, memory, state,
    sound, cycles and registers): identical over 280K+ frames of 6 Pokemon games, ~85% of
    Unbound's 1,785 DMA units per frame on the fast path.
- **The SPU renderer isn't the bottleneck**: it is idle 15 of every 18 ms. Its "CPU waited
  6-7 ms per frame" for P1/P2 is the emulator thread yielding the PPU to the other emulators
  at frame end.
- **Tried, no gain**: at most two emulation threads running a frame at once (a semaphore, one
  per PPU hardware thread): 56.4 vs 56.6 fps, per-emulator time unchanged. lv2 wasn't switching
  them mid-frame much; the limit is CPU time per emulator. On 2 SMT threads,
  4 x 7.5 ms = 15 ms per hardware thread, plus the main loop.
- **Heard game first + sound skip, measured** (merged from "Sound only for the heard game"
  above):
  - The main loop now waits only for the heard game. As merged, a late game's queued frame was
    cancelled at every frame end, so after each late frame it idled until the next release:
    - P3/P4 at 49 and 42 frames per second;
    - 211 frames emulated per second in all, against 226 before.
  - A late game now keeps one frame queued, and never builds a longer backlog. With 4 players
    the emulators no longer stop at a per-frame barrier, so both hardware threads stay busy:

    | Setup | Display | Heard game | Other games |
    |---|---|---|---|
    | 10 s | 59.9 fps | 60 frames/s | 60 frames/s |
    | 2 minutes | 59.9 fps avg (min 58.9) | 60 every second | avg 58.8-59.0, min 54, under 59 in ~35% of seconds |

    The audio was never padded with silence.
  - With `SAMPLEALL 1` (unheard games compute their sound) the others drop to 57-58: the sound
    skip is worth ~1-2 frames per second each.
  - 1 Unbound player alone: 4.98 ms per frame (loop), against ~7.5 with 4. SMT costs each
    emulator ~1.5x.
  - **Linked games had fallen to 30 fps** (4 FireReds at the cable club desk, `tools/bench.sh 4`;
    59.5 in older builds).
    - Since the cable toggle, games plug in at different moments, so their frames end at
      different points of the cable's time.
    - A child whose frame ends later than the parent's next one slept on the cable, waiting for
      the parent. The parent waited for the main loop's next release, and the main loop waited
      for the child until its 33 ms timeout, every frame.
    - The main loop no longer waits for a game asleep on its cable; a game falling asleep wakes
      it.
    - A linked game keeps the old cap: no extra queued frame, since it can't run ahead of its
      partners.
    - Now 59.9 fps, 60 frames per second each, link blocked 3.4-7.6 ms per frame.
  - The dev build's run-loop timers (5 `mftb` per JIT dispatch) cost ~1%: they are now a
    `TIMES=1` build option. Without them, over 2 minutes:
    - display 59.86 fps (min 58.0);
    - heard game 60 every second;
    - others 59.67-59.77 on average (min 56), under 59 in 8-12 of 120 seconds.
  - **All 4 Unbound on one cable** (`LINK ON` each, after the bench): everyone is favored, so
    all share the PPU equally. Over 60 s:
    - each game 59.97-60.00 frames per second (min 59);
    - display avg 59.68 fps (min 57.9);
    - link blocked 2.2-6.1 ms per frame.
  - **Left to reach a steady 60 for all four: ~1% of CPU.** Next: native LDM/STM/PUSH/POP for
    RAM in the JIT; every PUSH/POP was a C call into mGBA (done, next item: steady 60 reached).
- **LDM/STM/PUSH/POP inline (2026-11-01, RPCS3 counts)**: the console's dev log split each
  emulator's ~7.6 ms per frame (56.6 fps with 4 players) into code 3.7, events 2.65, dispatch
  1.1, interpreter 0.2. Unbound runs ~3,865 LDM/STM/PUSH/POP per GBA frame (the stack in IWRAM,
  the m4a mixer's buffers), and every one went through mGBA's `GBALoadMultiple`/
  `GBAStoreMultiple` (roughly 250 PPC instructions each plus load-hit-store stalls on the synced
  registers and `cpu->cycles`); now 92.6% run inline (`docs/jit.md`, "Memory routines"), ~40-90
  instructions each. Left on mGBA: 256 STM to OAM, 15 to IO, 10 LDM from ROM, ~4 STM onto
  translated code per frame. Self-check clean, ~10 minutes each unchained and chained (see
  `docs/jit.md`).
- **Measured on the console (2026-11-02, CFW, dev build): a steady 60 for all four.** Same
  scenes, same saves, against the build before it (`TIMES` off in both):

  | Scene | Before | Inline LDM/STM |
  |---|---|---|
  | 4 Unbound unlinked, 2 min: display | 59.86 fps avg (min 58.0) | 59.92 avg (min 59.9) |
  | the 3 unheard games | 59.67-59.77 avg (min 56), under 59 in 8-12 s | 60 every second |
  | ms per frame per emulator | 6.56-8.08 | 5.16-5.64 |
  | main loop's frame work | 10.5 ms | 9.0 ms |
  | 4 Unbound on one cable, 60 s: display | 59.68 avg (min 57.9) | 59.92 (min 59.9) |
  | each game | 59.97-60.00 (min 59) | 60.00 (min 59) |
  | 4 FireRed on one cable at the desk, 60 s | 59.9 fps, 60 each | 59.92 (min 59.9), 60 each |

  ~2 ms per emulator per frame, more than the instruction counts promised (0.3-0.6 ms): the old
  path also paid load-hit-store stalls on every synced register and `cpu->cycles`, which RPCS3
  doesn't show. FireRed's four game pictures are pixel-identical to the previous build's
  (screenshot crops). The code cache grew ~3% (Unbound P1: 3.87 of 4 MB), no flush in 2
  minutes. Audio never padded with silence.
- **Again with the sound heard on the TV** (2026-11-02 evening, after the stereo-only
  PARAM.SFO, 2 minutes): every game at 60 every second, display 59.91 avg; the heard game's
  sound at normal level in an `AUDIO` capture, never padded. The one second at 59.0 was Unbound
  saving by itself (no input at the time): that frame took 33 ms, one frame dropped.
- **4 players walking at random for 5 minutes** (all 4 changing maps, NPC dialogs, new code
  translated, unlinked): every second at 59.7-60.2 fps, every game at 60 (min 59), 5.2-5.9 ms
  per emulator. The only dip was the dev screenshot (~1.4 s stall each).
- **Headroom left** (4 Unbound, unlinked): each emulator needs ~5.4 ms of a ~16.7 ms frame on a
  shared hardware thread; two per hardware thread make ~11 ms, plus the main loop. Not measured
  with 4 players yet: 4 battles at once, the Trade Center's busy cable, a code cache flush (one
  player self-checked into a wild battle, see `docs/jit.md`).

### UI drawing (2026-11-01, RPCS3 counts, not yet measured on the console)

The new menus, legends and 2-4 slots pushed a frame's Tiny3D commands past 16 KB, and Tiny3D's
64 KB command buffer (two 32 KB segments) then made the main thread **wait for the RSX in the
middle of drawing, until the display refresh** (the RSX sits at the previous flip's wait in the
other segment): one segment jump per frame blocking 16.4 ms in RPCS3, in every scene with 2+
slots, the 4-slot join screen, the carousel and the save screens. That wait shows up as `slots`
(and `flip` shrinks to ~0). Details in `docs/ps3-homebrew-notes.md` (Graphics).

Fixes, same pixels (screenshots of 21 scenes at 1080p and 720p, 1-4 players, menus, pause, PERF,
compared pixel by pixel with `FRAME 100` holding the animations: 0 differing pixels outside the
game screens, the PERF numbers and the save list's "1M AGO"): a 1 MB command buffer (alone, it
keeps the jumps but 0 ms blocked: checked at 720p); automatic
batching with the font, sprites, background tile and a white texel in one sheet; boxes, game
frames and the join screen's controller rim fill only what shows. Per frame at 1080p, before ->
after (dev log `gfx` line, RPCS3; blocked = main thread waiting mid-frame):

| Scene | Draw calls | Shader switches | Mpx filled | KB of commands | Mid-frame waits |
|---|---|---|---|---|---|
| main menu | 19 -> 1 | 4 -> 1 | 4.48 -> 4.42 | 11.4 -> 8.9 | 0 |
| About | 26 -> 1 | 6 -> 1 | 7.58 -> 6.58 | 12.5 -> 9.1 | 0 |
| join screen, 4 pads | 47 -> 1 | 10 -> 1 | 6.31 -> 4.77 | 16.3 -> 8.9 | 16.5 ms -> 0 |
| carousel | 50 -> 6 | 8 -> 1 | 5.59 -> 5.03 | 15.5 -> 9.6 | 16.5 ms -> 0 |
| rename keyboard | 114 -> 3 | 14 -> 1 | 10.7 -> 9.0 | 24.2 -> 9.2 | 16.4 ms -> 0 |
| 1 game | 13 -> 2 | 4 -> 1 | 9.78 -> 5.59 | 10.6 -> 9.0 | 0 |
| 2 games + 2 carousels | 155 -> 15 | 26 -> 1 | 7.93 -> 5.52 | 29.1 -> 10.8 | 16.4 ms -> 0 |
| 4 games, PERF on | 103 -> 9 | 18 -> 1 | 9.97 -> 5.74 | 21.2 -> 10.0 | 16.4 ms -> 0 |

- 720p: the same draw calls and waits; fill e.g. 4 games 4.47 -> 2.58 Mpx, rename keyboard
  4.78 -> 4.02.
- Quads stay ~the same (40-1700; the About page's text is the most). ~9 KB of every frame's
  commands are Tiny3D's own per-flip setup (it reloads its vertex program each frame).
- `flip` is now the frame's slack: the wait for the previous frame's flip (`vsync wait` in the
  `gfx` line), e.g. 8.6-8.9 ms with 4 games in RPCS3.
- Texture uploads of the game screens (PPU `memcpy` into RSX memory, 240x160 RGB565, once per
  emulated frame per player): 0.04 ms per frame for 1 game, 0.09 ms for 4 in RPCS3 (measure on
  the console: `upload` in the `gfx` line).
- The background is still two full-screen blended layers (~4.2 Mpx at 1080p): drawing only the
  tile's non-empty 8x8 cells (27% of it, 23 runs per tile) would save ~1.5 Mpx for ~440 more
  quads; worth it only if the console's `rsx` time says the RSX is the limit.
- To measure on the console (dev build): the `gfx` line in menus, then with 1-4 games
  (`tools/bench.sh 4`): `segment jumps` must be `0.00+1.00` with ~0 ms blocked; `rsx` (the RSX's
  time from the frame's first to last command; the report-based timestamps have only run in
  RPCS3 so far) against `vsync wait` tells whether `flip` waits for the refresh or for a late
  RSX; `phases` `slots` is now the drawing's CPU cost alone; `upload` the screen copies.

## Budget model

- The PPU is one core with 2 hardware threads; both together do ~1.5x the work of one.
- A frame is 16.7 ms, so ~25 ms of single-thread work per frame, ~24 ms after the UI.
- Each running game costs its per-player time; N players need N x per-player <= ~24 ms
  (1 player only needs <= 16.7 ms, it can use just one thread).

| Per player | Before | SPU renderer (done) | JIT | Both |
|---|---|---|---|---|
| Cost (all measured) | 12.1 ms | 10.5 ms (-2.3) | 5.85 ms | 3.86 ms |
| 2 players | 24.2, borderline | 21, ok (59.9 fps measured) | 11.7, ok (60 fps measured) | 7.7, ok (60 fps measured) |
| 3 players | 36, no | 31.5, no | ~17.6, ok | ~11.6, ok (not measured) |
| 4 players | 48, no | 42, no (34.7 fps measured) | ~23.4, borderline | 15.4, ok (60 fps measured) |

Only SPU rendering **and** a JIT reach 4 players, as predicted: confirmed on the console
(2026-11-01).

**SMT gives less with JIT code**: with 2 linked players each JIT emulator took 9.3 ms instead
of 5.85 alone (~1.25x for both threads together, vs ~1.5x for the interpreter), so the JIT alone
likely misses 60 fps at 4 players (~2 x 9.3 = 18.6 ms per hardware thread); with the SPU
renderer too it holds (measured above).

Link cable waiting comes on top and grows with the number of linked GBAs (see phase 0).

## Link cable options

- **One 4-way cable**: every GBA waits for the slowest of four. Can be dropped if too expensive.
- **2 x 2 cables** (e.g. P1+P3 trade while P2+P4 battle): one `GBASIOLockstepCoordinator` per
  cable (`LINK_CABLES=2`: P1+P3, P2+P4). Each pair only waits for its partner, so the cost
  stays at today's 2-player level. Choosing partners in the UI is not done; the pause menu's
  LINK CABLE entry plugs a player into its cable.
- Games now plug in and out at any time (pause menu, `emu.c`): the plugging GBA re-announces its
  SIO mode, link IDs follow plugging order, and our `lockstep.c` build fixes the lone-GBA clock
  (see the notes). Unplugging during a link session gives the games' own communication error
  screen, like a real cable; checked in RPCS3 that every game keeps running.

## Plan

### Phase 0: two measurements (small, decide the rest)
1. 4 players linked with today's interpreter: per-emulator ms and link blocked time per player
   (dev log `link P<n>: ... sleeps, ... ms blocked`). Compare with 2 players and with 2 x 2 (a
   quick hack with two coordinators is enough to measure).
   **Done (2026-11-01, CFW PS3, 4 FireReds at the cable club desk, `tools/bench.sh 4`, build
   option `LINK_CABLES=0|1|2`):**

   | Cables | fps | Per emulator, ms (wall) | Link blocked, ms/frame |
   |---|---|---|---|
   | none | 28.9 | 16.4-16.8 | 0 |
   | one 4-way | 28.9 | 20.0-25.3 | 3.7-8.7 |
   | 2 x 2 (P1+P3, P2+P4) | 29.3 | 19.9-22.1 | 4.2-8.2 |
   | 2 players, one cable (before) | 54-58 | 15.5-16.5 | 0.5 secondary, 2 primary |

   - The cable layout makes no difference today: four interpreters saturate the PPU (~4 x 12.5
     ms of single-thread work per frame on ~1.5 threads' worth of throughput), and a blocked
     emulator just hands its hardware thread to another one. Waits only cost frames once the
     emulators are cheap enough to leave the PPU idle: re-measure after the JIT.
   - 2 x 2 blocks as much as 4-way: with 4 threads on 2 hardware threads, most waiting is for a
     partner that is descheduled, not for the cable protocol.
   - With more emulator threads than hardware threads, per-emulator ms (wall time) include time
     spent descheduled, so they overstate the cost; compare fps instead.
   - Each emulator's renderer: 2.9 ms per frame in this scene (SMT shared), what phase 1 moves
     off the PPU.
   - In menus (FireRed's Continue screen) 4 players run at 36.3 fps, linked or not: always check
     the scene (the first runs here measured menus by mistake).
   - `LINK_CABLES=2` is kept as a build option to re-measure; the real 2 x 2 needs a partner UI.
2. Maximum JIT code cache in `.text`: reserve 4, 8, 16 MB; check build, SELF size, install and
   load time, and that code at the end of the block executes (`tests/jitprobe` style).
   **Done (2026-11-01)**: 4-32 MB all build and run in RPCS3 (which needs the code segment
   flagged RWX); the EBOOT only grows from 80 KB to 108 KB (compressed zeros). **CFW console:
   4 MB and 16 MB with the plain R-X segment work** (code at the end runs, rewrites seen, launch
   unchanged at 11 s). The one HEN attempt (32 MB + RWX flag together) hung the HEN console (see
   the notes). The JIT uses 16 MB (4 MB per player); FireRed fills up to ~3.9 MB of its slice,
   so a bigger cache (untested on hardware above 16 MB) would avoid occasional full flushes.

### Phase 1: mGBA's renderer on the SPUs (done 2026-11-01; open items at the end)
- mGBA has a proxy renderer (`src/gba/extra/proxy.c`, `src/feature/video-logger.c`) that turns
  every write to video registers, VRAM, OAM and palette into a command stream, built for drawing
  on another thread. One SPU per player replays that stream with mGBA's software renderer
  (built for the SPU) and DMAs the finished 240x160 frame into the player's screen texture.
- Constraints: 256 KB local store for code + VRAM copy (96 KB) + OAM/palette (2 KB) + renderer
  state + line buffers. Mid-frame changes (raster effects) must be applied per scanline, in
  stream order. SPU branches are statically predicted (hint or fall through, ~18 cycles per
  miss), fine for a renderer's regular loops.
- Saves ~2.5-3 ms per player of PPU time. Measure: per-player ms and fps with 2 players in the
  cable club rooms (today 45-47 fps).
- **How it's built** (`include/spu_render.h`, `source/spu_render.c`, `spu/render.c`):
  - mGBA's GBA core wraps its software renderer in the proxy renderer when `core->videoLogger`
    is set before `reset()`. Our logger's `writeData` appends the packets to a 256 KB ring per
    player in main memory and publishes the write position at each scanline and frame.
  - One SPU thread per player slot, started at boot, sleeps on a lock-line reservation of the
    write position, replays packets into mGBA's software renderer (compiled for the SPU, `-Os`)
    and DMAs each 480-byte line to the frame buffer named by our `SR_OUTPUT` packet. VRAM blocks
    go straight from the ring into its VRAM copy by DMA.
  - At the end of each frame the emulation thread waits until the SPU has caught up (it trails
    by about a line), then hands the frame over as before. A stalled SPU (2 s) is logged and
    its player's screen stops updating; the app never hangs.
  - `RENDER PPU|SPU|CHECK` (dev command) picks the renderer of games started afterwards; CHECK
    runs both and compares every frame, logging differences.
- **Results (2026-11-01, CFW PS3, FireRed `SAVE 2` at the cable club desk):**
  - Pixel-identical: CHECK with 4 players at once compared ~4,400-4,700 consecutive frames per
    player (boot, intro, title screen, menus, the "previously" recap, overworld): 0 differences.
    The first frame needs pre-filling white like mGBA's renderer init does (the BIOS skip starts
    that frame at line 126); before that fix it was the only difference.
  - The main thread still copies each finished frame into the player's texture, as before. DMAing
    lines straight into RSX memory would need a third texture per player (the GPU must never
    read a frame being written); not done, the copy's cost wasn't measured separately.
  - 1 player: **12.76 -> 10.46 ms per frame** (same build and save, PPU vs SPU renderer; the
    12.1 above was measured on the other save pair). On the PPU, queueing the stream costs
    0.07 ms per frame and the wait for the SPU at the end of a frame 0.02 ms.
  - 2 players linked, PPU -> SPU renderer, same build and saves (`SAVE 2` + `SAVE 3`):
    at the desk **51.1 -> 59.9 fps** (per emulator 17.1-18.0 -> 14.4-14.8 ms); in the cable club
    Trade Center **44.3 -> 52.6 fps** (16.4-16.7 -> 13.4-14.0 ms; P1/P2 saves swapped between
    the two runs). The earlier 54-58 / 45-47 fps were measured on the other save pair.
  - 4 players on one cable at the desk: **28.9 -> 34.7 fps**, all four SPUs drawing in parallel
    (4.1-4.2 ms per frame each, 0.015 ms of waiting per frame on the PPU).
  - The SPU takes 4.2 ms to draw a frame (`-Os`; the PPU renderer took 2.3 ms), in parallel with
    the CPU. It only has to keep pace: once a JIT makes the CPU part faster than ~4 ms per
    frame, the frame-end wait grows, then compile the hot paths (mode 0 backgrounds) with `-O2`
    if local store allows, or stop waiting at the frame end (triple buffering).
- **Open items:**
  - 2 players are solid at the desk (59.9 fps) but not in the cable club rooms (52.6 fps):
    that needs the JIT (phase 2).
  - Once the JIT is in, re-measure the SPU's 4.2 ms per frame against the CPU's (see above) and
    the link cable layouts (phase 0.1: waits only cost frames once the PPU has idle time).
  - The main thread's copy of each frame into its texture: measure it, then decide whether to
    DMA straight into RSX memory (third texture per player).
  - Save list order: saves with equal mtimes (copies restored over FTP, 1 s resolution) sort
    in no defined order (`compare_saves` in `library.c` has no tie-break), and in the benchmarks
    players saw the same list in different orders; `tools/bench.sh` works around it. Fix:
    tie-break on the name.

### Phase 2: a JIT (dynarec) inside mGBA, step by step
Why inside mGBA rather than a PowerPC backend for gpSP: mGBA's timing, memory map, saves, link
cable (trades verified on hardware), renderer and our threading all stay; only the instruction
execution loop changes. gpSP would mean redoing all of that and its link support is unverified.

The JIT is general (ARM/Thumb instructions are the same for every game), not per game. Games
differ in how much they stress edge cases (self-modifying code, exact timing, rare
instructions), so mGBA's interpreter stays as a fallback, plus a per-game "no JIT" switch.

1. **Skeleton**: split code into blocks (up to a branch), cache translations in the `.text` code
   cache, each translated instruction calls mGBA's existing handler. Correct from day one,
   little faster. Call generated code with `mtctr`/`bctrl` (ELFv1: function pointers point to
   descriptors), flush with `dcbst`/`sync`/`icbi`/`isync` after writing.
2. **Self-check mode (dev builds)**: run the JIT and the interpreter side by side, compare all
   GBA registers (and memory writes) after every block, stop at the first difference.
3. **Translate for real, most-used first**: Thumb first (Pokemon is mostly Thumb), keep GBA
   registers in PowerPC registers within a block, compute flags only when read, count cycles
   once per block, stop at mGBA's next event, chain blocks.
4. **Self-modifying code**: Pokemon copies code into IWRAM/EWRAM and runs it; writes to memory
   holding translated code must invalidate those translations.
5. Measure each step: ms per frame, 1 and 2 players, same scene as the table above.

Progress (details, design and numbers in `docs/jit.md`):
- 2026-11-01, all in RPCS3: steps 1-4 done. The JIT is cycle-exact with mGBA's interpreter by
  design and the self-check (an interpreted twin core compared after every block) is clean
  over long randomized FireRed sessions, chained and unchained. Native Thumb and ARM (the m4a
  mixer runs natively), lazy flags in CR fields, cycles per segment, block chaining, inline
  IWRAM/EWRAM/ROM accesses with mGBA's prefetch stall reimplemented, SMC invalidation per
  halfword, and **exact idle-loop skipping** (FireRed's VBlank wait loop was ~45% of all GBA
  instructions in the overworld). Two linked FireReds on the JIT connect at the cable club and
  enter the trade center.
- **Measured on the CFW console** (FireRed at the cable club desk, same build and save, JIT vs
  `nojit`): **1 player 11.2 -> 5.85 ms per frame** (CPU emulation 8.9 -> 3.7 ms with the
  renderer's ~2.2 ms left as is); **2 linked players 15.1/14.4 -> 9.3/9.4 ms per player**, a
  solid 60 fps with the main loop waiting 0.25 ms (the interpreter used 16.6 of the 16.7 ms).
  The self-check ran on the console too: 190 s of random play, 38M comparisons, clean.
- Budget: 4 players need ~4 x 5.85 = ~23 ms of single-thread time per frame; with the ~1.25x
  SMT measured for JIT code (above) that misses 60 fps with the JIT alone; with the SPU renderer
  (~2.2 ms less per player) ~15 ms, comfortable. Remaining JIT overheads (instruction counts):
  ~2600 returns to the C dispatcher per frame (mostly events), the C path for IO/VRAM accesses,
  the renderer. Open items (cache size, profiling, release build, other games): `docs/jit.md`.
- What instruction counts say (platform-independent, `jitprof`): of ~108K GBA instructions per
  overworld frame, ~43K are skipped idle iterations, ~65K run as native code (~54% Thumb, ~46%
  ARM, ~10% of them memory accesses mostly inline now), <0.5% through mGBA's handlers, and
  ~2600 returns to the C dispatcher per frame (mostly at events).

### Phase 3: 3-4 players for real
- 3-4 player UI polish, cable setup UI (2 x 2 or 4-way, per phase 0), trades and battles tested
  on hardware with several cables.

## How to measure

- Dev build (`tools/ps3.sh deploy`), then scripted input (`tools/ps3.sh cmd ...`, see
  CONTRIBUTING.md) to load a save; read `tools/ps3.sh log`. Per second the log prints fps, phase
  timings, `perf P<n>` (frames, avg ms, renderer ms, max ms), and `link P<n>` (sleeps, blocked
  ms). A stalled player prints its state and mGBA's lockstep state.
- Benchmark scene used so far: four FireRed saves (two pairs of identical copies) standing in
  front of the cable club desk. `tools/bench.sh <n>` gets there; per-save costs differ a bit
  (12.1 vs 12.76 ms for 1 player), so compare runs on the same saves (the log names them). The
  Trade Center route below saves the game: back the saves up over FTP first and restore them
  after.
- Cable club Trade Center with 2 players (each step one press, ~1-2 s apart, check with a
  screenshot): from the desk spot each player does UP, RIGHT, RIGHT, UP (now facing the
  right-hand receptionist, "CENTRE TROC"), then A x4: welcome, "CENTRE TROC", save "OUI",
  overwrite "OUI" (this saves the game). Then P1 only: A ("all players ready"), A ("LINK 2J"),
  A ("Démarrer connexion..." as the text scrolls), A (confirm): both walk into the room. Avoid
  extra A presses in the room (the trade table).
- 1-player runs repeat within ~0.05 ms; 2-player runs vary ~±2 fps, so compare several runs.
