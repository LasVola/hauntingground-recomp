#include "hg/game_disc.h"
#include "hg/log.h"
#include "hg/sha256.h"
#include "runtime/ps2_cd_image.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>

namespace hg
{
    namespace
    {
        bool readWholeFile(const PS2CdImage &image, std::string_view ps2Path, std::vector<uint8_t> &out)
        {
            PS2CdImage::Entry entry{};
            if (!image.find(ps2Path, entry) || entry.directory)
                return false;
            out.resize(static_cast<size_t>(entry.size));
            return image.read(static_cast<uint64_t>(entry.lba) * PS2CdImage::kSectorSize, out.data(), out.size());
        }

        std::string lower(std::string s)
        {
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        }

        bool cnfBootsGame(const std::vector<uint8_t> &cnf)
        {
            const std::string text(cnf.begin(), cnf.end());
            return text.find("BOOT2") != std::string::npos && text.find(kSystemCnfBoot) != std::string::npos;
        }

        // Any ISO whose SYSTEM.CNF boots cdrom0:\SLUS_210.75;1 counts here; the boot ELF hash is checked afterwards.
        bool bootsGame(const std::filesystem::path &path)
        {
            // PS2CdImage is a singleton; verifyDiscAndPrepare reopens the chosen image afterwards.
            PS2CdImage &probe = PS2CdImage::instance();
            if (!probe.open(path))
                return false;
            std::vector<uint8_t> cnf;
            return readWholeFile(probe, "SYSTEM.CNF", cnf) && cnfBootsGame(cnf);
        }

        // The data files the game opens by name. A missing required file is an error; a different size is only
        // reported (the recompiled code depends on the boot ELF alone, which is hash-checked).
        bool checkDiscFiles(const PS2CdImage &image, std::string &error)
        {
            for (const DiscFile &file : kDiscFiles)
            {
                PS2CdImage::Entry entry{};
                if (!image.find(file.path, entry) || entry.directory)
                {
                    if (file.required)
                    {
                        error = std::string(file.path) + " (" + file.what + ") not found on the disc; expected " + kSerial;
                        return false;
                    }
                    HG_WARN(Boot, file.path << " (" << file.what << ") not found on the disc");
                    continue;
                }
                if (entry.size != file.size)
                    HG_WARN(Boot, file.path << " is " << entry.size << " bytes; the " << kSerial << " disc has " << file.size);
                else
                    HG_INFO(Boot, file.path << " " << entry.size << " bytes, lba " << entry.lba << " (" << file.what << ")");
            }
            // DATA.CVM: a CRI CVM volume starts with "CVMH" (recon/elf_disc/ELF_DISC.md section 4). Only reported.
            PS2CdImage::Entry cvm{};
            uint8_t header[4]{};
            if (image.find("DATA.CVM", cvm) &&
                image.read(static_cast<uint64_t>(cvm.lba) * PS2CdImage::kSectorSize, header, sizeof(header)) &&
                std::memcmp(header, "CVMH", 4) != 0)
                HG_WARN(Boot, "DATA.CVM does not start with \"CVMH\"");
            return true;
        }
    }

    std::filesystem::path findDiscImage(const std::vector<std::filesystem::path> &searchRoots)
    {
        for (const auto &root : searchRoots)
        {
            std::error_code error;
            if (!std::filesystem::is_directory(root, error))
                continue;
            for (const auto &entry : std::filesystem::directory_iterator(root, error))
            {
                if (entry.is_regular_file(error) && lower(entry.path().extension().string()) == ".iso" && bootsGame(entry.path()))
                    return entry.path();
            }
        }
        return {};
    }

    DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &imagePath, const std::filesystem::path &cacheDir)
    {
        DiscCheckResult result;
        PS2CdImage &image = PS2CdImage::instance();
        if (!image.open(imagePath))
        {
            result.error = "cannot mount " + imagePath.string() + " as an ISO9660 disc image";
            return result;
        }

        std::vector<uint8_t> systemCnf;
        if (!readWholeFile(image, "SYSTEM.CNF", systemCnf))
        {
            result.error = "SYSTEM.CNF not found; this is not a PS2 game disc";
            return result;
        }
        if (!cnfBootsGame(systemCnf))
        {
            result.error = std::string("SYSTEM.CNF does not boot ") + kSystemCnfBoot + "; expected " + kGameTitle + " (" + kSerial + ")";
            return result;
        }
        if (!checkDiscFiles(image, result.error))
            return result;

        std::vector<uint8_t> bootElf;
        if (!readWholeFile(image, kBootElfPath, bootElf))
        {
            result.error = std::string("boot executable ") + kBootElfPath + " missing";
            return result;
        }
        const std::string bootHash = Sha256::hex(bootElf.data(), bootElf.size());
        if (bootHash != kBootElfSha256)
        {
            result.error = std::string("boot executable hash mismatch (") + bootHash + "); the recompiled code was built from " +
                           kSerial + " " + kBootElfSha256;
            return result;
        }
        HG_INFO(Boot, kBootElfPath << " sha256 " << bootHash << " (matches " << kSerial << ")");

        std::error_code error;
        std::filesystem::create_directories(cacheDir, error);
        result.bootElfPath = cacheDir / kBootElfPath;
        std::ofstream out(result.bootElfPath, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bootElf.data()), static_cast<std::streamsize>(bootElf.size()));
        if (!out)
        {
            result.error = "cannot write " + result.bootElfPath.string();
            return result;
        }
        result.ok = true;
        return result;
    }
}
