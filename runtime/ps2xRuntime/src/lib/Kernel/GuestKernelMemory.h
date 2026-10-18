#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

namespace ps2x::kernel_memory
{
    // Runtime-private data in guest RAM lives in the EE kernel area (physical 0x00000-0x7FFFF), which user programs never
    // address directly: their ELFs load at 0x100000 and the SDK kernel patches go through syscall 0x5A Copy to
    // 0x80074000-0x80076740 and 0x00082000 (Haunting Ground and God Hand, port_prep/ee/MEMORY.md). The program's own heap
    // (SetupHeap start .. EndOfHeap, managed by its sbrk) and its stack must stay untouched.
    //   0x002F0-0x002F8   syscall-table probe words (initializeEeKernelState)
    //   0x11B80-0x12380   syscall-table mirror (SetSyscall, 0x80011F80 + 4 * number)
    //   0x30000-0x32000   scratch slots (below)
    //   0x40000-0x48000   RPC packets / RPC server descriptors / TLS / boot-mode pools
    //   0x48000-0x74000   async invocation stacks
    constexpr uint32_t kScratchSlotPoolBase = 0x00030000u;
    constexpr uint32_t kScratchSlotBytes = 0x00000080u;
    constexpr uint32_t kScratchSlotCount = 64u;
    constexpr uint32_t kScratchSlotPoolBytes = kScratchSlotBytes * kScratchSlotCount;

    constexpr uint32_t kRpcPacketPoolBase = 0x00040000u;
    constexpr uint32_t kRpcPacketPoolBytes = 0x00001000u;
    constexpr uint32_t kRpcServerPoolBase = 0x00041000u;
    constexpr uint32_t kRpcServerPoolBytes = 0x00002000u;
    constexpr uint32_t kTlsPoolBase = 0x00043000u;
    constexpr uint32_t kTlsPoolBytes = 0x00004000u;
    constexpr uint32_t kBootModePoolBase = 0x00047000u;
    constexpr uint32_t kBootModePoolBytes = 0x00001000u;

    constexpr uint32_t kCallbackStackFloor = 0x00048000u;
    constexpr uint32_t kCallbackStackTop = 0x00074000u;
    constexpr uint32_t kCallbackStackSize = 0x00002000u;

    static_assert(kScratchSlotCount == 64u, "the scratch-slot bitmap is one 64-bit word");
    static_assert(kScratchSlotPoolBase + kScratchSlotPoolBytes <= kRpcPacketPoolBase, "scratch slots overlap the RPC pools");

    constexpr uint32_t kLegacyRpcServerPoolBase = 0x01F10000u;
    constexpr uint32_t kLegacyTlsPoolBase = 0x01F20000u;
    constexpr uint32_t kLegacyBootModePoolBase = 0x01F30000u;
    constexpr uint32_t kLegacyPoolBytes = 0x00010000u;

    constexpr bool inLegacyPool(uint32_t address, uint32_t poolBase) noexcept
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        return physical >= poolBase && physical < poolBase + kLegacyPoolBytes;
    }

    // Short-lived guest-visible buffers the runtime hands to guest code it invokes (a SIF command packet for a handler,
    // a libmpeg stream-callback record). On the PS2 these live in library-static memory (sifcmd's receive buffer,
    // libmpeg's work area), never in a new block of the program's heap, so they are not taken from guestMalloc: in a
    // program whose own sbrk owns SetupHeap..EndOfHeap (Haunting Ground's newlib heap 0x1992000-0x1FF8000) that would
    // overwrite live heap data. Freed when the invocation completes; reset when a save state is loaded (the loaded
    // scheduler has no invocation in flight). Returns 0 when all slots are busy or the size does not fit a slot.
    inline std::atomic<uint64_t> g_scratchSlotsInUse{0u};

    inline uint32_t allocScratchSlot(uint32_t bytes) noexcept
    {
        if (bytes == 0u || bytes > kScratchSlotBytes)
            return 0u;
        uint64_t used = g_scratchSlotsInUse.load(std::memory_order_acquire);
        for (;;)
        {
            const uint64_t freeSlots = ~used;
            if (freeSlots == 0u)
                return 0u;
            const uint32_t index = static_cast<uint32_t>(std::countr_zero(freeSlots));
            if (g_scratchSlotsInUse.compare_exchange_weak(used, used | (1ull << index), std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
                return kScratchSlotPoolBase + index * kScratchSlotBytes;
        }
    }

    inline bool isScratchSlot(uint32_t address) noexcept
    {
        const uint32_t physical = address & 0x1FFFFFFFu;
        return physical >= kScratchSlotPoolBase && physical < kScratchSlotPoolBase + kScratchSlotPoolBytes &&
               ((physical - kScratchSlotPoolBase) % kScratchSlotBytes) == 0u;
    }

    inline void freeScratchSlot(uint32_t address) noexcept
    {
        if (!isScratchSlot(address))
            return;
        const uint32_t index = ((address & 0x1FFFFFFFu) - kScratchSlotPoolBase) / kScratchSlotBytes;
        g_scratchSlotsInUse.fetch_and(~(1ull << index), std::memory_order_acq_rel);
    }

    inline void resetScratchSlots() noexcept
    {
        g_scratchSlotsInUse.store(0u, std::memory_order_release);
    }

    // Set once the program's own sbrk asks EndOfHeap (syscall 0x3E): from then on SetupHeap..EndOfHeap belongs to the
    // guest, and a runtime guestMalloc there (HLE malloc/MPEG/GS/font helpers, StartThread without a stack) can overlap
    // live guest heap data. guestMalloc reports that case (diagnostic only; allocation behaviour is unchanged).
    inline std::atomic<bool> g_guestSbrkSeen{false};
}
