#include <stdafx.h>
#include "input_script.h"
#include <sstream>
#include <gpu/video.h>
#include <os/logger.h>

bool CameraPlayerPosition(float* xyz); // kernel/camera_hooks.cpp

// Scripted pad input for automated tests, plus a keyboard mapping so the game can be
// played without a controller. Both are merged into pad 0 by hid::GetState.

namespace
{
    enum class EventType { Button, Stick, Trigger, Shot, Quit, Goto };

    struct Event
    {
        double time = 0;
        double duration = 0;
        EventType type = EventType::Button;
        uint16_t buttons = 0;
        int side = 0; // 0 = left, 1 = right
        float x = 0, y = 0;
        std::filesystem::path file;
        bool fired = false;
    };

    std::vector<Event> g_events;
    std::mutex g_mutex;
    bool g_active = false;
    const auto g_start = std::chrono::steady_clock::now();

    uint16_t ParseButton(std::string name)
    {
        for (auto& c : name) c = char(toupper((unsigned char)c));
        static const std::pair<const char*, uint16_t> names[] =
        {
            { "A", XAMINPUT_GAMEPAD_A }, { "B", XAMINPUT_GAMEPAD_B }, { "X", XAMINPUT_GAMEPAD_X }, { "Y", XAMINPUT_GAMEPAD_Y },
            { "START", XAMINPUT_GAMEPAD_START }, { "BACK", XAMINPUT_GAMEPAD_BACK },
            { "UP", XAMINPUT_GAMEPAD_DPAD_UP }, { "DOWN", XAMINPUT_GAMEPAD_DPAD_DOWN },
            { "LEFT", XAMINPUT_GAMEPAD_DPAD_LEFT }, { "RIGHT", XAMINPUT_GAMEPAD_DPAD_RIGHT },
            { "LB", XAMINPUT_GAMEPAD_LEFT_SHOULDER }, { "RB", XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
            { "LS", XAMINPUT_GAMEPAD_LEFT_THUMB }, { "RS", XAMINPUT_GAMEPAD_RIGHT_THUMB },
        };
        for (auto& [n, b] : names)
            if (name == n)
                return b;
        return 0;
    }

    // Test navigation (Step 35p): "at <t> goto <x> <y> <seconds>" steers the left stick so that the Prince walks to
    // the world position (x, y). The stick is relative to the camera, whose direction is not known here: it is
    // estimated from how the Prince actually moves under the stick (world direction = stick direction + offset),
    // starting with a push "up" and corrected all the time. Stops within 0.6 units of the target.
    struct GotoState { double lastT = -1; float lastX = 0, lastY = 0; float offset = 0; bool haveOffset = false; float stickAngle = 1.5708f; const void* owner = nullptr; int stuck = 0; float wiggle = 0; };
    GotoState g_goto;

    int16_t ToAxis(float v)
    {
        v = std::clamp(v, -1.0f, 1.0f);
        return int16_t(v * 32767.0f);
    }
}

double InputScript::Now()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - g_start).count();
}

bool InputScript::IsActive()
{
    return g_active;
}

