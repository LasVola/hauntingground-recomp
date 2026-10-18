cmake_minimum_required(VERSION 3.21)

# Haunting Ground (USA, SLUS-21075) native PC port. Cloned from the God Hand port's top-level CMakeLists.txt (itself
# adapted from the Urban Reign port, forks/urban-reign-recomp @2fa53c4); port_prep/WORKLOG.md lists what changed.
project(HauntingGroundPC LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# runtime/: the PS2Recomp runtime (EE/IOP/VU/GS emulation), recompiler and offline tools (LightVelox/PS2Recomp
# sotc-port ac9efa0 + the Urban Reign port's changes). thirdparty/: PCSX2's GS and what it needs (optional below).
option(HG_BUILD_RECOMP "Build ps2_recomp from runtime/ps2xRecomp" OFF)
# runtime/ps2xTest holds the offline tools: vu1_recomp (VU1 microcode AOT), gs_replay, vu0_audit.
option(HG_BUILD_TOOLS "Build the offline tools in runtime/ps2xTest (vu1_recomp...)" OFF)
# runtime/CMakeLists.txt (Urban's) reads these two names.
set(URBAN_BUILD_RECOMP ${HG_BUILD_RECOMP})
set(URBAN_BUILD_TOOLS ${HG_BUILD_TOOLS})

# In-game ImGui "Runtime Debugger" (threads, kernel, IOP/SIF, RPC history, GS, DMA, memory); F1 toggles it.
option(HG_DEBUG_UI "Build the ImGui runtime debugger into hauntingground.exe" ON)
set(PS2X_ENABLE_DEBUG_UI ${HG_DEBUG_UI} CACHE BOOL "" FORCE)
# FFmpeg MPEG decoder for the libmpeg HLE. OFF: ON downloads a prebuilt LGPL FFmpeg at configure time
# (runtime/ps2xRuntime/CMakeLists.txt, ExternalProject ffmpeg_external).
option(HG_FFMPEG "Decode MPEG video with FFmpeg (downloads a prebuilt FFmpeg)" OFF)
set(PS2X_ENABLE_FFMPEG ${HG_FFMPEG} CACHE BOOL "" FORCE)
# Controllers through SDL3 (downloads the SDL3 source at configure time). OFF: GLFW's gamepads (raylib) still work.
option(HG_SDL_INPUT "Read controllers through SDL3" OFF)
set(PS2X_ENABLE_SDL_INPUT ${HG_SDL_INPUT} CACHE BOOL "" FORCE)
# Runtime logs: the normal ones stay, the very verbose ones (per-call function log tracker) are off.
set(PS2X_ENABLE_AGRESSIVE_LOGS OFF CACHE BOOL "" FORCE)
set(PS2X_ENABLE_SCCACHE OFF CACHE BOOL "" FORCE)

add_subdirectory(runtime EXCLUDE_FROM_ALL)
add_subdirectory(src)

# PCSX2's GS as pcsx2gs.dll (DX11), loaded at run time when [Video] Renderer=pcsx2. Optional and OFF: its wrapper still
# carries Urban Reign's GameIndex fixes and serial (thirdparty/pcsx2-gs/compat/Bridge.cpp, HostStubs.cpp), and the
# runtime falls back to its own GS when the DLL is missing (gs_pcsx2_backend.cpp, GSPcsx2Backend::Requested).
option(HG_PCSX2_GS "Build PCSX2's GS (pcsx2gs.dll)" OFF)
if(HG_PCSX2_GS AND WIN32)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/thirdparty/pcsx2-gs/CMakeLists.txt")
        message(FATAL_ERROR "HG_PCSX2_GS=ON needs thirdparty/pcsx2-gs (copy it from the Urban Reign port)")
    endif()
    add_subdirectory(thirdparty/pcsx2-gs EXCLUDE_FROM_ALL)
    if(TARGET hauntingground)
        add_dependencies(hauntingground pcsx2gs)
    endif()
    target_link_libraries(ps2_runtime PRIVATE pcsx2gs_api)
endif()

# HG_LTO: also /GL for ps2_iop and the port sources (linked with /LTCG). ps2_runtime always gets /GL in
# RelWithDebInfo/Release from the runtime's own EnableFastReleaseMode, and hauntingground.exe links /LTCG in RelWithDebInfo
# (src/CMakeLists.txt). The generated code (~10k files) never gets /GL.
option(HG_LTO "Whole-program optimization of the runtime, the IOP and the port (not the generated code)" OFF)
if(MSVC AND HG_LTO AND TARGET hauntingground)
    foreach(target ps2_runtime ps2_iop hauntingground)
        target_compile_options(${target} PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/GL>)
    endforeach()
    set_property(TARGET ps2_runtime ps2_iop APPEND PROPERTY STATIC_LIBRARY_OPTIONS $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
    target_link_options(hauntingground PRIVATE $<$<CONFIG:RelWithDebInfo,Release>:/LTCG>)
endif()
