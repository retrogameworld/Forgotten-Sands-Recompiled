// From UnleashedRecomp hid/driver/sdl_hid.cpp. Removed: App/GameWindow/Sonic LED hooks.
#include <stdafx.h>
#include <SDL.h>
#include <user/config.h>
#include <hid/hid.h>
#include <hid/input_script.h>
#include <os/logger.h>
#include <gpu/video.h>

extern bool g_inputMarkerQuery; // video.cpp (FS_TEST_INPUT_MARKER)
#include <kernel/xdm.h>

#define TRANSLATE_INPUT(S, X) SDL_GameControllerGetButton(controller, S) << FirstBitLow(X)
#define VIBRATION_TIMEOUT_MS 5000

class Controller
{
public:
    SDL_GameController* controller{};
    SDL_Joystick* joystick{};
    SDL_JoystickID id{ -1 };
    XAMINPUT_GAMEPAD state{};
    XAMINPUT_VIBRATION vibration{ 0, 0 };
    int index{};

    Controller() = default;

    explicit Controller(int index) : Controller(SDL_GameControllerOpen(index))
    {
        this->index = index;
    }

    Controller(SDL_GameController* controller) : controller(controller)
    {
        if (!controller)
            return;

        joystick = SDL_GameControllerGetJoystick(controller);
        id = SDL_JoystickInstanceID(joystick);
    }

    SDL_GameControllerType GetControllerType() const
    {
        return SDL_GameControllerGetType(controller);
    }

    hid::EInputDevice GetInputDevice() const
    {
        switch (GetControllerType())
        {
            case SDL_CONTROLLER_TYPE_PS3:
            case SDL_CONTROLLER_TYPE_PS4:
            case SDL_CONTROLLER_TYPE_PS5:
                return hid::EInputDevice::PlayStation;
            case SDL_CONTROLLER_TYPE_XBOX360:
            case SDL_CONTROLLER_TYPE_XBOXONE:
                return hid::EInputDevice::Xbox;
            default:
                return hid::EInputDevice::Unknown;
        }
    }

    const char* GetControllerName() const
    {
        auto result = SDL_GameControllerName(controller);

        if (!result)
            return "Unknown Device";

        return result;
    }

    void Close()
    {
        if (!controller)
            return;

        SDL_GameControllerClose(controller);

        controller = nullptr;
        joystick = nullptr;
        id = -1;
    }

    bool CanPoll()
    {
        return controller;
    }

    void PollAxis()
    {
        if (!CanPoll())
            return;

        auto& pad = state;

        pad.sThumbLX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTX);
        pad.sThumbLY = ~SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_LEFTY);

        pad.sThumbRX = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTX);
        pad.sThumbRY = ~SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_RIGHTY);

        pad.bLeftTrigger = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) >> 7;
        pad.bRightTrigger = SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) >> 7;
    }

    void Poll()
    {
        if (!CanPoll())
            return;

        auto& pad = state;

        pad.wButtons = 0;

        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_DPAD_UP, XAMINPUT_GAMEPAD_DPAD_UP);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_DPAD_DOWN, XAMINPUT_GAMEPAD_DPAD_DOWN);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_DPAD_LEFT, XAMINPUT_GAMEPAD_DPAD_LEFT);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, XAMINPUT_GAMEPAD_DPAD_RIGHT);

        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_START, XAMINPUT_GAMEPAD_START);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_BACK, XAMINPUT_GAMEPAD_BACK);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_TOUCHPAD, XAMINPUT_GAMEPAD_BACK);

        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_LEFTSTICK, XAMINPUT_GAMEPAD_LEFT_THUMB);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_RIGHTSTICK, XAMINPUT_GAMEPAD_RIGHT_THUMB);

        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_LEFTSHOULDER, XAMINPUT_GAMEPAD_LEFT_SHOULDER);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, XAMINPUT_GAMEPAD_RIGHT_SHOULDER);

        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_A, XAMINPUT_GAMEPAD_A);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_B, XAMINPUT_GAMEPAD_B);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_X, XAMINPUT_GAMEPAD_X);
        pad.wButtons |= TRANSLATE_INPUT(SDL_CONTROLLER_BUTTON_Y, XAMINPUT_GAMEPAD_Y);
    }

    void SetVibration(const XAMINPUT_VIBRATION& vibration)
    {
        if (!CanPoll())
            return;

        // Step 37: the game sets the rumble every frame, mostly to the same values. On Android each call is a call
        // into Java and the vibrator service from the game's thread (the tester: the game lagged and the sound glitched on
        // the Odin); now only changes go out, and the same values again every 2 s (the rumble times out after 5 s).
        // FS_NO_RUMBLE_DEDUPE=1 = every call as before.
        static const bool s_noDedupe = getenv("FS_NO_RUMBLE_DEDUPE") != nullptr;
        const uint64_t now = SDL_GetTicks64();
        const bool same = vibration.wLeftMotorSpeed == this->vibration.wLeftMotorSpeed && vibration.wRightMotorSpeed == this->vibration.wRightMotorSpeed;
        this->vibration = vibration;
        rumbleStats.calls++;
        if (same && now - lastRumbleSent < 2000 && lastRumbleSent != 0 && !s_noDedupe)
            return;
        lastRumbleSent = now;
        const uint64_t t0 = SDL_GetPerformanceCounter();
        SDL_GameControllerRumble(controller, vibration.wLeftMotorSpeed, vibration.wRightMotorSpeed, VIBRATION_TIMEOUT_MS);
        rumbleStats.sent++;
        rumbleStats.ms += double(SDL_GetPerformanceCounter() - t0) * 1000.0 / double(SDL_GetPerformanceFrequency());
        if (now - rumbleStats.lastLog >= 5000)
        {
            if (rumbleStats.lastLog != 0)
                LOGFN("[hid] rumble: {} calls from the game, {} sent to the controller, {:.1f} ms spent (last 5 s)", rumbleStats.calls, rumbleStats.sent, rumbleStats.ms);
            rumbleStats = {};
            rumbleStats.lastLog = now;
        }
    }

    uint64_t lastRumbleSent = 0;
    struct { uint32_t calls = 0, sent = 0; double ms = 0.0; uint64_t lastLog = 0; } rumbleStats;

    void SetLED(const uint8_t r, const uint8_t g, const uint8_t b) const
    {
        SDL_GameControllerSetLED(controller, r, g, b);
    }
};

