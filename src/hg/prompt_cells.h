#pragma once

// Haunting Ground (SLUS_210.75) button prompts: the cell builder (port_prep/prompts/DESIGN.md 3.2).
//
// Every PS2 button icon the US game shows is one glyph of the small font SUBSCR/MSG_BASE.TEX (port_prep/prompts/RECON.md):
// glyphs 0xA0..0xA9 (message bytes 0xC0..0xC9), ten 16x16 cells of a PSMT4 256x256 sheet at v = 160, u = cell * 16.
// The sheet is resident in guest RAM for the whole session (Game+0x2AC0 = 0x0048A4C0). This file redraws those ten
// cells for the player's PC bindings. From port_prep/prompts/tools/src/hg_prompt_cells.* (the offline preview builder)
// with the approved look fixed (2026-10-18): keyboard = mid-grey key caps with white Segoe UI Bold labels, Xbox-style
// = white letters on the game's own round buttons / white LB RB LT RT View Menu shapes. The preview's Look switches
// (light caps, PS-colour labels, coloured Xbox letters, semibold) are gone.
//
// Pure functions: given the sheet's original 32 KB image and its 1 KB CLUT and the labels, they write the patched
// image. No runtime state, no guest RAM access, and the CLUT is never changed: every new texel is index 0
// (transparent) or the nearest grey entry of the palette the game draws that cell with. Nothing outside the ten cells
// (bytes 0..79 of image rows 160..175) is ever written.

#include <cstddef>
#include <cstdint>
#include <string>

namespace hg::prompt_cells
{
    // ---- where (RECON.md sections 1 and 3: OpenHuey scene_boot.c / system.c + hidden runs p1 and p2) ----
    constexpr uint32_t kGamePointer = 0x0044E978u;     // *(u32*) = static Game (0x00487A00 in p1/p2)
    constexpr uint32_t kFontFileInGame = 0x2AC0u;       // Game + 0x2AC0 = the resident MSG_BASE.TEX (33,824 B)
    constexpr uint32_t kFontFileRam = 0x0048A4C0u;      // = 0x00487A00 + 0x2AC0 (for reference; the code follows the pointer)
    constexpr uint32_t kEntryOffset = 0x10u;           // the .TEX entry (TexCache group 0x14 points here: 0x0048A4D0)
    constexpr uint32_t kImageOffset = 0x20u;           // PSMT4 256x256, linear rows of 128 B, even x = low nibble
    constexpr uint32_t kClutOffset = 0x8020u;          // CT32 16x16 block, CSM1: 16 palettes of 16 colours
    constexpr size_t kFileBytes = 33824u;
    constexpr size_t kImageBytes = 32768u;
    constexpr size_t kClutBytes = 1024u;
    constexpr int kSheetWidth = 256;
    constexpr int kCellSize = 16;
    constexpr int kIconRow = 160;                       // v of glyphs 0xA0..0xAF
    constexpr size_t kRowBytes = 128u;                  // 256 texels x 4 bit
    constexpr size_t kCellRowBytes = 8u;                // 16 texels x 4 bit
    // The .TEX file header (file+0: 1 entry) and entry (file+0x10: psm 0x14 PSMT4, cpsm 0 CT32, 256 x 256, image 0x800
    // qw, CLUT 0x40 qw, data +0x10), as on the disc and in RAM (RECON.md 3).
    constexpr uint8_t kFileHeader[16] = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    constexpr uint8_t kEntry[16] = {0x14, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x08, 0x40, 0x00, 0x10, 0x00, 0x00, 0x00};
    // sha256 of the disc's bytes (RECON.md; tools/prompt_preview.exe recomputes them from the disc file at every run).
    constexpr const char *kShaImage = "fdb2f70201e467e43a3a70bc3ffdf21e3d9928ba9389b06a814b4d208ccda51b";
    constexpr const char *kShaClut = "c466a6865c3472eca5fc6a3a8008dab9193ccb1a66c1dc8cc0b3231c62915bed";
    // The ten cells only: bytes 0..79 of rows 160..175, concatenated (1,280 B). What a patch overwrites.
    constexpr const char *kShaCells = "3f875dc4a7a92ae704bceca5dade50141676b8b4febc440b6357d78c6db33518";
    // FNV-1a 64 of the disc's image without the ten cells (rows in order, rows 160..175 from byte 80 on: 31,488 B) and
    // of its CLUT (1,024 B). The ownership check before any write: the sheet in RAM is the disc's font. Computed from
    // the disc's SUBSCR/MSG_BASE.TEX (sha256 5c9c687d27daef47...) by port_prep/prompts/tools/fnv_consts.py.
    constexpr uint64_t kFnvOutsideCells = 0x7D6B349AB84848A6ull;
    constexpr uint64_t kFnvClut = 0xCAA94DF2C85BF3FEull;
    constexpr size_t kCellBlockBytes = kCellSize * kCellRowBytes * 10u; // 1,280

