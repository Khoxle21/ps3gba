# GBA Wireless Adapter

The GBA Wireless Adapter (AGB-015), emulated for every player's GBA game, so the games link
without a cable: Pokemon FireRed's Union Room (trades, greetings) works between the players of one
ps3gba session, up to 4 at once.

## Status (2026-11-03)

- **Works on the CFW console**: two FireReds traded in the Union Room (trading board), then four
  players in the room with two trades, the saves checked after each; 59.9-60 fps throughout,
  ~5-6 ms of emulation per player. `tools/bench.sh 4` (the cable) unchanged: 59.9 fps, 60/60
  frames per player.
- Desktop harness: the same trades with 2 and 4 players, also with games stalling at random (a
  late emulation thread), and the Union Room's trainer card exchange.
- **Pokemon Unbound (2.1.1.1) can't trade over it**: the game detects the adapter, but its own
  scripts closed the Union Room and link trading ("the Union Room is no longer available",
  "trading is no longer available from the Cable Club. Please use Unbound Cloud"), the cable too.
- Not tried: battles and chat in the Union Room, the Direct Corner over the adapter, LeafGreen,
  Emerald, HEN, other wireless games (wireless multiboot isn't implemented).

## Using it

- Pause menu of a GBA game: WIRELESS ADAPTER: ON/OFF. One port: turning it on unplugs the link
  cable, and the other way round. Games start with neither.
- Plug it in before talking to the Union Room attendant (FireRed checks for it there; also at
  boot, harmlessly).
- In the Union Room each player needs their own trainer: the game takes two players with the same
  name and trainer ID for one (two copies of one save still trade; with 3-4 such copies the
  trading board showed nothing). `tools/save_trainer.py` copies a save as another trainer.
- Trades go through the trading board: one player registers a Pokemon with the attendant beside
  it, the other checks the board, picks the offer and offers a Pokemon of the wanted type.

## The device (what the emulation follows)

Sources: [afska/gba-link-connection](https://github.com/afska/gba-link-connection)'s
`docs/wireless_adapter.md` (MIT, every command) and what the games' own library sends and expects
(Nintendo's librfu, in pret's FireRed decompilation: `librfu_stwi.c`, `librfu_intr.c`,
`librfu_sio32id.c`, `librfu_rfu.c`). No code from VBA-M (GPL-2.0, the other emulation of it).

- An SPI device on the serial port in Normal 32-bit mode: each transfer swaps one word each way,
  so the adapter's word for a transfer is decided by the previous one.
- Reset: the game holds SD high in GPIO mode (`RCNT` 0x80A2), then both sides swap the letters of
  "NINTENDO". The adapter's side is the slave half of librfu's own `Sio32IDIntr`, one word late;
  once the GBA echoed the adapter's id (0x8001) the adapter answers 0x80000000 (else the game's
  first command fails and is retried 130 ms later).
- Commands: `0x9966LLCC` and LL words, answered `0x9966LL(CC+0x80)` and its words; 0x80000000 is
  "nothing to say" both ways. Between words the GBA waits for SI to answer SO: SI is the opposite
  of SO while the GBA clocks, equal to SO while the adapter clocks; we answer at once.
- After a "wait" command (0x25 send and wait, 0x27 wait, 0x37 retransmit and wait; 0x35 too) the
  GBA becomes the clock slave until the adapter clocks a request to it: 0x28 data arrived (a host
  gets it right after its own send-and-wait, with every client acked), 0x29 and the lost slot and
  reason (a client lost its host), 0x27 the Setup timeout (frames, 32 in FireRed). The GBA acks
  with CC+0x80; then it is the master again.
- Replies the games parse (librfu): SystemStatus 0x13 (id, a client's slot bit, state at bits
  24-31: 0 idle, 1-2 host, 3 searching, 4 connecting, 5 connected); host poll 0x1a (`id | slot <<
  16` per client); scan 0x1d/0x1e (per open room: `id | next free slot << 16`, then its 6
  broadcast words); connect poll 0x20/0x21 (`id | slot << 16` when connected, status at bits
  24-31 else: 0 is success, anything else "still trying" then failure); LinkStatus 0x11 (signal
  per slot: FireRed's librfu 1024 only takes a client after 4 host polls with its signal on).
- Data: a host sends up to 87 bytes (header: byte count), a client 16 (header: count << (8 + 5 *
  slot)); bytes go least significant first in each word. A host's receive has one 5-bit count per
  slot from bit 8 and the bytes end to end; a client's the host's count in bits 0-6. A header
  alone ("ghost send") resends a few bytes; games use one to let clients talk.

## How it fits ps3gba

- `source/rfu.c`: one `struct rfu` per player, an mGBA `GBASIODriver` (no mGBA patch): it times
  Normal 32-bit transfers itself from `writeSIOCNT`, since the clock can come from either side
  (the login even switches clock mid-transfer), and polls the hub every ~1 ms while waiting. The
  player's emulation thread plugs it in between two slices, like the cable (`emu.c`).
- The hub (the radio): every adapter in the process is in range of every other. Shared state
  under one mutex (lv2 on the PS3, pthread in the harness); a waiting adapter reads a volatile
  stamp first and only locks when something changed. A host's send reaches each client's queue at
  once and brings back the oldest packet that client queued for it, so nothing waits for another
  game's thread. Queues of 8 packets instead of hardware's single buffers: games whose threads
  drift a frame apart lose nothing (hardware drops a packet that isn't read in time; the games'
  own sequence numbers would then count an error).
- Games with the adapter on are "favored" like cable partners when the heard game has one too:
  same thread priority, and the main loop waits for them, so they stay within a frame.
- New ids at each new room or connection; connections succeed at once (a connect poll retries
  while the room is closed); links are never lost by the radio, only by a player leaving
  (reset, Bye, unplug, quit), so connection recovery (0x32-0x34) always fails.
- Cost: nothing measurable. Four FireReds in the Union Room, two trading: 59.9-60 fps, ~5-6 ms per
  emulator per frame, the same as without the adapter.

## Tests

- `tools/rfu_test.sh <script> <rom> <save> [<rom> <save>...]`: 2-4 mGBA cores in one process on
  the Mac (in the toolchain container), each with the adapter, a frame of each in turn on one
  thread; script commands at the top of `tests/rfu/harness.c` (presses, holds, waits, stalls,
  screenshots, `TRACE ON` for every command and reply). Each game's save is written to
  `build/rfu_test/shots/P<n>.sav` at the end. A run of a few thousand frames takes ~10-30 s.
- `tests/rfu/trade.txt`: two FireReds trade (any save standing at the Union Room attendant, a
  Normal type second in the party; copies of one save are fine). `tests/rfu/trade4.txt`: four
  players, two trades at once (one save copied as four trainers with `tools/save_trainer.py`).
- On the console: the same through the dev commands (`<p> WIRELESS ON`, `<p> HOLD <button>
  <frames>` to walk), checked with screenshots: the harness scripts' waits fit (CROSS for A,
  CIRCLE for B, WAITGBA for WAIT).
- Games, saves and transcripts of real games stay out of the repo.
