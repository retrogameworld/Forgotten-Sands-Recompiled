#pragma once
// Step 36: unpack the player's own Xbox 360 disc image into the game folder (see iso_extract.cpp).
namespace IsoExtract
{
    // true if the file holds an Xbox 360 game disc file system (XDVDFS).
    bool IsGameDisc(const std::filesystem::path& iso);
    // Copies every file of the disc image into `to`. progress(done, total) is called while copying (bytes).
    bool ExtractAll(const std::filesystem::path& iso, const std::filesystem::path& to, std::string& error,
        std::function<void(uint64_t, uint64_t)> progress = nullptr);
}
