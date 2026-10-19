#include "hg/hiding_prompts.h"

#include "hg/function_hooks.h"
#include "hg/log.h"

#include <atomic>
#include <cstdint>

namespace hg::hiding_prompts
{
    namespace
    {
        // AvoidPrompt_Show(self, id): self[0x11040] = id, self[0x11044] = 30 (shown for 30 ticks). Called only through
        // the prompt object's vtable slot +0xC by event command 0xB4 (EventCmd_Run 0x2029B0, jalr at 0x2066DC).
        constexpr uint32_t kShow = 0x0031DDF0u;
        // AVOID.TEX rows: 0 "Hiding...", 1 "Coast Clear", 2 "Flee!", 3 "Status Restored".
        constexpr uint8_t kHiding = 0u;
        constexpr uint8_t kCoastClear = 1u;

        std::atomic<bool> g_hidden{false};

        void show(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint8_t id = static_cast<uint8_t>(getRegU32(ctx, 5)); // a1; the game stores it as a byte
            if (g_hidden.load(std::memory_order_relaxed) && (id == kHiding || id == kCoastClear))
                return; // nothing stored: the notice never appears; the caller continues after its jalr
            if (const GuestFunction original = FunctionHooks::instance().original(kShow))
                original(rdram, ctx, runtime);
            else
                runtime->requestStop();
        }
    }

    bool install(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kShow))
        {
            HG_WARN(Hook, "hiding prompts: AvoidPrompt_Show 0x31ddf0 not found; HideHidingPrompts has no effect");
            return false;
        }
        const bool ok = FunctionHooks::instance().replace(kShow, "AvoidPrompt_Show (HideHidingPrompts)", &show,
                                                          ReplacementStatus::Native,
                                                          "optional: skip notices 0 Hiding... / 1 Coast Clear");
        HG_INFO(Game, "hiding prompts: " << (g_hidden.load() ? "hidden (\"Hiding...\" and \"Coast Clear\" are not shown, "
                                                               "like the Japanese release)"
                                                             : "shown (US default; HideHidingPrompts=1 hides them)"));
        return ok;
    }

    void setHidden(bool value) { g_hidden.store(value, std::memory_order_relaxed); }
    bool hidden() { return g_hidden.load(std::memory_order_relaxed); }
}