#ifdef __ANDROID__
#include <jni.h>
// Step 37 (the tester: "add support for touch screen controls"): the Android app's on-screen pad (TouchPadView) sends its
// state here; it is merged into pad 0 like a second controller. Buttons are XInput bits, 0x10000 / 0x20000 = left /
// right trigger; sticks are -1..1 in screen directions (y down).
static std::atomic<uint32_t> g_touchButtons{0};
// a quick tap (finger down and up within a few ms) is still reported for at least 60 ms, so the game sees it
static std::atomic<uint64_t> g_touchPressMs[18];
static uint64_t TouchNowMs() { return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count()); }
static std::atomic<int32_t> g_touchLeft{0}, g_touchRight{0}; // x in the low 16 bits, y in the high 16 bits (XInput units)
static int32_t PackStick(float x, float y)
{
    const int16_t ix = int16_t(std::clamp(x, -1.0f, 1.0f) * 32767.0f);
    const int16_t iy = int16_t(std::clamp(-y, -1.0f, 1.0f) * 32767.0f); // XInput: up is positive
    return int32_t(uint32_t(uint16_t(ix)) | (uint32_t(uint16_t(iy)) << 16));
}
extern "C" JNIEXPORT void JNICALL Java_com_fsrecomp_forgottensands_TouchPadView_nativeTouchPad(JNIEnv*, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry)
{
    const uint32_t newly = uint32_t(buttons) & ~g_touchButtons.load();
    const uint64_t now = TouchNowMs();
    for (int i = 0; i < 18; i++) if (newly & (1u << i)) g_touchPressMs[i] = now;
    g_touchButtons = uint32_t(buttons);
    g_touchLeft = PackStick(lx, ly);
    g_touchRight = PackStick(rx, ry);
    static std::atomic<bool> s_logged{false};
    if (buttons != 0 && !s_logged.exchange(true))
        LOGN("[hid] on-screen touch pad in use");
}
#endif

std::array<Controller, 4> g_controllers;
Controller* g_activeController;

inline Controller* EnsureController(uint32_t dwUserIndex)
{
    if (!g_controllers[dwUserIndex].controller)
        return nullptr;

    return &g_controllers[dwUserIndex];
}

inline size_t FindFreeController()
{
    for (size_t i = 0; i < g_controllers.size(); i++)
    {
        if (!g_controllers[i].controller)
            return i;
    }

    return -1;
}

inline Controller* FindController(int which)
{
    for (auto& controller : g_controllers)
    {
        if (controller.id == which)
            return &controller;
    }

    return nullptr;
}

