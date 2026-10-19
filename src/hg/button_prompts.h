#pragma once

// Haunting Ground's button prompts show the player's PC bindings (port_prep/prompts/DESIGN.md). Every PS2 button icon
// the game draws is one of ten glyph cells of the resident small font (SUBSCR/MSG_BASE.TEX at Game+0x2AC0). Those ten
// cells are redrawn with the bound keys (keyboard: grey key caps with the key's name; Xbox-style controller: A/B/X/Y,
// LB/RB, LT/RT, View, Menu) and written into the font in guest RAM at the start of a text draw (Task_BeginDraw,
// 0x382000), after which the font's texture-cache entry is released so the game uploads it again itself. A PlayStation
// controller gets the original art back. HG_PROMPTS=0 leaves the font alone (PS2 art).

#include "ps2_runtime.h"

namespace hg::button_prompts
{
    // After FunctionHooks::attach, before runtime.run(): observes Task_BeginDraw, registers a vblank frame hook (finds
    // and keeps the disc's font cells once) and the save-state record "hg.button_prompts", and starts the builder thread.
    void install(PS2Runtime &runtime);
}
