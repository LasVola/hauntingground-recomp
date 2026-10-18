#pragma once

// SDK functions located in SLUS_210.75 (Haunting Ground, SDK 3.0.2) and bound by ADDRESS to the runtime's HLE handlers
// at start-up. GENERIC mechanism from the Urban Reign port's src/urban/hle_bindings.h (via the God Hand port). The
// Haunting Ground table (hg_sdk_functions.inc) comes from port_prep/hle-bindings/tools/make_hg_sdk_table.py; the
// evidence for each row is in port_prep/hle-bindings/HG_BINDINGS.md.

#include "ps2_runtime.h"

#include <cstdint>
#include <string_view>

namespace hg
{
    struct SdkFunction
    {
        uint32_t address;
        std::string_view name;
        std::string_view method;
        float score;
    };

    // SDK functions listed in hg/hg_sdk_functions.inc (address 0 = none).
    const SdkFunction *findSdkFunction(std::string_view name);
    const SdkFunction *sdkFunctionAt(uint32_t address);

    // Binds the located SDK functions whose work the runtime does at the API level (SIF/IOP, file I/O, CD/DVD,
    // DECI2/TTY; MPEG/IPU only with HG_MPEG_HLE=1 or PS2X_IPU=0) to the runtime's handlers, plus the port's own
    // sceSifLoadStartModule / sceSifLoadStartModuleBuffer (hle_bindings.cpp kHleExtras).
    // HG_HLE_SKIP=name,name leaves those as recompiled code.
    void installHleBindings(PS2Runtime &runtime);
}
