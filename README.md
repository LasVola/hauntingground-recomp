# Haunting Ground Recompiled

> [!WARNING]
> **Early work in progress.** So far one person has played it, for about 16 minutes, in a normal window (2K
> launcher, October 19, 2026). That player saw a short black flash of the screen and a few glitches in a cutscene;
> both are still being investigated. Everything else listed as working below was seen in hidden, scripted test runs
> (no visible window, sound muted, input from a script) on October 18-19, 2026. Saving and loading are untested.
> Expect bugs and crashes. Bug reports with the log (`build\port\bin\logs\play_last.log`) and, for a picture
> problem, a bug snapshot (press **F8** right after it, see below) are welcome.

**A native PC port of Haunting Ground (PS2, 2005) made by static recompilation. No emulator.**

Haunting Ground was released by Capcom on the PS2 only. This project turns the game's own PS2 code into a Windows
program using [PS2Recomp](https://github.com/ran-j/PS2Recomp). You supply your own copy of the game.

## Status

Played by a person (October 19, 2026): about 16 minutes in a normal window with `Start-Haunting-Ground-2K.cmd`, on
a build from before the bug recorder. Reported: a short black flash of the screen and a few glitches in a cutscene
(open, see "Known issues"). The VU1 code images met for the first time in play were added to the author's
ahead-of-time VU1 build afterwards (see "VU1 programs").

Seen working in hidden test runs (October 18-19, 2026; window hidden, sound muted, input from a script):

- **Recompiled:** all 11,452 functions of the boot program `SLUS_210.75`, with no stubs and no unhandled instructions.
- **Boot to gameplay:** Capcom logo, opening movie, title screen, main menu, NEW GAME, intro movie, then the first
  room (room 0x2A in the game's numbering). In that room a scripted stick input moves Fiona (seen with both
  renderers).
- **Speed:** with the VU1 programs compiled ahead of time (see below), the first room runs at full speed (60 fields/s)
  on the GPU renderer, at 1x and at 3x internal resolution. Without them it ran at about 14.
- **Picture:** the GPU renderer (the default) shows the same picture as the CPU renderer on the logo, the movies, the
  title, the menu and the first room (pixel comparison, 1x). At 3x the first room matched the 1x picture (at most 2
  of 286,720 pixels clearly different) after a fix for the edges of the shadow volumes.
- **Sound:** the movies, the menu and the first room produce sound in recordings of the port's audio output. It has
  not been checked in detail beyond that.
- **PC-key prompts:** on the OPTIONS screen the button icons showed the default keys (both renderers); with the
  prompts turned off the game's font stayed exactly as on the disc.
- **Bug recorder:** an F8 snapshot (started by a test hook, not a real key press) and an automatic snapshot of a
  simulated black field were written and checked (images decode, the recorded GS stream replays to the same picture).

Not tested yet:

- memory card saving and loading (no save point was reached in a test run; a new, empty memory card still has to be
  formatted by the game). A fix for failed card writes is in the code, checked by source and build only;
- most of the game after the first rooms. Other rooms may use VU1 programs that are not compiled yet. They then run
  slower (see "VU1 programs" below);
- the PC-key prompts with a controller on screen (the Xbox-style art was checked offline only), switching devices
  and F1 rebinding live, and other screens that show button icons;
- a real F8 key press and its on-screen message; the bug recorder at 1x;
- save states (F5 / F9).

## New since the first upload

- **2K launcher** `Start-Haunting-Ground-2K.cmd`: 3x internal resolution (1920x1344 for the game's 640x448 picture)
  in a maximized window (F11 = borderless full screen and back), with every known hardware shader loaded at start.
  `hauntingground.ini` is not changed by it. The 1x launcher now preloads the shaders too.
- **PC-key button prompts:** the game's ten PS2 button icons show the keys you have bound (grey key caps with white
  labels). With an Xbox-style controller they show A B X Y / LB RB LT RT / View Menu, with a PlayStation controller
  the original icons. They follow F1 rebinds and the last device used. A button with no key keeps its PS2 icon.
  `HG_PROMPTS=0` (environment) turns them off.
- **Bug recorder** (on by default, `HG_BUG_RECORDER=0` turns it off): keeps the last ~3 s of small frames and ~2 s
  of GS commands in RAM (at most 256 MB for the commands). **F8** saves a bug snapshot to
  `build\port\bin\logs\bugshots\<time>_<field>\` (`contact.png` = the last 48 frames, black ones framed red;
  `gs.gsr` = the recorded GS stream; a message shows the folder). A black flash (1-6 black or empty fields between
  normal ones) saves one by itself, at most one per 30 s and 20 per session. A snapshot at 3x takes about 350 MB
  (F8) or 175 MB (automatic) of disk; below 8 GB free no GS stream is written, below 2 GB only a text file. Delete
  old snapshot folders when you no longer need them.
- **VU1 hybrid ahead-of-time code:** VU1 programs run whole-image compiled code first, then content-keyed compiled
  units where no whole-image code exists, then the interpreter (see "VU1 programs").
- Fixes: shadow-volume edges at internal resolutions above 1x; a failed memory card write is no longer reported to
  the game as written; the two audio debug dumps have separate switches (`PS2X_AUDIO_DUMP` = SPU2 output,
  `PS2X_AUDIO_BACKEND_DUMP` = the audio backend's WAV) instead of sharing one.

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

To skip a movie, press Start (this pauses it), then Triangle. F11 switches full screen on and off, F8 saves a bug
snapshot (see "New since the first upload"), and F12 opens the runtime debugger. To quit, close the window.

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

Start the game with `Start-Haunting-Ground.cmd` (1x, or `python -I build.py run`) or `Start-Haunting-Ground-2K.cmd`
(3x, maximized window). The log of the last session is `build\port\bin\logs\play_last.log` (the one before is kept
as `play_<time>.log`).

### VU1 programs (needed for full speed in 3D)

1. Play once with `PS2X_VU1_CAPTURE=<full path of an empty folder>` set in the same console, for example
   `set PS2X_VU1_CAPTURE=C:\hg-vu1` and then `Start-Haunting-Ground.cmd`. Later, any program that is still not
   compiled is logged as `[vu1-aot] image ... has no compiled code` and saved as `vu1_<hash>.bin` in
   `build\port\bin\logs\vu1_missing\`. Copy those files into the same folder.
2. Build `vu1_recomp`: `python -I build.py configure -DHG_BUILD_TOOLS=ON`, then `python -I build.py build vu1_recomp`.
3. Run `build\port\runtime\ps2xTest\vu1_recomp.exe <that folder> generated\vu1`, then `python -I build.py build`.

Without these steps VU1 runs in the interpreter, which makes 3D scenes slow. The first room used 36 VU1 code images;
the author's current build has 75 (including the ones met so far in play).

How a VU1 program is run (Haunting Ground's default, `PS2X_VU1_CONTENT_AOT=2`): whole-image compiled code first (the
fast path above); where an image has none, a content-keyed compiled unit for the exact program bytes, if one was
built; the interpreter last. A whole-image miss is still logged and saved to `vu1_missing`. `PS2X_VU1_CONTENT_AOT=1`
tries content units first (exact but about three times the VU1 cost), `0` uses whole-image code only. The
content-keyed units are made with `vu1_recomp --content <inputs> generated\vu1` from input files that a development
tool outside this repository produces, so a fresh clone has only the whole-image path and the interpreter.
`[vu1-lookup]` lines in the log (with `PS2X_PACING_STATS=1`, set by both launchers) count which path ran.

These are the steps used during development. A build from a completely fresh clone has not been tried yet.

### Other commands

- `python -I build.py status` shows what exists so far.
- `python -I build.py boot --seconds 120` runs the game once, hidden and muted, with a time limit. The output goes to
  `build\port\bin\logs`.

## Known issues

- **Black flash (under investigation):** the screen sometimes goes black for a moment. The cause is not known. In
  hidden test runs a black frame was seen only with the GPU profiler on, and there the game's own picture was not
  black (the black frame came from the port's presentation path). The bug recorder saves a snapshot of such a flash
  by itself; pressing F8 right after one also helps.
- **Cutscene glitches (under investigation):** reported from play, not captured yet. Press F8 right after one.
- Saving and loading are untested (see "Status").
- `hauntingground.ini` defaults are set up for hidden test runs (`Headless=1`, `TimeLimitSeconds=120`). If you start
  `hauntingground.exe` directly, it runs hidden and stops after 120 s. Both `Start-Haunting-Ground*.cmd` launchers
  and `python -I build.py run` open a normal window with no time limit.
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
  - module loading: `sceSifLoadStartModule` returns the module's real start result;
  - GS: shadow-volume edges above 1x internal resolution; a recording ring for the bug recorder
    (`gs_recording`, `gs_threaded_backend`);
  - VU1: the content-keyed compiled units and the hybrid lookup (`ps2_vu1_core.cpp`, `ps2_vu1_content.h`,
    `vu1_recomp --content`);
  - memory card: a failed host write is reported as failed.

  Each change is commented where it is made.
- **Port-side features:** the PC-key button prompts (`src/hg/button_prompts`, `prompt_cells`, `prompt_art`) redraw
  the ten button icons of the game's small font in guest RAM after checking that the font is the disc's own; the bug
  recorder is `runtime/ps2xRuntime/src/lib/ps2_bug_recorder.cpp`.

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