bool InputScript::Load(const std::filesystem::path& path)
{
    std::ifstream in(path);
    if (!in)
    {
        LOGFN_ERROR("Input script not found: {}", (const char*)path.u8string().c_str());
        return false;
    }

    std::vector<Event> events;
    std::string line;
    int lineNumber = 0;
    while (std::getline(in, line))
    {
        lineNumber++;
        if (auto hash = line.find('#'); hash != std::string::npos)
            line.resize(hash);

        std::istringstream ss(line);
        std::string at, cmd;
        Event e;
        if (!(ss >> at))
            continue;
        if (at != "at" || !(ss >> e.time >> cmd))
        {
            LOGFN_WARNING("Input script line {}: expected 'at <seconds> <command>'", lineNumber);
            continue;
        }
        for (auto& c : cmd) c = char(tolower((unsigned char)c));

        bool ok = true;
        if (cmd == "press" || cmd == "hold")
        {
            std::string button;
            ss >> button;
            e.type = EventType::Button;
            e.buttons = ParseButton(button);
            e.duration = 0.15;
            if (cmd == "hold" && !(ss >> e.duration))
                ok = false;
            ok = ok && e.buttons != 0;
        }
        else if (cmd == "stick" || cmd == "trigger")
        {
            std::string side;
            ss >> side;
            e.side = (side == "R" || side == "r") ? 1 : 0;
            if (cmd == "stick")
            {
                e.type = EventType::Stick;
                ok = bool(ss >> e.x >> e.y >> e.duration);
            }
            else
            {
                e.type = EventType::Trigger;
                ok = bool(ss >> e.x >> e.duration);
            }
        }
        else if (cmd == "goto")
        {
            e.type = EventType::Goto;
            ok = bool(ss >> e.x >> e.y >> e.duration);
        }
        else if (cmd == "shot")
        {
            std::string file;
            ss >> file;
            e.type = EventType::Shot;
            e.file = file;
            if (e.file.is_relative())
                e.file = path.parent_path() / e.file;
            ok = !file.empty();
        }
        else if (cmd == "quit")
        {
            e.type = EventType::Quit;
        }
        else
        {
            ok = false;
        }

        if (!ok)
        {
            LOGFN_WARNING("Input script line {} not understood: {}", lineNumber, line);
            continue;
        }
        events.push_back(e);
    }

    std::lock_guard lock(g_mutex);
    g_events = std::move(events);
    g_active = true;
    LOGFN("Input script {}: {} events", (const char*)path.u8string().c_str(), g_events.size());
    return true;
}

static void ApplyKeyboard(XAMINPUT_GAMEPAD& pad)
{
    // Only while our window has keyboard focus.
    if (SDL_GetKeyboardFocus() == nullptr)
        return;

    int count = 0;
    const Uint8* keys = SDL_GetKeyboardState(&count);
    if (!keys)
        return;

    auto down = [&](SDL_Scancode s) { return int(s) < count && keys[s]; };

    static const std::pair<SDL_Scancode, uint16_t> map[] =
    {
        { SDL_SCANCODE_RETURN, XAMINPUT_GAMEPAD_A }, { SDL_SCANCODE_SPACE, XAMINPUT_GAMEPAD_A },
        { SDL_SCANCODE_ESCAPE, XAMINPUT_GAMEPAD_B }, { SDL_SCANCODE_BACKSPACE, XAMINPUT_GAMEPAD_B },
        { SDL_SCANCODE_Q, XAMINPUT_GAMEPAD_X }, { SDL_SCANCODE_E, XAMINPUT_GAMEPAD_Y },
        { SDL_SCANCODE_TAB, XAMINPUT_GAMEPAD_BACK }, { SDL_SCANCODE_P, XAMINPUT_GAMEPAD_START },
        { SDL_SCANCODE_UP, XAMINPUT_GAMEPAD_DPAD_UP }, { SDL_SCANCODE_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN },
        { SDL_SCANCODE_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT }, { SDL_SCANCODE_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT },
        { SDL_SCANCODE_1, XAMINPUT_GAMEPAD_LEFT_SHOULDER }, { SDL_SCANCODE_3, XAMINPUT_GAMEPAD_RIGHT_SHOULDER },
    };
    for (auto& [s, b] : map)
        if (down(s))
            pad.wButtons |= b;

    float x = float(down(SDL_SCANCODE_D)) - float(down(SDL_SCANCODE_A));
    float y = float(down(SDL_SCANCODE_W)) - float(down(SDL_SCANCODE_S));
    if (x != 0) pad.sThumbLX = ToAxis(x);
    if (y != 0) pad.sThumbLY = ToAxis(y);
    if (down(SDL_SCANCODE_Z)) pad.bLeftTrigger = 255;
    if (down(SDL_SCANCODE_C)) pad.bRightTrigger = 255;
}

