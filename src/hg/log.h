#pragma once

// Port log: "[  12.3456][BOOT] message" lines, Info to stdout, Warn/Error to stderr.
// GENERIC: adapted from the Urban Reign port's src/urban/log.h (namespace, macro prefix and env name renamed, the Dll
// category added).

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>

namespace hg::log
{
    enum class Category : uint32_t
    {
        Boot = 1u << 0,
        Loader = 1u << 1,
        File = 1u << 2,
        Ee = 1u << 3,
        Iop = 1u << 4,
        Gs = 1u << 5,
        Vu = 1u << 6,
        Spu2 = 1u << 7,
        Input = 1u << 8,
        Game = 1u << 9,
        Hook = 1u << 10,
        Dll = 1u << 11, // God Hand SNR2 DLL loads; unused in Haunting Ground (kept so HG_TRACE=DLL still parses)
    };

    enum class Level
    {
        Error,
        Warn,
        Info,
        Trace,
    };

    // HG_TRACE=BOOT,LOADER,FILE,EE,IOP,GS,VU,SPU2,INPUT,GAME,HOOK,DLL|ALL: categories whose Trace lines print.
    void configureFromEnvironment();
    void setTraceMask(uint32_t mask);
    bool traceEnabled(Category category);
    void write(Category category, Level level, std::string_view message);
    // Seconds since the log started: the time stamp of every log line ("[  12.3456]").
    double elapsedSeconds();
    std::string_view name(Category category);
}

#define HG_LOG(cat, lvl, expr)                                                      \
    do                                                                             \
    {                                                                              \
        if ((lvl) != ::hg::log::Level::Trace || ::hg::log::traceEnabled(cat)) \
        {                                                                          \
            std::ostringstream hgLogStream_;                                       \
            hgLogStream_ << expr;                                                  \
            ::hg::log::write((cat), (lvl), hgLogStream_.str());               \
        }                                                                          \
    } while (0)

#define HG_INFO(cat, expr) HG_LOG(::hg::log::Category::cat, ::hg::log::Level::Info, expr)
#define HG_WARN(cat, expr) HG_LOG(::hg::log::Category::cat, ::hg::log::Level::Warn, expr)
#define HG_ERROR(cat, expr) HG_LOG(::hg::log::Category::cat, ::hg::log::Level::Error, expr)
#define HG_TRACE(cat, expr) HG_LOG(::hg::log::Category::cat, ::hg::log::Level::Trace, expr)

