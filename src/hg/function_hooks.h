#pragma once

// Run-time replacement and entry observation of recompiled guest functions (through the dispatch table).
// GENERIC: the Urban Reign port's src/urban/function_hooks.h via the God Hand port (namespace renamed). God Hand's
// installDllLoadLogger / installHiresFlipCopy were removed for Haunting Ground.

#include "ps2_runtime.h"

#include <cstdint>
#include <functional>
#include <string>

namespace hg
{
    using GuestFunction = PS2Runtime::RecompiledFunction;
    using EntryObserver = std::function<void(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)>;

    enum class ReplacementStatus
    {
        Recompiled,
        Observed,
        Native,
        Hle,
        TemporaryStub,
    };

    struct FunctionBinding
    {
        uint32_t address = 0;
        std::string name;
        ReplacementStatus status = ReplacementStatus::Recompiled;
        std::string reason;
    };

    class FunctionHooks
    {
    public:
        static FunctionHooks &instance();

        void attach(PS2Runtime &runtime);
        // Runs `observer` each time the guest calls `address` (before the recompiled function; the function itself is
        // unchanged). Only calls that go through the dispatch table are seen (direct_tail_calls = false in the toml).
        bool observeEntry(uint32_t address, std::string name, EntryObserver observer);
        bool replace(uint32_t address, std::string name, GuestFunction implementation, ReplacementStatus status, std::string reason);
        GuestFunction original(uint32_t address) const;
        void logBindings() const;

    private:
        FunctionHooks() = default;
        static void observedTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

        PS2Runtime *m_runtime = nullptr;
    };
}
