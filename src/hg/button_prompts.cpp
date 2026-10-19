// Haunting Ground button prompts (port_prep/prompts/DESIGN.md 3.3-3.5). Modelled on the God Hand port's
// src/godhand/button_prompts.cpp; the place and the way the art reaches the screen are Haunting Ground's own:
//
// - The ten PS2 button icons are glyph cells 0xA0..0xA9 of the small font SUBSCR/MSG_BASE.TEX, which stays in guest
//   RAM for the whole session at Game+0x2AC0 (0x0048A4C0). Unlike God Hand's sheets it is not re-sent on every draw:
//   the texture cache (gTexCache, group 0x14) keeps it in a VRAM layer and uploads it only when it hands out a new
//   layer (TexCache_Layer bit 31, Task_BeginDraw 0x382070 -> 0x38218C, gRenderer +0x44).
// - So the cells are written on the game thread at the entry of Task_BeginDraw (0x382000, called by Task_DrawPage
//   through the dispatch table), and when the font is resident its cache entry is released with the same two writes
//   the cache's own LRU eviction makes (layer = -1, age = -1, OpenHuey renderer.c TexCache_Layer). The TexCache_Layer
//   call Task_BeginDraw is about to make then takes a free layer with bit 31 and the game uploads image + CLUT from RAM.
//
// Safety: nothing is written unless the file header and .TEX entry match, the sheet outside the ten cells and its
// CLUT hash to the disc's bytes (FNV, prompt_cells.h), the texture-cache entry points at this file, and every one of
// the ten cells holds bytes this code knows (the disc's cell, a cell this session built, or grey-only key-cap art as
// this code draws it, e.g. from a save state of another session). After a save-state load the cells are checked again
// at the next text draw and the font is re-uploaded once, so a state never leaves art that does not match the device.
// Save states carry a small record ("hg.button_prompts": the disc's ten cells, sha256-checked on load) so a state
// loaded before this session ever saw the original font (F9 during the boot logos) is corrected too.
// HG_PROMPTS=0 installs nothing.

#include "hg/button_prompts.h"

