# jitprobe: can a PS3 app run code it writes?

A dynarec (translating GBA ARM code to PowerPC at runtime) needs memory the app can both write
and execute. This tiny standalone app (title `PGBA00099`, "JIT probe") checks which memory allows it.

## How it works

Each probe writes `li r3,42 ; blr` into a region, flushes the caches (`dcbst`/`sync`/`icbi`/`isync`)
and jumps there with `mtctr`/`bctrl`. Returning 42 means the region is executable.

Executing non-executable memory kills the app, so **one launch runs one probe**: every step is
appended to `/dev_hdd0/jitprobe.txt` before it happens (not `/dev_hdd0/tmp`: RPCS3 empties it at startup), and the next launch marks a probe
that started but never ended as `CRASHED`, then runs the next one. After the last probe it
writes `done`.

| # | Region | What it tests |
|---|--------|---------------|
| 0 | all below | page attributes only (`sys_memory_get_page_attribute`, syscall 351), safe |
| 1 | heap (`memalign`) | plain malloc'd memory |
| 2 | `.bss` | static data |
| 3 | `sysMemoryAllocate`, 64 KB pages | lv2 memory |
| 4 | `sysMemoryAllocate`, 1 MB pages | lv2 memory |
| 5 | `jit_cave` in `.text` | a plain user-mode write into the app's own code |
| 6 | `jit_cave` via lv2 poke (syscall 7) | kernel write into `.text`; only runs if `/dev_hdd0/jitprobe_allow_poke` exists (a kernel fault could hang the console) |
| 7 | middle and end of the cave | code-cache size: write, run, rewrite, run again (build with `CAVE_MB=n`) |

## Results (2026-11-01)

Identical on Evilnat 4.93 CFW and PS3HEN 3.5.0:

- 0: every region reports `0x40000` (read-write), `.text` included: attributes say nothing about execute rights.
- 1-4: crash (not executable). The console survives; the app just exits.
- **5: returns 42. `.text` is writable and executable from user mode**, so a dynarec can keep its
  code cache in a block reserved inside `.text`.
- 6: not run (not needed after 5).

Details and the HEN remote-testing method: `docs/ps3-homebrew-notes.md` ("Executable memory").

Probe 7 (2026-11-01): RPCS3 runs it for 4-32 MB caves only when the code segment is flagged RWX
(`WRITABLE_TEXT=1`); the HEN attempt with 32 MB + RWX hung the console (see the notes).

## Running it

```sh
PS3=<ip> tools/jitprobe.sh deploy    # build, upload to /dev_hdd0/game/PGBA00099, reset results
PS3=<ip> tools/jitprobe.sh run       # launch once (= one probe), show results; repeat until "done"
CAVE_MB=16 FROM=7 WRITABLE_TEXT=1 PPU=interp tools/jitprobe.sh rpcs3   # probe 7 in RPCS3
```

On CFW the new folder shows up in the XMB after a restart. On HEN it never does: swap the EBOOT
into a listed homebrew temporarily (see the notes).
