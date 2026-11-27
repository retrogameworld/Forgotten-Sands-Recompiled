#pragma once

// Step 38f (PC): RGBA8 picture (outW x outH) of the "Achievement unlocked" notice for the notice text that
// Achievements::PollNotice gives ("id \t name \t gamerscore \t icon path"), sized for a screen of that height.
bool BuildAchievementToast(const std::string& notice, uint32_t screenHeight, std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH);
