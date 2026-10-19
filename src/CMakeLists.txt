# Haunting Ground game layer (hauntingground.exe) and the recompiled game code. Cloned from the God Hand port's
# src/CMakeLists.txt (adapted from the Urban Reign port's); God Hand's cheats, button prompts and SNR2 module packs
# were removed on 2026-10-18 (Haunting Ground loads no code at run time). Haunting Ground's own button prompts
# (hg/button_prompts, hg/prompt_art, hg/prompt_cells) were added the same day.

# HG_ARCH: instruction set the compiler may use in the generated code (game code + VU1 AOT, the bulk of the build).
# SSE41 (default): one exe for every CPU with SSE4.1 (MSVC has no /arch:SSE4.2, so its own code stays SSE2 and the
# runtime's explicit SSE4.1 intrinsics still apply). AVX2: only to compare speed; the exe then needs an AVX2 CPU.
set(HG_ARCH "SSE41" CACHE STRING "Instruction set of the generated code: SSE41 (default) or AVX2 (comparison only)")
set_property(CACHE HG_ARCH PROPERTY STRINGS SSE41 AVX2)
if(HG_ARCH STREQUAL "AVX2")
    set(HG_ARCH_FLAG /arch:AVX2)
else()
    set(HG_ARCH_FLAG "")
endif()