#include "hg/function_hooks.h"
#include "hg/log.h"
#include "hg/prompt_cells.h"
#include "hg/sha256.h"
#include "ps2_host_input.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace hg::button_prompts
{
    namespace
    {
        namespace cells = hg::prompt_cells;
        namespace bindings = ps2_host_input::prompts;

        constexpr uint32_t kTaskBeginDraw = 0x00382000u;   // Task_BeginDraw (OpenHuey text.c:756-763)
        constexpr uint32_t kTexCachePointer = 0x0044E4E8u; // gTexCache (OpenHuey include/globals.h)
        constexpr uint32_t kFontGroup = 0x14u;             // TEXCACHE_SLOT(0, 0x14): MSG_BASE.TEX (scene_boot.c:266-286)
        constexpr uint32_t kTexEntryBytes = 12u;           // tex u32, age s16, layer s16, group s8, index u8, pad 2
        constexpr uint32_t kRamBytes = 0x2000000u;
        constexpr size_t kMaxKnown = 64u;

        using Block = std::array<uint8_t, cells::kCellBlockBytes>;

        struct Built
        {
            uint64_t version = 0u;
            bool original = false; // the disc's cells (PlayStation pad, or nothing bound to the ten buttons)
            Block block{};         // what the ten cells should hold (the original block when `original`)
            std::string summary;
        };

        // Shared state is on the heap and never freed: the builder thread is detached and may still run at exit.
        struct State
        {
            uint8_t *ram = nullptr;

            // The disc's sheet: written once by tryCapture() before `captured` (release), read-only afterwards.
            std::mutex captureMutex;
            std::atomic<bool> captured{false};
            std::vector<uint8_t> origImage = std::vector<uint8_t>(cells::kImageBytes);
            std::vector<uint8_t> origClut = std::vector<uint8_t>(cells::kClutBytes);
            Block origBlock{};
            std::atomic<uint32_t> nextCaptureTry{0u};
            bool warnedPatchedFirst = false; // under captureMutex
            uint32_t foreignTries = 0u;      // under captureMutex: copies whose sheet was not the disc's
            bool warnedForeign = false;      // under captureMutex
            // The disc's cells as recorded in a loaded save state (sha256-checked), for a state loaded before this
            // session saw the original font (F9 during the boot logos). Under captureMutex.
            bool haveStateOrig = false;
            Block stateOrig{};

            // Builder thread -> game thread (the whole object is replaced).
            std::mutex builtMutex;
            std::shared_ptr<const Built> built;
            std::atomic<uint64_t> builtVersion{0u};

            std::atomic<uint32_t> vblanks{0u}; // frame hook

            // Game thread only (the Task_BeginDraw observer).
            std::shared_ptr<const Built> current;
            uint32_t seenLoads = 0u;
            std::vector<Block> known; // every block this session built (plus the original)
            uint32_t ownerRetryAt = 0u;
            bool warnedOwner = false;
            bool warnedEntry = false;
            bool warnedUnknown = false;
            uint32_t writes = 0u;
        };

        State &state()
        {
            static State *s = new State;
            return *s;
        }

        uint32_t phys(uint32_t address)
        {
            return address & 0x1FFFFFFu;
        }

        bool inRam(uint32_t address, size_t size)
        {
            const uint32_t p = phys(address);
            return p >= 0x100000u && static_cast<size_t>(p) + size <= kRamBytes;
        }

        uint32_t rd32(const uint8_t *ram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, ram + phys(address), sizeof(value));
            return value;
        }

        // Guest address of the resident MSG_BASE.TEX when its file header and .TEX entry are there, else 0.
        uint32_t findFont(const uint8_t *ram)
        {
            const uint32_t game = rd32(ram, cells::kGamePointer);
            const uint32_t file = game + cells::kFontFileInGame;
            if (!inRam(game, 0u) || !inRam(file, cells::kFileBytes))
                return 0u;
            const uint8_t *f = ram + phys(file);
            if (std::memcmp(f, cells::kFileHeader, sizeof(cells::kFileHeader)) != 0 ||
                std::memcmp(f + cells::kEntryOffset, cells::kEntry, sizeof(cells::kEntry)) != 0)
                return 0u;
            return file;
        }

        // The sheet outside the ten cells and the CLUT are the disc's.
        bool ownedSheet(const uint8_t *image, const uint8_t *clut)
        {
            return cells::fnvOutsideCells(image) == cells::kFnvOutsideCells && cells::fnv1a64(clut, cells::kClutBytes) == cells::kFnvClut;
        }

        std::string shaHex(const void *data, size_t size)
        {
            Sha256 sha;
            sha.update(data, size);
            return sha.finishHex();
        }

        // First sight of the font with the disc's cells: keep image, CLUT and cells (the builder draws from them and a
        // PlayStation pad gets them back). Any thread; the bytes are copied first and the copy is checked, so a file
        // still being loaded is simply tried again later.
        void tryCapture(const uint8_t *ram, uint32_t file)
        {
            State &s = state();
            const uint32_t now = s.vblanks.load(std::memory_order_relaxed);
            if (now < s.nextCaptureTry.load(std::memory_order_relaxed))
                return;
            std::unique_lock<std::mutex> lock(s.captureMutex, std::try_to_lock);
            if (!lock.owns_lock() || s.captured.load(std::memory_order_relaxed))
                return;
            s.nextCaptureTry.store(now + 4u, std::memory_order_relaxed);
            const uint8_t *f = ram + phys(file);
            std::vector<uint8_t> image(f + cells::kImageOffset, f + cells::kImageOffset + cells::kImageBytes);
            std::vector<uint8_t> clut(f + cells::kClutOffset, f + cells::kClutOffset + cells::kClutBytes);
            if (!ownedSheet(image.data(), clut.data()))
            {
                // Not loaded completely yet (a few tries), or not the disc's font (a modified or translated disc):
                // then never written; after ~4 s tell once and look only every 2 s.
                if (++s.foreignTries >= 60u)
                {
                    s.nextCaptureTry.store(now + 120u, std::memory_order_relaxed);
                    if (!s.warnedForeign)
                    {
                        s.warnedForeign = true;
                        HG_WARN(Game, "button prompts: MSG_BASE.TEX at 0x" << std::hex << file << std::dec
                                                                           << " is not the disc's font (modified disc?); PS2 art, nothing written");
                    }
                }
                return;
            }
            s.foreignTries = 0u;
            Block block;
            cells::cellBytes(image.data(), block.data());
            bool fromState = false;
            if (shaHex(block.data(), block.size()) != cells::kShaCells)
            {
                if (!s.haveStateOrig)
                {
                    if (!s.warnedPatchedFirst)
                    {
                        s.warnedPatchedFirst = true;
                        HG_WARN(Game, "button prompts: the font's button cells were already replaced before the original was seen (a "
                                      "save state of an older build loaded right at boot?); the prompts stay as they are until the game "
                                      "restarts");
                    }
                    s.nextCaptureTry.store(now + 60u, std::memory_order_relaxed); // a later state or file reload may bring it
                    return;
                }
                // The state recorded the disc's cells (sha256-checked when it was loaded); the rest of the sheet and the
                // CLUT are the disc's (checked above), so this is the disc's sheet.
                block = s.stateOrig;
                cells::putCellBytes(image.data(), block.data());
                fromState = true;
            }
            s.origImage = std::move(image);
            s.origClut = std::move(clut);
            s.origBlock = block;
            s.captured.store(true, std::memory_order_release);
            HG_INFO(Game, "button prompts: font MSG_BASE.TEX found at 0x" << std::hex << file << std::dec << "; the disc's button cells kept"
                                                                          << (fromState ? " (from the loaded save state's record)" : "")
                                                                          << " (sha256 " << cells::kShaCells << ")");
        }

        // Save-state record "hg.button_prompts": the disc's ten cells when this session has them. A state loaded
        // before the font was ever seen (F9 during the boot logos) brings patched cells; this record lets the prompts
        // follow the device and bindings anyway. States without it (older builds, HG_PROMPTS=0) load as before. Never
        // throws (a throw would fail the whole load): a short or wrong record is ignored.
        constexpr uint32_t kRecordVersion = 1u;

        void saveRecord(ps2x::state::Writer &out)
        {
            State &s = state();
            uint32_t version = kRecordVersion;
            uint8_t have = s.captured.load(std::memory_order_acquire) ? 1u : 0u;
            ps2x::state::fields(out, version, have);
            if (have)
                out.raw(s.origBlock.data(), s.origBlock.size());
        }

        void loadRecord(ps2x::state::Reader &in)
        {
            uint32_t version = 0u;
            uint8_t have = 0u;
            if (in.remaining() < sizeof(version) + sizeof(have))
                return;
            in.raw(&version, sizeof(version));
            in.raw(&have, sizeof(have));
            if (version != kRecordVersion || have != 1u || in.remaining() < cells::kCellBlockBytes)
                return;
            Block block;
            in.raw(block.data(), block.size());
            if (shaHex(block.data(), block.size()) != cells::kShaCells)
                return;
            State &s = state();
            std::lock_guard<std::mutex> lock(s.captureMutex);
            if (!s.haveStateOrig && !s.captured.load(std::memory_order_acquire))
            {
                s.stateOrig = block;
                s.haveStateOrig = true;
                s.nextCaptureTry.store(0u, std::memory_order_relaxed);
            }
        }

        // Every vblank (EE thread, ps2x::pnach frame hook): until the original is kept, look for the font (read only).
        void onVblank()
        {
            State &s = state();
            s.vblanks.fetch_add(1u, std::memory_order_relaxed);
            if (s.captured.load(std::memory_order_acquire) || !s.ram)
                return;
            if (const uint32_t file = findFont(s.ram))
                tryCapture(s.ram, file);
        }

        bool sameCell(const uint8_t *a, const uint8_t *b, int cell)
        {
            const size_t width = cells::kCellRowBytes * cells::CellCount;
            for (int y = 0; y < cells::kCellSize; ++y)
                if (std::memcmp(a + y * width + cell * cells::kCellRowBytes, b + y * width + cell * cells::kCellRowBytes, cells::kCellRowBytes) != 0)
                    return false;
            return true;
        }

        enum class Found
        {
            Original,     // the disc's cells
            ThisSession,  // a block this session wrote
            OtherSession, // cells this code draws, not from this session (a save state)
            Unknown
        };

        const char *foundName(Found found)
        {
            switch (found)
            {
            case Found::Original:
                return "the PS2 art";
            case Found::ThisSession:
                return "earlier PC art";
            case Found::OtherSession:
                return "PC art of another session (save state)";
            case Found::Unknown:
                break;
            }
            return "unknown bytes";
        }

        // What the ten cells hold now. Each cell must be the disc's cell, that cell of a block this session built, or
        // grey-only art (index 0 or a grey entry of the cell's palette: everything this code draws is grey, the PS2
        // face symbols use the coloured entries 11-15). Anything else is not this code's and is never overwritten.
        Found classify(const uint8_t *image, const uint8_t *clut)
        {
            const State &s = state();
            Block now;
            cells::cellBytes(image, now.data());
            if (now == s.origBlock)
                return Found::Original;
            for (const Block &k : s.known)
                if (now == k)
                    return Found::ThisSession;
            bool foreign = false;
            for (int cell = 0; cell < cells::CellCount; ++cell)
            {
                if (sameCell(now.data(), s.origBlock.data(), cell))
                    continue;
                bool knownCell = false;
                for (const Block &k : s.known)
                    if (sameCell(now.data(), k.data(), cell))
                    {
                        knownCell = true;
                        break;
                    }
                if (knownCell)
                    continue;
                uint8_t pal[16][4];
                cells::palette(clut, cells::kCellCsa[cell], pal);
                uint8_t idx[cells::kCellSize * cells::kCellSize];
                cells::cellIndices(image, cell, idx);
                for (uint8_t k : idx)
                    if (k != 0u && !(pal[k][0] == pal[k][1] && pal[k][1] == pal[k][2]))
                        return Found::Unknown;
                foreign = true;
            }
            return foreign ? Found::OtherSession : Found::ThisSession;
        }

        void remember(const Block &block)
        {
            State &s = state();
            for (const Block &k : s.known)
                if (k == block)
                    return;
            if (s.known.size() >= kMaxKnown)
                s.known.erase(s.known.begin());
            s.known.push_back(block);
        }

        // Task_BeginDraw entry (game thread): make the ten cells hold the newest build, and have the game upload the
        // font again when it changed. Costs one 1,280-byte compare per text box when nothing changed.
        void onBeginDraw(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            State &s = state();
            const uint32_t file = findFont(rdram);
            if (!file)
                return;
            if (!s.captured.load(std::memory_order_acquire))
                tryCapture(rdram, file);

            const uint64_t version = s.builtVersion.load(std::memory_order_acquire);
            if (!s.current || s.current->version != version)
            {
                std::unique_lock<std::mutex> lock(s.builtMutex, std::try_to_lock);
                if (lock.owns_lock())
                    s.current = s.built;
            }
            if (!s.current || !s.captured.load(std::memory_order_acquire))
                return; // nothing built yet: the font keeps what it has
            const Built &want = *s.current;

            uint8_t *f = rdram + phys(file);
            uint8_t *image = f + cells::kImageOffset;
            const uint8_t *clut = f + cells::kClutOffset;
            const uint32_t loads = ps2x::savestate::loadCount();
            const bool reloaded = loads != s.seenLoads;
            const bool same = cells::cellsEqual(image, want.block.data());
            if (same && !reloaded)
                return;

            // The font's texture-cache entry must be this file.
            const uint32_t tc = rd32(rdram, kTexCachePointer);
            const uint32_t entryAddress = tc + 4u + kFontGroup * kTexEntryBytes;
            if (!inRam(tc, 0u) || !inRam(entryAddress, kTexEntryBytes))
                return;
            uint8_t *entry = rdram + phys(entryAddress);
            uint32_t tex;
            int16_t layer;
            std::memcpy(&tex, entry, sizeof(tex));
            std::memcpy(&layer, entry + 6, sizeof(layer));
            const int8_t group = static_cast<int8_t>(entry[8]);
            const uint8_t index = entry[9];
            if (phys(tex) != phys(file + cells::kEntryOffset) || group != static_cast<int8_t>(kFontGroup) || index != 0u)
            {
                if (!s.warnedEntry)
                {
                    s.warnedEntry = true;
                    HG_WARN(Game, "button prompts: texture-cache entry 0x14 at 0x" << std::hex << entryAddress << " holds tex 0x" << tex
                                                                                    << " group 0x" << (static_cast<int>(group) & 0xFF) << " index "
                                                                                    << std::dec << static_cast<int>(index) << ", not the font; waiting");
                }
                return;
            }

            // Ownership (only on a mismatch): the rest of the sheet and the CLUT are the disc's, the cells are known.
            const uint32_t now = s.vblanks.load(std::memory_order_relaxed);
            if (now < s.ownerRetryAt)
                return;
            if (!ownedSheet(image, clut))
            {
                s.ownerRetryAt = now + 120u;
                if (!s.warnedOwner)
                {
                    s.warnedOwner = true;
                    HG_WARN(Game, "button prompts: the font at 0x" << std::hex << file << std::dec
                                                                  << " is not the disc's MSG_BASE.TEX outside the button cells; not written");
                }
                return;
            }
            remember(s.origBlock);
            const Found found = same ? Found::ThisSession : classify(image, clut);
            if (found == Found::Unknown)
            {
                s.ownerRetryAt = now + 120u;
                if (!s.warnedUnknown)
                {
                    s.warnedUnknown = true;
                    HG_WARN(Game, "button prompts: the font's button cells hold bytes this code did not draw; left alone");
                }
                return;
            }
            s.seenLoads = loads;

            if (!same)
            {
                cells::putCellBytes(image, want.block.data());
                remember(want.block);
            }
            // Release the font's layer exactly as the cache's LRU eviction does; Task_BeginDraw's TexCache_Layer call
            // right after this then hands out a layer with bit 31 and the game uploads the font from RAM.
            const bool resident = layer >= 0;
            if (resident)
            {
                const int16_t none = -1;
                std::memcpy(entry + 4, &none, sizeof(none)); // age
                std::memcpy(entry + 6, &none, sizeof(none)); // layer
            }
            ++s.writes;
            if (s.writes <= 32u || s.writes % 256u == 0u)
            {
                if (same)
                    HG_INFO(Game, "button prompts: save state loaded; the cells already show " << want.summary << "; "
                                                                                               << (resident ? "font layer released for a re-upload" : "font not in VRAM"));
                else
                    HG_INFO(Game, "button prompts: cells now show " << want.summary << " (replaced " << foundName(found) << "); "
                                                                    << (resident ? "font layer " + std::to_string(layer) + " released for a re-upload"
                                                                                 : std::string("font not in VRAM, it uploads at its next use"))
                                                                    << (reloaded ? " (after a save-state load)" : "") << " [write " << s.writes << "]");
            }
        }

        // ---- builder thread ----

        std::string labelOf(const bindings::Binding &binding)
        {
            if (binding.type == bindings::Type::None)
                return {}; // nothing of that kind bound: the cell keeps the PS2 art
            return std::string(binding.compact, strnlen(binding.compact, sizeof(binding.compact)));
        }

        // Cell -> PS2 input (prompt_cells.h Cell order).
        constexpr bindings::Input kCellInput[cells::CellCount] = {
            bindings::Circle, bindings::Triangle, bindings::Square, bindings::Cross, bindings::L1,
            bindings::L2,     bindings::R1,       bindings::R2,     bindings::Select, bindings::Start,
        };
        constexpr const char *kCellName[cells::CellCount] = {"circle", "triangle", "square", "cross", "L1", "L2", "R1", "R2", "select", "start"};

        cells::Labels labelsFor(const bindings::Snapshot &snapshot, bindings::Device device)
        {
            const bool pad = device == bindings::Device::Controller;
            const auto &set = pad ? snapshot.controller : snapshot.keys;
            cells::Labels labels;
            labels.style = pad ? cells::Style::Xbox : cells::Style::Keys;
            for (int cell = 0; cell < cells::CellCount; ++cell)
                labels.text[cell] = labelOf(set[kCellInput[cell]]);
            return labels;
        }

        std::string describe(const cells::Labels &labels, const char *device)
        {
            std::string text = device;
            text += ":";
            for (int cell = 0; cell < cells::CellCount; ++cell)
            {
                text += cell ? ", " : " ";
                text += kCellName[cell];
                text += " ";
                text += labels.text[cell].empty() ? std::string("(PS2)") : labels.text[cell];
            }
            return text;
        }

        // Polls the published bindings every 100 ms and builds the ten cells when the bindings, the last device or the
        // pad style changed (a few ms of GDI text).
        void builderLoop()
        {
            State &s = state();
            uint32_t lastGeneration = 0u;
            bindings::Device lastDevice = bindings::Device::Keys;
            bindings::PadStyle lastStyle = bindings::PadStyle::Unknown;
            bool done = false;
            bool warnedFailure = false;
            std::chrono::steady_clock::time_point retryAt{};
            uint64_t version = 0u;
            for (;;)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (!s.captured.load(std::memory_order_acquire))
                    continue;
                const uint32_t generation = bindings::generation();
                if (generation == 0u)
                    continue; // bindings not published yet
                const bindings::Device device = bindings::lastDevice();
                const bindings::PadStyle style = bindings::padStyle();
                if (done && generation == lastGeneration && device == lastDevice && style == lastStyle)
                    continue;
                const auto now = std::chrono::steady_clock::now();
                if (now < retryAt)
                    continue;

                auto next = std::make_shared<Built>();
                next->version = version + 1u;
                uint32_t labelsGeneration = generation;
                const bool pad = device == bindings::Device::Controller;
                if (pad && style == bindings::PadStyle::PlayStation)
                {
                    next->original = true;
                    next->summary = "PlayStation controller: the PS2 art";
                }
                else
                {
                    const bindings::Snapshot snapshot = bindings::snapshot();
                    labelsGeneration = snapshot.generation; // what these labels are (a rebind after the poll is seen next time)
                    const cells::Labels labels = labelsFor(snapshot, device);
                    bool any = false;
                    for (const std::string &text : labels.text)
                        any |= !text.empty();
                    const char *name = !pad ? "keyboard" : style == bindings::PadStyle::Xbox ? "Xbox controller" : "controller (Xbox layout)";
                    if (!any)
                    {
                        next->original = true;
                        next->summary = std::string(name) + ": nothing bound to the prompt buttons, the PS2 art";
                    }
                    else
                    {
                        std::vector<uint8_t> out(cells::kImageBytes);
                        if (!cells::buildCells(s.origImage.data(), s.origClut.data(), labels, out.data()))
                        {
                            if (!warnedFailure)
                            {
                                warnedFailure = true;
                                HG_WARN(Game, "button prompts: could not draw " << describe(labels, name)
                                                                                << " (GDI); trying again every 2 s, the prompts keep their art");
                            }
                            retryAt = now + std::chrono::seconds(2);
                            continue;
                        }
                        cells::cellBytes(out.data(), next->block.data());
                        next->summary = describe(labels, name);
                    }
                }
                if (next->original)
                    next->block = s.origBlock;
                version = next->version;
                {
                    std::lock_guard<std::mutex> lock(s.builtMutex);
                    s.built = next;
                }
                s.builtVersion.store(version, std::memory_order_release);
                lastGeneration = labelsGeneration;
                lastDevice = device;
                lastStyle = style;
                done = true;
                warnedFailure = false;
                HG_INFO(Game, "button prompts: built " << next->summary << " (cells sha256 " << shaHex(next->block.data(), next->block.size()).substr(0, 16)
                                                       << ")");
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (const char *value = std::getenv("HG_PROMPTS"); value && *value == '0')
        {
            HG_INFO(Game, "button prompts: off (HG_PROMPTS=0), PS2 art");
            return;
        }
        State &s = state();
        s.ram = runtime.memory().getRDRAM();
        if (!FunctionHooks::instance().observeEntry(kTaskBeginDraw, "Task_BeginDraw (button prompts)", &onBeginDraw))
        {
            HG_WARN(Game, "button prompts: cannot observe Task_BeginDraw 0x382000; PS2 art");
            return;
        }
        ps2x::pnach::addFrameHook(&onVblank);
        ps2x::savestate::registerExtension("hg.button_prompts", &saveRecord, &loadRecord);
        std::thread(builderLoop).detach();
        HG_INFO(Game, "button prompts: on (keyboard: key caps of the bound keys; Xbox-style controller: A B X Y, LB RB, LT RT, "
                      "View, Menu; PlayStation controller: PS2 art)");
    }
}