static void SetControllerInputDevice(Controller* controller)
{
    g_activeController = controller;

    hid::g_inputDevice = controller->GetInputDevice();
    hid::g_inputDeviceController = hid::g_inputDevice;

    auto controllerType = (hid::EInputDeviceExplicit)controller->GetControllerType();
    auto controllerName = controller->GetControllerName();

    // Only proceed if the controller type changes.
    if (hid::g_inputDeviceExplicit != controllerType)
    {
        hid::g_inputDeviceExplicit = controllerType;

        if (controllerType == hid::EInputDeviceExplicit::Unknown)
        {
            LOGFN("Detected controller: {} (Unknown Controller Type)", controllerName);
        }
        else
        {
            LOGFN("Detected controller: {}", controllerName);
        }
    }
}

static void SetControllerTimeOfDayLED(Controller& controller, bool isNight)
{
    auto r = isNight ? 22 : 0;
    auto g = isNight ? 0 : 37;
    auto b = isNight ? 101 : 184;

    controller.SetLED(r, g, b);
}

int HID_OnSDLEvent(void*, SDL_Event* event)
{
    switch (event->type)
    {
        case SDL_CONTROLLERDEVICEADDED:
        {
            const auto freeIndex = FindFreeController();

            if (freeIndex != -1)
            {
                auto controller = Controller(event->cdevice.which);

                g_controllers[freeIndex] = controller;

                if (!g_activeController)
                    SetControllerInputDevice(&g_controllers[freeIndex]);
            }

            break;
        }

        case SDL_CONTROLLERDEVICEREMOVED:
        {
            auto* controller = FindController(event->cdevice.which);

            if (controller)
                controller->Close();

            break;
        }

        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP:
        case SDL_CONTROLLERAXISMOTION:
        case SDL_CONTROLLERTOUCHPADDOWN:
        {
            auto* controller = FindController(event->cdevice.which);

            if (!controller)
                break;

            if (event->type == SDL_CONTROLLERAXISMOTION)
            {
                if (abs(event->caxis.value) > 8000)
                {
                    SDL_ShowCursor(SDL_DISABLE);
                    SetControllerInputDevice(controller);
                }

                controller->PollAxis();
            }
            else
            {
                SDL_ShowCursor(SDL_DISABLE);
                SetControllerInputDevice(controller);

                controller->Poll();
            }

            break;
        }

        case SDL_KEYDOWN:
        case SDL_KEYUP:
            hid::g_inputDevice = hid::EInputDevice::Keyboard;
            break;

        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        {
            SDL_ShowCursor(SDL_ENABLE);

            hid::g_inputDevice = hid::EInputDevice::Mouse;

            break;
        }

        case SDL_WINDOWEVENT:
        {
            if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            {
                // Stop vibrating controllers on focus lost.
                for (auto& controller : g_controllers)
                    controller.SetVibration({ 0, 0 });
            }

            break;
        }

    }

    return 0;
}

void hid::Init()
{
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_GAMECUBE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS3, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS4_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_PLAYER_LED, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_PS5_RUMBLE, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_WII, "1");
    SDL_SetHint(SDL_HINT_XINPUT_ENABLED, "1");
    
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0"); // Uses Button Labels. This hint is disabled for Nintendo Controllers.

    SDL_InitSubSystem(SDL_INIT_EVENTS);

    // FS_MUTE (Claude's automated test runs): never open the physical controller, so a test running in the
    // background cannot take the tester's pad away from his own game window (scripted input still works).
    // Step 37: FS_TEST_PAD=1 (Odin test runs) opens the device's own controller anyway, driven by the test with
    // sendevent; rumble stays off (FS_MUTE).
    if (const char* m = getenv("FS_MUTE"); m && *m && *m != '0' && getenv("FS_TEST_PAD") == nullptr)
    {
        LOGN("[hid] test run (FS_MUTE): physical controllers are not opened");
        return;
    }

    SDL_AddEventWatch(HID_OnSDLEvent, nullptr);

    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
}

