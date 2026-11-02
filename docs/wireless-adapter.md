# GBA Wireless Adapter: research and plan

Status: research only (2026-11-02), nothing implemented. Goal: trade (and battle) in games that
only link wirelessly, Pokemon Unbound first, between the players of one ps3gba session.

## Why

- Pokemon FireRed/LeafGreen and Emerald reach the Union Room (trade, battle, chat with up to 4
  others) only with the Wireless Adapter (AGB-015) plugged in; Pokemon Unbound, built on FireRed,
  trades that way. Our emulated link cable doesn't help there.
- The adapter supports 1 host and up to 4 clients: our 4 players fit.

## The device

- An SPI peripheral on the GBA's serial port, in Normal 32-bit mode. It starts with a login
  handshake: both sides exchange the letters of "NINTENDO", each half-word answered with the
  inverse of the other side's previous one. Then the game sends commands as
  32-bit words: a header `0x9966LLCC` (`LL` data words follow, `CC` the command), the adapter
  acknowledges with `0x996600(CC + 0x80)` and its own data words.
- Commands (afska's docs list them all): Hello `0x10`, Setup `0x17`, Broadcast `0x16` (the host
  advertises 6 words), StartHost `0x19`, PollConnections `0x1a`, BroadcastRead `0x1c-0x1e`
  (clients scan), Connect `0x1f`, IsConnectionComplete `0x20`, FinishConnection `0x21`,
  SendData `0x24`, SendDataWait `0x25`, ReceiveData `0x26`, Wait `0x27`, DisconnectClient
  `0x30`, Bye `0x3d`, plus status commands (`0x11-0x15`) and RetransmitAndWait `0x37`.
- "Wait" commands reverse the roles: the adapter becomes the clock master and sends a word to
  the GBA when data arrives or a timeout passes (the serial interrupt wakes the game).
- Packets are small (the host sends up to ~87 bytes per transfer, each client up to 16), and the
  games' library (Nintendo's librfu, visible in the pret decompilations of FireRed and Emerald)
  counts timeouts in frames.

## Existing implementations

| Project | License | State |
|---|---|---|
| VBA-M (`src/core/gba/gbaLink.cpp`) | GPL-2.0 | Works for the Pokemon games: over a thousand lines of adapter emulation, between instances on one PC (IPC) or over a network (sockets). Maintained in 2026 (link fixes through October), with an Emerald trade/battle test harness that takes `--link rfu`. |
| GBE+ | GPL-2.0 | Partial: menus reachable, multiplayer not working (its release notes). |
| mGBA | MPL-2.0 | None (no wireless code in our snapshot of 2026-10-30). |
| [afska/gba-link-connection](https://github.com/afska/gba-link-connection) | MIT | Not an emulator: the best protocol documentation (`docs/wireless_adapter.md`, every command, waiting, wireless multiboot, credits davidgfnet and kuiper.dev) and a library driving a real adapter. |
| denopqrihg's spec v1.1 (2005) | - | Older, partial command list. |

**License**: copying VBA-M's code would make ps3gba as a whole GPL-2.0. Recommended: write our
adapter from afska's MIT documentation and the games' own library (decompilations), and use VBA-M
only as a behavior reference and test oracle (run the same scenario there and compare the serial
transcripts), not as code to copy. That keeps ps3gba under MPL-2.0.

## How it fits ps3gba

- **No mGBA patch**: mGBA attaches accessories through `struct GBASIODriver`
  (`include/mgba/gba/interface.h`): `writeSIOCNT`, `start` and `finishNormal32` cover an SPI
  device in Normal 32-bit mode, as mGBA's own BattleChip Gate driver does. One virtual adapter
  per player, in our code (e.g. `source/rfu.c`).
- **One process, no network**: VBA-M needs IPC or sockets because each player is a separate
  emulator. Ours are 4 cores in one process: a shared in-memory hub routes broadcasts, connections
  and packets between the adapters, with a mailbox per adapter (lock-free or a short mutex, like
  `share.c`).
- **Timing**: the games tolerate radio latency (retransmits, timeouts in frames), so no
  cycle-level lockstep like the cable's (which costs ~3.5 ms per frame per player on Unbound):
  delivering packets at frame granularity should do. Our emulators stay within about a frame of
  each other; if a late unheard game misses timeouts, wirelessly connected games can be
  "favored" like cable partners (`emu.c`).
- **UI**: a pause-menu switch like the cable's, WIRELESS ADAPTER ON/OFF, exclusive with the link
  cable (one serial port). FireRed shows its Wireless Communication System counter only with the
  adapter detected, so when the games look for it decides whether it must be on before booting:
  to be tested. A dev command `<player> WIRELESS ON|OFF` for scripts.
- The JIT is unaffected: serial register accesses go through mGBA's I/O handlers.

## Feasibility

Good for the Pokemon games' Union Room trades: the protocol is documented under MIT, VBA-M proves
high-level emulation works for them, mGBA has the right driver hook, and one process is the
simplest topology there is. Risks:
1. The exact acknowledgement and "wait" timing librfu expects (VBA-M needed tuning: compare
   transcripts with it).
2. Timeouts across our emulator threads when a game runs late.
3. Unbound's engine (CFRU) may differ from FireRed in its wireless code: test it.
4. Tests need real games and saves at the Union Room: scripted, kept out of the repo.

## Plan

1. **Reference transcript**: run two Emeralds (or FireReds) trading in VBA-M's `--link rfu`
   harness with its link tracing on; record the command sequence for entering the Union Room,
   hosting, joining and one trade. Read the matching librfu code in the decompilation for the
   timeouts.
2. **Desktop harness** (fast iteration, like `tools/mgba_exact.sh`): two, then four mGBA cores in
   one process with our adapter and hub, scripted input to the Union Room, the serial transcript
   compared with VBA-M's.
3. **Adapter and hub**: Hello/Setup, broadcast and scan, host and connect, send/receive/wait,
   disconnect and bye; per-adapter mailboxes; frame-level delivery.
4. **In ps3gba**: the pause-menu switch, the dev command and log lines, then RPCS3 and the
   console: FireRed to FireRed trade, Emerald, Unbound trade (the goal), 3-4 players in the
   Union Room, a battle.
5. **Cost and docs**: per-frame cost with 4 connected players on the console, CONTRIBUTING and
   notes, a 0.1 step in CHANGELOG.md.

## Tests and assets

- The owner provides a FireRed save standing in front of the Union Room (Pokemon Center 2F); copy
  it for each player (saves are per game and per name: `saves/<game>/<save>.sav`), on the console
  and in RPCS3. Games, saves and VBA-M transcripts of real games stay out of the repo.
- Scripted input drives both players into the Union Room and through a trade; check each step
  with screenshots (presses during fades get lost) and the new log lines.
- Unbound: the owner's Unbound saves are on the CFW console; use copies, never their real saves.

## Done means

Two players trade in FireRed's Union Room on the console (then Unbound, then 3-4 players), the
link cable and everything else work as before (`tools/bench.sh 4` still at 60), the docs are
updated, and CHANGELOG.md gets a 0.1 step (its number decided when merging: another feature may
land first).
