// ee_kernel_test: offline check of the EE kernel layout Haunting Ground's crt0 relies on (port_prep/ee/MEMORY.md).
// Console program, no window, no game code: it drives the runtime's syscall dispatcher with HG's crt0 arguments
// (SLUS_210.75 0x100198-0x1001E4) and checks the kernel-area scratch slots. Exit code 0 = all checks passed.

#include "ps2_runtime.h"
#include "ps2_syscalls.h"
#include "runtime/ee_scheduler.h"
#include "GuestKernelMemory.h"

#include <cstdio>
#include <cstring>

// No recompiled game code here (same empty table as vu1_recomp.cpp).
const uint32_t g_ps2RecompiledFunctionTableBase = 0u;
const uint32_t g_ps2RecompiledFunctionTableEnd = 0u;
const uint32_t g_ps2RecompiledFunctionTableSlotCount = 0u;
PS2Runtime::RecompiledFunction g_ps2RecompiledFunctionTable[1] = {};

namespace
{
    int g_failures = 0;

    void check(bool ok, const char *what, uint32_t got, uint32_t want)
    {
        std::printf("%s %-58s got 0x%08X want 0x%08X\n", ok ? "PASS" : "FAIL", what, got, want);
        if (!ok)
            ++g_failures;
    }

    void setReg(R5900Context &ctx, int reg, uint32_t value)
    {
        ctx.r[reg] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(value)));
    }

    uint32_t reg32(const R5900Context &ctx, int reg)
    {
        return static_cast<uint32_t>(_mm_cvtsi128_si32(ctx.r[reg]));
    }

    uint32_t syscall(PS2Runtime &rt, R5900Context &ctx, uint32_t number)
    {
        const bool handled = ps2_syscalls::dispatchNumericSyscall(number, rt.memory().getRDRAM(), &ctx, &rt);
        if (!handled)
        {
            std::printf("FAIL syscall 0x%X not handled\n", number);
            ++g_failures;
        }
        return reg32(ctx, 2);
    }

    void scratchSlots()
    {
        namespace km = ps2x::kernel_memory;
        km::resetScratchSlots();
        uint32_t first = 0u, last = 0u, fifth = 0u;
        bool inRange = true, distinct = true;
        for (uint32_t i = 0; i < km::kScratchSlotCount; ++i)
        {
            const uint32_t a = km::allocScratchSlot(112u);
            if (i == 0u)
                first = a;
            if (i == 5u)
                fifth = a;
            if (a < km::kScratchSlotPoolBase || a >= km::kScratchSlotPoolBase + km::kScratchSlotPoolBytes)
                inRange = false;
            if (i != 0u && a != last + km::kScratchSlotBytes)
                distinct = false;
            last = a;
        }
        check(first == km::kScratchSlotPoolBase, "scratch: first slot", first, km::kScratchSlotPoolBase);
        check(inRange && distinct, "scratch: 64 slots, consecutive, inside 0x30000-0x32000", last,
              km::kScratchSlotPoolBase + 63u * km::kScratchSlotBytes);
        check(km::allocScratchSlot(16u) == 0u, "scratch: 65th allocation fails (0)", km::allocScratchSlot(16u), 0u);
        km::freeScratchSlot(fifth);
        const uint32_t again = km::allocScratchSlot(32u);
        check(again == fifth, "scratch: a freed slot is reused", again, fifth);
        km::resetScratchSlots();
        check(km::allocScratchSlot(0x81u) == 0u, "scratch: 0x81 bytes do not fit a slot", 0u, 0u);
        const uint32_t afterReset = km::allocScratchSlot(1u);
        check(afterReset == km::kScratchSlotPoolBase, "scratch: reset frees every slot", afterReset, km::kScratchSlotPoolBase);
        km::resetScratchSlots();
    }

    void crt0Layout(PS2Runtime &rt)
    {
        uint8_t *rdram = rt.memory().getRDRAM();
        R5900Context ctx{};
        // HG crt0 0x100198-0x1001C8: SetupThread(gp 0x4828F0, stack -1, size 0x8000, args 0x19709C0, root 0x100220).
        setReg(ctx, 4, 0x004828F0u);
        setReg(ctx, 5, 0xFFFFFFFFu);
        setReg(ctx, 6, 0x00008000u);
        setReg(ctx, 7, 0x019709C0u);
        setReg(ctx, 8, 0x00100220u);
        setReg(ctx, 29, PS2_RAM_SIZE - 0x10u);
        const uint32_t sp = syscall(rt, ctx, 0x3Cu);
        check(sp == 0x02000000u, "SetupThread(-1, 0x8000) returns the RAM top", sp, 0x02000000u);
        const GuestThread *main = rt.eeScheduler().thread(rt.eeScheduler().currentThreadId());
        check(main && main->stack == 0x01FF8000u, "main thread stack base", main ? main->stack : 0u, 0x01FF8000u);
        check(main && main->stackSize == 0x8000u, "main thread stack size", main ? main->stackSize : 0u, 0x8000u);
        check(reg32(ctx, 28) == 0x004828F0u, "gp", reg32(ctx, 28), 0x004828F0u);

        // HG crt0 0x1001D0-0x1001E4: SetupHeap(_end 0x1992000, -1).
        setReg(ctx, 4, 0x01992000u);
        setReg(ctx, 5, 0xFFFFFFFFu);
        syscall(rt, ctx, 0x3Du);
        const uint32_t end = syscall(rt, ctx, 0x3Eu);
        check(end == 0x01FF8000u, "EndOfHeap = main stack base", end, 0x01FF8000u);

        // HG main 0x20D860: malloc(0x02000000 - 0x8000 - 0x2010 - _end) = 0x663FF0 right after crt0. Its newlib heap
        // grows through libkernl sbrk (0x26C8A0, heap pointer 0x3EA99C = 0x1992000), which fails when the new end
        // passes EndOfHeap. Estimate of that request: 8-byte header + 16-byte MINSIZE, page-rounded (newlib).
        const uint32_t request = (0x663FF0u + 8u + 16u + 0xFFFu) & ~0xFFFu;
        check(0x01992000u + request <= end, "main() pre-grow sbrk(0x665000) fits below EndOfHeap", 0x01992000u + request, end);

        // _GetGsDxDyOffset(mode 2, &dx, &dy, &dw, &dh): all four written (0).
        const uint32_t out = 0x01970000u;
        for (uint32_t i = 0; i < 4u; ++i)
        {
            const uint32_t junk = 0xDEADBEEFu;
            std::memcpy(rdram + out + i * 4u, &junk, 4u);
        }
        setReg(ctx, 4, 2u);
        setReg(ctx, 5, out);
        setReg(ctx, 6, out + 4u);
        setReg(ctx, 7, out + 8u);
        setReg(ctx, 8, out + 12u);
        syscall(rt, ctx, 0x80u);
        uint32_t words[4];
        std::memcpy(words, rdram + out, sizeof(words));
        check((words[0] | words[1] | words[2] | words[3]) == 0u, "_GetGsDxDyOffset writes dx/dy/dw/dh = 0",
              words[0] | words[1] | words[2] | words[3], 0u);

        // sceSifSetReg / sceSifGetReg through their syscalls (0x79 / 0x7A).
        setReg(ctx, 4, 0x80000007u);
        setReg(ctx, 5, 0x12345678u);
        syscall(rt, ctx, 0x79u);
        setReg(ctx, 4, 0x80000007u);
        const uint32_t value = syscall(rt, ctx, 0x7Au);
        check(value == 0x12345678u, "sceSifGetReg returns what sceSifSetReg stored", value, 0x12345678u);

        // The runtime's own guestMalloc arena is the same range: after EndOfHeap it reports the overlap on stderr.
        const uint32_t block = rt.guestMalloc(0x40u);
        check(block == 0x01992000u, "guestMalloc arena starts at SetupHeap base (overlap, reported)", block, 0x01992000u);
        rt.guestFree(block);
    }
}

int main()
{
    scratchSlots();
    PS2Runtime rt;
    if (!rt.memory().initialize(PS2_RAM_SIZE))
    {
        std::printf("FAIL memory init\n");
        return 2;
    }
    crt0Layout(rt);
    std::printf("%s: %d failure(s)\n", g_failures == 0 ? "OK" : "FAILED", g_failures);
    return g_failures == 0 ? 0 : 1;
}
