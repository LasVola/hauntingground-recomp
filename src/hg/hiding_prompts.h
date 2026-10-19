#pragma once

// Optional: hide the "Hiding..." and "Coast Clear" notices like the Japanese release (Demento) does.
// hauntingground.ini [Game] HideHidingPrompts=1 (env HG_HIDE_HIDING_PROMPTS=1). "Flee!" and "Status Restored" stay.
// Research: codex/hideprompt/REPORT.md (AVOID.TEX rows 0-3, the one call site = event command 0xB4).

class PS2Runtime;

namespace hg::hiding_prompts
{
    // Replaces AvoidPrompt_Show (0x0031DDF0); always installed so the setting can change while the game runs.
    bool install(PS2Runtime &runtime);
    void setHidden(bool hidden);
    bool hidden();
}
