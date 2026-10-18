#pragma once

#include <cstdint>
#include "ps2x/state_io.h"
#include <map>
#include <string>
#include <vector>

namespace ps2x::iop
{
    class IopHost;
}

namespace ps2x::iop::detail
{
    struct IopCpuState;
    class IopGuestExecutor;
    class IopMemory;

    class IopIoman
    {
    public:
        IopIoman(IopHost &host, IopMemory &memory) noexcept;
        ~IopIoman();

        IopIoman(const IopIoman &) = delete;
        IopIoman &operator=(const IopIoman &) = delete;

        void reset();
        [[nodiscard]] bool dispatchImport(uint16_t ordinal, IopCpuState &cpu, IopGuestExecutor &executor);

        template <typename Ar>
        void serialize(Ar &ar)
        {
            if constexpr (Ar::kLoading)
                closeAll(); // files come back from the optional IFIL section (serializeFiles)
            ps2x::state::fields(ar, m_devices);
        }

        // Open files (optional savestate section): the host handle is not saved; a restored file is reopened by path
        // on its next access.
        template <typename Ar>
        void serializeFiles(Ar &ar)
        {
            if constexpr (Ar::kLoading)
                closeAll();
            ps2x::state::fields(ar, m_files);
        }

        [[nodiscard]] size_t openFileCount() const noexcept { return m_files.size(); }

    private:
        struct Device
        {
            uint32_t address = 0u;
            uint32_t gp = 0u;
            std::string name;

            template <typename Ar>
            void serialize(Ar &ar)
            {
                ps2x::state::fields(ar, address, gp, name);
            }
        };

        struct File
        {
            std::string guestPath;
            std::string hostPath;
            uint32_t flags = 0u;
            uint64_t position = 0u;
            uint64_t size = 0u;
            uint64_t handle = 0u; // host file handle (not saved)

            template <typename Ar>
            void serialize(Ar &ar)
            {
                ps2x::state::fields(ar, guestPath, hostPath, flags, position, size);
                if constexpr (Ar::kLoading)
                    handle = 0u;
            }
        };

        [[nodiscard]] int32_t open(uint32_t nameAddress, uint32_t flags);
        [[nodiscard]] int32_t close(int32_t fd);
        [[nodiscard]] int32_t read(int32_t fd, uint32_t buffer, uint32_t size);
        [[nodiscard]] int32_t write(int32_t fd, uint32_t buffer, uint32_t size);
        [[nodiscard]] int32_t lseek(int32_t fd, int32_t offset, uint32_t whence);
        [[nodiscard]] int32_t remove(uint32_t nameAddress);
        [[nodiscard]] File *file(int32_t fd);
        [[nodiscard]] bool ensureOpen(File &file);
        void closeAll();

        IopHost &m_host;
        IopMemory &m_memory;
        std::vector<Device> m_devices;
        std::map<int32_t, File> m_files;
        uint32_t m_logs = 0u;
    };
}
