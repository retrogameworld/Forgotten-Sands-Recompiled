#include <stdafx.h>
#include <hid/input_script.h>
#include "hid.h"
#include <user/config.h>

// From UnleashedRecomp hid/hid.cpp (GameWindow focus check replaced by a config flag).

hid::EInputDevice hid::g_inputDevice;
hid::EInputDevice hid::g_inputDeviceController;
hid::EInputDeviceExplicit hid::g_inputDeviceExplicit;

uint16_t hid::g_prohibitedButtons;
bool hid::g_isLeftStickProhibited;
bool hid::g_isRightStickProhibited;

void hid::SetProhibitedInputs(uint16_t wButtons, bool leftStick, bool rightStick)
{
    hid::g_prohibitedButtons = wButtons;
    hid::g_isLeftStickProhibited = leftStick;
    hid::g_isRightStickProhibited = rightStick;
}

bool hid::IsInputAllowed()
{
    // Scripted test input (hidden window, never focused) is always allowed.
    return Config::AllowBackgroundInput || InputScript::IsActive() || (SDL_GetKeyboardFocus() != nullptr);
}

bool hid::IsInputDeviceController()
{
    return hid::g_inputDevice != hid::EInputDevice::Keyboard &&
        hid::g_inputDevice != hid::EInputDevice::Mouse;
}
