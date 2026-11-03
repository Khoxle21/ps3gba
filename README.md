# ps3gba

**Play Game Boy Advance games on a PS3, up to 4 players at once on the same TV.**
Developed by Khoxle.

ps3gba turns a modded PlayStation 3 into a 4-player Game Boy Advance emulator. The screen splits in
up to four, each player gets their own controller, game and save, and the consoles can be linked
with a virtual link cable or Wireless Adapter to trade and battle, like with real GBAs back in the
day.
Game Boy and Game Boy Color games work too.

No games are included: use dumps of the cartridges you own.

## What you can do

- Up to 4 players side by side, each in their own game (or the same one).
- Link the games together from the pause menu to trade or battle: with the link cable, or with the
  GBA Wireless Adapter for games that use it, such as the Union Room of Pokemon FireRed (up to 4
  players).
- Manage saves: several per game, rename, copy, delete.
- Add games, covers and saves from your phone: each player scans a QR code and gets a small web
  page, working only on the same network as the PS3. Missing box art is fetched automatically.
- A pixel-art menu with music, a 60 fps display, and the sound of player 1's game (or of the
  first player in a game).

## What you need

- A PS3 that can run homebrew: custom firmware (tested with Evilnat 4.93) or PS3HEN (tested with
  3.5.0).
- [webMAN MOD](https://github.com/aldostools/webMAN-MOD) on the PS3, to copy files to it over the
  network (FTP).
- One controller per player.

## Install and play

1. Get the latest `ps3gba-vX.Y.Z.pkg` from this project's
   [releases](https://github.com/Khoxle21/ps3gba/releases) (or build it yourself, see
   [CONTRIBUTING.md](CONTRIBUTING.md)) and install it on the PS3: copy it to a USB stick or to
   `/dev_hdd0/packages/`, then use Package Manager > Install Package Files.
2. Copy your games to `/dev_hdd0/ps3gba/roms/` on the PS3 over FTP (`.gba`, `.gb`, `.gbc`), or
   add them later from the phone page.
3. Start ps3gba from the XMB, press Start, choose PLAY. Each player presses Start on their
   controller to join, picks a game and a save.

In game: Cross is A, Circle is B, L3 or R3 opens the pause menu (resume, quit, link cable,
wireless adapter, frame rate). Every menu shows which buttons do what at the bottom.

## How it works, in short

A PS3 is far more powerful than a Game Boy Advance, but very differently built, so emulating four
of them at full speed takes a few tricks:

- The emulation itself is [mGBA](https://mgba.io), a very accurate GBA emulator, adapted to the
  PS3 (each player's emulator runs on its own thread).
- Instead of interpreting the GBA's program one instruction at a time, ps3gba translates it into
  native PS3 code as it runs (a "JIT"), which is several times faster.
- Drawing each GBA's picture is handed to the PS3's Cell coprocessors (SPUs), one per player.
- Only the game you hear computes its sound; the others skip it without noticing.

Together, four players hold 60 frames per second even on demanding games.

## Learn more

- [CONTRIBUTING.md](CONTRIBUTING.md): build it yourself, develop and test (for developers and
  coding agents).
- [CHANGELOG.md](CHANGELOG.md): what changed in each version.
- [docs/](docs/): the deep dives: what we learned about PS3 hardware
  ([ps3-homebrew-notes.md](docs/ps3-homebrew-notes.md)), how the JIT works
  ([jit.md](docs/jit.md)), and how 4 players reached 60 fps
  ([performance-plan.md](docs/performance-plan.md)).

## License and credits

ps3gba is developed by Khoxle and is free software under the Mozilla Public License 2.0
([LICENSE](LICENSE)), like mGBA. It is a fan project, not affiliated with Sony or Nintendo.

Built on [mGBA](https://github.com/mgba-emu/mgba) by Jeffrey Pfau (endrift) and contributors
(MPL-2.0); PSL1GHT, ps3toolchain and ps3libraries by ps3dev; Tiny3D by Hermes; Cairo, Pixman,
FreeType, libpng and zlib; stb_vorbis by Sean Barrett; QR Code generator by Project Nayuki. Music:
"Chill Overworld" by ATMANAN (CC-BY 4.0). Font: Press Start 2P by codeman38 (SIL OFL 1.1). Details
in [data/CREDITS.txt](data/CREDITS.txt).
