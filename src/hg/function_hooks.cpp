// Run-time replacement and entry observation of recompiled guest functions (dispatch-table slots).
// GENERIC: the God Hand port's FunctionHooks class (itself from the Urban Reign port). God Hand's SNR2 DLL logger,
// DLL slots, savestate extension and flicker-filter hook were removed for Haunting Ground (no runtime code loading).
#include "hg/function_hooks.h"
#include "hg/log.h"

#include <cstdlib>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hg
{
    namespace
    {
        struct Hook
        {
            FunctionBinding binding;
            GuestFunction original = nullptr;
            std::vector<EntryObserver> observers;
            std::shared_ptr<const std::vector<EntryObserver>> snapshot;
        };

        std::mutex g_mutex;
        std::unordered_map<uint32_t, Hook> g_hooks;

        const char *statusName(ReplacementStatus status)
        {
            switch (status)
            {
            case ReplacementStatus::Recompiled:
                return "recomp";
            case ReplacementStatus::Observed:
                return "recomp+observed";
            case ReplacementStatus::Native:
                return "native";
            case ReplacementStatus::Hle:
                return "hle";
            case ReplacementStatus::TemporaryStub:
                return "TEMPORARY-STUB";
            }
            return "?";
        }
    }

    FunctionHooks &FunctionHooks::instance()
    {
        static FunctionHooks hooks;
        return hooks;
    }

    void FunctionHooks::attach(PS2Runtime &runtime)
    {
        m_runtime = &runtime;
    }

    GuestFunction FunctionHooks::original(uint32_t address) const
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const auto it = g_hooks.find(address);
        return it == g_hooks.end() ? nullptr : it->second.original;
    }

    bool FunctionHooks::observeEntry(uint32_t address, std::string name, EntryObserver observer)
    {
        if (!m_runtime)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it = g_hooks.find(address);
        if (it == g_hooks.end())
        {
            if (!m_runtime->hasFunction(address))
            {
                HG_ERROR(Hook, "cannot observe 0x" << std::hex << address << " (" << name << "): no recompiled function");
                return false;
            }
            GuestFunction original = m_runtime->lookupFunction(address);
            if (!original)
            {
                HG_ERROR(Hook, "cannot observe 0x" << std::hex << address << " (" << name << "): no recompiled function");
                return false;
            }
            Hook hook;
            hook.binding = FunctionBinding{address, name, ReplacementStatus::Observed, "entry observer"};
            hook.original = original;
            it = g_hooks.emplace(address, std::move(hook)).first;
            if (!m_runtime->replaceFunction(address, &FunctionHooks::observedTrampoline))
            {
                g_hooks.erase(it);
                return false;
            }
        }
        it->second.observers.push_back(std::move(observer));
        it->second.snapshot = std::make_shared<const std::vector<EntryObserver>>(it->second.observers);
        return true;
    }

    bool FunctionHooks::replace(uint32_t address, std::string name, GuestFunction implementation, ReplacementStatus status, std::string reason)
    {
        if (!m_runtime)
            return false;
        std::lock_guard<std::mutex> lock(g_mutex);
        // Addresses the recompiler did not see as function entries have an empty slot; lookupFunction would log an error.
        GuestFunction original = m_runtime->hasFunction(address) ? m_runtime->lookupFunction(address) : nullptr;
        auto &hook = g_hooks[address];
        if (!hook.original)
            hook.original = original;
        hook.binding = FunctionBinding{address, std::move(name), status, std::move(reason)};
        return m_runtime->replaceFunction(address, implementation);
    }

    void FunctionHooks::observedTrampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // The dispatcher stores the called address in ctx->pc before invoking the slot.
        const uint32_t address = ctx->pc;
        GuestFunction original = nullptr;
        std::vector<EntryObserver> observers;
        std::shared_ptr<const std::vector<EntryObserver>> snapshot;
        static const bool fast = [] {
            const char *setting = std::getenv("HG_FAST_OBSERVERS");
            return !setting || std::string_view(setting) != "0";
        }();
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const auto it = g_hooks.find(address);
            if (it == g_hooks.end())
            {
                HG_ERROR(Hook, "trampoline entered with unknown pc 0x" << std::hex << address);
                runtime->requestStop();
                return;
            }
            original = it->second.original;
            if (fast)
                snapshot = it->second.snapshot;
            else
                observers = it->second.observers;
        }
        for (const auto &observer : snapshot ? *snapshot : observers)
            observer(rdram, ctx, runtime);
        original(rdram, ctx, runtime);
    }

    void FunctionHooks::logBindings() const
    {
        std::map<uint32_t, FunctionBinding> sorted;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (const auto &[address, hook] : g_hooks)
                sorted.emplace(address, hook.binding);
        }
        for (const auto &[address, binding] : sorted)
        {
            HG_INFO(Hook, "0x" << std::hex << std::setw(8) << std::setfill('0') << address << std::dec << " " << binding.name << " ["
                               << statusName(binding.status) << "] " << binding.reason);
        }
    }
}
