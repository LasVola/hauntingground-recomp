#include "iop_ioman.h"

#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"
#include "../services/iop_rpc.h"
#include "ps2x/iop/iop_host.h"
#include "ps2x/iop/ps2_path.h"

#include <algorithm>
#include <sstream>

namespace ps2x::iop::detail
{
    namespace
    {
        // SCE ioman flags and errors (negative errno values, as ioman returns them).
        constexpr uint32_t kOpenWrite = 0x0002u;  // O_WRONLY (O_RDWR = 3)
        constexpr uint32_t kOpenCreate = 0x0200u; // O_CREAT
        constexpr uint32_t kOpenTruncate = 0x0400u;
        constexpr int32_t kErrNoEntry = -2;     // ENOENT
        constexpr int32_t kErrBadFile = -9;     // EBADF
        constexpr int32_t kErrNoDevice = -19;   // ENODEV
        constexpr int32_t kErrInvalid = -22;    // EINVAL
        constexpr int32_t kErrTooMany = -24;    // EMFILE
        constexpr int32_t kErrReadOnlyFs = -30; // EROFS
        // Descriptors 0-2 are the console (tty) streams on the IOP; files the HLE opens start at 3. The table size is
        // this HLE's limit, not a measured ioman value.
        constexpr int32_t kFirstFileDescriptor = 3;
        constexpr int32_t kMaxOpenFiles = 32;
        constexpr uint32_t kMaxLoggedOpens = 64u;
    }

    IopIoman::IopIoman(IopHost &host, IopMemory &memory) noexcept
        : m_host(host), m_memory(memory)
    {
    }

    IopIoman::~IopIoman()
    {
        closeAll();
    }

    void IopIoman::closeAll()
    {
        for (auto &[fd, entry] : m_files)
        {
            (void)fd;
            if (entry.handle != 0u)
                m_host.closeHostFile(entry.handle);
            entry.handle = 0u;
        }
        m_files.clear();
    }

    void IopIoman::reset()
    {
        m_devices.clear();
        closeAll();
        m_logs = 0u;
    }

    IopIoman::File *IopIoman::file(int32_t fd)
    {
        const auto found = m_files.find(fd);
        return found != m_files.end() ? &found->second : nullptr;
    }

    bool IopIoman::ensureOpen(File &entry)
    {
        if (entry.handle != 0u)
            return true;
        entry.handle = m_host.openHostFile(entry.hostPath);
        return entry.handle != 0u;
    }

    // open(name, flags, mode). The read-only disc (cdrom0:/cdrom:, also a bare path) and the host device go through the
    // host's file translation, the same way IRX modules are read (cdrom0:\MODULES\X.IRX resolves inside the disc image).
    // Devices added by an IRX with AddDrv (MCMAN's "mc") are not dispatched to their driver functions by this HLE.
    int32_t IopIoman::open(uint32_t nameAddress, uint32_t flags)
    {
        const std::string name = m_memory.readString(nameAddress, 1024u);
        const ParsedPs2Path parsed = parsePs2Path(name);
        int32_t result = 0;
        std::string hostPath;
        if (!parsed || (parsed.device != Ps2PathDevice::Cdrom && parsed.device != Ps2PathDevice::Host))
            result = kErrNoDevice;
        else if (parsed.device == Ps2PathDevice::Cdrom && (flags & (kOpenWrite | kOpenCreate | kOpenTruncate)) != 0u)
            result = kErrReadOnlyFs;
        else if (parsed.device == Ps2PathDevice::Host && (flags & (kOpenWrite | kOpenCreate | kOpenTruncate)) != 0u)
            result = kErrReadOnlyFs; // the host adapter only reads files
        else if (m_files.size() >= static_cast<size_t>(kMaxOpenFiles))
            result = kErrTooMany;
        else
        {
            hostPath = m_host.translateGuestPath(name);
            File entry;
            entry.guestPath = name;
            entry.hostPath = hostPath;
            entry.flags = flags;
            entry.handle = hostPath.empty() ? 0u : m_host.openHostFile(hostPath);
            uint64_t size = 0u;
            if (entry.handle == 0u || !m_host.hostFileSize(entry.handle, size))
            {
                if (entry.handle != 0u)
                    m_host.closeHostFile(entry.handle);
                result = kErrNoEntry;
            }
            else
            {
                entry.size = size;
                int32_t fd = kFirstFileDescriptor;
                while (m_files.contains(fd))
                    ++fd;
                m_files.emplace(fd, std::move(entry));
                result = fd;
            }
        }
        if (m_logs < kMaxLoggedOpens)
        {
            ++m_logs;
            std::ostringstream out;
            out << "[IOP] ioman open('" << name << "', 0x" << std::hex << flags << std::dec << ") = " << result;
            if (result >= 0)
                out << " size " << m_files[result].size;
            m_host.log(result >= 0 ? LogLevel::Info : LogLevel::Warning, out.str());
        }
        return result;
    }

    int32_t IopIoman::close(int32_t fd)
    {
        const auto found = m_files.find(fd);
        if (found == m_files.end())
            return kErrBadFile;
        if (found->second.handle != 0u)
            m_host.closeHostFile(found->second.handle);
        m_files.erase(found);
        return 0;
    }