set(HG_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated")
set(HG_RUNTIME_INCLUDE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/include")

if(NOT EXISTS "${HG_GENERATED_DIR}/register_functions.cpp")
    if(HG_BUILD_RECOMP)
        # Configure for building ps2_recomp only (build.py generate on a fresh tree): no hauntingground target yet.
        message(WARNING
            "Recompiled game code not found in ${HG_GENERATED_DIR}; configuring without the hauntingground target.\n"
            "Build ps2_recomp, then run: python build.py generate")
        return()
    endif()
    message(FATAL_ERROR
        "Recompiled game code not found in ${HG_GENERATED_DIR}.\n"
        "Put your own Haunting Ground (USA) disc image in game/ and run: python build.py generate")
endif()

file(GLOB HG_GENERATED_SOURCES CONFIGURE_DEPENDS "${HG_GENERATED_DIR}/*.cpp")

set(HG_HOST_SOURCES
    main.cpp
    hg/button_prompts.cpp
    hg/call_profile.cpp
    hg/call_trace.cpp
    hg/function_hooks.cpp
    hg/game_disc.cpp
    hg/hang_dump.cpp
    hg/hle_bindings.cpp
    hg/host_window.cpp
    hg/log.cpp
    hg/prompt_art.cpp
    hg/prompt_cells.cpp
    hg/sha256.cpp
    hg/thread_policy.cpp
)

# hauntingground.ini (top level) is built into hauntingground.exe as its default settings: hg_generated/hg/default_ini.h.
# The header is rewritten only when the text changes, so editing hauntingground.ini rebuilds main.cpp alone.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/hauntingground.ini")
file(READ "${CMAKE_SOURCE_DIR}/hauntingground.ini" HG_DEFAULT_INI)
string(LENGTH "${HG_DEFAULT_INI}" HG_DEFAULT_INI_LENGTH)
if(HG_DEFAULT_INI_LENGTH GREATER 16000)
    message(FATAL_ERROR "hauntingground.ini is ${HG_DEFAULT_INI_LENGTH} bytes; MSVC string literals stop at 16380 (split it)")
endif()
if(HG_DEFAULT_INI MATCHES "\\)GHINI\"")
    message(FATAL_ERROR "hauntingground.ini must not contain )GHINI\" (the raw string delimiter)")
endif()
set(HG_INI_HEADER "${CMAKE_CURRENT_BINARY_DIR}/hg_generated/hg/default_ini.h")
file(WRITE "${HG_INI_HEADER}.tmp"
    "// Generated from hauntingground.ini by src/CMakeLists.txt. Do not edit.\n"
    "#pragma once\n"
    "namespace hg\n"
    "{\n"
    "    inline constexpr const char *kDefaultIni = R\"GHINI(${HG_DEFAULT_INI})GHINI\";\n"
    "}\n")
file(COPY_FILE "${HG_INI_HEADER}.tmp" "${HG_INI_HEADER}" ONLY_IF_DIFFERENT)

# Does the runtime have ps2_host_display::setHeadless (LightVelox/PS2Recomp sotc-port 29d26f9)? The Urban Reign runtime
# (2fa53c4) does not; hg/host_window.cpp then hides the raylib window itself.
file(STRINGS "${HG_RUNTIME_INCLUDE}/runtime/ps2_display_options.h" HG_SET_HEADLESS_DECL REGEX "void[ \t]+setHeadless[ \t]*\\(")
if(HG_SET_HEADLESS_DECL)
    set(HG_RUNTIME_HAS_SET_HEADLESS 1)
else()
    set(HG_RUNTIME_HAS_SET_HEADLESS 0)
endif()
message(STATUS "Haunting Ground: runtime setHeadless available: ${HG_RUNTIME_HAS_SET_HEADLESS}")

# Reuse an already-built game code library when only host sources change (the full EE build is long).
set(HG_GAME_CODE_LIBRARY "" CACHE FILEPATH "Reuse an unchanged generated EE library for host-only builds")
if(HG_GAME_CODE_LIBRARY)
    if(NOT EXISTS "${HG_GAME_CODE_LIBRARY}")
        message(FATAL_ERROR "Cached generated EE library does not exist: ${HG_GAME_CODE_LIBRARY}")
    endif()
    add_library(hg_game_code STATIC IMPORTED)
    set_target_properties(hg_game_code PROPERTIES IMPORTED_LOCATION "${HG_GAME_CODE_LIBRARY}")
    target_include_directories(hg_game_code INTERFACE "${HG_GENERATED_DIR}")
    target_link_libraries(hg_game_code INTERFACE ps2_runtime)
    # The huge-function sources live in a sibling library built next to it.
    get_filename_component(HG_GAME_CODE_LIBRARY_DIR "${HG_GAME_CODE_LIBRARY}" DIRECTORY)
    target_link_libraries(hg_game_code INTERFACE "${HG_GAME_CODE_LIBRARY_DIR}/hg_game_code_huge.lib")
else()
    # /d2OptimizeHugeFunctions forces full optimization of functions MSVC would otherwise skip. Fine for most files, but
    # one huge function (Urban Reign: ~11k guest instructions, 2 MB of C++) ran over 40 minutes with it. Files above the
    # threshold go into hg_game_code_huge, which keeps MSVC's default huge-function handling.
    set(HG_HUGE_SOURCE_BYTES 900000 CACHE STRING "Generated files larger than this skip /d2OptimizeHugeFunctions")
    # Files that go to hg_game_code_huge whatever their size (file names, ';'-separated). Size alone misses them: the
    # cost comes from one function whose every guest instruction is a resume case (it has a JALR). Measured on the
    # first Haunting Ground build (2026-10-18): func_001AC0D0_0x1ac0d0.cpp (691 KB, 3,256 instructions, 3,256 cases)
    # with /d2OptimizeHugeFunctions was still in cl after 45 CPU minutes at 39 GB private memory (PC near out of RAM)
    # and was stopped.
    set(HG_HUGE_FORCE "func_001AC0D0_0x1ac0d0.cpp" CACHE STRING
        "Generated files always compiled in hg_game_code_huge (no /d2OptimizeHugeFunctions)")
    set(HG_HUGE_SOURCES "")
    foreach(source IN LISTS HG_GENERATED_SOURCES)
        file(SIZE "${source}" sourceBytes)
        get_filename_component(sourceName "${source}" NAME)
        if(source MATCHES "register_functions\\.cpp$")
            continue()
        endif()
        if(sourceBytes GREATER HG_HUGE_SOURCE_BYTES OR sourceName IN_LIST HG_HUGE_FORCE)
            list(APPEND HG_HUGE_SOURCES "${source}")
        endif()
    endforeach()
    if(HG_HUGE_SOURCES)
        list(REMOVE_ITEM HG_GENERATED_SOURCES ${HG_HUGE_SOURCES})
    endif()
    list(LENGTH HG_HUGE_SOURCES HG_HUGE_COUNT)
    message(STATUS "Haunting Ground: ${HG_HUGE_COUNT} generated files (over ${HG_HUGE_SOURCE_BYTES} bytes or in HG_HUGE_FORCE) go to hg_game_code_huge")

    # An empty static library is an error in some generators: a stub source keeps it valid when no file is huge.
    set(HG_HUGE_STUB "${CMAKE_CURRENT_BINARY_DIR}/hg_generated/huge_stub.cpp")
    file(WRITE "${HG_HUGE_STUB}.tmp" "// Keeps hg_game_code_huge non-empty.\nnamespace hg { int hugeStub() { return 0; } }\n")
    file(COPY_FILE "${HG_HUGE_STUB}.tmp" "${HG_HUGE_STUB}" ONLY_IF_DIFFERENT)
    add_library(hg_game_code_huge STATIC ${HG_HUGE_SOURCES} "${HG_HUGE_STUB}")
    target_include_directories(hg_game_code_huge PUBLIC "${HG_GENERATED_DIR}")
    target_link_libraries(hg_game_code_huge PUBLIC ps2_runtime)
    # Each huge file needs several GB in MSVC's pass 2; compiling them all at once runs out of heap (C1002).
    set(HG_HUGE_JOBS 2 CACHE STRING "Concurrent compiles of the huge generated sources (Ninja)")
    set_property(GLOBAL APPEND PROPERTY JOB_POOLS hg_huge=${HG_HUGE_JOBS})
    set_property(TARGET hg_game_code_huge PROPERTY JOB_POOL_COMPILE hg_huge)
    if(MSVC)
        target_compile_options(hg_game_code_huge PRIVATE /bigobj /fp:precise ${HG_ARCH_FLAG} /GS- /W0)
        # Huge files that still compile in reasonable time with /d2OptimizeHugeFunctions (file names, ';'-separated).
        # Urban Reign listed three of its own here; Haunting Ground's are not measured yet (TODO after the first build).
        set(HG_HUGE_OPTIMIZED "" CACHE STRING "Huge generated files still compiled with /d2OptimizeHugeFunctions")
        foreach(source IN LISTS HG_HUGE_SOURCES)
            get_filename_component(sourceName "${source}" NAME)
            if(sourceName IN_LIST HG_HUGE_OPTIMIZED)
                set_source_files_properties("${source}" PROPERTIES COMPILE_OPTIONS "/d2OptimizeHugeFunctions")
            endif()
        endforeach()
    endif()

    add_library(hg_game_code STATIC ${HG_GENERATED_SOURCES})
    target_link_libraries(hg_game_code PUBLIC hg_game_code_huge)
    target_include_directories(hg_game_code PUBLIC "${HG_GENERATED_DIR}")
    target_link_libraries(hg_game_code PUBLIC ps2_runtime)
    set_target_properties(hg_game_code PROPERTIES UNITY_BUILD OFF)
    target_precompile_headers(hg_game_code PRIVATE
        "${HG_RUNTIME_INCLUDE}/ps2_runtime_macros.h"
        "${HG_RUNTIME_INCLUDE}/ps2_runtime.h"
        "${HG_GENERATED_DIR}/ps2_recompiled_functions.h"
        "${HG_GENERATED_DIR}/ps2_recompiled_stubs.h"
        "${HG_RUNTIME_INCLUDE}/ps2_syscalls.h"
        "${HG_RUNTIME_INCLUDE}/ps2_stubs.h"
    )
    if(MSVC)
        # No /GL here, ever: whole-program optimization over ~10k generated files blew up link time and memory (Urban
        # Reign docs/ROADMAP.md A4), and the God Hand port's Release/LTCG link died in "Generating code" (its STATUS_2026-10-16.md).
        target_compile_options(hg_game_code PRIVATE /bigobj /fp:precise ${HG_ARCH_FLAG} /GS- /d2OptimizeHugeFunctions /W0 /MP $<$<CONFIG:RelWithDebInfo>:/Ob2>)
        # One ~100k-line table initializer that runs once at startup: compiles in seconds without PCH/optimizer.
        set_source_files_properties("${HG_GENERATED_DIR}/register_functions.cpp" PROPERTIES
            COMPILE_OPTIONS "/Od" SKIP_PRECOMPILE_HEADERS ON)
    endif()
endif()

add_executable(hauntingground ${HG_HOST_SOURCES})
# hg/button_prompts.cpp reads ps2_host_input::prompts (runtime/ps2xRuntime/src/lib/ps2_host_input.h, not a public header).
set_source_files_properties(hg/button_prompts.cpp PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib")
target_include_directories(hauntingground PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/hg_generated"
    "${HG_GENERATED_DIR}")
target_link_libraries(hauntingground PRIVATE hg_game_code ps2_runtime)
target_compile_definitions(hauntingground PRIVATE HG_RUNTIME_HAS_SET_HEADLESS=${HG_RUNTIME_HAS_SET_HEADLESS})

if(MSVC)
    target_compile_options(hauntingground PRIVATE /fp:precise /W3 /utf-8)
    target_compile_definitions(hauntingground PRIVATE _CRT_SECURE_NO_WARNINGS HG_ARCH_NAME="${HG_ARCH}")
    # /FORCE:MULTIPLE: WinAPI vs raylib CloseWindow clash. Large stack: deep guest call chains run on the host stack.
    # /LTCG in RelWithDebInfo as Urban does: ps2_runtime is compiled with /GL by EnableFastReleaseMode
    # (runtime/ps2xRuntime/cmake/ReleaseMode.cmake, CMakeLists.txt:609-611), so the link does code generation for the
    # runtime objects only. The generated game code never gets /GL (above), so it is not part of that step.
    target_link_options(hauntingground PRIVATE /FORCE:MULTIPLE /STACK:16777216 $<$<CONFIG:RelWithDebInfo>:/LTCG>)
    target_link_libraries(hauntingground PRIVATE dbghelp winmm)
endif()

if(HG_DEBUG_UI AND TARGET rlImGui)
    target_sources(hauntingground PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/ps2_debug_panel.cpp")
    target_include_directories(hauntingground PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib")
    target_link_libraries(hauntingground PRIVATE rlImGui imgui)
    target_compile_definitions(hauntingground PRIVATE PS2X_ENABLE_DEBUG_UI=1)
endif()

# FFmpeg is off by default (HG_FFMPEG); with it on, the runtime stages its DLLs next to the exe.
if(COMMAND ps2x_stage_ffmpeg_runtime_dlls)
    ps2x_stage_ffmpeg_runtime_dlls(hauntingground)
endif()

# VU1 microcode compiled ahead of time: capture the code images with PS2X_VU1_CAPTURE, then run vu1_recomp (configure
# with -DHG_BUILD_TOOLS=ON) into generated/vu1. Images not compiled stay interpreted. GENERIC (Urban, SotC).
# An empty or missing generated/vu1 gives no hg_vu1_code library and no HG_HAS_VU1_PROGRAMS define, so main.cpp neither
# declares nor calls registerGeneratedVu1Programs().
# Content-keyed units (vu1_recomp --content, Haunting Ground 2026-10-18) live in the same folder as vu1_content_*.cpp; their
# registry vu1_content_programs.cpp gives HG_HAS_VU1_CONTENT_PROGRAMS (main.cpp registers them and turns
# PS2X_VU1_CONTENT_AOT on by default).
file(GLOB HG_VU1_SOURCES CONFIGURE_DEPENDS "${HG_GENERATED_DIR}/vu1/*.cpp")
if(HG_VU1_SOURCES)
    add_library(hg_vu1_code STATIC ${HG_VU1_SOURCES})
    target_include_directories(hg_vu1_code PRIVATE "${CMAKE_SOURCE_DIR}/runtime/ps2xRuntime/src/lib/vu")
    target_link_libraries(hg_vu1_code PUBLIC ps2_runtime)
    target_link_libraries(hauntingground PRIVATE hg_vu1_code)
    target_compile_definitions(hauntingground PRIVATE HG_HAS_VU1_PROGRAMS=1)
    if(EXISTS "${HG_GENERATED_DIR}/vu1/vu1_content_programs.cpp")
        target_compile_definitions(hauntingground PRIVATE HG_HAS_VU1_CONTENT_PROGRAMS=1)
    endif()
    if(MSVC)
        target_compile_options(hg_vu1_code PRIVATE /bigobj /fp:precise ${HG_ARCH_FLAG} /W0 /MP /d2OptimizeHugeFunctions $<$<CONFIG:RelWithDebInfo>:/Ob2>)
    endif()
endif()

set_target_properties(hauntingground PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)
