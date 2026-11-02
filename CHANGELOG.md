# Changelog

Newest first. A major step or optimization is a 0.1 step, a display change or a fix 0.0.1;
1.0.0 will be the first release. The build shows the newest version on the welcome screen
(the Makefile reads the first `## x.y.z - date` heading below).

## 0.17.8 - 2026-11-02
- Faster startup: game covers load in the background after the first frame (on the console the
  first frame came 2.6 s after start, now 0.9 s). Covers added over FTP now appear on the next
  library refresh too.

## 0.17.7 - 2026-11-02
- The app is named ps3gba, one word in lowercase: XMB title, menus, About page, phone page.

## 0.17.6 - 2026-11-02
- Welcome screen: "DEVELOPED BY KHOXLE" and the version in the bottom right corner, barely
  visible; one quote fewer.

## 0.17.5 - 2026-11-02
- Welcome screen: retro quotes type in and erase under PRESS START; the juggling scene is a bit
  smaller and sits higher.

## 0.17.4 - 2026-11-02
- About page: the author's message.

## 0.17.3 - 2026-11-02
- Main menu: QUIT goes back to the XMB.

## 0.17.2 - 2026-11-02
- Fix: no sound on TVs that can't play multichannel audio (the app declared 5.1/7.1, Dolby and
  DTS; it now declares stereo, what it outputs).

## 0.17.1 - 2026-11-02
- Fix: games could read back video register bits the GBA drops (SPU renderer's proxy).

## 0.17.0 - 2026-11-02
- JIT: multi-register loads and stores (LDM/STM, PUSH/POP) run natively in IWRAM and EWRAM
  instead of calling mGBA: ~2 ms less per emulator per frame. 4 Pokemon Unbound players hold a
  steady 60 fps.

## 0.16.1 - 2026-11-01
- Fix: linked games had dropped to 30 fps (the main loop waited for a game asleep on its cable).

## 0.16.0 - 2026-11-01
- mGBA DMA fast path: copies run in one go instead of one word per event, identical to
  unmodified mGBA (new exactness test). A game that falls behind keeps a frame queued.

## 0.15.0 - 2026-11-01
- Only the heard game makes sound; the others skip it, invisibly to them. The heard game and its
  cable partners run first.

## 0.14.0 - 2026-11-01
- Players of the same game share one ROM: 4 Pokemon Unbound players fit in memory (the 4th had
  run out and slowed everyone down).

## 0.13.0 - 2026-11-01
- JIT: resumes in the middle of a translated block instead of translating partial ones: ~20x
  fewer discarded translations with self-modifying code.

## 0.12.0 - 2026-11-01
- Faster UI drawing: no CPU waits for the GPU mid-frame, draw calls batched through one sprite
  sheet (103 to 9 with 4 games).

## 0.11.0 - 2026-11-01
- Link cable switch in the pause menu; games start unplugged (the cable costs ~3.5 ms per frame
  per player when nobody trades). Fixes a freeze when joining while linked.

## 0.10.2 - 2026-11-01
- PERF overlay: each game's frame rate first, red below 59 fps.

## 0.10.1 - 2026-11-01
- Fix: sound cut out when a game slowed down (the output rate bends, 43 ms queued).

## 0.10.0 - 2026-11-01
- JIT handles self-modifying code: a translation comes back when its code is written back
  (Pokemon Unbound's sound mixer rewrites itself ~100 times a second).

## 0.9.6 - 2026-11-01
- Phone page: the buttons work again; covers missing on the PS3 are fetched from
  libretro-thumbnails by the phone.

## 0.9.5 - 2026-11-01
- Main menu between the welcome screen and the games: PLAY, ABOUT, HELP.

## 0.9.4 - 2026-11-01
- Game carousel: one catalog per system (GBA, GBC, GB tabs), L1/R1 to switch.

## 0.9.3 - 2026-11-01
- Menu music ("Chill Overworld" by ATMANAN) until a game starts.

## 0.9.2 - 2026-11-01
- A legend of the buttons that do something in every menu.

## 0.9.1 - 2026-11-01
- Welcome screen: the PS3 juggles the four GBAs until Start or Cross.

## 0.9.0 - 2026-11-01
- GB and GBC games (mGBA's GB core). Covers found by ROM header, so ROM hacks show the
  original's; otherwise a blank Game Pak of the system.

## 0.8.0 - 2026-11-01
- Phone web server: a QR code per player to upload games, covers and saves, download saves and
  set a name.

## 0.7.0 - 2026-11-01
- Save management: rename, copy and delete from the saves list.

## 0.6.2 - 2026-11-01
- Fix: the JIT slowed down while a game saved.

## 0.6.1 - 2026-11-01
- Starting a game no longer freezes the others (it loads on its player's thread); Cross is A in
  game; XMB icon.

## 0.6.0 - 2026-11-01
- JIT: ARM and Thumb code translated to PowerPC, cycle-exact, idle loops skipped, self-checked
  against the interpreter. With the SPU renderer: 4 linked players at 60 fps (FireRed).

## 0.5.0 - 2026-11-01
- mGBA's renderer runs on the SPUs, one per player, pixel-identical.

## 0.4.1 - 2026-11-01
- Fix: the link cable on the console.

## 0.4.0 - 2026-10-31
- Runs on a real PS3 (CFW). Each player's emulator runs on its own thread.

## 0.3.0 - 2026-10-31
- Pixel-art UI and player flow: join with any controller, game carousel with covers, saves
  list, pause menu.

## 0.2.0 - 2026-10-31
- Up to 4 players side by side, linked by an emulated link cable.

## 0.1.0 - 2026-10-31
- First boot: one GBA game on the PS3 (mGBA core, Tiny3D rendering, PSL1GHT), with sound and
  frame pacing on the display's refresh.
