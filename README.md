# Haunting Ground Recompiled

> [!WARNING]
> **Early work in progress. Not played by a human yet.**
> Everything listed as working below was seen only in hidden, scripted test runs (no visible window, sound muted,
> input from a script) on October 18, 2026. The game boots, plays the Capcom logo and the opening movie, reaches the
> title screen and menu, starts a NEW GAME and enters the first room, where scripted input moves Fiona and the room
> runs at full speed. Nothing past that first room has been run. Saving, loading and internal resolutions above 1x are untested. Expect bugs and
> crashes. Bug reports with the log (`build\port\bin\logs\play_last.log`) are welcome.

**A native PC port of Haunting Ground (PS2, 2005) made by static recompilation. No emulator.**

Haunting Ground was released by Capcom on the PS2 only. This project turns the game's own PS2 code into a Windows
program using [PS2Recomp](https://github.com/ran-j/PS2Recomp). You supply your own copy of the game.

## Status

Seen working in hidden test runs (October 18, 2026):

- **Recompiled:** all 11,452 functions of the boot program `SLUS_210.75`, with no stubs and no unhandled instructions.
- **Boot to gameplay:** Capcom logo, opening movie, title screen, main menu, NEW GAME, intro movie, then the first
  room (room 0x2A in the game's numbering). In that room a scripted stick input moves Fiona (seen with both
  renderers).
- **Speed:** with the VU1 programs compiled ahead of time (see below), the first room runs at full speed (60 fields/s)
  on the GPU renderer. Without them it ran at about 14.
- **Picture:** the GPU renderer (the default) shows the same picture as the CPU renderer on the logo, the movies, the
  title, the menu and the first room (pixel comparison), at 1x internal resolution.
- **Sound:** the movies, the menu and the first room produce sound in recordings of the port's audio output. Nobody
  has listened to it during a normal session yet.

Not tested yet:

- play by a human, or any session in a visible window;
- memory card saving and loading (no save point was reached; the new, empty memory card still has to be formatted
  by the game);
- anything after the first room. Other rooms may use VU1 programs that are not compiled yet. They then run slower
  (see "VU1 programs" below);
- internal resolution above 1x. Haunting Ground's interlace flicker filter has no fix in this port yet, so edges may
  look doubled above 1x;
- save states (F5 / F9).

## Controls

Press **F1** in game (or **Back + Start** on a controller) to change any key or button. Changes are saved to
`controls.ini` next to `hauntingground.exe` and work right away. The action names come from this port's Controls
screen. Not all of them have been checked in play yet.

| Action | Keyboard | Controller |
|--------|----------|------------|
| Move | W A S D | Left stick |
| Run (hold) / OK | K | A (Cross) |
| Act / examine | L | B (Circle) |
| Shove / kick | J | X (Square) |
| Throw / set down / cancel | I | Y (Triangle) |
| Crouch | Q | LB (L1) |
| Flee / escape | E | RB (R1) |
| L2 / R2 (no action named yet) | 1 / 3 | LT / RT |
| Hewie: go / attack | T | Right stick up |
| Hewie: come back | G | Right stick down |
| Hewie: scold | F | Right stick left |
| Hewie: praise / tend | H | Right stick right |
| Hewie: stay / come | V | R3 (right stick click) |
| L3 (no action named yet) | C | L3 (left stick click) |
| Pause | Enter | Start |
| Menu | Backspace | Back (Select) |
| Menus | Arrow keys | D-pad |

To skip a movie, press Start (this pauses it), then Triangle. F11 switches full screen on and off, and F12 opens the
runtime debugger. To quit, close the window.

## Build and run

You need Windows 10/11, Visual Studio 2022 (or the Build Tools) with the x64 C++ tools (CMake and Ninja come with
it), Python 3.10+, git, and your own legally owned copy of Haunting Ground (USA, SLUS-21075). Check your disc image
against [DISC_HASHES.md](DISC_HASHES.md). The game's boot file must match, or the port refuses to start.

```powershell
git clone https://github.com/LasVola/hauntingground-recomp.git
cd hauntingground-recomp
copy "<path to your copy>\Haunting Ground (USA).iso" game\
python -I build.py generate    # builds the recompiler, then recompiles SLUS_210.75 from your disc into generated\
python -I build.py configure   # CMake + Ninja; downloads raylib, imgui and rlImGui on the first configure
python -I build.py build       # build\port\bin\hauntingground.exe
```

Instead of copying the image into `game\`, you can pass `--iso <your image>` to `generate`, set `HG_ISO`, or set
`[Game] Iso=` in `hauntingground.ini`. The full build compiles over 11,000 generated files and needed close to 30 GB
of RAM with many compilers running at once. Set `HG_JOBS=8` (or lower) to run fewer.

Start the game with `Start-Haunting-Ground.cmd` (or `python -I build.py run`). The log of the last session is
`build\port\bin\logs\play_last.log`.

### VU1 programs (needed for full speed in 3D)

1. Play once with `PS2X_VU1_CAPTURE=<full path of an empty folder>` set in the same console, for example
   `set PS2X_VU1_CAPTURE=C:\hg-vu1` and then `Start-Haunting-Ground.cmd`. Later, any program that is still not
   compiled is logged as `[vu1-aot] image ... has no compiled code` and saved as `vu1_<hash>.bin` in
   `build\port\bin\logs\vu1_missing\`. Copy those files into the same folder.
2. Build `vu1_recomp`: `python -I build.py configure -DHG_BUILD_TOOLS=ON`, then `python -I build.py build vu1_recomp`.
3. Run `build\port\runtime\ps2xTest\vu1_recomp.exe <that folder> generated\vu1`, then `python -I build.py build`.

Without these steps VU1 runs in the interpreter, which makes 3D scenes slow. The first room used 36 VU1 code images.

These are the steps used during development. A build from a completely fresh clone has not been tried yet.

### Other commands

- `python -I build.py status` shows what exists so far.
- `python -I build.py boot --seconds 120` runs the game once, hidden and muted, with a time limit. The output goes to
  `build\port\bin\logs`.

## Known issues

- Not played by a human yet; see the warning at the top.
- `hauntingground.ini` defaults are set up for hidden test runs (`Headless=1`, `TimeLimitSeconds=120`). If you start
  `hauntingground.exe` directly, it runs hidden and stops after 120 s. `Start-Haunting-Ground.cmd` and
  `python -I build.py run` open a normal window with no time limit.
- When the opening movie is skipped, the title screen is silent. The game's own code does this (it plays the title
  sound only when the movie was not skipped), so it is not a bug in the port.
- Rooms with VU1 programs that are not compiled run slowly until those programs are added (see above).

## How it works

- **The main program** (`SLUS_210.75`) is recompiled to C++. The disc's ELF has one section that holds code and data
  together. `recomp/hauntingground_sections.csv` splits it, so that only real code is translated. `build.py generate`
  gives the recompiler a copy with those section headers, while the runtime loads the disc's own ELF unchanged.
- **Function boundaries and names** (`recomp/hauntingground_functions.csv`) come from the
  [OpenHuey](https://github.com/ArnautDaniel/OpenHuey) decompilation's symbols plus the PS2 SDK entry points.
- **SDK functions** are bound by address to runtime handlers (`src/hg/hle_bindings.cpp`). Haunting Ground loads no
  code at run time, so there are no overlays.
- **Runtime fixes made for this game:**
  - EE: the kernel stack top for `SetupThread`;
  - IOP: alarms (`SetAlarm`), RPC binding before the server is up, sysclib `strtol`/ctype, file I/O, `sceCdSearchFile`
    on the real disc image, and module placement through the IOP allocator;
  - recompiler: the `PMFHL` instruction variants;
  - module loading: `sceSifLoadStartModule` returns the module's real start result.

  Each change is commented where it is made.

## Credits and legal

- [PS2Recomp](https://github.com/ran-j/PS2Recomp) by ran-j and contributors: the recompiler and runtime.
- [LightVelox](https://github.com/LightVelox): the [sotc-port fork of PS2Recomp](https://github.com/LightVelox/PS2Recomp)
  and the [Shadow of the Colossus port](https://github.com/LightVelox/sotc-vibe-pc).
- [thiagordsfx](https://github.com/thiagordsfx): the [Urban Reign port](https://github.com/thiagordsfx/urban-reign-recomp).
  Its runtime and VU1 compiler are what this port builds on.
- [ArnautDaniel/OpenHuey](https://github.com/ArnautDaniel/OpenHuey) (GPL-3.0): function names and boundaries.
- [God Hand Recompiled](https://github.com/LasVola/godhand-recomp): this port started as a copy of it.
- [Ace Combat 5 static recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation): the IPU (movie)
  decoder (Apache-2.0).
- [PCSX2](https://github.com/PCSX2/pcsx2): a reference for how the hardware behaves.
- Made with AI assistance (Claude Code and OpenAI Codex).

The code here is GPL-3.0 licensed ([LICENSE](LICENSE)), like PS2Recomp. Third-party licenses are in `docs/licenses/`.
**No game data is included.** You need your own copy of the game.
**Haunting Ground** is a trademark of Capcom. Capcom owns the game. This project is not affiliated with Capcom.