void InputScript::Apply(XAMINPUT_GAMEPAD& pad)
{
    ApplyKeyboard(pad);

    if (!g_active)
        return;

    const double now = Now();
    std::lock_guard lock(g_mutex);
    for (auto& e : g_events)
    {
        if (now < e.time || now >= e.time + e.duration)
            continue;

        switch (e.type)
        {
        case EventType::Button:
            pad.wButtons |= e.buttons;
            break;
        case EventType::Stick:
            if (e.side == 0) { pad.sThumbLX = ToAxis(e.x); pad.sThumbLY = ToAxis(e.y); }
            else { pad.sThumbRX = ToAxis(e.x); pad.sThumbRY = ToAxis(e.y); }
            break;
        case EventType::Trigger:
            (e.side == 0 ? pad.bLeftTrigger : pad.bRightTrigger) = uint8_t(std::clamp(e.x, 0.0f, 1.0f) * 255.0f);
            break;
        case EventType::Goto:
        {
            float pos[3];
            if (!::CameraPlayerPosition(pos))
                break;
            GotoState& g = g_goto;
            if (g.owner != &e) { g.owner = &e; g.lastT = -1; g.stuck = 0; g.wiggle = 0; } // (the camera offset learned so far is kept)
            const float dx = e.x - pos[0], dy = e.y - pos[1];
            if (dx * dx + dy * dy < 0.36f)
                break; // there
            if (g.lastT < 0) { g.lastT = now; g.lastX = pos[0]; g.lastY = pos[1]; }
            else if (now - g.lastT >= 0.2)
            {
                const float mx = pos[0] - g.lastX, my = pos[1] - g.lastY;
                // Not moving (against a wall or a gate): try to slide along it, a little more to one side each time.
                if (mx * mx + my * my < 0.0025f) { g.stuck++; if (g.stuck >= 3) { const int k = (g.stuck - 3) / 2 + 1; g.wiggle = ((g.stuck & 1) ? 0.6f : -0.6f) * float(std::min(k, 2)); } }
                else { g.stuck = 0; g.wiggle = 0; }
                if (mx * mx + my * my > 0.04f && g.wiggle == 0) // moved: where did this stick direction take him?
                {
                    const float measured = std::atan2(my, mx) - g.stickAngle;
                    if (!g.haveOffset) { g.offset = measured; g.haveOffset = true; }
                    else
                    {
                        float d = measured - g.offset;
                        while (d > 3.14159f) d -= 6.28318f;
                        while (d < -3.14159f) d += 6.28318f;
                        g.offset += 0.5f * d;
                    }
                }
                g.lastT = now; g.lastX = pos[0]; g.lastY = pos[1];
            }
            if (g.haveOffset)
                g.stickAngle = std::atan2(dy, dx) - g.offset + g.wiggle;
            pad.sThumbLX = ToAxis(std::cos(g.stickAngle));
            pad.sThumbLY = ToAxis(std::sin(g.stickAngle));
            break;
        }
        default:
            break;
        }
    }
}

bool InputScript::Update()
{
    if (!g_active)
        return true;

    const double now = Now();
    std::lock_guard lock(g_mutex);
    for (auto& e : g_events)
    {
        if (e.fired || now < e.time)
            continue;

        if (e.type == EventType::Button || e.type == EventType::Stick || e.type == EventType::Trigger || e.type == EventType::Goto)
        {
            e.fired = true;
            LOGFN("[script] {:.1f}s input event (buttons 0x{:04X}, stick {:.2f},{:.2f})", now, e.buttons, e.x, e.y);
        }
        else if (e.type == EventType::Shot)
        {
            e.fired = true;
            LOGFN("[script] {:.1f}s screenshot {} ({} frames so far)", now, (const char*)e.file.u8string().c_str(), Video::GetPresentedFrameCount());
            Video::RequestScreenshot(e.file);
        }
        else if (e.type == EventType::Quit)
        {
            e.fired = true;
            LOGFN("[script] {:.1f}s quit", now);
            return false;
        }
    }
    return true;
}
