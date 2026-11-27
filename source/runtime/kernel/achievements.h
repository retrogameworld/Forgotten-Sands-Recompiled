#pragma once

// Step 38f: the game's Xbox 360 achievements (see achievements.cpp)
namespace Achievements
{
    void Init(const std::filesystem::path& userDir);  // after the XEX is loaded
    void Write(uint32_t count, uint32_t pointer);     // XUserWriteAchievements (XMsg 0xFB / 0xB0008)
    bool PollNotice(std::string& out);                // next new unlock for the on-screen notice

    // the game's achievements with their unlock state, for XamUserCreateAchievementEnumerator (the game reads them
    // at the start; with no list it never unlocked any)
    struct Detail { uint32_t id, imageId, gamerscore, flags; std::string name, description, locked; int64_t unlocked; };
    std::vector<Detail> Details();
}
