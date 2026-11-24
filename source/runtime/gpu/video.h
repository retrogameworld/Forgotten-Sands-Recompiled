#pragma once

// Host graphics: SDL2 window + plume (Vulkan) device, and the replacement of Torchlight's
// statically linked Xbox 360 Direct3D layer (UnleashedRecomp-style GUEST_FUNCTION hooks).
// The guest never touches the Xenos ring buffer, fences or interrupts: CreateDevice returns a
// host-owned device, resources are host objects living in guest memory, and draws are recorded
// into a plume command list that is submitted when the game calls Swap/Present.
// Hooked addresses and their meaning: see the list at the end of video.cpp and
// work/d3d_research/d3d_functions.md.

namespace Video
{
    // Creates the SDL window (hidden unless --window) and the Vulkan device/swapchain.
    bool CreateHostDevice(const char* sdlVideoDriver = nullptr);
    bool IsCreated();
    void ToggleFullscreen();
    void PumpEvents();
    void Shutdown();

    // Frames presented by the guest (Swap/Present hooks).
    uint64_t GetPresentedFrameCount();
    // Writes the next presented frame to a .bmp file (test screenshots, no window capture needed).
    void RequestScreenshot(const std::filesystem::path& path);
}
