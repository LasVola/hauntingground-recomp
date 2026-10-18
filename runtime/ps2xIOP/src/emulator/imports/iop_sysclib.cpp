#include "iop_sysclib.h"

#include "iop_format.h"
#include "../core/iop_cpu.h"
#include "../core/iop_memory.h"

#include <cctype>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

namespace ps2x::iop::detail
{
    namespace
    {
        // Sony IOP sysclib ctype flags (_U 0x01, _L 0x02, _N 0x04, _S 0x08, _P 0x10, _C 0x20, _X 0x40, _B 0x80), as in
        // the table SYSCLIB 2.03 (Haunting Ground IOPRP300.IMG) reads at 0x1AD1 + c: '\t'..'\r' are _S only (no _C) and
        // ' ' is _S|_P (no _B). Bytes 0x80-0xFF have no flags.
        uint32_t sysclibCtype(uint32_t c) noexcept
        {
            if (c >= 0x80u)
                return 0u;
            const int ch = static_cast<int>(c);
            if (ch == ' ')
                return 0x18u;
            if (ch >= '\t' && ch <= '\r')
                return 0x08u;
            uint32_t flags = 0u;
            if (ch >= 'A' && ch <= 'Z')
                flags |= 0x01u;
            if (ch >= 'a' && ch <= 'z')
                flags |= 0x02u;
            if (ch >= '0' && ch <= '9')
                flags |= 0x04u;
            if (ch > ' ' && ch < 0x7F && !((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z')))
                flags |= 0x10u;
            if (ch < ' ' || ch == 0x7F)
                flags |= 0x20u;
            if ((ch >= 'A' && ch <= 'F') || (ch >= 'a' && ch <= 'f'))
                flags |= 0x40u;
            return flags;
        }

        // SYSCLIB 2.03 strtol (export 36, code 0x1320) / strtoul (38, 0x14E0), which IRX argument parsers use (Haunting
        // Ground SNDDRV parses "0xFFFFFFFF" with base 16). 32-bit arithmetic that wraps (no ERANGE clamp: the host's
        // strtol gave 0x7FFFFFFF on Windows). Leading _S characters are skipped; strtol takes any number of '-' (each
        // flips the sign, no '+'); a base outside 2..36 or 0 means 10; then "0x"/"0X" selects 16 and "0b"/"0B" 2 (a lone
        // leading '0' is consumed), or a leading 'o'/'O' selects 8, whatever base was passed. Digits are 0-9 and letters
        // (a/A = 10); the first character that is not below the base ends the number. *endptr (when not null) points at
        // it. A null string returns 0 and leaves *endptr alone (end stays 0 here).
        uint32_t sysclibStrtoul(const IopMemory &memory, uint32_t s, uint32_t base, bool isSigned, uint32_t &end)
        {
            end = 0u;
            if (s == 0u)
                return 0u;
            const auto at = [&](uint32_t address) { return static_cast<uint32_t>(memory.read8(address)); };
            for (uint32_t guard = 0u; guard < (1u << 20) && (sysclibCtype(at(s)) & 0x08u) != 0u; ++guard)
                ++s;
            int32_t sign = 1;
            if (isSigned)
            {
                for (uint32_t guard = 0u; guard < (1u << 20) && at(s) == '-'; ++guard)
                {
                    ++s;
                    sign = -sign;
                }
            }
            if (base - 2u >= 35u)
                base = 0u;
            if (base == 0u)
                base = 10u;
            const uint32_t first = at(s);
            if (first == '0')
            {
                ++s;
                const uint32_t prefix = at(s);
                if (prefix == 'X' || prefix == 'x')
                {
                    ++s;
                    base = 16u;
                }
                else if (prefix == 'B' || prefix == 'b')
                {
                    ++s;
                    base = 2u;
                }
            }
            else if (first == 'O' || first == 'o')
            {
                ++s;
                base = 8u;
            }
            uint32_t value = 0u;
            for (uint32_t guard = 0u; guard < (1u << 20); ++guard)
            {
                const uint32_t c = at(s);
                const uint32_t flags = sysclibCtype(c);
                uint32_t digit = 9999999u; // what SYSCLIB's digit helper (0xBD0) returns for a non-alphanumeric character
                if ((flags & 0x04u) != 0u)
                    digit = c - '0';
                else if ((flags & 0x03u) != 0u)
                    digit = (c | 0x20u) - 87u;
                if (digit >= base)
                    break;
                value = value * base + digit;
                ++s;
            }
            end = s;
            return sign < 0 ? 0u - value : value; // the low word of value * sign (mult + mflo)
        }
    }

    IopSysclib::IopSysclib(IopMemory &memory) noexcept
        : m_memory(memory)
    {
    }

    bool IopSysclib::dispatchImport(uint16_t ordinal, IopCpuState &cpu)
    {
        const uint32_t a0 = cpu.gpr[4];
        const uint32_t a1 = cpu.gpr[5];
        const uint32_t a2 = cpu.gpr[6];
        const auto setV0 = [&](uint32_t value)
        {
            cpu.gpr[2] = value;
        };
        const auto compare = [&](uint32_t lhs, uint32_t rhs, uint32_t count) -> int32_t
        {
            for (uint32_t i = 0; i < count; ++i)
            {
                const uint8_t left = m_memory.read8(lhs + i);
                const uint8_t right = m_memory.read8(rhs + i);
                if (left != right)
                    return static_cast<int32_t>(left) - static_cast<int32_t>(right);
            }
            return 0;
        };
        const auto copy = [&](uint32_t destination, uint32_t source, uint32_t count)
        {
            for (uint32_t i = 0; i < count; ++i)
                m_memory.write8(destination + i, m_memory.read8(source + i));
        };
        const auto appendString = [&](uint32_t destination, uint32_t source, std::optional<uint32_t> maxAppend = std::nullopt)
        {
            uint32_t destinationOffset = 0;
            while (m_memory.read8(destination + destinationOffset) != 0u && destinationOffset < (1u << 20))
                ++destinationOffset;

            uint32_t sourceOffset = 0;
            while (sourceOffset < (1u << 20) && (!maxAppend || sourceOffset < *maxAppend))
            {
                const uint8_t character = m_memory.read8(source + sourceOffset);
                m_memory.write8(destination + destinationOffset + sourceOffset, character);
                ++sourceOffset;
                if (character == 0u)
                    return;
            }
            m_memory.write8(destination + destinationOffset + sourceOffset, 0u);
        };

        switch (ordinal)
        {
        case 4: // setjmp - enough for callers which only test the initial return.
            setV0(0);
            return true;
        case 5: // longjmp, TODO bc w can do it without the BIOS jmp_buf ABI.
            setV0(a1 == 0u ? 1u : a1);
            return true;
        case 6:
            setV0(static_cast<uint32_t>(std::toupper(static_cast<unsigned char>(a0))));
            return true;
        case 7:
            setV0(static_cast<uint32_t>(std::tolower(static_cast<unsigned char>(a0))));
            return true;
        case 8: // look_ctype_table(c): the sysclib ctype flags of c (_U 0x01, _L 0x02, _N 0x04, _S 0x08, _P 0x10,
                // _C 0x20, _X 0x40, _B 0x80). IRX argument parsers test (flags & 4) for a digit (CRI_ADXI 0x2ca40,
                // SDRDRV); returning 0 made every "name=<number>" module argument be ignored. Values: sysclibCtype.
            setV0(sysclibCtype(a0 & 0xFFu));
            return true;
        case 9: // get_ctype_table: no guest copy of the table is provided yet.
            setV0(0);
            return true;
        case 10: // memchr
            for (uint32_t i = 0; i < a2; ++i)
            {
                if (m_memory.read8(a0 + i) == static_cast<uint8_t>(a1))
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        case 11:
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 12:
            copy(a0, a1, a2);
            setV0(a0);
            return true;
        case 13:
        {
            std::vector<uint8_t> temporary(a2);
            for (uint32_t i = 0; i < a2; ++i)
                temporary[i] = m_memory.read8(a1 + i);
            (void)m_memory.writeRam(a0, temporary.data(), temporary.size());
            setV0(a0);
            return true;
        }
        case 14:
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write8(a0 + i, static_cast<uint8_t>(a1));
            setV0(a0);
            return true;
        case 15: // bcmp
            setV0(static_cast<uint32_t>(compare(a0, a1, a2)));
            return true;
        case 16: // bcopy(src,dst,n)
            copy(a1, a0, a2);
            setV0(0);
            return true;
        case 17:
            for (uint32_t i = 0; i < a1; ++i)
                m_memory.write8(a0 + i, 0u);
            setV0(0);
            return true;
        case 18: // prnt
            setV0(0);
            return true;
        case 19: // sprintf(buffer, format, ...)
        case 42: // vsprintf(buffer, format, va_list)
        {
            // Arguments substituted (iop_format.h); this used to copy the format literal.
            IopVarArgs args = ordinal == 19u ? IopVarArgs(m_memory, cpu, 2u) : IopVarArgs(m_memory, cpu.gpr[6]);
            const std::string text = formatIopString(m_memory, m_memory.readString(a1, 4096u), args);
            for (size_t i = 0; i <= text.size(); ++i)
            {
                m_memory.write8(a0 + static_cast<uint32_t>(i), i < text.size() ? static_cast<uint8_t>(text[i]) : 0u);
            }
            setV0(static_cast<uint32_t>(text.size()));
            return true;
        }
        case 20:
            appendString(a0, a1);
            setV0(a0);
            return true;
        case 21: // strchr
        case 25: // index
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                {
                    setV0(a0 + i);
                    return true;
                }
                if (character == 0u)
                    break;
            }
            setV0(0);
            return true;
        }
        case 22: // strcmp
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 23: // strcpy
        {
            uint32_t i = 0;
            for (;; ++i)
            {
                const uint8_t character = m_memory.read8(a1 + i);
                m_memory.write8(a0 + i, character);
                if (character == 0u)
                    break;
            }
            setV0(a0);
            return true;
        }
        case 24: // strcspn
        {
            const std::string reject = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || reject.find(character) != std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 26: // rindex
        case 32: // strrchr
        {
            const uint8_t needle = static_cast<uint8_t>(a1);
            uint32_t found = 0u;
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const uint8_t character = m_memory.read8(a0 + i);
                if (character == needle)
                    found = a0 + i;
                if (character == 0u)
                    break;
            }
            setV0(found);
            return true;
        }
        case 27:
            setV0(static_cast<uint32_t>(m_memory.readString(a0, 1u << 20).size()));
            return true;
        case 28:
            appendString(a0, a1, a2);
            setV0(a0);
            return true;
        case 29: // strncmp
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t left = m_memory.read8(a0 + i);
                const uint8_t right = m_memory.read8(a1 + i);
                if (left != right)
                {
                    setV0(static_cast<uint32_t>(static_cast<int32_t>(left) - static_cast<int32_t>(right)));
                    return true;
                }
                if (left == 0u)
                    break;
            }
            setV0(0);
            return true;
        case 30: // strncpy
        {
            bool ended = false;
            for (uint32_t i = 0; i < a2; ++i)
            {
                const uint8_t character = ended ? 0u : m_memory.read8(a1 + i);
                if (character == 0u)
                    ended = true;
                m_memory.write8(a0 + i, character);
            }
            setV0(a0);
            return true;
        }
        case 31: // strpbrk
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            for (uint32_t i = 0; i < (1u << 20); ++i)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + i));
                if (character == 0)
                    break;
                if (accept.find(character) != std::string::npos)
                {
                    setV0(a0 + i);
                    return true;
                }
            }
            setV0(0);
            return true;
        }
        case 33: // strspn
        {
            const std::string accept = m_memory.readString(a1, 4096u);
            uint32_t count = 0;
            for (; count < (1u << 20); ++count)
            {
                const char character = static_cast<char>(m_memory.read8(a0 + count));
                if (character == 0 || accept.find(character) == std::string::npos)
                    break;
            }
            setV0(count);
            return true;
        }
        case 34: // strstr
        {
            const std::string needle = m_memory.readString(a1, 4096u);
            if (needle.empty())
            {
                setV0(a0);
                return true;
            }
            const std::string haystack = m_memory.readString(a0, 1u << 20);
            const size_t position = haystack.find(needle);
            setV0(position == std::string::npos
                      ? 0u
                      : a0 + static_cast<uint32_t>(position));
            return true;
        }
        case 35: // strtok state is intentionally not shared across modules yet.
            setV0(0);
            return true;
        case 36: // strtol(s, endptr, base)
        case 38: // strtoul(s, endptr, base): SYSCLIB 2.03 semantics (sysclibStrtoul)
        {
            uint32_t end = 0u;
            setV0(sysclibStrtoul(m_memory, a0, a2, ordinal == 36, end));
            if (a0 != 0u && a1 != 0u)
                m_memory.write32(a1, end);
            return true;
        }
        case 37: // atob(s, int *result): *result = strtol(s, &end, 10), returns end (SYSCLIB 2.03 0x14AC)
        {
            uint32_t end = 0u;
            const uint32_t value = sysclibStrtoul(m_memory, a0, 10u, true, end);
            if (a1 != 0u)
                m_memory.write32(a1, value);
            setV0(end);
            return true;
        }
        case 40: // _wmemcopy, count is 32-bit words
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, m_memory.read32(a1 + i * 4u));
            setV0(a0);
            return true;
        case 41:
            for (uint32_t i = 0; i < a2; ++i)
                m_memory.write32(a0 + i * 4u, a1);
            setV0(a0);
            return true;
        case 43:
            setV0(0);
            return true;
        default:
            return false;
        }
    }
}