    int32_t IopIoman::read(int32_t fd, uint32_t buffer, uint32_t size)
    {
        File *entry = file(fd);
        if (entry == nullptr)
            return kErrBadFile;
        if (!ensureOpen(*entry))
            return kErrNoEntry;
        const uint64_t available = entry->position < entry->size ? entry->size - entry->position : 0u;
        const uint32_t wanted = static_cast<uint32_t>(std::min<uint64_t>(size, available));
        const uint32_t physical = IopMemory::physicalAddress(buffer);
        if (wanted != 0u && (physical >= IopMemory::RamSize || wanted > IopMemory::RamSize - physical))
            return kErrInvalid;
        std::vector<uint8_t> bytes(wanted);
        size_t bytesRead = 0u;
        if (wanted != 0u && !m_host.readHostFile(entry->handle, entry->position, bytes.data(), bytes.size(), bytesRead))
            return kErrInvalid;
        if (bytesRead != 0u && !m_memory.writeRam(buffer, bytes.data(), bytesRead))
            return kErrInvalid;
        entry->position += bytesRead;
        return static_cast<int32_t>(bytesRead);
    }

    int32_t IopIoman::write(int32_t fd, uint32_t, uint32_t)
    {
        File *entry = file(fd);
        if (entry == nullptr || (entry->flags & kOpenWrite) == 0u)
            return kErrBadFile; // every file here is open for reading only
        return kErrReadOnlyFs;
    }

    int32_t IopIoman::lseek(int32_t fd, int32_t offset, uint32_t whence)
    {
        File *entry = file(fd);
        if (entry == nullptr)
            return kErrBadFile;
        int64_t base = 0;
        switch (whence)
        {
        case 0u: // SEEK_SET
            base = 0;
            break;
        case 1u: // SEEK_CUR
            base = static_cast<int64_t>(entry->position);
            break;
        case 2u: // SEEK_END
            base = static_cast<int64_t>(entry->size);
            break;
        default:
            return kErrInvalid;
        }
        const int64_t position = base + offset;
        if (position < 0 || position > 0x7FFFFFFF)
            return kErrInvalid;
        entry->position = static_cast<uint64_t>(position);
        return static_cast<int32_t>(position);
    }

    int32_t IopIoman::remove(uint32_t nameAddress)
    {
        const ParsedPs2Path parsed = parsePs2Path(m_memory.readString(nameAddress, 1024u));
        if (!parsed || (parsed.device != Ps2PathDevice::Cdrom && parsed.device != Ps2PathDevice::Host))
            return kErrNoDevice;
        return kErrReadOnlyFs;
    }

    bool IopIoman::dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor)
    {
        constexpr size_t kMaxDevices = 16u;
        const uint32_t a0 = cpu.gpr[4];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };

        switch (ordinal)
        {
        case 4: // open(name, flags, mode)
            setV0(static_cast<uint32_t>(open(a0, cpu.gpr[5])));
            return true;
        case 5: // close(fd)
            setV0(static_cast<uint32_t>(close(static_cast<int32_t>(a0))));
            return true;
        case 6: // read(fd, buffer, size)
            setV0(static_cast<uint32_t>(read(static_cast<int32_t>(a0), cpu.gpr[5], cpu.gpr[6])));
            return true;
        case 7: // write(fd, buffer, size)
            setV0(static_cast<uint32_t>(write(static_cast<int32_t>(a0), cpu.gpr[5], cpu.gpr[6])));
            return true;
        case 8: // lseek(fd, offset, whence)
            setV0(static_cast<uint32_t>(lseek(static_cast<int32_t>(a0), static_cast<int32_t>(cpu.gpr[5]), cpu.gpr[6])));
            return true;
        case 10: // remove(name)
            setV0(static_cast<uint32_t>(remove(a0)));
            return true;
        case 20: // AddDrv
        {
            if (a0 == 0u || m_devices.size() >= kMaxDevices)
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t nameAddress = m_memory.read32(a0);
            const uint32_t operations = m_memory.read32(a0 + 16u);
            const std::string name = m_memory.readString(nameAddress, 64u);
            if (nameAddress == 0u || operations == 0u || name.empty())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            m_devices.push_back({a0, cpu.gpr[28], name});
            const uint32_t init = m_memory.read32(operations);
            if (init != 0u)
            {
                const int32_t result = static_cast<int32_t>(
                    executor.executeGuestFunction(init, a0, 0u, 0u, 0u, cpu.gpr[28]));
                if (result < 0)
                {
                    m_devices.pop_back();
                    setV0(0xFFFFFFFFu);
                    return true;
                }
            }

            setV0(0u);
            return true;
        }
        case 21: // DelDrv
        {
            const std::string name = m_memory.readString(a0, 64u);
            const auto device = std::find_if(
                m_devices.begin(), m_devices.end(),
                [&](const Device &candidate)
                { return candidate.name == name; });
            if (device == m_devices.end())
            {
                setV0(0xFFFFFFFFu);
                return true;
            }

            const uint32_t operations = m_memory.read32(device->address + 16u);
            const uint32_t deinit = operations != 0u ? m_memory.read32(operations + 4u) : 0u;
            if (deinit != 0u)
                (void)executor.executeGuestFunction(deinit, device->address, 0u, 0u, 0u, device->gp);
            m_devices.erase(device);
            setV0(0u);
            return true;
        }
        default:
            return false;
        }
    }
}
