#pragma once

// Scripted test input (--input-script <file>) and keyboard-to-pad mapping.
//
// Script lines (time in seconds since program start, '#' starts a comment):
//   at 12.0 press A            tap a button (0.15 s)
//   at 14 hold DOWN 0.3        hold a button for 0.3 s
//   at 20 stick L 0 1 2.0      left stick x=0 y=1 (range -1..1) for 2 s (R = right stick)
//   at 21 trigger R 1 0.5      right trigger fully pressed for 0.5 s
//   at 25 shot shot.bmp        take a screenshot (relative paths are next to the script)
//   at 60 quit                 exit the program
// Buttons: A B X Y START BACK UP DOWN LEFT RIGHT LB RB LS RS

namespace InputScript
{
    bool Load(const std::filesystem::path& path);
    bool IsActive();

    // Seconds since program start (the script clock).
    double Now();

    // Adds the scripted (and keyboard) input for this moment to the pad state.
    void Apply(XAMINPUT_GAMEPAD& pad);

    // Called by the main loop: runs "shot" and "quit" events. Returns false to quit.
    bool Update();
}
