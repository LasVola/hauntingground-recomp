#pragma once

// The player's own Haunting Ground (USA) disc image: found, checked and its boot ELF copied out for the runtime.
// ADAPTED from the God Hand port's src/godhand/game_disc.h (itself from the Urban Reign port); serial, boot ELF, hash
// and the disc file list are Haunting Ground's.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hg
{
    // Haunting Ground (USA), SLUS-21075. Hash: DISC_HASHES.md (SLUS_210.75 row). File list and sizes read from the
    // ISO9660 directory of the Haunting Ground (USA) disc image on 2026-10-18 (port_prep/gamelayer WORKLOG); they
    // equal the files extracted from it.
    inline constexpr const char *kSerial = "SLUS-21075";
    inline constexpr const char *kGameTitle = "Haunting Ground";
    inline constexpr const char *kBootElfPath = "SLUS_210.75";
    inline constexpr const char *kBootElfSha256 = "3b374d53a499d2c17b205274ee9eb34280768f294f970ebf6ae6731f6a2dacb8";
    // SYSTEM.CNF is "BOOT2 = cdrom0:\SLUS_210.75;1", "VER = 1.01", "VMODE = NTSC" (57 bytes).
    inline constexpr const char *kSystemCnfBoot = "cdrom0:\\SLUS_210.75;1";

    struct DiscFile
    {
        const char *path;   // ISO9660 path (PS2CdImage::normalizeKey accepts '/' or '\\', no ";1")
        uint64_t size;      // size on the SLUS-21075 disc; a different size is reported, not fatal
        bool required;      // missing = this is not a usable Haunting Ground (USA) disc
        const char *what;
    };

    // Root: SYSTEM.CNF, SLUS_210.75, DATA.CVM (CRI ROFS volume with all game data), 0CT4FC_.TXT, MODULES/ (IOPRP300.IMG and
    // 12 IRX the game loads by name from cdrom0:\MODULES\). 00DUMMY..02DUMMY are padding and are not checked.
    inline constexpr DiscFile kDiscFiles[] = {
        {"SYSTEM.CNF", 57u, true, "boot configuration"},
        {"SLUS_210.75", 3650608u, true, "boot ELF"},
        {"DATA.CVM", 1577156608u, true, "CRI CVM/ROFS volume (game data)"},
        {"MODULES/IOPRP300.IMG", 278305u, true, "IOP replacement (reboot) image"},
        {"MODULES/SIO2MAN.IRX", 6641u, true, "IOP module"},
        {"MODULES/SIO2D.IRX", 11289u, true, "IOP module"},
        {"MODULES/DBCMAN.IRX", 15653u, true, "IOP module"},
        {"MODULES/LIBSD.IRX", 30085u, true, "IOP module"},
        {"MODULES/CRI_ADXI.IRX", 69261u, true, "IOP module"},
        {"MODULES/MODHSYN.IRX", 63037u, true, "IOP module"},
        {"MODULES/MODMIDI.IRX", 21941u, true, "IOP module"},
        {"MODULES/MODMSIN.IRX", 1929u, true, "IOP module"},
        {"MODULES/SNDDRV.IRX", 82848u, true, "IOP module"},
        {"MODULES/DS2O_S1.IRX", 10941u, true, "IOP module"},
        {"MODULES/MCMAN.IRX", 96181u, true, "IOP module"},
        {"MODULES/MCSERV.IRX", 7385u, true, "IOP module"},
        {"0CT4FC_.TXT", 8u, false, "text \"data.cvm\""},
    };

    struct DiscCheckResult
    {
        bool ok = false;
        std::string error;
        std::filesystem::path bootElfPath;
    };

    // The first *.iso in the search roots whose SYSTEM.CNF boots cdrom0:\SLUS_210.75;1 (empty path if none).
    std::filesystem::path findDiscImage(const std::vector<std::filesystem::path> &searchRoots);
    // Mounts the image (PS2CdImage singleton), checks SYSTEM.CNF, the disc files above and the boot ELF's SHA-256, and
    // writes the boot ELF to cacheDir/SLUS_210.75 for PS2Runtime::loadELF.
    DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &image, const std::filesystem::path &cacheDir);
}
