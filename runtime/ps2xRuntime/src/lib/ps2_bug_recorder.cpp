// Instant-replay bug recorder (see runtime/ps2_bug_recorder.h). Haunting Ground port, 2026-10-19.
#include "runtime/ps2_bug_recorder.h"
#include "runtime/gs/gs_recording.h"
#include "ThreadNaming.h"
#include "ThreadPriority.h"
#include "raylib.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace ps2x::bugrec
{
    namespace
    {
        using Clock = std::chrono::steady_clock;

        constexpr uint32_t kThumbWidth = 320u;
        constexpr size_t kThumbFrames = 180u; // ~3 s at 60 fields/s, 320x224 RGB = ~39 MB
        constexpr size_t kContactFrames = 48u;
        constexpr uint32_t kContactColumns = 8u;
        constexpr double kAutoGapSeconds = 30.0;
        constexpr int kAutoMax = 20;
        constexpr int kGsWaitMs = 1500;
        constexpr double kStatsEverySeconds = 30.0;
        // Black flash: a near-black frame (mean luma <= 4 of 255 and <= 0.5% of its pixels brighter than 32) for 1..6
        // fields, with 3 steady normal frames right before (mean >= 12, max/min <= 1.6) and a normal frame right after.
        // A fade has dim frames between normal and black (or after black), so it does not match.
        constexpr float kDarkLuma = 4.0f;
        constexpr float kDarkBrightFraction = 0.005f;
        constexpr float kNormalLuma = 12.0f;
        constexpr float kSteadyRatio = 1.6f;
        constexpr int kMaxDarkFields = 6;
        // Automatic bundles keep the newest 2 GS segments (15-30 GS fields, ~0.5-1 s before the flash) instead of the
        // whole ring (~2 s): the flash is at most a few fields before the request, and the .gsr is 2-4x smaller.
        constexpr size_t kAutoGsSegments = 2u;
        // Disk guard: below 2 GB free no bundle is written (only info.txt), below 8 GB no gs.gsr (the big part).
        constexpr uint64_t kMinFreeBytes = uint64_t(2) << 30u;
        constexpr uint64_t kGsMinFreeBytes = uint64_t(8) << 30u;
        constexpr float kOverlaySeconds = 3.0f;

        struct Thumb
        {
            uint64_t field = 0u;
            double seconds = 0.0;
            uint32_t width = 0u;
            uint32_t height = 0u;
            uint32_t sourceWidth = 0u;
            uint32_t sourceHeight = 0u;
            uint32_t displayFbp = 0u;
            bool blank = false; // no image this field: the host showed black (thumbnail all black)
            bool midClear = false; // composed between the game's display clear and its copy (kFrameMidClear)
            bool repeat = false;   // nothing new this field: the last image stayed (kFrameRepeat)
            float luma = 0.0f;
            float bright = 0.0f;
            std::vector<uint8_t> rgb;
        };

        struct Job
        {
            std::string reason;
            std::string note;
            uint64_t field = 0u;
            double seconds = 0.0;
            std::vector<Thumb> thumbs; // oldest first
            std::vector<uint64_t> marked;
            size_t keepGsSegments = 0u;
            bool notify = false; // host overlay when written (F8)
        };

        struct Sample
        {
            uint64_t field;
            float luma;
        };

        struct Recorder
        {
            Clock::time_point t0 = Clock::now();
            std::string dir = "logs/bugshots";
            // Main thread only.
            std::vector<Thumb> ring = std::vector<Thumb>(kThumbFrames);
            size_t next = 0u;
            size_t count = 0u;
            uint32_t tableWidth = 0u;
            uint32_t tableHeight = 0u;
            uint32_t thumbWidth = 0u;
            uint32_t thumbHeight = 0u;
            std::vector<uint32_t> xs0, xs1, ys0, ys1;
            std::deque<Sample> history;
            int darkRun = 0;
            bool runQualified = false;
            float runPre = 0.0f;
            float runDarkMax = 0.0f;
            bool runBlank = false;
            std::vector<uint64_t> runFields;
            double lastAuto = -1e9;
            int autoCount = 0;
            uint64_t suppressed = 0u;
            double lastSuppressedLog = -1e9;
            double lastStats = 0.0;
            uint64_t testBlankField = 0u; // PS2X_BUGSHOT_TEST_BLANK=<field>: that field's thumbnail is replaced by a blank one
            bool testBlankDone = true;
            uint64_t injectedField = UINT64_MAX;
            // Writer thread. pending: bundles requested and not yet written (main thread increments, writer decrements).
            std::mutex mutex;
            std::condition_variable cv;
            std::deque<Job> jobs;
            bool writerStarted = false;
            std::atomic<int> pending{0};
            // Host overlay note (main thread and writer).
            std::mutex overlayMutex;
            std::string overlayText;
            Clock::time_point overlayTime{};
        };

        Recorder &recorder()
        {
            static Recorder *instance = new Recorder(); // never destroyed: the writer may still run at exit
            return *instance;
        }

        double nowSeconds()
        {
            return std::chrono::duration<double>(Clock::now() - recorder().t0).count();
        }

        void setOverlay(const std::string &text)
        {
            Recorder &r = recorder();
            std::lock_guard<std::mutex> lock(r.overlayMutex);
            r.overlayText = text;
            r.overlayTime = Clock::now();
        }

        bool writePng(const std::filesystem::path &path, const uint8_t *rgb, uint32_t width, uint32_t height)
        {
            Image image{};
            image.data = const_cast<uint8_t *>(rgb);
            image.width = static_cast<int>(width);
            image.height = static_cast<int>(height);
            image.mipmaps = 1;
            image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8;
            int size = 0;
            unsigned char *png = ExportImageToMemory(image, ".png", &size);
            if (!png || size <= 0)
                return false;
            bool ok = false;
            if (std::FILE *file = std::fopen(path.string().c_str(), "wb"))
            {
                ok = std::fwrite(png, 1, static_cast<size_t>(size), file) == static_cast<size_t>(size);
                ok = std::fclose(file) == 0 && ok;
            }
            MemFree(png);
            return ok;
        }

        // 3x5 digits for the field numbers on the contact sheet.
        constexpr std::array<uint16_t, 10> kDigits = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};

        void fillRect(std::vector<uint8_t> &canvas, uint32_t canvasWidth, uint32_t canvasHeight, int x0, int y0, int w, int h,
                      uint8_t r, uint8_t g, uint8_t b)
        {
            for (int y = std::max(0, y0); y < std::min<int>(static_cast<int>(canvasHeight), y0 + h); ++y)
                for (int x = std::max(0, x0); x < std::min<int>(static_cast<int>(canvasWidth), x0 + w); ++x)
                {
                    uint8_t *p = canvas.data() + (static_cast<size_t>(y) * canvasWidth + x) * 3u;
                    p[0] = r;
                    p[1] = g;
                    p[2] = b;
                }
        }

        void drawNumber(std::vector<uint8_t> &canvas, uint32_t canvasWidth, uint32_t canvasHeight, int x, int y, uint64_t value)
        {
            const std::string text = std::to_string(value);
            constexpr int kScale = 2;
            const int width = static_cast<int>(text.size()) * 4 * kScale + kScale;
            fillRect(canvas, canvasWidth, canvasHeight, x, y, width, 7 * kScale, 0, 0, 0);
            for (size_t i = 0; i < text.size(); ++i)
            {
                const uint16_t glyph = kDigits[static_cast<size_t>(text[i] - '0')];
                for (int row = 0; row < 5; ++row)
                    for (int col = 0; col < 3; ++col)
                        if ((glyph >> (14 - (row * 3 + col))) & 1u)
                            fillRect(canvas, canvasWidth, canvasHeight, x + kScale + static_cast<int>(i) * 4 * kScale + col * kScale,
                                     y + kScale + row * kScale, kScale, kScale, 255, 255, 255);
            }
        }

        void frameRect(std::vector<uint8_t> &canvas, uint32_t canvasWidth, uint32_t canvasHeight, int x, int y, int w, int h)
        {
            constexpr int t = 3;
            fillRect(canvas, canvasWidth, canvasHeight, x, y, w, t, 230, 20, 20);
            fillRect(canvas, canvasWidth, canvasHeight, x, y + h - t, w, t, 230, 20, 20);
            fillRect(canvas, canvasWidth, canvasHeight, x, y, t, h, 230, 20, 20);
            fillRect(canvas, canvasWidth, canvasHeight, x + w - t, y, t, h, 230, 20, 20);
        }

        std::string localStamp()
        {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &now);
#else
            localtime_r(&now, &tm);
#endif
            char text[32];
            std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &tm);
            return text;
        }

        void writeBundle(Job &job)
        {
            Recorder &r = recorder();
            const double started = nowSeconds();
            // The GS ring first: it ends at the next field change after this point.
            std::shared_ptr<GSRing::Collected> gs = GSRing::Collect(kGsWaitMs, job.keepGsSegments);
            const std::string name = localStamp() + "_" + std::to_string(job.field);
            const std::filesystem::path dir = std::filesystem::path(r.dir) / name;
            std::error_code error;
            std::filesystem::create_directories(dir, error);
            if (error)
            {
                std::fprintf(stderr, "[bugshot] %s field %llu: cannot create %s (%s)\n", job.reason.c_str(),
                             static_cast<unsigned long long>(job.field), dir.string().c_str(), error.message().c_str());
                if (job.notify)
                    setOverlay("Bug snapshot failed: cannot create " + dir.generic_string());
                return;
            }
            // Disk guard (a 2K bundle is a few hundred MB, mostly gs.gsr).
            std::error_code spaceError;
            const std::filesystem::space_info space = std::filesystem::space(dir, spaceError);
            const uint64_t freeBytes = spaceError ? UINT64_MAX : static_cast<uint64_t>(space.available);
            std::string diskNote;
            if (freeBytes < kMinFreeBytes)
            {
                diskNote = "disk nearly full (" + std::to_string(freeBytes >> 20u) + " MB free): no frames, no gs.gsr";
                job.thumbs.clear();
                gs.reset();
            }
            else if (freeBytes < kGsMinFreeBytes && gs)
            {
                diskNote = "less than 8 GB free (" + std::to_string(freeBytes >> 20u) + " MB): no gs.gsr";
                gs.reset();
            }
            size_t pngs = 0u;
            if (std::FILE *csv = std::fopen((dir / "frames.csv").string().c_str(), "w"))
            {
                std::fprintf(csv, "index,field,seconds,mean_luma,bright_fraction,width,height,source_width,source_height,file,"
                                  "display_fbp,blank,mid_clear,repeat\n");
                for (size_t i = 0; i < job.thumbs.size(); ++i)
                {
                    const Thumb &t = job.thumbs[i];
                    const std::string file = "frame_" + std::to_string(t.field) + ".png";
                    if (writePng(dir / file, t.rgb.data(), t.width, t.height))
                        ++pngs;
                    std::fprintf(csv, "%zu,%llu,%.3f,%.2f,%.4f,%u,%u,%u,%u,%s,%u,%d,%d,%d\n", i, static_cast<unsigned long long>(t.field),
                                 t.seconds, t.luma, t.bright, t.width, t.height, t.sourceWidth, t.sourceHeight, file.c_str(),
                                 t.displayFbp, t.blank ? 1 : 0, t.midClear ? 1 : 0, t.repeat ? 1 : 0);
                }
                std::fclose(csv);
            }
            // Contact sheet: the last 48 frames, field numbers top left, marked fields (the flash) framed in red.
            if (!job.thumbs.empty())
            {
                const size_t first = job.thumbs.size() > kContactFrames ? job.thumbs.size() - kContactFrames : 0u;
                uint32_t cellWidth = 0u;
                uint32_t cellHeight = 0u;
                for (size_t i = first; i < job.thumbs.size(); ++i)
                {
                    cellWidth = std::max(cellWidth, job.thumbs[i].width);
                    cellHeight = std::max(cellHeight, job.thumbs[i].height);
                }
                const size_t cells = job.thumbs.size() - first;
                const uint32_t columns = static_cast<uint32_t>(std::min<size_t>(kContactColumns, cells));
                const uint32_t rows = static_cast<uint32_t>((cells + kContactColumns - 1u) / kContactColumns);
                const uint32_t sheetWidth = columns * (cellWidth + 4u);
                const uint32_t sheetHeight = rows * (cellHeight + 4u);
                std::vector<uint8_t> sheet(static_cast<size_t>(sheetWidth) * sheetHeight * 3u, 40u);
                for (size_t i = first; i < job.thumbs.size(); ++i)
                {
                    const Thumb &t = job.thumbs[i];
                    const uint32_t cell = static_cast<uint32_t>(i - first);
                    const int x0 = static_cast<int>((cell % kContactColumns) * (cellWidth + 4u) + 2u);
                    const int y0 = static_cast<int>((cell / kContactColumns) * (cellHeight + 4u) + 2u);
                    for (uint32_t y = 0; y < t.height; ++y)
                        std::memcpy(sheet.data() + (static_cast<size_t>(y0 + y) * sheetWidth + x0) * 3u,
                                    t.rgb.data() + static_cast<size_t>(y) * t.width * 3u, static_cast<size_t>(t.width) * 3u);
                    if (std::find(job.marked.begin(), job.marked.end(), t.field) != job.marked.end())
                        frameRect(sheet, sheetWidth, sheetHeight, x0, y0, static_cast<int>(t.width), static_cast<int>(t.height));
                    drawNumber(sheet, sheetWidth, sheetHeight, x0 + 4, y0 + 4, t.field);
                }
                writePng(dir / "contact.png", sheet.data(), sheetWidth, sheetHeight);
            }
            std::string gsSummary = diskNote.empty() ? "no GS ring (PS2X_BUG_RECORDER GS part off)" : diskNote;
            bool gsOk = false;
            if (gs)
                gsOk = GSRing::Write(*gs, (dir / "gs.gsr").string(), gsSummary);
            const uint64_t firstField = job.thumbs.empty() ? 0u : job.thumbs.front().field;
            const uint64_t lastField = job.thumbs.empty() ? 0u : job.thumbs.back().field;
            if (std::FILE *info = std::fopen((dir / "info.txt").string().c_str(), "w"))
            {
                std::fprintf(info, "Bug bundle (instant replay). Written by the runtime's bug recorder (HG_BUG_RECORDER=0 turns it off).\n");
                std::fprintf(info, "reason: %s\n", job.reason.c_str());
                std::fprintf(info, "trigger: field %llu at %.3f s after start\n", static_cast<unsigned long long>(job.field), job.seconds);
                if (!job.note.empty())
                    std::fprintf(info, "%s\n", job.note.c_str());
                std::fprintf(info, "frames: %zu thumbnails, fields %llu-%llu (%u x %u from %u x %u), frame_<field>.png, frames.csv "
                                   "(mean luma 0-255), contact.png = the last %zu with field numbers%s\n",
                             job.thumbs.size(), static_cast<unsigned long long>(firstField), static_cast<unsigned long long>(lastField),
                             job.thumbs.empty() ? 0u : job.thumbs.back().width, job.thumbs.empty() ? 0u : job.thumbs.back().height,
                             job.thumbs.empty() ? 0u : job.thumbs.back().sourceWidth, job.thumbs.empty() ? 0u : job.thumbs.back().sourceHeight,
                             std::min(kContactFrames, job.thumbs.size()), job.marked.empty() ? "" : " (red frame = the black fields)");
                std::fprintf(info, "gs.gsr: %s%s%s\n", gsSummary.c_str(), gsOk ? "" : " (not written)",
                             job.keepGsSegments ? " (automatic bundle: only the newest GS segments, ~0.5-1 s)" : "");
                std::fprintf(info, "frames.csv display_fbp = the frame buffer shown; blank = 1: no image that field (no display "
                                   "circuit on), the screen was black\n");
                std::fprintf(info, "frames.csv mid_clear = 1: the image was composed between the game's clear of the display buffer "
                                   "and its copy into it (a black frame); repeat = 1: nothing new that field, the last image stayed\n");
                std::fprintf(info, "replay: hg_replay play gs.gsr <outprefix> --fields (PS2X_GS_SCALE=3 for 2K); a frame shows field N "
                                   "about 1-2 fields after the GS drew it (async present)\n");
                std::fclose(info);
            }
            std::fprintf(stderr, "[bugshot] %s field %llu: %s (%zu frames %llu-%llu; GS %s) in %.1f s\n", job.reason.c_str(),
                         static_cast<unsigned long long>(job.field), dir.generic_string().c_str(), pngs,
                         static_cast<unsigned long long>(firstField), static_cast<unsigned long long>(lastField),
                         gsOk ? gsSummary.c_str() : ("none: " + gsSummary).c_str(), nowSeconds() - started);
            std::fflush(stderr);
            if (job.notify)
                setOverlay("Bug snapshot saved: " + dir.generic_string());
        }

        void writerLoop()
        {
            ThreadNaming::SetCurrentThreadName("BugRecorder");
#if defined(_WIN32)
            ::SetThreadPriority(::GetCurrentThread(), -1); // THREAD_PRIORITY_BELOW_NORMAL: never ahead of the game
#endif
            Recorder &r = recorder();
            for (;;)
            {
                Job job;
                {
                    std::unique_lock<std::mutex> lock(r.mutex);
                    r.cv.wait(lock, [&] { return !r.jobs.empty(); });
                    job = std::move(r.jobs.front());
                    r.jobs.pop_front();
                }
                writeBundle(job);
                job = Job{}; // frees the frames before the next request may come
                r.pending.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

        void detect(Recorder &r, const Thumb &t)
        {
            const bool dark = t.luma <= kDarkLuma && t.bright <= kDarkBrightFraction;
            const bool normal = t.luma >= kNormalLuma;
            if (dark)
            {
                if (r.darkRun == 0)
                {
                    r.runQualified = r.history.size() >= 3u;
                    float lo = 1e9f, hi = 0.0f;
                    for (size_t i = r.history.size() >= 3u ? r.history.size() - 3u : 0u; i < r.history.size(); ++i)
                    {
                        lo = std::min(lo, r.history[i].luma);
                        hi = std::max(hi, r.history[i].luma);
                    }
                    r.runQualified = r.runQualified && lo >= kNormalLuma && hi <= lo * kSteadyRatio;
                    r.runPre = r.history.empty() ? 0.0f : r.history.back().luma;
                    r.runDarkMax = 0.0f;
                    r.runBlank = false;
                    r.runFields.clear();
                }
                ++r.darkRun;
                r.runBlank = r.runBlank || t.blank;
                r.runDarkMax = std::max(r.runDarkMax, t.luma);
                r.runFields.push_back(t.field);
            }
            else if (r.darkRun > 0)
            {
                if (r.runQualified && r.darkRun <= kMaxDarkFields && normal)
                {
                    const double now = nowSeconds();
                    const bool injected = std::find(r.runFields.begin(), r.runFields.end(), r.injectedField) != r.runFields.end();
                    char note[320];
                    std::snprintf(note, sizeof(note),
                                  "black flash: fields %llu-%llu (%d near-black, %s), mean luma before %.1f, during <= %.1f, after %.1f%s",
                                  static_cast<unsigned long long>(r.runFields.front()), static_cast<unsigned long long>(r.runFields.back()),
                                  r.darkRun, r.runBlank ? "blank = no image" : "black image", r.runPre, r.runDarkMax, t.luma,
                                  injected ? " [test: PS2X_BUGSHOT_TEST_BLANK injected it, not a real flash]" : "");
                    const bool busy = r.pending.load(std::memory_order_acquire) > 0;
                    if (now - r.lastAuto >= kAutoGapSeconds && r.autoCount < kAutoMax && !busy)
                    {
                        r.lastAuto = now;
                        ++r.autoCount;
                        const std::vector<uint64_t> marked = r.runFields;
                        r.history.push_back({t.field, t.luma});
                        r.darkRun = 0;
                        requestBundle("auto (black flash)", t.field, note, marked, kAutoGsSegments);
                        return;
                    }
                    ++r.suppressed;
                    if (now - r.lastSuppressedLog >= kAutoGapSeconds)
                    {
                        r.lastSuppressedLog = now;
                        std::fprintf(stderr, "[bugrec] %s; no bundle (%s)\n", note,
                                     r.autoCount >= kAutoMax ? "20 automatic bundles already"
                                     : busy                  ? "a bundle is still being written"
                                                             : "less than 30 s after the last one");
                    }
                }
                r.darkRun = 0;
            }
            r.history.push_back({t.field, t.luma});
            while (r.history.size() > 16u)
                r.history.pop_front();
        }
    }

    bool enabled()
    {
        static const bool on = []
        {
            const char *value = std::getenv("PS2X_BUG_RECORDER");
            return value && *value == '1';
        }();
        return on;
    }

    uint64_t testField()
    {
        const char *value = std::getenv("PS2X_BUGSHOT_TEST_FIELD");
        return value && *value ? std::strtoull(value, nullptr, 10) : 0u;
    }

    void start()
    {
        Recorder &r = recorder();
        if (const char *dir = std::getenv("PS2X_BUG_RECORDER_DIR"); dir && *dir)
            r.dir = dir;
        if (!enabled())
        {
            if (const char *value = std::getenv("PS2X_BUG_RECORDER"); value && *value)
                std::fprintf(stderr, "[bugrec] off (PS2X_BUG_RECORDER=%s)\n", value);
            return;
        }
        r.t0 = Clock::now();
        if (const char *value = std::getenv("PS2X_BUGSHOT_TEST_BLANK"); value && *value)
        {
            r.testBlankField = std::strtoull(value, nullptr, 10);
            r.testBlankDone = r.testBlankField == 0u;
        }
        std::fprintf(stderr,
                     "[bugrec] on: last ~3 s of frames (320 px) + ~2 s of GS commands (%s) kept in RAM; F8 or a black flash "
                     "writes %s/<time>_<field>/%s\n",
                     GSRing::Active() ? "ring active" : "ring off", r.dir.c_str(),
                     r.testBlankDone ? "" : " (test: PS2X_BUGSHOT_TEST_BLANK set)");
    }

    namespace
    {
        // Main thread: the thumbnail just filled in at r.ring[r.next] joins the ring and the black-flash check.
        void commitThumb(Recorder &r, Thumb &t)
        {
            const double seconds = t.seconds;
            const float luma = t.luma;
            const uint64_t field = t.field;
            r.next = (r.next + 1u) % kThumbFrames;
            r.count = std::min(r.count + 1u, kThumbFrames);
            detect(r, t); // may hand the ring's frames to the writer (requestBundle)
            if (seconds - r.lastStats >= kStatsEverySeconds)
            {
                r.lastStats = seconds;
                std::fprintf(stderr, "[bugrec] %zu frames in RAM (to field %llu, luma %.1f); %s; auto bundles %d, flashes not saved %llu\n",
                             r.count, static_cast<unsigned long long>(field), luma, GSRing::Stats().c_str(), r.autoCount,
                             static_cast<unsigned long long>(r.suppressed));
            }
        }
    }

    void onBlankFrame(uint64_t field)
    {
        if (!enabled())
            return;
        Recorder &r = recorder();
        Thumb &t = r.ring[r.next];
        t.field = field;
        t.seconds = nowSeconds();
        t.width = r.thumbWidth ? r.thumbWidth : kThumbWidth;
        t.height = r.thumbHeight ? r.thumbHeight : 224u;
        t.sourceWidth = 0u;
        t.sourceHeight = 0u;
        t.displayFbp = 0u;
        t.blank = true;
        t.midClear = false;
        t.repeat = false;
        t.rgb.assign(static_cast<size_t>(t.width) * t.height * 3u, 0u);
        t.luma = 0.0f;
        t.bright = 0.0f;
        commitThumb(r, t);
    }

    void onPresentedFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint64_t field, uint32_t displayFbp, uint32_t flags)
    {
        if (!enabled() || !rgba || width < 2u || height < 2u)
            return;
        Recorder &r = recorder();
        if (!r.testBlankDone && field >= r.testBlankField)
        {
            // Test only: this field's thumbnail becomes a blank one (the screen itself is not touched).
            r.testBlankDone = true;
            r.injectedField = field;
            onBlankFrame(field);
            return;
        }
        if (width != r.tableWidth || height != r.tableHeight)
        {
            // Two taps per axis at 1/4 and 3/4 of each output pixel's source span: an exact 2x2 box at 1x (640 -> 320),
            // 2x2 of the 6x6 span at 3x.
            r.tableWidth = width;
            r.tableHeight = height;
            r.thumbWidth = std::min(kThumbWidth, width);
            r.thumbHeight = std::max(1u, static_cast<uint32_t>(static_cast<uint64_t>(height) * r.thumbWidth / width));
            const auto taps = [](uint32_t out, uint32_t in, std::vector<uint32_t> &a, std::vector<uint32_t> &b)
            {
                a.resize(out);
                b.resize(out);
                for (uint32_t i = 0; i < out; ++i)
                {
                    a[i] = std::min(in - 1u, static_cast<uint32_t>((static_cast<uint64_t>(i) * 4u + 1u) * in / (static_cast<uint64_t>(out) * 4u)));
                    b[i] = std::min(in - 1u, static_cast<uint32_t>((static_cast<uint64_t>(i) * 4u + 3u) * in / (static_cast<uint64_t>(out) * 4u)));
                }
            };
            taps(r.thumbWidth, width, r.xs0, r.xs1);
            taps(r.thumbHeight, height, r.ys0, r.ys1);
        }
        Thumb &t = r.ring[r.next];
        t.field = field;
        t.seconds = nowSeconds();
        t.width = r.thumbWidth;
        t.height = r.thumbHeight;
        t.sourceWidth = width;
        t.sourceHeight = height;
        t.displayFbp = displayFbp;
        t.blank = false;
        t.midClear = (flags & kFrameMidClear) != 0u;
        t.repeat = (flags & kFrameRepeat) != 0u;
        t.rgb.resize(static_cast<size_t>(t.width) * t.height * 3u);
        uint64_t lumaSum = 0u;
        uint32_t brightCount = 0u;
        uint8_t *out = t.rgb.data();
        const size_t stride = static_cast<size_t>(width) * 4u;
        for (uint32_t y = 0; y < t.height; ++y)
        {
            const uint8_t *row0 = rgba + r.ys0[y] * stride;
            const uint8_t *row1 = rgba + r.ys1[y] * stride;
            for (uint32_t x = 0; x < t.width; ++x)
            {
                const size_t a = static_cast<size_t>(r.xs0[x]) * 4u;
                const size_t b = static_cast<size_t>(r.xs1[x]) * 4u;
                const uint32_t red = (row0[a] + row0[b] + row1[a] + row1[b] + 2u) >> 2u;
                const uint32_t green = (row0[a + 1u] + row0[b + 1u] + row1[a + 1u] + row1[b + 1u] + 2u) >> 2u;
                const uint32_t blue = (row0[a + 2u] + row0[b + 2u] + row1[a + 2u] + row1[b + 2u] + 2u) >> 2u;
                out[0] = static_cast<uint8_t>(red);
                out[1] = static_cast<uint8_t>(green);
                out[2] = static_cast<uint8_t>(blue);
                out += 3;
                const uint32_t luma = (77u * red + 150u * green + 29u * blue) >> 8u;
                lumaSum += luma;
                brightCount += luma > 32u ? 1u : 0u;
            }
        }
        const uint32_t pixels = t.width * t.height;
        t.luma = static_cast<float>(static_cast<double>(lumaSum) / pixels);
        t.bright = static_cast<float>(static_cast<double>(brightCount) / pixels);
        commitThumb(r, t);
    }

    bool requestBundle(const std::string &reason, uint64_t field, const std::string &note, const std::vector<uint64_t> &marked,
                       size_t keepGsSegments)
    {
        if (!enabled())
            return false;
        Recorder &r = recorder();
        const bool notify = reason.rfind("auto", 0) != 0;
        // One bundle at a time: a second request while one is written would collect the GS ring again later (another
        // few hundred MB) with mostly the same frames.
        if (r.pending.load(std::memory_order_acquire) > 0)
        {
            std::fprintf(stderr, "[bugrec] %s at field %llu: skipped, the previous bundle is still being written\n", reason.c_str(),
                         static_cast<unsigned long long>(field));
            if (notify)
                setOverlay("Bug snapshot: still saving the previous one");
            return false;
        }
        Job job;
        job.reason = reason;
        job.note = note;
        job.field = field;
        job.seconds = nowSeconds();
        job.marked = marked;
        job.keepGsSegments = keepGsSegments;
        job.notify = notify;
        // The frames move to the writer (no copy on this thread); the ring starts again empty. The ring vector itself
        // stays (its moved-from entries are refilled), so a reference into it held by the caller stays valid.
        job.thumbs.reserve(r.count);
        for (size_t i = 0; i < r.count; ++i)
            job.thumbs.push_back(std::move(r.ring[(r.next + kThumbFrames - r.count + i) % kThumbFrames]));
        r.next = 0u;
        r.count = 0u;
        std::fprintf(stderr, "[bugrec] %s at field %llu: writing a bundle (%zu frames)\n", reason.c_str(),
                     static_cast<unsigned long long>(field), job.thumbs.size());
        if (notify)
            setOverlay("Bug snapshot: saving the last 3 seconds...");
        r.pending.fetch_add(1, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(r.mutex);
            r.jobs.push_back(std::move(job));
            if (!r.writerStarted)
            {
                r.writerStarted = true;
                std::thread(writerLoop).detach();
            }
        }
        r.cv.notify_one();
        return true;
    }

    bool overlayMessage(std::string &text, float &alpha)
    {
        Recorder &r = recorder();
        std::lock_guard<std::mutex> lock(r.overlayMutex);
        if (r.overlayText.empty())
            return false;
        const float seconds = std::chrono::duration<float>(Clock::now() - r.overlayTime).count();
        if (seconds >= kOverlaySeconds)
        {
            r.overlayText.clear();
            return false;
        }
        text = r.overlayText;
        alpha = seconds < kOverlaySeconds - 0.5f ? 1.0f : (kOverlaySeconds - seconds) / 0.5f;
        return true;
    }
}
