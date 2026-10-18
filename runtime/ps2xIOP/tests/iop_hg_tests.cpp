// Haunting Ground IOP bring-up tests (2026-10-18): the IOP semantics HG's module set needs.
//  - sysclib strtol/strtoul/atob and look_ctype_table as SYSCLIB 2.03 (IOPRP300) implements them
//  - thbase SetAlarm/CancelAlarm (MCMAN paces card commands with alarms that signal a semaphore)
//  - an unanswered IOP sceSifBindRpc waits for the EE's reply (Capcom SNDDRV polls its EE server from a prio-27
//    thread; its prio-27 RPC server threads must still run)
//  - ioman open/read/lseek/close on cdrom0: through the host file translation
//  - sceCdSearchFile on the CD image itself (the LSNs sceCdRead uses)
//  - HLE RPC services stay dormant while the game's own IRX of that name runs physically
#include "iop_compat_test_support.h"

#include "emulator/core/iop_cpu.h"
#include "emulator/core/iop_kernel.h"
#include "emulator/core/iop_memory.h"
#include "emulator/imports/iop_cdvd.h"
#include "emulator/imports/iop_ioman.h"
#include "emulator/imports/iop_sysclib.h"
#include "emulator/services/iop_rpc.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace
{
    using namespace iop_test;
    using namespace ps2x::iop::detail;

    class NullExecutor final : public IopGuestExecutor
    {
    public:
        uint32_t executeGuestFunction(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override { return 0u; }
    };

    uint32_t callSysclib(IopSysclib &sysclib, uint16_t ordinal, uint32_t a0, uint32_t a1 = 0u, uint32_t a2 = 0u)
    {
        IopCpuState cpu{};
        cpu.gpr[4] = a0;
        cpu.gpr[5] = a1;
        cpu.gpr[6] = a2;
        require(sysclib.dispatchImport(ordinal, cpu), "sysclib import not handled");
        return cpu.gpr[2];
    }

    void testSysclibStrtol()
    {
        IopMemory memory;
        IopSysclib sysclib(memory);
        constexpr uint32_t text = 0x2000u;
        constexpr uint32_t end = 0x2100u;
        const auto put = [&](const char *value)
        {
            (void)memory.writeRam(text, value, std::strlen(value) + 1u);
        };

        put("0xFFFFFFFF"); // Haunting Ground SNDDRV argv[1], parsed with base 16
        require(callSysclib(sysclib, 36u, text, end, 16u) == 0xFFFFFFFFu, "strtol 0xFFFFFFFF did not wrap to 32 bits");
        require(memory.read32(end) == text + 10u, "strtol end pointer not after the digits");
        put("4167");
        require(callSysclib(sysclib, 36u, text, 0u, 10u) == 4167u, "strtol decimal");
        put("0x10");
        require(callSysclib(sysclib, 36u, text, 0u, 10u) == 16u, "a 0x prefix selects base 16 whatever base is given");
        put("077");
        require(callSysclib(sysclib, 36u, text, 0u, 0u) == 77u, "base 0 is 10 (no octal guess from a leading 0)");
        put("o17");
        require(callSysclib(sysclib, 36u, text, 0u, 10u) == 15u, "a leading o selects base 8");
        put("0b101");
        require(callSysclib(sysclib, 36u, text, 0u, 10u) == 5u, "a 0b prefix selects base 2");
        put(" \t-12zz");
        require(callSysclib(sysclib, 36u, text, end, 10u) == static_cast<uint32_t>(-12), "strtol sign after blanks");
        require(memory.read32(end) == text + 5u, "strtol end pointer at the first non-digit");
        put("--7");
        require(callSysclib(sysclib, 36u, text, 0u, 10u) == 7u, "each '-' flips the sign");
        put("-7");
        require(callSysclib(sysclib, 38u, text, end, 10u) == 0u, "strtoul takes no sign");
        require(memory.read32(end) == text, "strtoul stops at '-'");
        put("fF");
        require(callSysclib(sysclib, 38u, text, 0u, 16u) == 255u, "strtoul hex letters");

        put("1234x");
        constexpr uint32_t result = 0x2200u;
        const uint32_t stop = callSysclib(sysclib, 37u, text, result);
        require(memory.read32(result) == 1234u && stop == text + 4u, "atob stores the value and returns the end");

        require(callSysclib(sysclib, 8u, ' ') == 0x18u, "ctype ' ' is _S|_P on the IOP");
        require(callSysclib(sysclib, 8u, '\t') == 0x08u, "ctype tab is _S only");
        require(callSysclib(sysclib, 8u, 'A') == 0x41u && callSysclib(sysclib, 8u, '7') == 0x04u, "ctype letters/digits");
        require(callSysclib(sysclib, 8u, 0x1Fu) == 0x20u && callSysclib(sysclib, 8u, 0xC1u) == 0u, "ctype control/high");
    }

    void testKernelAlarms()
    {
        IopMemory memory;
        IopKernel kernel(memory);
        kernel.reset();
        constexpr uint32_t clock = 0x3000u;
        memory.write32(clock, 10u); // below the 100 us minimum
        memory.write32(clock + 4u, 0u);

        IopCpuState cpu{};
        cpu.gpr[4] = clock;
        cpu.gpr[5] = 0x12340u; // handler
        cpu.gpr[6] = 0x777u;   // arg
        cpu.gpr[28] = 0x9990u;
        require(kernel.dispatchThreadImport(35u, cpu, 1000u) && cpu.gpr[2] == 0u, "SetAlarm failed");
        require(memory.read32(clock) == IopKernel::kAlarmMinCycles, "SetAlarm did not raise a short clock to the minimum");
        require(kernel.nextAlarmCycle(~0ull) == 1000u + IopKernel::kAlarmMinCycles, "alarm scheduled at the wrong cycle");
        require(kernel.dispatchThreadImport(35u, cpu, 2000u) && static_cast<int32_t>(cpu.gpr[2]) == -104,
                "a second SetAlarm for the same handler+arg was not refused (KE_FOUND_HANDLER)");

        require(!kernel.takeDueAlarm(1000u + IopKernel::kAlarmMinCycles - 1u), "alarm fired early");
        auto due = kernel.takeDueAlarm(1000u + IopKernel::kAlarmMinCycles);
        require(due && due->handler == 0x12340u && due->argument == 0x777u && due->gp == 0x9990u, "alarm entry lost data");
        kernel.rearmAlarm(*due, 50000u); // handler returned 50000: next target counts from the previous one
        require(kernel.nextAlarmCycle(~0ull) == 1000u + IopKernel::kAlarmMinCycles + 50000u, "alarm re-arm interval");

        IopCpuState cancel{};
        cancel.gpr[4] = 0x12340u;
        cancel.gpr[5] = 0x777u;
        require(kernel.dispatchThreadImport(37u, cancel, 3000u) && cancel.gpr[2] == 0u, "CancelAlarm failed");
        require(kernel.alarmCount() == 0u, "CancelAlarm left the alarm");
        require(kernel.dispatchThreadImport(38u, cancel, 3000u) && static_cast<int32_t>(cancel.gpr[2]) == -105,
                "iCancelAlarm of a missing alarm did not return KE_NOTFOUND_HANDLER");
    }

    // module_start: SetAlarm(&clock{5000,0}, handler, &marker) and store its result; handler: *marker = 0xA1A1, return 0.
    Irx alarmIrx()
    {
        Irx image(0x10000u, 0x800u);
        image.words(0x00u, {
            0x27BDFFE0u, 0xAFBF001Cu,             // addiu sp,-32; sw ra,28(sp)
            0x3C040001u, 0x34840400u,             // a0 = 0x10400 (clock)
            0x3C050001u, 0x34A50300u,             // a1 = 0x10300 (handler)
            0x3C060001u, 0x34C60480u,             // a2 = 0x10480 (marker)
            0x0C00401Du, 0u,                      // jal 0x10074 SetAlarm
            0x3C010001u, 0xAC220484u,             // sw v0, 0x10484
            0x8FBF001Cu, 0x00001021u, 0x03E00008u, 0x27BD0020u,
        });
        image.words(0x60u, {0x41E00000u, 0u, 0x0102u, 0x61626874u, 0x00006573u, // thbase
                             0x03E00008u, 0x24000023u, 0u, 0u});                 // 35 SetAlarm
        image.words(0x300u, {0x3402A1A1u, 0xAC820000u, 0x03E00008u, 0x00001021u});
        image.words(0x400u, {5000u, 0u});
        image.words(0x480u, {0u, 0xFFFFFFFFu});
        return image;
    }

    void testAlarmHandlerRuns()
    {
        Host host(0x4000u);
        IopSubsystem iop(host);
        alarmIrx().install(host, 0x1000u);
        const ModuleLoadResult module = iop.loadModuleBuffer(0x1000u);
        require(module.handled && module.startResult == 0, "alarm IRX did not start");
        uint32_t words[2] = {};
        require(iop.readMemory(0x10480u, words, sizeof(words)), "read marker");
        require(words[1] == 0u, "SetAlarm returned an error to the IRX");
        require(words[0] == 0u, "alarm handler ran before its time");
        iop.runEeCycles(8u * 20000u);
        iop.flushPendingWork();
        require(iop.readMemory(0x10480u, words, sizeof(words)) && words[0] == 0xA1A1u,
                "SetAlarm handler did not run (thbase SetAlarm was a no-op)");
    }

    // Two prio-27 threads, as in Capcom SNDDRV: A (created first) binds an EE server in a loop with no delay until
    // cd->server != 0; B writes a marker. No EE server is registered, so A never gets one.
    Irx bindPollIrx()
    {
        Irx image(0x10000u, 0x800u);
        image.words(0x00u, {
            0x27BDFFE0u, 0xAFBF001Cu,
            0x3C040001u, 0x34840500u, 0x0C00401Du, 0u,          // CreateThread(param A)
            0x00402021u, 0x0C00401Fu, 0x00002821u,              // StartThread(id, 0)
            0x3C040001u, 0x34840520u, 0x0C00401Du, 0u,          // CreateThread(param B)
            0x00402021u, 0x0C00401Fu, 0x00002821u,              // StartThread(id, 0)
            0x8FBF001Cu, 0x00001021u, 0x03E00008u, 0x27BD0020u,
        });
        image.words(0x60u, {0x41E00000u, 0u, 0x0102u, 0x61626874u, 0x00006573u, // thbase
                             0x03E00008u, 0x24000004u,                           // 4 CreateThread
                             0x03E00008u, 0x24000006u, 0u, 0u});                 // 6 StartThread
        image.words(0x90u, {0x41E00000u, 0u, 0x0101u, 0x63666973u, 0x0000646Du, // sifcmd
                             0x03E00008u, 0x2400000Fu, 0u, 0u});                 // 15 sceSifBindRpc
        image.words(0x200u, {
            0x3C040001u, 0x34840600u,             // a0 = cd 0x10600
            0x3C057777u, 0x34A57779u,             // a1 = 0x77777779
            0x0C004029u, 0x00003021u,             // jal 0x100A4 sceSifBindRpc, a2 = 0
            0x3C010001u, 0x8C230624u, 0u,         // v1 = cd->server
            0x1060FFF6u, 0u,                      // beq v1, zero, 0x10200
            0x03E00008u, 0u,
        });
        image.words(0x280u, {0x3C010001u, 0x3402B0B0u, 0xAC220680u, 0x03E00008u, 0u});
        image.words(0x500u, {0x02000000u, 0u, 0x10200u, 0x800u, 27u});
        image.words(0x520u, {0x02000000u, 0u, 0x10280u, 0x800u, 27u});
        return image;
    }

    void testUnansweredBindWaits()
    {
        Host host(0x4000u);
        IopSubsystem iop(host);
        bindPollIrx().install(host, 0x1000u);
        const ModuleLoadResult module = iop.loadModuleBuffer(0x1000u);
        require(module.handled && module.startResult == 0, "bind-poll IRX did not start");
        iop.runEeCycles(8u * 200000u);
        iop.flushPendingWork();
        uint32_t marker = 0u;
        require(iop.readMemory(0x10680u, &marker, sizeof(marker)) && marker == 0xB0B0u,
                "an equal-priority thread never ran while another polled sceSifBindRpc for a missing EE server");
        uint32_t server = 1u;
        require(iop.readMemory(0x10624u, &server, sizeof(server)) && server == 0u, "unanswered bind set cd->server");
    }

    void testIomanCdromFile()
    {
        Host host(0x1000u);
        host.file = {'S', 'D', 'T', '!', 1, 2, 3, 4, 5, 6};
        IopMemory memory;
        IopIoman ioman(host, memory);
        ioman.reset();
        NullExecutor executor;
        constexpr uint32_t name = 0x2000u;
        constexpr uint32_t buffer = 0x3000u;
        const char path[] = "cdrom0:\\SOUND\\BGM.SDT;1";
        (void)memory.writeRam(name, path, sizeof(path));
        const auto call = [&](uint16_t ordinal, uint32_t a0, uint32_t a1 = 0u, uint32_t a2 = 0u)
        {
            IopCpuState cpu{};
            cpu.gpr[4] = a0;
            cpu.gpr[5] = a1;
            cpu.gpr[6] = a2;
            require(ioman.dispatchImport(ordinal, cpu, executor), "ioman import not handled");
            return static_cast<int32_t>(cpu.gpr[2]);
        };

        require(call(4u, name, 0x0201u) == -30, "open for writing on cdrom0: did not fail with EROFS");
        const int32_t fd = call(4u, name, 0x0001u);
        require(fd >= 3, "ioman open on cdrom0: failed");
        require(call(6u, static_cast<uint32_t>(fd), buffer, 4u) == 4 && memory.read32(buffer) == 0x21544453u, "ioman read");
        require(call(8u, static_cast<uint32_t>(fd), 0u, 2u) == 10, "lseek SEEK_END did not return the size");
        require(call(8u, static_cast<uint32_t>(fd), static_cast<uint32_t>(-3), 1u) == 7, "lseek SEEK_CUR");
        require(call(6u, static_cast<uint32_t>(fd), buffer, 100u) == 3 && memory.read8(buffer) == 4u, "read at the end");
        require(call(6u, static_cast<uint32_t>(fd), buffer, 100u) == 0, "read past the end");
        require(call(7u, static_cast<uint32_t>(fd), buffer, 1u) == -9, "write to a read-only file");
        require(call(10u, name) == -30, "remove on cdrom0:");
        require(call(5u, static_cast<uint32_t>(fd)) == 0 && call(5u, static_cast<uint32_t>(fd)) == -9, "close");
        const char mc[] = "mc0:SAVE";
        (void)memory.writeRam(name, mc, sizeof(mc));
        require(call(4u, name, 1u) == -19, "a driver-less device did not fail with ENODEV");
        host.file.clear();
        (void)memory.writeRam(name, path, sizeof(path));
        require(call(4u, name, 1u) == -2, "a missing file did not fail with ENOENT");
    }

    // A host whose CD image is a real file (sceCdSearchFile/sceCdRead read it with openHostFile/readHostFile).
    class ImageHost final : public IopHost
    {
    public:
        explicit ImageHost(std::filesystem::path imagePath) : image(std::move(imagePath)) {}
        ~ImageHost() override
        {
            if (stream)
                std::fclose(stream);
        }
        bool readGuest(uint32_t, void *, size_t) const override { return false; }
        bool writeGuest(uint32_t, const void *, size_t) override { return false; }
        bool zeroGuest(uint32_t, size_t) override { return false; }
        bool normalizeGuestAddress(uint32_t, uint32_t &) const override { return false; }
        uint32_t allocateIopHandle(IopHandleKind) override { return 1u; }
        uint32_t allocateGuest(uint32_t, uint32_t) override { return 0u; }
        void freeGuest(uint32_t) override {}
        void audioCommand(uint32_t, uint32_t, GuestBuffer, GuestBuffer) override {}
        std::string hostPath(HostPathKind kind) const override
        {
            if (kind == HostPathKind::CdImage)
                return image.string();
            if (kind == HostPathKind::CdRoot)
                return std::filesystem::temp_directory_path().string(); // not the disc tree, like HG's game_data
            return {};
        }
        std::string translateGuestPath(std::string_view path) const override { return std::string(path); }
        uint64_t openHostFile(std::string_view path) override
        {
            if (std::filesystem::path(std::string(path)) != image)
                return 0u;
            if (!stream)
                stream = std::fopen(image.string().c_str(), "rb");
            return stream ? 7u : 0u;
        }
        bool hostFileSize(uint64_t, uint64_t &) const override { return false; }
        bool readHostFile(uint64_t handle, uint64_t offset, void *destination, size_t size, size_t &bytesRead) override
        {
            bytesRead = 0u;
            if (handle != 7u || !stream || std::fseek(stream, static_cast<long>(offset), SEEK_SET) != 0)
                return false;
            bytesRead = std::fread(destination, 1u, size, stream);
            return true;
        }
        void closeHostFile(uint64_t) override {}
        int32_t memoryCard(const MemoryCardRequest &) override { return 0; }
        bool hasGuestFunction(uint32_t) const override { return false; }
        bool invokeGuestFunction(uint64_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t *) override { return false; }
        void log(LogLevel, std::string_view) override {}

    private:
        std::filesystem::path image;
        std::FILE *stream = nullptr;
    };

    void putRecord(std::vector<uint8_t> &disc, size_t offset, uint32_t lsn, uint32_t size, bool directory, std::string_view name)
    {
        const size_t length = 33u + name.size() + ((33u + name.size()) & 1u);
        disc[offset] = static_cast<uint8_t>(length);
        for (int i = 0; i < 4; ++i)
        {
            disc[offset + 2u + i] = static_cast<uint8_t>(lsn >> (8 * i));
            disc[offset + 9u - i] = static_cast<uint8_t>(lsn >> (8 * i));
            disc[offset + 10u + i] = static_cast<uint8_t>(size >> (8 * i));
            disc[offset + 17u - i] = static_cast<uint8_t>(size >> (8 * i));
        }
        disc[offset + 25u] = directory ? 2u : 0u;
        disc[offset + 32u] = static_cast<uint8_t>(name.size());
        std::memcpy(disc.data() + offset + 33u, name.data(), name.size());
    }

    void testCdSearchFileInImage()
    {
        constexpr size_t kSector = 2048u;
        std::vector<uint8_t> disc(24u * kSector, 0u);
        uint8_t *pvd = disc.data() + 16u * kSector;
        pvd[0] = 1u;
        std::memcpy(pvd + 1u, "CD001", 5u);
        putRecord(disc, 16u * kSector + 156u, 20u, kSector, true, std::string_view("\0", 1u));
        disc[17u * kSector] = 255u;
        std::memcpy(disc.data() + 17u * kSector + 1u, "CD001", 5u);
        size_t at = 20u * kSector;
        putRecord(disc, at, 20u, kSector, true, std::string_view("\0", 1u));
        at += disc[at];
        putRecord(disc, at, 20u, kSector, true, std::string_view("\1", 1u));
        at += disc[at];
        putRecord(disc, at, 21u, kSector, true, "SOUND");
        at = 21u * kSector;
        putRecord(disc, at, 21u, kSector, true, std::string_view("\0", 1u));
        at += disc[at];
        putRecord(disc, at, 20u, kSector, true, std::string_view("\1", 1u));
        at += disc[at];
        putRecord(disc, at, 22u, 5000u, false, "BGM.SDT;1");
        std::memcpy(disc.data() + 22u * kSector, "BGM-DATA", 8u);

        const auto suffix = std::to_string(static_cast<unsigned long long>(
            std::chrono::steady_clock::now().time_since_epoch().count()));
        const std::filesystem::path imagePath = std::filesystem::temp_directory_path() / ("ps2x-hg-cdimage-" + suffix + ".iso");
        {
            std::ofstream out(imagePath, std::ios::binary);
            out.write(reinterpret_cast<const char *>(disc.data()), static_cast<std::streamsize>(disc.size()));
        }
        bool passed = false;
        std::string failure;
        {
            ImageHost host(imagePath);
            IopMemory memory;
            IopKernel kernel(memory);
            kernel.reset();
            IopCdvd cdvd(host, memory, kernel);
            cdvd.reset();
            constexpr uint32_t result = 0x2400u;
            constexpr uint32_t name = 0x2480u;
            constexpr uint32_t buffer = 0x4000u;
            const char path[] = "\\sound\\bgm.sdt;1";
            (void)memory.writeRam(name, path, sizeof(path));
            (void)memory.zeroRam(buffer, kSector);
            IopCpuState cpu{};
            cpu.gpr[4] = result;
            cpu.gpr[5] = name;
            const bool found = cdvd.dispatchImport(10u, cpu) && cpu.gpr[2] == 1u;
            IopCpuState read{};
            read.gpr[4] = memory.read32(result);
            read.gpr[5] = 1u;
            read.gpr[6] = buffer;
            const bool readOk = found && cdvd.dispatchImport(6u, read) && read.gpr[2] == 1u;
            if (!found)
                failure = "sceCdSearchFile did not find the file in the CD image";
            else if (memory.read32(result) != 22u || memory.read32(result + 4u) != 5000u)
                failure = "sceCdSearchFile did not return the image's LSN/size";
            else if (memory.readString(result + 8u, 16u) != "BGM.SDT")
                failure = "sceCdSearchFile returned the wrong name";
            else if (!readOk || memory.readString(buffer, 8u) != "BGM-DATA")
                failure = "sceCdRead at the LSN found did not return the file's data";
            else
                passed = true;
        }
        std::error_code error;
        std::filesystem::remove(imagePath, error);
        require(passed, failure.c_str());
    }

    void testHleDormantForPhysicalIrx()
    {
        constexpr uint32_t kDbcmanSid = 0x80001300u;
        Host host(0x4000u);
        IopSubsystem iop(host);
        Irx image(0x10000u, 0x100u);
        image.words(0u, {0x03E00008u, 0x00001021u}); // module_start: return 0, no RPC server
        host.file = image.bytes;
        const ModuleLoadResult physical = iop.loadModule("cdrom0:\\MODULES\\DBCMAN.IRX;1");
        require(physical.handled && physical.moduleId > 0 && physical.moduleId < 0x40000000, "DBCMAN.IRX was not loaded physically");
        require(!iop.canBindRpc(kDbcmanSid), "the HLE DBCMAN route answered a bind while the game's DBCMAN.IRX runs");
        require(!iop.handleRpc(request(kDbcmanSid, 0x80001363u)).handled, "the HLE DBCMAN served a request for the IRX");

        Host fallbackHost(0x4000u);
        IopSubsystem fallback(fallbackHost);
        const ModuleLoadResult hle = fallback.loadModule("rom0:DBCMAN");
        require(hle.moduleId >= 0x40000000 && fallback.canBindRpc(kDbcmanSid), "HLE DBCMAN fallback no longer activates");
    }
}

int main()
{
    const Test tests[] = {
        {"sysclib strtol/strtoul/atob/ctype (SYSCLIB 2.03)", testSysclibStrtol},
        {"thbase alarms (kernel)", testKernelAlarms},
        {"thbase SetAlarm handler runs on the IOP clock", testAlarmHandlerRuns},
        {"unanswered sceSifBindRpc lets equal-priority threads run", testUnansweredBindWaits},
        {"ioman file I/O on cdrom0:", testIomanCdromFile},
        {"sceCdSearchFile in the CD image", testCdSearchFileInImage},
        {"HLE service dormant for a physical IRX", testHleDormantForPhysicalIrx},
    };
    return run(tests);
}
