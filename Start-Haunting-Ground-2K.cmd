@echo off
rem Starts Haunting Ground (PC recomp) at 3x internal resolution ("2K": 1920x1344 for the game's 640x448 picture),
rem in a maximized window (F11 = borderless full screen and back). Close the game window to quit.
rem Same as Start-Haunting-Ground.cmd otherwise; hauntingground.ini is not changed (it keeps InternalResolution=1,
rem the variable below wins over it for this start only). Remove the HG_START_MAXIMIZED line for a small window.
rem The minimized "Haunting Ground 2K log" window only collects the log; it closes by itself with the game.
rem Disc image: none is passed here. build.py finds your own image: HG_ISO, then [Game] Iso= in hauntingground.ini,
rem then the first *.iso that boots SLUS_210.75 in game\ next to this file. Set HG_ISO=<path> before starting
rem to use another one.
rem Log of the last session: build\port\bin\logs\play_last.log (the one before is kept as play_<time>.log).
cd /d "%~dp0"
rem Internal resolution 3x (the runtime reads PS2X_GS_SCALE; same as InternalResolution=3 in the ini).
set PS2X_GS_SCALE=3
set HG_START_FULLSCREEN=0
set HG_START_MAXIMIZED=1
rem Load every known hardware raster shader (build\port\bin\game_data\gs_shaders\hw_keys.txt, the shaders earlier
rem sessions compiled) in the first seconds instead of on first use: without it the 3x picture fell back to the slow
rem compute rasterizer for ~0.5 s when the first room appeared (141 ms dropped in a test run; with it 0).
set PS2X_GS_HW_PRELOAD=1
rem Per-2 s EE pacing line ([vblank-stats]) in the log.
set PS2X_PACING_STATS=1
if not exist "build\port\bin\logs" mkdir "build\port\bin\logs"
python -I build.py keep-play-log
start "Haunting Ground 2K log" /min cmd /c "python -I build.py run > build\port\bin\logs\play_last.log 2>&1"