    // Cell k = glyph 0xA0 + k = message byte 0xC0 + k.
    enum Cell : int
    {
        Circle,
        Triangle,
        Square,
        Cross,
        L1,
        L2,
        R1,
        R2,
        Select, // the grey bar
        Start,  // the grey triangle
        CellCount
    };
    static_assert(kCellBlockBytes == kCellSize * kCellRowBytes * CellCount);
    // The CLUT (colour code 0x03 c, low 6 bits) every US message puts before the glyph: MSG_BASE #28-35, #137-138,
    // MSG_SUB #8, #91, #93, #101-108, #130-131. 4 and 5 = grey ramp 1-10 + PS colours 11-15, 6 = grey ramp 1-15.
    constexpr uint8_t kCellCsa[CellCount] = {4, 5, 4, 5, 6, 6, 6, 6, 6, 6};

    enum class Style : uint8_t
    {
        Keys, // key caps with the bound key
        Xbox  // A/B/X/Y on the game's own round buttons, LB/RB bumpers, LT/RT triggers, View, Menu
              // (PlayStation pads: no patch at all, the original bytes go back)
    };

    struct Labels
    {
        Style style = Style::Keys;
        // Per cell: ps2_host_input::prompts::Binding::compact of the bound key ("K", "Bksp", "Ent", "Spc", "Shft",
        // "F12", "N8", "M1", the arrows U+2190..U+2193). Empty = keep that cell's original art. Xbox: the label picks the
        // cell's Xbox shape ("A", "LB", "LT", "View", "Menu"). Text is at most 3 characters: known 4-letter names are
        // shortened ("Ctrl" -> "Ctl", "Misc" -> "Msc", "TPad" -> "TP"; the keyboard "Menu" key is drawn as three bars),
        // anything else keeps its first three characters.
        std::string text[CellCount];
    };

    // Writes origImage with the ten cells redrawn into outImage (kImageBytes; may equal origImage). False (outImage
    // untouched) when an argument is null or a label could not be drawn. Takes a few milliseconds (GDI text).
    bool buildCells(const uint8_t *origImage, const uint8_t *clut, const Labels &labels, uint8_t *outImage);

    // 16 RGBA entries (alpha as stored, 0x80 = opaque) of palette csa of a 16-colour CSM1 CT32 CLUT.
    void palette(const uint8_t *clut, int csa, uint8_t rgba[16][4]);
    // Cell indices (row-major 16x16) of a PSMT4 sheet image.
    void cellIndices(const uint8_t *image, int cell, uint8_t indices[kCellSize * kCellSize]);
    // The 1,280 bytes kShaCells covers, out of a sheet image (row y of the block = bytes 0..79 of image row 160 + y).
    void cellBytes(const uint8_t *image, uint8_t out[kCellBlockBytes]);
    // The reverse: writes such a block into the ten cells of a sheet image (only bytes 0..79 of rows 160..175).
    void putCellBytes(uint8_t *image, const uint8_t block[kCellBlockBytes]);
    // True when the block's bytes equal the ten cells of the image.
    bool cellsEqual(const uint8_t *image, const uint8_t block[kCellBlockBytes]);

    uint64_t fnv1a64(const uint8_t *data, size_t size, uint64_t seed = 0xCBF29CE484222325ull);
    // FNV-1a 64 of an image without the ten cells (what kFnvOutsideCells is for the disc's image).
    uint64_t fnvOutsideCells(const uint8_t *image);
}
