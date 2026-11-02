# JIT (ARM/Thumb -> PowerPC dynarec) design

Phase 2 of `docs/performance-plan.md`. Code: `source/jit/` (`jit.c` cache, dispatcher,
invalidation, idle loops; `translate.c` translation; `memory.c` load/store routines; `check.c`
self-check and profiler; `ppc.h` encoders). Hooks into the app: `emu.c` calls `jit_run_loop()`
instead of `core->runLoop()`. **No mGBA source change.** Switches: `DATA_DIR/nojit` exists ->
every game on the interpreter; `DATA_DIR/nojit_games.txt` -> the game files listed (one per
line, as named in the roms folder) on the interpreter.

## Goal: identical to mGBA's interpreter, cycle for cycle

The JIT must produce exactly the state mGBA's interpreter would: same registers, same memory,
same cycle count at every instruction boundary where something outside the CPU can look. That
keeps every timing-dependent behavior (IRQs, DMA, timers, the link cable's lockstep) unchanged,
and it makes a strict self-check possible (any difference is a bug).

How mGBA's interpreter works (`src/arm/arm.c`): `ARMRunLoop` executes instructions while
`cpu->cycles < cpu->nextEvent`, then calls `processEvents` once. Each instruction adds its cycles
(1 + sequential waitstates of the code region, plus memory waits) to `cpu->cycles` at its end.
Memory handlers (`GBALoad32`...) add their waits to a counter passed by pointer and read
`cpu->gprs[15]` (prefetch stall logic, open bus). Writes to IO can change `nextEvent` (DMA, IRQ,
halt), and a mode switch (`BX` to ARM) sets `nextEvent = cycles` to force event processing.

The JIT keeps these semantics:
- `jit_run_loop` = `ARMRunLoop`: `while (cycles < nextEvent) run a block; processEvents()`.
- Translated code checks `cycles >= nextEvent` once per segment (see "Segments") and leaves at
  exactly the instruction where the interpreter would stop. `nextEvent` is read from the ARMCore
  at each check, so C code may change it at any time.
- Before calling an mGBA handler it stores what the interpreter would have: PC = instruction + 2
  words, the two prefetched opcodes, `cycles`. After the call it reloads `cycles`.
- At every exit the dispatcher restores the interpreter's "about to execute X" state:
  `gprs[15] = X + 1 word`, `prefetch[] = opcodes at X and X + 1 word` (`set_next`).

## Code cache

- 16 MB of zeros reserved in `.text` (`jit_cache`, top-level asm `.space`), 4 MB per player
  slot; `make JIT_CACHE_MB=n` changes it (then touch `source/jit/jit.c`: make doesn't track
  flags). `.text` is the only memory a PS3 app can write and execute (`tests/jitprobe`).
- Each slice starts with shared routines emitted at startup: `enter` (saves r14-r31, LR, CR,
  loads the GBA state, jumps), `exit_link`/`exit_regs`/`exit_mem`, `sync_out`/`sync_in`, store
  stubs, the memory routines. The tail (`REF_STUB_WORDS`) holds the self-check reference core's
  store stubs.
- Full? Everything is dropped (`flush_all`) and translation starts over. Blocks: max 64
  instructions, RAM blocks never cross a 256-byte page.
- Lookup: open-addressing hash of `address | thumb` -> the newest block translated there, alive
  or dead (`MAP_SIZE` 65536, `MAX_BLOCKS` 24576; FireRed's overworld and menus reach ~16000
  blocks in 3.5-4 MB, Unbound's ~14600 in 3.7 MB). Dead blocks stay until the next flush (see
  "Self-modifying code": they can be revived).
- After writing code: `dcbst` each 128-byte line, `sync`, `icbi` each line, `isync`.
- Generated code calls C with a relative `bl` to the function's entry (ELFv1: a function pointer
  is a descriptor `{entry, TOC, env}`; take `entry`). r2 (TOC) is never changed, so C callees
  just work. Out of `bl` range (32 MB): `mtctr`/`bctrl`.
