@echo off
rem Starts Haunting Ground (PC recomp) in a window. Close the game window to quit.
rem The minimized "Haunting Ground log" window only collects the log; it closes by itself with the game.
rem Disc image: none is passed here. build.py finds your own image: HG_ISO, then [Game] Iso= in hauntingground.ini,
rem then the first *.iso that boots SLUS_210.75 in game\ next to this file. Set HG_ISO=<path> before starting
rem to use another one.
rem Log of the last session: build\port\bin\logs\play_last.log (the one before is kept as play_<time>.log).
cd /d "%~dp0"
rem Load every known hardware raster shader (build\port\bin\game_data\gs_shaders\hw_keys.txt) in the first seconds
rem instead of on first use, so entering a room does not hitch on a shader compile (same as the 2K launcher).
set PS2X_GS_HW_PRELOAD=1
rem Per-2 s EE pacing line ([vblank-stats]) in the log.
set PS2X_PACING_STATS=1
if not exist "build\port\bin\logs" mkdir "build\port\bin\logs"
python -I build.py keep-play-log
start "Haunting Ground log" /min cmd /c "python -I build.py run > build\port\bin\logs\play_last.log 2>&1"
