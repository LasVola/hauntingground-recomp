#pragma once

// Hang diagnostics. GENERIC: the Urban Reign port's src/urban/hang_dump.h (URBAN_* env names -> HG_*), plus
// dumpNow() for the run time limit.

class PS2Runtime;

namespace hg::hangdump
{
    // HG_HANG_DUMP=<seconds>: after that delay, print the native stack of every thread in the process to stderr.
    // HG_HANG_DUMP_REPEAT=<seconds>: keep dumping at that interval afterwards.
    // HG_HANG_DUMP_NATIVE=0: skip the native stacks (guest kernel state and call profile only).
    // Each dump also writes HG_DUMP_RAM=<file> (32 MB EE RAM), HG_DUMP_VRAM=<file> (4 MB GS VRAM, host copy;
    // authoritative with PS2X_GS_GPU=0) and hex-dumps HG_DUMP_MEM=addr:len[,addr:len...].
    void startFromEnvironment();

    // Once set, dumps also include the guest EE kernel state (threads, PCs, wait reasons, semaphores, DMA, timers).
    void setRuntime(PS2Runtime *runtime);

    // One dump now, on the calling thread (the time-limit watchdog uses it). Takes about one second (PC sampling).
    void dumpNow(bool nativeStacks);
}