- C calls generated code through `run_code` (inline asm `mtctr`/`bctrl`, all volatile registers
  clobbered); `enter` builds a real ABI frame (288 bytes: header, parameter save area for C
  callees, r14-r31, then scratch words for translated code; LR at 16(r1) and CR at 8(r1) of
  the caller's frame).

## Registers and flags

| PPC | Use |
|---|---|
| r14-r28 | GBA r0-r14 (all non-volatile, so C helpers preserve them) |
| r29 | `struct jit*` (the fields translated code reads come first, offsets < 32 KB) |
| r30 | `cycles` |
| r31 | `struct ARMCore*` |
| cr2.lt, cr2.eq, cr3.lt, cr4.lt | GBA N, Z, C, V (non-volatile CR fields) |
| r0, r3-r12, cr0, cr1, cr5-cr7 | scratch (clobbered by calls) |

Translated code is in one of two states: **REG** (GBA state in the PPC registers above) or
**MEM** (in the ARMCore, as after an mGBA handler call). `sync_out`/`sync_in` switch. CPSR flags
go through `mfcr` + 4 `rlwimi` (out) and 4 rotates + `mtcrf 0x38` (in); both only at
transitions to/from C.

## Translation (`translate.c`)

Two passes per block. **Decode** gives each instruction a kind (`K_ALU`: native with static
cycles; `K_CALL`: native around a C call, dynamic cycles; `K_FALLBACK`: mGBA's handler;
`K_BRANCH`: native, ends the block), its static cycles and the flags it defines and uses.
**Analyze** places segment checks and computes flag liveness, then **emit**.

- **Segments**: a check (possible exit) precedes the first instruction and every instruction
  after a call or fallback. A segment runs whole iff `cycles + (cycles of all its instructions
  but the last) < nextEvent`, so one `addi`/`cmpw`/`bge` per segment gives exactly the
  interpreter's stopping point; when it fails the block exits with `EXIT_INTERP` and the
  dispatcher interprets up to the event. Cycles accumulate at translation time and are added to
  r30 once per segment (and in exit stubs).
- **Flags**: computed only when live. At every check and at the end of the block all four are
  live (an exit lets the interpreter or an IRQ see CPSR). N/Z: `cmpwi cr2, result, 0`.
  C after an addition: `cmplw cr3, result, operand` (wrapped). C after a subtraction:
  `cmplw cr3, a, b` gives "borrow", so C is tracked as held inverted (`c_inv`) and fixed with one
  `crnot` only where it leaves the block. V: two `xor` and an `and` (one `andc` for an
  immediate operand), then `cmpwi cr4`. Conditional branches test CR bits directly (HI/LS,
  GE/LT, GT/LE combine bits into cr0 with `crandc`/`crnor`/`creqv` first).
- **Memory**: each load/store is `li r5, PC` + `bl` to one of the core's shared routines
  (`memory.c`, below). LDM/STM/PUSH/POP (`emit_lsm`) call the core's LDM/STM routine with the
  lowest word's address and the word count. When all the words are in IWRAM or EWRAM it returns
  their host address, having added mGBA's wait to r30; the registers then move with inline
  `lwbrx`/`stwbrx` and the base is written back with one `addi` (a loaded PC goes to r3, for the
  WritePC exit). Otherwise the instruction branches to its mGBA path, emitted after the block:
  base, list, direction and PC into registers and a `bl` to the core's shared routine for
  `GBALoadMultiple`/the STM wrapper, which goes through `cpu->gprs` for all 15 registers (shared,
  so each site costs ~6 words instead of syncing its list inline). Empty lists always take the
  mGBA path (its quirk: PC, 64 bytes).
- **Literal pools**: `LDR rd, [PC, #n]` in ROM code is folded into a constant (ROM is immutable;
  its cycles are static too: `N32(ROM) + 2` plus the instruction's own).
- **Branches**: a static branch within the region adds WritePC's `2 + N + S`, zeroes
  `gba->memory.lastPrefetchedPc` (ROM code only: it only matters for ROM prefetch) and leaves
  through a **chain exit**. BL's two halves fold into one static branch. Static branches skip
  `setActiveRegion` unless mGBA watches them: idle optimization DETECT (every branch), or a
  branch to the game's known idle loop address (REMOVE mode halts there; Emerald has one).
  Those, indirect branches (BX, POP {pc}, MOV/ADD PC) and branches to other regions exit with
  `EXIT_WRITEPC`/`EXIT_BX` and the dispatcher runs mGBA's own `ThumbWritePC`/`ARMWritePC`/
  `_ARMSetMode`, then adds the branch's own cycles, in the interpreter's order: WritePC may
  halt (`nextEvent = cycles`) and a mode switch sets `nextEvent = cycles` too. The branch's
  cycles travel in r5 (POP {pc} and LDM with PC: measured around their memory call).
- **Chaining**: a chain exit is 4 fixed words (`lis`/`ori` r3, `li` r4, `bl exit_link`); the
  dispatcher, seeing `EXIT_CHAIN`, rewrites its first word into `b target` once the target block
  exists (`jit_link`). Killing a block restores the sites linked into it and recycles their
  link entries (`free_links`): revived blocks get linked again after every kill, and `MAX_LINKS`
  would otherwise fill up between flushes.
- **Multiplies** (`K_DYN`: native, dynamic cycles, a segment boundary after): `ARM_WAIT_SMUL`
  computed inline (`srawi`/`xor`/`ori 1`/`cntlzw`/`srwi 3`/`subfic 4`: 1-4 cycles by how many top
  bytes are all 0 or all 1), through the memory routines' stall routine only for ROM code with
  prefetch on (else the stall is the identity). Thumb MUL, ARM MUL/MLA.
- **ARM**: data processing with an immediate or an immediately shifted register (the shifter's
  carry exactly as mGBA: LSL #0 and rotation 0 keep C, LSR/ASR #0 mean 32), MUL/MLA, every
  LDR/STR/LDRB/STRB/LDRH/STRH/LDRSB addressing mode (pre/post, up/down, writeback before a load
  and after a store, like mGBA), LDM/STM (LDM with PC exits with `EXIT_WRITEPC`), B/BL/BX.
  **Conditional execution**: a `bc` over the instruction; ALU instructions cost the same cycles
  either way, calls and multiplies settle their cycles in r30 on both paths; C is canonical
  before the condition so both paths agree. Flag liveness treats a conditional definition as
  not killing the old flags. Careful: for LDR/STR, bit 25 set means a *register* offset (for
  data processing it means an immediate).
- **Still mGBA's handler** (fallback, from REG state through `sync_out`/`sync_in`): Thumb ADC,
  SBC, shifts by register, LDRSH, SWI, BKPT, undefined, LDMIA with an empty list; ARM MSR/MRS,
  SWI, shifts by register, ADC/SBC/RSC, RRX, long multiplies, MULS/MLAS (mGBA sets their C from
  a stale `shifterCarryOut`), LDRSH, SWP, LDRT/STRT, LDM/STM with the S bit or PC stored, writes
  to PC from ALU or LDR, coprocessor.

## Memory routines (`memory.c`)

One set per core in its slice, re-emitted when the self-check is attached (its store routines
then log inline). Call: r4 = address, r5 = PC as the interpreter sees it, r6 = value; loads
return r3; the access's wait is added to r30. Two variants of each (code running from ROM with
the prefetch buffer on, or not), chosen at translation (waitstate changes flush the cache).
- **IWRAM/EWRAM**: `lwbrx`/`lhbrx`/`lbzx` and the byte-reversed stores on `base + (address &
  size - 1)` (mirrors for free); unaligned loads go to mGBA (it rotates them). Stores first test
  the halfword's translated-code bit (`iwram_bits`/`ewram_bits`): set -> mGBA's store wrapper,
  which invalidates.
- **ROM loads** (regions 8-12, within the ROM, offset >= 0x100 so GPIO and the header stay with
  mGBA): direct; wait from a per-region table (non-sequential waitstate + 2), no stall.
- **Waits** as mGBA: IWRAM 0, EWRAM its non-sequential waitstate, +2 for a load, +1 for a store,
  then, for code in ROM with prefetch, **mGBA's `GBAMemoryStall` reimplemented exactly** as a
  shared routine (`emit_stall`: reads/writes `lastPrefetchedPc`, the loop over prefetched
  halfwords, the N->S conversion). The multiply wait uses it too.
- Anything else (IO, VRAM, palette, OAM, SRAM, unaligned): the C path, which stores the
  interpreter's PC and cycles first and passes `&cpu->cycles` as mGBA's cycle counter, so mGBA
  adds its wait exactly where the interpreter's local counter would. Its LR is saved in the
  frame (`FRAME_SCRATCH_LR`). For loads from unmapped regions (0, 1, above 15) it also sets
  `cpu->prefetch[]` (the opcodes at PC - 1 word and PC): mGBA's open-bus value comes from it.
- **LDM/STM** (`emit_lsm_routine`: load, store, and load with PC, per variant): r4 = the
  lowest word's address (mGBA aligns it, so unaligned bases need nothing special), r6 = the
  word count n. Inline when the region is IWRAM or EWRAM and the n words don't run past its
  end (mGBA masks each address into the region, i.e. wraps: left to it), and for stores when
  none of their 2n halfwords holds translated code (one 64-bit test: two `lwbrx` of the
  code-bit words, shift, `sld.`), else mGBA's wrapper must invalidate. The wait is exactly
  `GBALoadMultiple`/`GBAStoreMultiple`'s: `S - N + n * (1 + S)` with the region's 32-bit
  waitstates (IWRAM: n), +1 for a load, then the stall routine for code in ROM with prefetch
  (the final address is below ROM, so mGBA stalls these too). With PC in the list the stall
  measures from the **loaded** PC (`GBALoadMultiple` sets `gprs[15]` before `GBAMemoryStall`
  reads it), so that routine passes the last word as r5. EWRAM's waits per n are a table
  (`ew_lsm_wait`) refreshed with the others. Returns cr5.eq clear and r3 = host address; cr5.eq
  set means "take the mGBA path", nothing done. With the self-check it logs (lowest word, 4n)
  like `jit_stm_c`. The mGBA path (`emit_lsm_mgba`) stores PC, cycles and r0-r14 into the
  ARMCore, calls mGBA, reloads them (loads) and the cycles.
- `nextEvent` is no longer kept in a register: segment checks load it (`lwz` from the ARMCore),
  so nothing has to reload it after calls that may change it.
- Self-check: the inline store paths append (aligned address, size) to `j->slog` exactly like
  `after_store` does for stores through mGBA; each core (JIT and reference) has its own log.

## Idle loops

Pokemon waits for VBlank by polling a RAM flag (FireRed: `ldrh r1,[r2,#0x1C]; add r0,r3,#0;
and r0,r1; cmp r0,#0; beq` back), ~45% of all GBA instructions in the overworld. mGBA's own
idle-loop detection is off for these games (overrides say `IDLE_LOOP_NONE`, mode REMOVE).
The JIT skips such loops **exactly**:
- Translation marks a block as a candidate when it branches back to its own start and only
  computes and loads (no stores, no calls but loads, no fallbacks), with load addresses built
  from registers the loop never writes (`find_idle_loop`). Its backward branch exits with
  `EXIT_LOOP` instead of chaining.
- The dispatcher compares the state at the loop head between consecutive iterations (registers,
  NZCV). Identical, and every load reads EWRAM/IWRAM/ROM (not IO: timer registers change
  without events): nothing can change until the next event, so all further iterations are
  identical. It adds the cycles of the `(nextEvent - cycles) / period - 1` whole iterations
  that certainly complete before the event and lets the block run the rest normally.
- A loop proven idle once gets its state snapshot when the dispatcher enters its head, so one
  iteration proves it again after each event (lastPrefetchedPc is 0 at the head, as in every
  iteration). 8 failed comparisons between consecutive iterations mark it `not_idle`; then its
  exit is chained like any other. A first iteration that differs from an entry snapshot is not a
  failure (the game just entered the loop): counting it marked FireRed's loop `not_idle` after 8
  frames.
- The self-check's reference core runs every skipped iteration: the skip is verified exact.
- Pokemon Unbound has no such loop: its `WaitForVBlank` (0x08000890) halts the CPU instead,
  `do { swi 0x02 } while (!(gMain.intrCheck & 1))` (BIOS Halt until the next interrupt), and
  mGBA already jumps to the next event while halted (no polling loop in its jitprof top 24).
  Its `0 idle skipped` is expected.

## Dev tools

- `DATA_DIR/jitprof`: counts block runs (a `lwz`/`addi`/`stw` per block entry) and logs every
  10 s which share of GBA instructions ran as native Thumb, mGBA handlers, memory calls and ARM,
  the Thumb handler opcodes, and the 24 hottest blocks with their opcodes; then, per GBA frame,
  the LDM/STM/PUSH/POP run and those that went through mGBA by the base's region (`L` loads, `S`
  stores), and the single loads and stores that went through mGBA by region. Instruction
  counts are platform-independent: the tool to prioritize while only RPCS3 is available.
- The dev log's `jit P<n>` line: blocks and cache use, translations, invalidations, revivals,
  flushes, interpreted instructions, resumed (dispatches entering a RAM block mid-way, see
  "Self-modifying code"), idle instructions skipped, dispatches (returns to the C dispatcher)
  and how many of them were WritePC exits, self-check comparisons.
- A self-check mismatch writes the block's PowerPC code to `DATA_DIR/jitblock.bin`:
  `ppu-objdump -D -b binary -m powerpc:common64 -EB --adjust-vma=<address in the log> jitblock.bin`.

## Exits

Translated code returns to the dispatcher with r3 = the next GBA address and r4 = reason |
instructions run << 8 (r5 = the exit's address for chain exits). Reasons: `EXIT_INTERP` (an
event falls within the next instructions: the dispatcher interprets until it, like
`ARMRunLoop`), `EXIT_PCSET` (an mGBA handler set PC itself: the state is already the
interpreter's), `EXIT_NEXT`/`EXIT_CHAIN` (continue at r3; a chain exit gets linked),
`EXIT_WRITEPC` (branch to r3 through mGBA's `ThumbWritePC`/`ARMWritePC`), `EXIT_BX` (mode
switch first), `EXIT_LOOP` (an idle loop candidate's backward branch). The dispatcher always
leaves the interpreter's "about to execute r3" state (`set_next`).

## Self-modifying code

- mGBA's `store8/16/32/storeMultiple` pointers in `cpu->memory` are replaced by wrappers
  (`jit_st*_c`) that call the original, then `after_store`. DMA and the HLE BIOS write through
  the same pointers, so they are caught too (`bios.c` only `memset`s RAM at reset).
- The wrappers need the `struct jit*` but mGBA's handlers have no context argument: each core
  gets tiny stubs in its slice (`li r7, j; b wrapper`) and an ELFv1 descriptor pointing at the
  stub; that descriptor is what goes into `cpu->memory`.
- IWRAM and EWRAM have a bit per halfword marking translated code, and per-256-byte-page lists
  of the blocks there (mirrors map to the same physical page). A store hitting a marked halfword
  kills the overlapping blocks (marked dead, chained branches into them undone) and rebuilds
  that page's bits. Code next to data in the same page is fine. STM invalidates exactly
  the words mGBA's `GBAStoreMultiple` wrote (one word more killed the code FireRed's flash
  library keeps right above a `push`, on every byte saved).
- If translated code is running (`in_jit`), the store also forces an exit at the next
  instruction: `nextEvent` is lowered to `cycles` (the next event check fails) and the dispatcher
  restores it afterwards as `min(saved, cycles + mTimingNextEvent())` (`mTimingNextEvent` counts
  from now, not like `cpu->nextEvent`: getting this wrong was the self-check's first catch).
  The block exits with `EXIT_INTERP`; after a forced exit the dispatcher goes back to translated
  code instead of interpreting up to the real event.
- Waitstates are baked into translations: a store to `WAITCNT` (0x04000204) or the EWRAM control
  register (0x04000800) that changes `waitstates*`/`prefetch` flushes the cache (forced exit,
  then `flush_all` in the dispatcher). FireRed does it twice at boot.
- **Revival**: a RAM block keeps a copy of the GBA code it translated (`src`, in the cache right
  after its code), and a dead block stays in the map with a chain of older dead translations of
  the same address (`variant`). When the dispatcher finds a dead block, it compares the RAM with
  the copies of the newest `MAX_VARIANTS` (8) translations there; a match comes back to life
  (map, page list, code bits) instead of being translated again. Exact: a translation depends
  only on its code bytes and the waitstates (which flush everything when they change), so the
  revived block is what translating again would give.
- Why (Unbound, 2026-11-01): its sound mixer (ARM code in IWRAM) patches its own unrolled
  resampling loop (0x03006214-0x03006348) with one of three 6-word templates (0x0300636c,
  0x03006384, 0x0300639c) whenever the channel it mixes next needs a different one: `add r0, pc,
  #428; addpl r0, r0, #24` (x2) picks it, `ldr r2, [pc, #0x194]; cmp r0, r2; beq` skips the
  patch when it is already there, else `str r0, [pc, #0x188]` remembers it and a 4-iteration
  loop at 0x030061e4 writes it with 4 `stm lr, {...}` per iteration (2 words to `lr` and `lr +
  152`, 4 words to `lr + 12` and `lr + 164`). ~100 rewrites per second (~1.7 per frame), every
  store changed the code (none wrote identical bytes). Each rewrite killed ~50-80 blocks: the
  loop's blocks plus the many partial ones that start mid-loop (an event exit interprets up to
  the event, then translated code resumes at whatever instruction comes next). Before: ~12-18K
  translations, ~5-11K invalidations and 2-4 full flushes per second (the dead code filled the 4
  MB slice in ~0.3 s; each flush then retranslated everything else too). After: ~10
  translations, ~5-8K invalidations each revived, 0 flushes (RPCS3, same overworld spot); the
  three templates' blocks take ~450 blocks / 360 KB of cache.
- **Resuming mid-block** (2026-11-01): what was left after revival came from the partial blocks.
  Measured in RPCS3 (Unbound, standing on the overworld, per second, temporary counters):
  5,145 of the 5,450 blocks killed had been made (translated or revived) at the start of a run
  loop, i.e. resuming after an event, at a pc strictly inside a live block; ~29 kills per run
  loop that killed anything, 1,111 forced exits, 2,208 page invalidations walking 49.8K list
  entries and rebuilding 1.02M halfwords of code bits, 5,460 links and 5,448 unlinks (one
  icache flush each). None came from IRQ returns or branch targets. Now:
  - RAM blocks record an **entry point** at each segment check (`emit_check`, after the
    pending cycles are added): there r30 is exact, all four flags are live (so the CR holds
    them) and the GBA registers are in their PPC registers, exactly as when a block starts.
    The only translation state carried over from earlier instructions is BL's known LR (no
    entry while it is set) and C held inverted (the entry goes through a 2-word stub,
    `crnot` + `b`). The table (instruction index << 24 | word offset) follows the block's
    code in the cache.
  - When the dispatcher **resumes in sequence** (start of a run loop, i.e. after events; after
    an `EXIT_INTERP`, i.e. a forced exit; after the BIOS's IRQ return `subs pc, lr, #4`) at a
    RAM pc with no live block of its own, it looks in the page's block list for live blocks
    strictly containing pc (exact addresses: a mirror's blocks share the page) and takes the
    nearest entry point at or after pc (`enclosing_entry`). The interpreter runs the
    instructions up to it, one by one as near events (they are in sequence: blocks end at
    branches; it stops at an event, if PC leaves the sequence, if the block dies or a flush is
    pending), then the block runs from the entry. Exits still count instructions from the
    block's start: the dispatcher subtracts the entry's index for the self-check. The idle
    loop snapshot is only taken when a block is entered at its head.
  - Exact by construction: the interpreted instructions are the interpreter's, and the
    translated code from the entry on sees the same state it sees when it falls through to
    that check (the self-check compares both against the reference core).
  - Not for branch targets (chain, WritePC, BX exits): a loop whose head sits inside a longer
    block keeps its own, chained block, else each iteration would interpret up to the next
    entry. Not for ROM: its code never changes, so its partial blocks cost only cache space,
    and ROM has no per-page block lists to find the enclosing block in.
  - Stores now invalidate each 256-byte page once (the rest of the write within the page)
    instead of once per written halfword holding code: one list walk and one code-bit rebuild
    per page, the same blocks killed.
  - Result, same spot, per second: kills (= revivals) 5,037 -> 412 (~2.6 per rewrite: the
    blocks really holding the patched code), forced exits 1,111 -> 260, page invalidations
    2,208 -> 261 (list entries walked 49.8K -> 2.4K, code-bit halfwords rebuilt 1.02M ->
    57K), links 5,460 -> 458 and unlinks 5,448 -> 445 (icache flushes), dispatches 108.6K
    -> 103.3K, interpreted 51.7K -> 62.2K (19.1K resumes, ~0.5 instructions interpreted
    each), cache 3,742 KB / 14,813 blocks -> 3,371 KB / 14,521. With random play (100 s):
    kills 5,350 -> 457, translations 449 -> 312, links 5,867 -> 741, interpreted 48.3K ->
    58.3K.
  - Considered and not done: making the mixer's blocks end at the patch boundaries (game
    specific, and the partial blocks would still overlap them); cheaper kills (batched
    icache flushes, cheaper relinking): 12x fewer kills made them minor; a per-page
    "volatile" mode (interpret or don't chain written pages): slower for the mixer itself,
    which runs far more often than it is patched.

## Self-check (dev builds)

`DATA_DIR/jitcheck` (i.e. `/dev_hdd0/ps3gba/jitcheck`) present when a game starts (containing
`chain`: blocks stay chained and are compared at each return to the dispatcher, with chain exits
counting instructions in `j->chained`; otherwise every block returns and is compared): `emu.c`
starts a second, reference core loaded exactly like the player's (same ROM, same save bytes, own
video buffer since the software renderer masks some video registers) and never plugs the link
cable (the reference core can't be on it). `check.c`:
- before each block: snapshot of the JIT core, same keys to the reference core;
- after each block exit (and each interpreted instruction): the reference core runs the same
  number of instructions with `jit_step` (fails if an event was due first), then compares all CPU
  state (registers, CPSR/SPSR, banked registers, cycles, nextEvent, halted, prefetch) and the
  sequence of stores (address, size) plus the stored bytes; when more stores came than the log
  holds (512, e.g. long chains or big DMAs), all of IWRAM and EWRAM instead of their bytes;
- in ROM code, also `lastPrefetchedPc` (the prefetch state `GBAMemoryStall` keeps; RAM code
  ignores it and leaving RAM resets it, so translated RAM code doesn't keep it);
- after `processEvents`: the reference core processes its events, CPU state compared again, and
  the DMA units and sound FIFOs (state the stores reveal only much later);
- once per frame: IWRAM, EWRAM, IO registers, palette, VRAM and OAM compared byte for byte.
The first difference is logged with the block's opcodes, the state before it and both states
after it (`*` marks differing fields), the reference core's PC, cycles and `lastPrefetchedPc`
after each instruction it ran for the comparison (which instruction's cycles differ); checking
then stops and the game keeps running. Cost: the reference core runs the plain interpreter,
plus a CPU-state copy per block.

## Testing in RPCS3

- Run the app's `.self` (built with an RWX code segment, see `Makefile`) with
  `tools/rpcs3.sh run`; the LLVM recompiler runs the JIT's code correctly. RPCS3 can't have
  compiled code written at runtime, and the JIT's code runs ~3x slower than the interpreter
  there: **RPCS3 timings say nothing about JIT speed.** Only real hardware measures it.
- RPCS3's PPU interpreter would have given a fair "PowerPC instructions executed" comparison
  (every instruction costs about the same there: mGBA's interpreter took 13.6 ms/frame in the
  benchmark scene), but it crashes with the JIT (see the notes): no such number.
- Scripts must wait in emulated frames (`WAITGBA n`): with the JIT or the self-check the game
  runs at a different host speed, and display-frame waits put presses at different game moments.
- `tools/jitcheck.sh` says "the game did not start" (exit 1) when the check never turned on,
  instead of a hollow "no mismatch": twice (2026-11-01) the game never started, because the
  previous session's last random batch was still in `dev_input.txt` and the new instance ran it
  in the menus, then waited forever on its `WAITGBA`. `tools/rpcs3.sh run` now deletes input
  queued for the instance it kills.

## Running on hardware

Done once on the CFW console (2026-11-01, results below). To repeat (CFW preferred; on HEN
change one thing per launch):
1. `./build.sh DEV=1 pkg`, `tools/ps3.sh deploy`, then load FireRed SAVE 1 into the game (the
   4th A press in case one lands early; `0 RIGHT` picks FireRed in the carousel):
   `tools/ps3.sh cmd "0 CONNECT" "WAIT 30" "0 START" "WAIT 30" "0 RIGHT" "WAIT 30" "0 CROSS"
   "WAIT 30" "0 CROSS" "WAITGBA 400" "0 START" "WAITGBA 150" "0 CIRCLE" "WAITGBA 150" "0 CIRCLE"
   "WAITGBA 150" "0 CIRCLE" "WAITGBA 150" "0 CIRCLE"` (one line); after ~40 s
   `tools/ps3.sh log | grep -E "perf P1|jit P1" | tail -6`. Interpreter: FTP a `nojit` file (any
   content) to `/dev_hdd0/ps3gba/`, quit and restart the game, same commands, then delete it.
2. Self-check on hardware: `echo chain > /dev_hdd0/ps3gba/jitcheck` (FTP), play a few minutes,
   grep the log for `MISMATCH`. Hardware differences RPCS3 hides (e.g. a stale instruction
   cache after a missing flush) would show up here or as crashes.
3. If the app doesn't start or hangs at boot: try `JIT_CACHE_MB=8` (the one HEN attempt with a
   32 MB RWX cave hung that console; the hardware EBOOT keeps the code segment R-X).
4. Measure 1 and 2 players at the cable club desk (`docs/performance-plan.md`), then think about
   the costs RPCS3 can't show: `mfcr`/`mtcrf` (microcoded on the PPE) at every dispatch, load-hit-
   store stalls on `cycles` in slow memory paths, instruction cache pressure (~3 MB of code).

## Status and measurements

| Step | RPCS3 (Mac, not representative) | Real PS3 (CFW) |
|---|---|---|
| Interpreter | 4.2 ms/frame | 11.2 ms (same build, `nojit`; 12.1 earlier on main) |
| 1. Skeleton: every instruction calls mGBA's handler | 13.7 ms | not measured |
| 3a. Native Thumb, memory via mGBA, chaining | 7.3 ms | not measured |
| + idle loop skipping, 3b. native ARM | 4.7 ms | not measured |
| + inline IWRAM/EWRAM/ROM accesses (final) | 4.5 ms | **5.85 ms** |

Hardware (CFW, FireRed SAVE at the cable club desk, two runs each, spread ~0.05 ms):
- 1 player: interpreter 11.2 ms (renderer ~2.3), JIT 5.81 / 5.85 ms (renderer ~2.15): 1.9x per
  frame, 2.4x for CPU emulation alone (8.9 -> 3.7 ms).
- 2 linked players: interpreter 15.1 / 14.4 ms per player, frame work 16.6 ms (59.8 fps);
  JIT 9.3 / 9.4 ms per player (renderer 3.6 each: the two threads share the PPU), 60 fps, the
  main loop waits 0.25 ms for the emulators. P2 blocks ~7.3 ms per frame on the cable (it now
  finishes its frames early and waits for the primary), P1 ~0.4 ms.
- Hardware self-check (`echo chain > jitcheck`, scripted load + 5 batches of random presses):
  190 s, 38.3M comparisons, no mismatch.
- Cache: up to 16.4K blocks / 3.9 MB of the 4 MB slice in a 1-player run (RPCS3 runs used less):
  near full, so occasional full flushes are possible; a 24-32 MB cache would avoid them (only
  16 MB is verified on hardware).

- Step 1 (skeleton): runs FireRed in RPCS3. Generated code per GBA instruction: event check, PC
  and prefetch stores, the handler call, reload of cycles/nextEvent, PC-changed check (~25 PPC
  instructions + exit stubs); ~1.5 MB of code for the overworld.
- Step 2 (self-check): clean over the scripted session (boot, intro, continue, overworld), ~1M
  blocks compared per second in RPCS3 (~40 fps with the check).
- Step 3a (native Thumb, memory through mGBA): self-check clean for 5 minutes of the walking
  session, unchained and chained (`echo chain > jitcheck`). ~270 bytes of code per block,
  FireRed's overworld + menus: ~11000 blocks, 2.8 MB (fits the 4 MB slice; the chained check
  build reaches 3.9 MB). RPCS3: 7.3 ms/frame (skeleton 13.7, interpreter 4.2).

- **Long randomized sessions** (`tools/jitcheck.sh`: FireRed SAVE 1, then batches of 60 random
  presses every 45 s: walking, A, B, START menus; it leaves the building, opens menus, talks): 15
  minutes found the open-bus bug below at frame 32471; after the fix, 25 minutes unchained
  (~1500 s, ~600K block comparisons per second) and 12 minutes chained, both clean. Other
  games (`GAME=n`): Emerald (has an RTC and a known idle loop) 6 minutes clean after the fix
  below, Ruby (new game: intro, naming...) 5 minutes clean.
- Profile of the overworld session (block runs x instructions, `jitprof`), before idle skipping:
  47% one idle loop, ARM 28% (the m4a mixer in IWRAM, then all handlers), memory calls 14%,
  Thumb handlers 0.3%. After idle skipping: 3.9M instructions/s executed and ~2.6M/s skipped;
  ARM 46%. After native ARM: 0 handlers in the mixer's blocks. ~166K returns to the C dispatcher
  per second (~2800 per frame: mostly at events, ~500 WritePC exits).

- **LDM/STM inline (2026-11-01)**: Pokemon Unbound (`GAME=Unbound PROFILE=1
  tools/jitcheck.sh`, 3 minutes of random play, 600-frame window) runs ~3,865 LDM/STM/PUSH/POP
  per GBA frame (1,935 loads, 1,927 stores), all through mGBA before: by the base's region IWRAM
  1,918 loads / 1,597 stores (the stack, the m4a mixer's `ldmia`/`stmia` on its buffers), EWRAM
  8 / 59, OAM 256 stores (OAM copies), IO 15 stores, ROM 10 loads. Now 3,580 per frame (92.6%)
  run inline; through mGBA are left the OAM, IO and ROM ones and ~4 stores per frame onto
  translated code (the mixer patching itself). Per instruction (PUSH {r4-r7, lr}): ~16 words
  at the site, ~25 in the routine (IWRAM store), plus the stall routine (~45 for code in ROM
  with prefetch); before: the C call to `GBAStoreMultiple` and `GBAMemoryStall`, then
  `after_store`'s walk over the 10 halfwords, roughly 250 instructions, with load-hit-store
  stalls on `cpu->cycles` and on every synced register (stored by one side, read back right away
  by the other). Code: 18.3 words per LDM/STM site instead of 14.1, +2.0% of all translated
  code (temporary counters over 78K translations); Unbound's random play still fills the 4 MB
  slice about every 40 s, as before. Self-check clean on the final build (with the stricter
  checks below and the proxy fix): Unbound unchained 608 s (315M block comparisons, 12.5M
  mid-block entries), chained 664 s with `SEED=7` (129M) and 665 s with `SEED=1` (142M),
  FireRed 353 s (205M). Hardware (2026-11-02, CFW): self-check clean with Unbound chained, 323 s
  (73.9M block comparisons; ~130 random presses, overworld then a wild battle); 4 Unbound players
  went from 6.6-8.1 to 5.2-5.6 ms per emulator per frame, a steady 60 for all four
  (`docs/performance-plan.md`). The load-hit-store stalls were most of the old cost.
- **Link cable (step 5)**: two FireReds on the JIT, linked, in RPCS3 (P2 on a copy of the
  save): both continue into the game, talk to the trade center attendant, save, the parent sees
  the child ("Quand tous les joueurs sont prêts..."), confirms, and both trainers appear in the
  trade center room on both screens. Before the fix below the same run stalled or crashed.
  Per-player ms in RPCS3 with the cable: ~4.9 / 5.3 ms (P2 blocked ~11.5 ms per frame waiting
  for the link, as with the interpreter; RPCS3's slow thread switches).
- The self-check can't run with the cable (the reference core can't be on it): link sessions
  are tested functionally only.

### Bugs found without the self-check

- With 2 players the translator's scratch state (a `static struct`) was shared by both
  emulation threads: concurrent translations corrupted each other (an ARM opcode reached the
  Thumb emitter, then "Illegal opcode" crashes, or both players stalled on the link). It is per
  core now (`j->tctx`). Lesson: no mutable static state in the JIT, each player translates on
  its own thread.
- The self-check's reference store stubs were written right after the player's slice (into the
  next slot's shared routines, or past the cache for slot 4) instead of its reserved tail;
  harmless only because the self-check runs with one player. The slice now ends before the tail.

### Bugs the self-check found

- Forced exit restored `nextEvent` from `mTimingNextEvent()` as if absolute; it counts from now.
  Showed as `next=204` vs `951` at the first WAITCNT write.
- Branches did not count their own prefetch cycles (3 cycles short per branch).
- The translator's stub list (static struct, cleared only up to the instruction array) was not
  reset: each new block re-patched older blocks' exit branches. Crashed in RPCS3 with
  "Illegal opcode 0" (a jump past the written code).
- The chained check's counter used `addi r0, r0, n`, which PowerPC reads as `li r0, n` (rA = r0
  means 0 in `addi`/`addis` and in load/store addressing). Found because the chained check saw
  a loop run more times than counted.
- ARM loads passed "no destination" (-1) to the shared memory emitter, which then wrote the
  value into `RG(-1)` = r13, the PPU thread pointer: C code crashed later on garbage pointers
  (no mismatch, a segfault in the check's own code). `RG()` is only valid for 0..14.
- ARM `LDR r12, [r11, r12, lsl #2]` (the HLE BIOS SWI table lookup) was translated as
  `[r11, #0]`: the operand helper read bit 25 with its data-processing meaning. The SWI
  dispatcher then ran SoftReset instead of CpuSet.
- Emerald (known idle loop, so mGBA halts on branches to it): `EXIT_WRITEPC` added the branch's
  own cycles before calling `ThumbWritePC`, whose halt sets `nextEvent = cycles` (113 vs 111).
  Harmless for timing (processEvents then uses cycles) but the check is strict: the dispatcher
  now adds them after, like the interpreter. Same run: every static branch went through C for
  Emerald (300K returns/s); now only branches to the idle loop address do.
- Inline LDM/STM, Unbound after ~1 minute (frame 3242, chained and unchained): `pop {r3-r7, pc}`
  in ROM code right after an EWRAM `ldr`, 2 cycles more than mGBA. mGBA's `GBALoadMultiple`
  writes the loaded PC into `gprs[15]` before calling `GBAMemoryStall`, which measures the
  prefetch overlap from it: no overlap with the new PC, so a stall of -2 where the
  instruction's own PC (overlapping the `ldr`'s prefetch) gave 0. The per-step trace of the
  reference core in the report showed it at once. Loads with PC now pass the loaded PC.
- Not a JIT bug, found the same week: Unbound, chained check with `SEED=7`, frame ~28,450
  (also on the JIT before inline LDM/STM): "stored bytes differ at 04000054", a DMA writing
  BLDY. mGBA keeps what the renderer returns in `io[]`; the reference core's software renderer
  masks BLDCNT, BLDALPHA, BLDY, WININ and WINOUT, the proxy renderer behind the player's SPU
  renderer didn't (Unbound wrote BLDY = 0x01xx). Fixed in mGBA's proxy
  (`patches/mgba/proxy-register-masks.patch`), which also stops games reading back bits the
  hardware drops. Found by keeping both store logs in the report and checking the DMA and FIFO
  state after events: nothing diverged but that register.
- After ~9 minutes of random play (frame 32471), FireRed did `ldrh r1, [r2]` with r2 =
  0xe55ec002 (an HLE BIOS opcode read back as a pointer): an unmapped address, so mGBA returns
  open bus built from `cpu->prefetch[]`, which translated code never updates. JIT 0x4804 vs
  0x4281. The slow path now sets the prefetch words for unmapped loads. (LDM from unmapped
  memory would still differ: not seen.)

## Open items (2026-11-01)

- **Cache size**: 16 MB (4 MB per player) is verified on hardware; FireRed fills up to 3.9 MB
  of its slice at the desk and 4029 KB (14,254 blocks) in the cable club Colosseum, so full
  flushes are close. 24-32 MB would avoid them but is
  untested on hardware (`JIT_CACHE_MB`; probe it with `tests/jitprobe` first, R-X, one change
  per launch). What hung the HEN console (RWX flag, 32 MB, or HEN itself) is still unknown.
- **Saving in game was slow under the JIT** (CFW, FireRed, 4 players saving at the cable club
  together: ~40 fps for ~5 s): per player per second 15K blocks invalidated and retranslated,
  700K instructions interpreted (~50K normally); emulators 13-18 ms per frame instead of ~7.
  Cause (RPCS3, logging the invalidated blocks): the flash library copies its 4-byte poll
  routine onto the stack (`0x03007d2c`) and `ProgramByte` pushes right below it for every byte
  saved; the STM wrapper invalidated one word too many, so each byte killed and retranslated
  the routine, and each kill's forced exit then interpreted up to the next event; the dead code
  filled the 4 MB cache about once a second (full flush). Fixed (2026-11-01): exact STM range,
  no interpreting after a forced exit. Same save in RPCS3: 15K -> 48 invalidations/s, 670K -> 46K interpreted,
  self-check clean through boot, load and save. Still costly by nature: the game busy-polls
  the flash chip (no idle loop to skip, ~190K WritePC dispatches/s, 10x normal) and the extra
  save code fills the 4 MB cache (one flush per save). Hardware: not measured yet.
- **Pokemon Unbound churned the cache** (CFW, 3 players: 8-16K translations, 3.6-8K
  invalidations, 1.5-3 full flushes per player per second; seconds with 3+ flushes at 56 fps,
  41-48 in heavy scenes). Cause (RPCS3, logging every invalidation with the writer's PC and
  registers, then disassembling an IWRAM dump): its self-patching sound mixer, see
  "Self-modifying code". Fixed (2026-11-01) by reviving dead translations. Self-check clean:
  Unbound unchained 631 s (282M block comparisons, 3.1M revivals) and chained 360 s (44M
  comparisons, 1.8M revivals); FireRed 309 s (180M comparisons, idle skipping unchanged).
  Then each rewrite still cost ~29 kills and revivals and ~6 forced exits (RPCS3; ~50-80 kills
  on the console): fixed (2026-11-01) by resuming mid-block ("Self-modifying code"), now ~2.6
  kills per rewrite, 12x fewer kills, links and unlinks, 18x less code-bit work. Self-check
  clean: Unbound unchained 589 s (298M block comparisons, 12.0M mid-block entries, 224K
  revivals), chained 631 s (89.7M comparisons, 13.0M mid-block entries, 240K revivals),
  FireRed 310 s (181M comparisons, 1.7M mid-block entries, idle skipping unchanged). Left:
  with random play Unbound's live code alone nearly fills the 4 MB slice (one flush every
  ~40 s before; the partial blocks no longer take ~370 KB of it): the cache size item above. To measure on the console: fps and
  per-emulator ms with 4 players, invalidated/revived/resumed/interpreted in the `jit P<n>`
  lines (`docs/performance-plan.md`, Unbound section).
- Translation costs 6.5 us per block on average (`jit P<n>` line: "N translated (x ms)").
- **4 players**: measured with the SPU renderer after merging (CFW, desk): 60 fps, 7.3-9.5 ms
  per emulator (wall time), main loop waiting 2.5 ms per frame. JIT alone with 4 players not
  measured, and likely short of 60 fps: SMT gives JIT code only ~1.25x (2 linked players: 5.85
  ms alone -> 9.3 ms each, renderer 2.15 -> 3.6, CPU part 3.7 -> 5.7; the interpreter got ~1.5x),
  so ~2 x 9.3 = 18.6 ms per hardware thread.
- **Not profiled on hardware**: where the remaining ~3.7 ms of CPU emulation per frame go on the
  PPE (e.g. `mfcr`/`mtcrf`, load-hit-store stalls, instruction cache misses with ~3-4 MB of code
  per player) is unknown; the overheads below come from instruction counts.
- **Release build** (`./build.sh` without `DEV=1`) never built or run with the JIT: every
  measurement used dev builds. Build it and play a few minutes before shipping.
- **Games covered**: FireRed (self-check in RPCS3 and on the CFW console), Emerald, Ruby and
  Pokemon Unbound (self-check in RPCS3; `GAME=Unbound tools/jitcheck.sh`). Any other game is
  untested; `nojit_games.txt` is the escape hatch.
  Link sessions are tested functionally only (the self-check can't run with the cable).
- **Remaining JIT overheads** (by instruction counts, not yet profiled on hardware): ~2600
  returns to the C dispatcher per frame, mostly at events (each saves/restores 18 registers and
  converts flags with `mfcr`/`mtcrf`, microcoded on the PPE); indirect branches (BX, POP {pc},
  ~350 per frame) always go through C (an inline hash lookup would chain them); IO/VRAM/palette/
  OAM accesses, and LDM/STM outside IWRAM/EWRAM, still call mGBA; ~800 instructions per frame
  interpreted near events. Unbound per GBA frame (RPCS3 `jitprof`, random play): ~1,735
  dispatches (~800 WritePC exits), ~1,080 instructions interpreted, and through mGBA ~285
  LDM/STM (256 to OAM), 717 single stores and 94 loads in ROM space (by the shape of the hot
  block 0x08a0b19e, five `strh` to one address then an `ldrh`: the RTC's serial GPIO port),
  109 palette stores, 44 IO accesses.
- **Exactness gaps known but not seen in games**: LDM from unmapped memory would read a stale
  open-bus value (single loads are fixed; LDM outside IWRAM/EWRAM still goes through mGBA);
  ARM MULS/MLAS stay on mGBA's handler because mGBA sets their C flag from a stale
  `shifterCarryOut` that translated code doesn't maintain.
- Still on mGBA's handlers (rare in Pokemon): Thumb ADC/SBC/shifts by register/LDRSH; ARM
  MSR/MRS, SWI, register shifts, ADC/SBC/RSC, long multiplies, LDRSH, SWP. Idle-loop skipping
  is Thumb only.
- RPCS3: use the LLVM recompiler (its PPU interpreter crashes with the JIT); RPCS3 timings
  don't reflect JIT speed.