uint32_t hid::GetState(uint32_t dwUserIndex, XAMINPUT_STATE* pState)
{
    static uint32_t packet;

    if (!pState)
        return ERROR_BAD_ARGUMENTS;

    memset(pState, 0, sizeof(*pState));

    pState->dwPacketNumber = packet++;

    // Torchlight: pad 0 is always reported as connected (physical pad + keyboard + --input-script
    // merged), so the game never shows "connect a controller" and can be driven by tests.
    if (dwUserIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    static const bool s_logTimes = getenv("FS_LOG_INPUT_TIMES") != nullptr;
    const auto pollStart = s_logTimes ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();
    if (g_activeController)
    {
        g_activeController->Poll();
        g_activeController->PollAxis();
        pState->Gamepad = g_activeController->state;
    }
    if (s_logTimes && !::g_inputMarkerQuery)
    {
        // how long reading the pad takes and how often the game asks (Step 37, controller lag)
        static std::atomic<uint64_t> s_calls{0}, s_us{0}, s_maxUs{0};
        static auto s_since = std::chrono::steady_clock::now();
        const auto now = std::chrono::steady_clock::now();
        const uint64_t us = uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(now - pollStart).count());
        s_us += us; if (us > s_maxUs) s_maxUs = us;
        if (++s_calls % 600 == 0)
        {
            const double secs = std::chrono::duration<double>(now - s_since).count();
            LOGFN("[inputtime] pad read by the game {:.0f} times/s, {:.3f} ms each on average, slowest {:.2f} ms (thread {})", 600 / secs, s_us / 600.0 / 1000.0, s_maxUs / 1000.0, (unsigned long)std::hash<std::thread::id>()(std::this_thread::get_id()) % 100000);
            s_us = 0; s_maxUs = 0; s_since = now;
        }
    }

#ifdef __ANDROID__
    {
        uint32_t tb = g_touchButtons.load();
        const uint64_t now = TouchNowMs();
        for (int i = 0; i < 18; i++) { const uint64_t t = g_touchPressMs[i].load(); if (t != 0 && now - t < 60) tb |= 1u << i; }
        const int32_t tl = g_touchLeft.load(), tr = g_touchRight.load();
        pState->Gamepad.wButtons |= uint16_t(tb & 0xFFFF);
        if (tb & 0x10000) pState->Gamepad.bLeftTrigger = 255;
        if (tb & 0x20000) pState->Gamepad.bRightTrigger = 255;
        if (tl != 0) { pState->Gamepad.sThumbLX = int16_t(tl & 0xFFFF); pState->Gamepad.sThumbLY = int16_t(uint32_t(tl) >> 16); }
        if (tr != 0) { pState->Gamepad.sThumbRX = int16_t(tr & 0xFFFF); pState->Gamepad.sThumbRY = int16_t(uint32_t(tr) >> 16); }
    }
#endif
    InputScript::Apply(pState->Gamepad);

    // Step 37 diag (the tester: "half a second of controller lag"): FS_LOG_INPUT_TIMES=1 logs the CLOCK_BOOTTIME ms at which
    // the game first sees a button change (compare with /proc/uptime at the press).
    if (static const bool s_log = getenv("FS_LOG_INPUT_TIMES") != nullptr; s_log && !::g_inputMarkerQuery)
    {
        static uint16_t s_prev = 0;
        if (pState->Gamepad.wButtons != s_prev)
        {
#ifdef __ANDROID__
            timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts);
            const double ms = ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#else
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
            LOGFN("[inputtime] buttons 0x{:04X} -> 0x{:04X} seen by the game at boottime {:.1f} ms (frame {})", s_prev, pState->Gamepad.wButtons, ms, Video::GetPresentedFrameCount());
            s_prev = pState->Gamepad.wButtons;
        }
    }
    return ERROR_SUCCESS;
}

uint32_t hid::SetState(uint32_t dwUserIndex, XAMINPUT_VIBRATION* pVibration)
{
    if (!pVibration)
        return ERROR_BAD_ARGUMENTS;

    if (dwUserIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    if (g_activeController)
        g_activeController->SetVibration(*pVibration);

    return ERROR_SUCCESS;
}

uint32_t hid::GetCapabilities(uint32_t dwUserIndex, XAMINPUT_CAPABILITIES* pCaps)
{
    if (!pCaps)
        return ERROR_BAD_ARGUMENTS;

    if (dwUserIndex != 0)
        return ERROR_DEVICE_NOT_CONNECTED;

    memset(pCaps, 0, sizeof(*pCaps));

    pCaps->Type = XAMINPUT_DEVTYPE_GAMEPAD;
    pCaps->SubType = XAMINPUT_DEVSUBTYPE_GAMEPAD; // TODO: other types?
    pCaps->Flags = 0;
    // Capabilities report which buttons/axes exist (all of them), not the current state.
    pCaps->Gamepad.wButtons = 0xF3FF;
    pCaps->Gamepad.bLeftTrigger = 0xFF;
    pCaps->Gamepad.bRightTrigger = 0xFF;
    pCaps->Gamepad.sThumbLX = pCaps->Gamepad.sThumbLY = pCaps->Gamepad.sThumbRX = pCaps->Gamepad.sThumbRY = int16_t(0xFFC0);
    pCaps->Vibration = { 0xFFFF, 0xFFFF };

    return ERROR_SUCCESS;
}
