#include <stdafx.h>
#include <deque>
#ifndef LOG_FIRST
#define LOG_FIRST(N, ...) do { static std::atomic<uint32_t> s_count; if (s_count.fetch_add(1) < (N)) LOGFN(__VA_ARGS__); } while (0)
#endif
#include "xam.h"
#include "xdm.h"
#include "function.h"
#include "import_log.h"
#include "kernel_objects.h"
#include "xstatus.h"
#include "xex_loader.h"
#include <hid/hid.h>
#include <cpu/guest_thread.h>
#include "xxHashMap.h"
#include <user/paths.h>
#include <user/config.h>
#include <os/logger.h>

// XAM (xam.xex) imports. Listener/notification, content roots, message box and input code
// are from UnleashedRecomp kernel/xam.cpp; the rest is new for Torchlight's import list.
// Content (saves) lives in <user>/save/<content name>/.

uint32_t RtlImageXexHeaderField(uint32_t headerBase, uint32_t key);

static constexpr uint64_t PLAYER_XUID = 0xE000000000BABE01ull; // offline profile
static constexpr const char* PLAYER_NAME = "Player";

// ---------------------------------------------------------------------------------------
// Overlapped helpers

static uint32_t CompleteOverlapped(XXOVERLAPPED* overlapped, uint32_t result, uint32_t length = 0)
{
    if (!overlapped)
        return result;

    overlapped->Error = result;
    overlapped->Length = length;
    overlapped->dwExtendedError = 0;

    if (overlapped->hEvent)
    {
        if (auto* ev = dynamic_cast<Event*>(ResolveWaitable(overlapped->hEvent)))
            ev->Set();
    }

    return X_ERROR_IO_PENDING;
}

static std::string WideToAscii(const be<uint16_t>* s)
{
    std::string r;
    if (!s)
        return r;
    while (s->get())
    {
        uint16_t c = (s++)->get();
        r.push_back(c < 128 ? char(c) : '?');
    }
    return r;
}

// ---------------------------------------------------------------------------------------
// Notifications (UnleashedRecomp)

struct XamListener : KernelObject
{
    uint64_t areas{};
    std::mutex mutex;
    std::vector<std::tuple<uint32_t, uint32_t>> notifications;

    XamListener();
    ~XamListener();
};

static std::mutex g_listenerMutex;
static std::unordered_set<XamListener*> g_listeners;

XamListener::XamListener()
{
    std::lock_guard lock(g_listenerMutex);
    g_listeners.insert(this);
}

XamListener::~XamListener()
{
    std::lock_guard lock(g_listenerMutex);
    g_listeners.erase(this);
}

uint32_t XamNotifyCreateListener(uint64_t qwAreas)
{
    auto* listener = CreateKernelObject<XamListener>();
    listener->areas = qwAreas;

    // A signed-in profile and a storage device are present from the start.
    listener->notifications.emplace_back(0x0000000A, 1); // XN_SYS_SIGNINCHANGED
    listener->notifications.emplace_back(0x0000000B, 0); // XN_SYS_STORAGEDEVICESCHANGED

    return GetKernelHandle(listener);
}

void XamNotifyEnqueueEvent(uint32_t dwId, uint32_t dwParam)
{
    std::lock_guard lock(g_listenerMutex);
    for (const auto& listener : g_listeners)
    {
        if (((1ull << MSG_AREA(dwId)) & listener->areas) == 0)
            continue;

        std::lock_guard l(listener->mutex);
        listener->notifications.emplace_back(dwId, dwParam);
    }
}

uint32_t XNotifyGetNext(uint32_t hNotification, uint32_t dwMsgFilter, be<uint32_t>* pdwId, be<uint32_t>* pParam)
{
    if (!IsKernelObject(hNotification))
        return false;

    auto* listener = dynamic_cast<XamListener*>(GetKernelObject(hNotification));
    if (!listener)
        return false;

    std::lock_guard lock(listener->mutex);
    for (size_t i = 0; i < listener->notifications.size(); i++)
    {
        auto [id, param] = listener->notifications[i];
        if (dwMsgFilter && id != dwMsgFilter)
            continue;

        if (pdwId) *pdwId = id;
        if (pParam) *pParam = param;
        listener->notifications.erase(listener->notifications.begin() + i);
        return true;
    }

    return false;
}

// ---------------------------------------------------------------------------------------
// Content roots (UnleashedRecomp) and content

static std::mutex g_rootMutex;
static xxHashMap<std::string> g_rootMap;

static std::string LowerRoot(std::string_view root)
{
    std::string r(root);
    for (auto& c : r) c = char(tolower(uint8_t(c)));
    while (!r.empty() && r.back() == ':') r.pop_back();
    return r;
}

std::string_view XamGetRootPath(const std::string_view& root)
{
    std::lock_guard lock(g_rootMutex);
    const auto result = g_rootMap.find(StringHash(LowerRoot(root)));
    if (result == g_rootMap.end())
        return "";
    return result->second;
}

void XamRootCreate(const std::string_view& root, const std::string_view& path)
{
    std::lock_guard lock(g_rootMutex);
    g_rootMap[StringHash(LowerRoot(root))] = std::string(path);
}

static void XamRootDelete(const std::string_view& root)
{
    std::lock_guard lock(g_rootMutex);
    g_rootMap.erase(StringHash(LowerRoot(root)));
}

static std::filesystem::path ContentPath(const XCONTENT_DATA* data)
{
    std::string name(data->szFileName, strnlen(data->szFileName, sizeof(data->szFileName)));
    if (name.empty())
        name = "unnamed";
    for (auto& c : name)
        if (c == '/' || c == '\\' || c == ':') c = '_';

    auto base = data->dwContentType == XCONTENTTYPE_SAVEDATA ? GetSavePath() : GetUserPath() / fmt::format("content_{:08X}", data->dwContentType.get());
    return base / std::filesystem::path(std::u8string_view((const char8_t*)name.c_str()));
}

uint32_t XamContentCreateEx(uint32_t dwUserIndex, const char* szRootName, const XCONTENT_DATA* pContentData,
    uint32_t dwContentFlags, be<uint32_t>* pdwDisposition, be<uint32_t>* pdwLicenseMask,
    uint32_t dwFileCacheSize, uint64_t uliContentSize, XXOVERLAPPED* pOverlapped)
{
    auto path = ContentPath(pContentData);
    std::error_code ec;
    const bool exists = std::filesystem::is_directory(path, ec);
    const uint32_t mode = dwContentFlags & 0xF;
    uint32_t result = X_ERROR_SUCCESS;
    uint32_t disposition = exists ? XCONTENT_EXISTING : XCONTENT_NEW;

    if (exists && mode == 1) // CREATE_NEW
        result = 183; // ERROR_ALREADY_EXISTS
    else if (!exists && (mode == 3 || mode == 5)) // OPEN_EXISTING / TRUNCATE_EXISTING
        result = X_ERROR_PATH_NOT_FOUND;
    else
    {
        if (!exists)
            std::filesystem::create_directories(path, ec);
        XamRootCreate(szRootName, (const char*)path.u8string().c_str());
    }

    LOGFN("XamContentCreateEx(root \"{}\", type {}, name \"{}\", mode {}) -> {} ({})", szRootName, pContentData->dwContentType.get(),
        std::string(pContentData->szFileName, strnlen(pContentData->szFileName, sizeof(pContentData->szFileName))), mode,
        result, (const char*)path.u8string().c_str());

    if (pdwDisposition)
        *pdwDisposition = disposition;
    if (pdwLicenseMask)
        *pdwLicenseMask = 0xFFFFFFFF;

    return CompleteOverlapped(pOverlapped, result);
}

uint32_t XamContentClose(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    XamRootDelete(szRootName);
    return CompleteOverlapped(pOverlapped, X_ERROR_SUCCESS);
}

uint32_t XamContentFlush(const char* szRootName, XXOVERLAPPED* pOverlapped)
{
    return CompleteOverlapped(pOverlapped, X_ERROR_SUCCESS);
}

uint32_t XamContentDelete(uint32_t userIndex, const XCONTENT_DATA* pContentData, XXOVERLAPPED* pOverlapped)
{
    LOGFN_WARNING("XamContentDelete(\"{}\") - not deleting anything (saves are kept)", std::string(pContentData->szFileName, strnlen(pContentData->szFileName, sizeof(pContentData->szFileName))));
    return CompleteOverlapped(pOverlapped, X_ERROR_SUCCESS);
}

uint32_t XamContentGetCreator(uint32_t userIndex, const XCONTENT_DATA* contentData, be<uint32_t>* isCreator, be<uint64_t>* xuid, XXOVERLAPPED* overlapped)
{
    if (isCreator)
        *isCreator = true;
    if (xuid)
        *xuid = PLAYER_XUID;
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

uint32_t XamContentGetLicenseMask(be<uint32_t>* mask, XXOVERLAPPED* overlapped)
{
    if (mask)
        *mask = 0xFFFFFFFF; // full game, not the trial
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

// ---------------------------------------------------------------------------------------
// Enumerators

struct XamEnumerator : KernelObject
{
    std::vector<uint8_t> items;
    size_t itemSize = 0;
    size_t count = 0;
    size_t position = 0;
    size_t perCall = 1;
    std::vector<uint8_t> extra; // XamCreateEnumeratorHandle private data (guest memory below)
    uint32_t extraGuest = 0;

    ~XamEnumerator() override
    {
        if (extraGuest)
            g_userHeap.Free(g_memory.Translate(extraGuest));
    }
};

uint32_t XamContentCreateEnumerator(uint32_t dwUserIndex, uint32_t DeviceID, uint32_t dwContentType,
    uint32_t dwContentFlags, uint32_t cItem, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum)
{
    auto* e = CreateKernelObject<XamEnumerator>();
    e->itemSize = sizeof(XCONTENT_DATA);
    e->perCall = std::max<uint32_t>(cItem, 1);

    std::filesystem::path base = dwContentType == XCONTENTTYPE_SAVEDATA ? GetSavePath() : GetUserPath() / fmt::format("content_{:08X}", dwContentType);
    std::error_code ec;
    for (auto& entry : std::filesystem::directory_iterator(base, ec))
    {
        if (!entry.is_directory(ec))
            continue;
        XCONTENT_DATA data{};
        data.DeviceID = 1;
        data.dwContentType = dwContentType;
        auto name = entry.path().filename().u8string();
        strncpy(data.szFileName, (const char*)name.c_str(), sizeof(data.szFileName) - 1);
        for (size_t i = 0; i < name.size() && i < XCONTENT_MAX_DISPLAYNAME - 1; i++)
            data.szDisplayName[i] = uint8_t(name[i]);
        auto* p = (uint8_t*)&data;
        e->items.insert(e->items.end(), p, p + sizeof(data));
        e->count++;
    }

    LOGFN("XamContentCreateEnumerator(type {}) -> {} items", dwContentType, e->count);

    if (pcbBuffer)
        *pcbBuffer = uint32_t(sizeof(XCONTENT_DATA) * e->perCall);
    *phEnum = GetKernelHandle(e);
    return X_ERROR_SUCCESS;
}

uint32_t XamCreateEnumeratorHandle(uint32_t userIndex, uint32_t appId, uint32_t openMessage, uint32_t closeMessage,
    uint32_t extraSize, uint32_t itemCount, uint32_t flags, be<uint32_t>* handle)
{
    auto* e = CreateKernelObject<XamEnumerator>();
    if (extraSize)
    {
        void* p = g_userHeap.Alloc(extraSize);
        memset(p, 0, extraSize);
        e->extraGuest = g_memory.MapVirtual(p);
    }
    *handle = GetKernelHandle(e);
    return X_ERROR_SUCCESS;
}

uint32_t XamGetPrivateEnumStructureFromHandle(uint32_t handle, be<uint32_t>* out)
{
    if (!IsKernelObject(handle))
        return X_STATUS_INVALID_HANDLE;
    auto* e = dynamic_cast<XamEnumerator*>(GetKernelObject(handle));
    if (!e)
        return X_STATUS_INVALID_HANDLE;
    *out = e->extraGuest;
    return X_STATUS_SUCCESS;
}

uint32_t XamEnumerate(uint32_t hEnum, uint32_t dwFlags, void* pvBuffer, uint32_t cbBuffer, be<uint32_t>* pcItemsReturned, XXOVERLAPPED* pOverlapped)
{
    if (!IsKernelObject(hEnum))
        return X_ERROR_INVALID_PARAMETER;

    auto* e = dynamic_cast<XamEnumerator*>(GetKernelObject(hEnum));
    if (!e)
        return X_ERROR_INVALID_PARAMETER;

    size_t returned = 0;
    while (returned < e->perCall && e->position < e->count && (returned + 1) * e->itemSize <= cbBuffer)
    {
        memcpy((uint8_t*)pvBuffer + returned * e->itemSize, e->items.data() + e->position * e->itemSize, e->itemSize);
        e->position++;
        returned++;
    }

    if (pcItemsReturned)
        *pcItemsReturned = uint32_t(returned);

    uint32_t result = returned ? X_ERROR_SUCCESS : X_ERROR_NO_MORE_FILES;
    return CompleteOverlapped(pOverlapped, result, uint32_t(returned));
}

// XamUserCreateAchievementEnumerator(titleId, userIndex, xuid (64-bit, r5), flags, offset, count, pcbBuffer, phEnum):
// no achievements (empty enumerator). Unimplemented, the game waited ~5 s after Start on the title screen.
PPC_FUNC(__imp__XamUserCreateAchievementEnumerator)
{
    auto* e = CreateKernelObject<XamEnumerator>();
    e->itemSize = 0x1C;
    const uint32_t count = ctx.r8.u32;
    if (auto pcb = reinterpret_cast<be<uint32_t>*>(g_memory.Translate(ctx.r9.u32)); ctx.r9.u32)
        *pcb = std::max<uint32_t>(1, count) * (0x1C + 0x200);
    if (auto ph = reinterpret_cast<be<uint32_t>*>(g_memory.Translate(ctx.r10.u32)); ctx.r10.u32)
        *ph = GetKernelHandle(e);
    LOGFN("XamUserCreateAchievementEnumerator(count {}) -> empty enumerator", count);
    ctx.r3.u64 = X_ERROR_SUCCESS;
}
uint32_t XamUserCreateStatsEnumerator(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount, void* xuids,
    uint32_t statSpecCount, void* statSpecs, be<uint32_t>* pcbBuffer, be<uint32_t>* phEnum)
{
    auto* e = CreateKernelObject<XamEnumerator>();
    e->itemSize = 1;
    if (pcbBuffer)
        *pcbBuffer = 0;
    if (phEnum)
        *phEnum = GetKernelHandle(e);
    return X_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Users / profiles

uint32_t XamUserGetSigninState(uint32_t userIndex)
{
    return userIndex == 0 ? 1 : 0; // signed in locally
}

uint32_t XamUserGetSigninInfo(uint32_t userIndex, uint32_t flags, XUSER_SIGNIN_INFO* info)
{
    if (userIndex == 0)
    {
        memset(info, 0, sizeof(*info));
        info->xuid = PLAYER_XUID;
        info->SigninState = 1;
        strcpy(info->Name, PLAYER_NAME);
        return X_ERROR_SUCCESS;
    }

    return X_ERROR_NO_SUCH_USER;
}

uint32_t XamUserGetXUID(uint32_t userIndex, uint32_t type, be<uint64_t>* xuid)
{
    if (userIndex != 0)
        return X_ERROR_NO_SUCH_USER;
    if (xuid)
        *xuid = PLAYER_XUID;
    return X_ERROR_SUCCESS;
}

uint32_t XamUserGetDeviceContext(uint32_t userIndex, uint32_t unk, be<uint32_t>* context)
{
    if (context)
        *context = 0;
    return userIndex == 0 ? X_ERROR_SUCCESS : X_ERROR_NO_SUCH_USER;
}

uint32_t XamWriteGamerTile(uint32_t a1, uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5, XXOVERLAPPED* overlapped)
{
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

// ---------------------------------------------------------------------------------------
// System UI. Nothing is drawn; each call logs and completes immediately.

uint32_t XamShowMessageBoxUI(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped)
{
    std::string buttons;
    for (uint32_t i = 0; i < cButtons; i++)
        buttons += (i ? " | " : "") + WideToAscii(pwszButtons[i].get());

    LOGFN_WARNING("XamShowMessageBoxUI: \"{}\" / \"{}\" [{}] -> button {}", WideToAscii(wszTitle), WideToAscii(wszText), buttons, dwFocusButton);

    if (pResult)
        *pResult = dwFocusButton;

    XamNotifyEnqueueEvent(9, 1);
    XamNotifyEnqueueEvent(9, 0);
    return CompleteOverlapped(pOverlapped, X_ERROR_SUCCESS);
}

uint32_t XamShowMessageBoxUIEx(uint32_t dwUserIndex, be<uint16_t>* wszTitle, be<uint16_t>* wszText, uint32_t cButtons,
    xpointer<be<uint16_t>>* pwszButtons, uint32_t dwFocusButton, uint32_t dwFlags, uint32_t unknown, be<uint32_t>* pResult, XXOVERLAPPED* pOverlapped)
{
    return XamShowMessageBoxUI(dwUserIndex, wszTitle, wszText, cButtons, pwszButtons, dwFocusButton, dwFlags, pResult, pOverlapped);
}

uint32_t XamShowDeviceSelectorUI(uint32_t userIndex, uint32_t contentType, uint32_t contentFlags, uint64_t totalRequested,
    be<uint32_t>* deviceId, XXOVERLAPPED* overlapped)
{
    LOGFN("XamShowDeviceSelectorUI -> device 1 (hard drive)");
    XamNotifyEnqueueEvent(9, 1);
    if (deviceId)
        *deviceId = 1;
    XamNotifyEnqueueEvent(9, 0);
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

uint32_t XamShowSigninUI(uint32_t paneCount, uint32_t flags)
{
    LOGFN("XamShowSigninUI -> profile \"{}\" already signed in", PLAYER_NAME);
    XamNotifyEnqueueEvent(9, 1);
    XamNotifyEnqueueEvent(9, 0);
    XamNotifyEnqueueEvent(0x0000000A, 1); // XN_SYS_SIGNINCHANGED
    return X_ERROR_SUCCESS;
}

uint32_t XamShowKeyboardUI(uint32_t userIndex, uint32_t flags, be<uint16_t>* defaultText, be<uint16_t>* title, be<uint16_t>* description,
    be<uint16_t>* buffer, uint32_t bufferLength, XXOVERLAPPED* overlapped)
{
    LOGFN_WARNING("XamShowKeyboardUI \"{}\" - keyboard not available, returning the default text", WideToAscii(title));
    if (buffer && bufferLength)
    {
        uint32_t i = 0;
        if (defaultText)
            for (; i + 1 < bufferLength && defaultText[i].get(); i++) buffer[i] = defaultText[i].get();
        buffer[i] = 0;
    }
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

static uint32_t GenericUI(const char* name)
{
    LOGFN_WARNING("{} - system UI is not available (ignored)", name);
    XamNotifyEnqueueEvent(9, 1);
    XamNotifyEnqueueEvent(9, 0);
    return X_ERROR_SUCCESS;
}

uint32_t XamShowAchievementsUI(uint32_t userIndex, uint32_t unk) { return GenericUI("XamShowAchievementsUI"); }
uint32_t XamShowMarketplaceUI(uint32_t userIndex, uint32_t type, uint64_t offerId, uint32_t contentTypes) { return GenericUI("XamShowMarketplaceUI"); }
uint32_t XamShowMessageComposeUI(uint32_t userIndex) { return GenericUI("XamShowMessageComposeUI"); }
uint32_t XamShowGamerCardUIForXUID(uint32_t userIndex, uint64_t xuid) { return GenericUI("XamShowGamerCardUIForXUID"); }

// ---------------------------------------------------------------------------------------
// Input (UnleashedRecomp, keyboard-to-pad mapping removed until there is a window)

// Forgotten Sands imports the 3-argument XamInputGetCapabilities(userIndex, flags, caps); the 4-argument
// layout UnleashedRecomp used is XamInputGetCapabilitiesEx (unk, userIndex, flags, caps).
uint32_t XamInputGetCapabilities(uint32_t userIndex, uint32_t flags, XAMINPUT_CAPABILITIES* caps)
{
    uint32_t result = hid::GetCapabilities(userIndex, caps);

    if (result == ERROR_SUCCESS)
    {
        ByteSwapInplace(caps->Flags);
        ByteSwapInplace(caps->Gamepad.wButtons);
        ByteSwapInplace(caps->Gamepad.sThumbLX);
        ByteSwapInplace(caps->Gamepad.sThumbLY);
        ByteSwapInplace(caps->Gamepad.sThumbRX);
        ByteSwapInplace(caps->Gamepad.sThumbRY);
        ByteSwapInplace(caps->Vibration.wLeftMotorSpeed);
        ByteSwapInplace(caps->Vibration.wRightMotorSpeed);
    }

    return result;
}

uint32_t XamInputGetState(uint32_t userIndex, uint32_t flags, XAMINPUT_STATE* state)
{
    memset(state, 0, sizeof(*state));

    uint32_t result = X_ERROR_DEVICE_NOT_CONNECTED;
    if (userIndex == 0 && hid::IsInputAllowed())
        result = hid::GetState(userIndex, state);

    state->Gamepad.wButtons &= ~hid::g_prohibitedButtons;
    { static std::atomic<uint32_t> s_calls, s_pressed; s_calls++; if (state->Gamepad.wButtons && s_pressed++ < 10) LOGFN("[input] XamInputGetState(user {}): buttons 0x{:04X} (call {})", userIndex, state->Gamepad.wButtons, s_calls.load()); }

    ByteSwapInplace(state->dwPacketNumber);
    ByteSwapInplace(state->Gamepad.wButtons);
    ByteSwapInplace(state->Gamepad.sThumbLX);
    ByteSwapInplace(state->Gamepad.sThumbLY);
    ByteSwapInplace(state->Gamepad.sThumbRX);
    ByteSwapInplace(state->Gamepad.sThumbRY);

    return result;
}

uint32_t XamInputSetState(uint32_t userIndex, uint32_t flags, XAMINPUT_VIBRATION* vibration)
{
    // FS_MUTE (Claude's automated test runs): no rumble on the tester's controller either.
    static const bool s_testRun = [] { const char* m = getenv("FS_MUTE"); return m && *m && *m != '0'; }();
    if (s_testRun || !hid::IsInputDeviceController() || !Config::Vibration)
        return X_ERROR_SUCCESS;

    ByteSwapInplace(vibration->wLeftMotorSpeed);
    ByteSwapInplace(vibration->wRightMotorSpeed);

    return hid::SetState(userIndex, vibration);
}

// Forgotten Sands' title screen ("Press START") reads buttons as keystrokes: report pad 0 button
// presses/releases as XINPUT_KEYSTROKE events (VirtualKey, Unicode, Flags, UserIndex, HidCode).
uint32_t XamInputGetKeystrokeEx(be<uint32_t>* userIndex, uint32_t flags, void* keystroke)
{
    struct Keystroke { be<uint16_t> virtualKey; be<uint16_t> unicode; be<uint16_t> flags; uint8_t userIndex; uint8_t hidCode; };
    static const std::pair<uint16_t, uint16_t> keys[] =
    {
        { XAMINPUT_GAMEPAD_A, 0x5800 }, { XAMINPUT_GAMEPAD_B, 0x5801 }, { XAMINPUT_GAMEPAD_X, 0x5802 }, { XAMINPUT_GAMEPAD_Y, 0x5803 },
        { XAMINPUT_GAMEPAD_RIGHT_SHOULDER, 0x5804 }, { XAMINPUT_GAMEPAD_LEFT_SHOULDER, 0x5805 },
        { XAMINPUT_GAMEPAD_DPAD_UP, 0x5810 }, { XAMINPUT_GAMEPAD_DPAD_DOWN, 0x5811 }, { XAMINPUT_GAMEPAD_DPAD_LEFT, 0x5812 },
        { XAMINPUT_GAMEPAD_DPAD_RIGHT, 0x5813 }, { XAMINPUT_GAMEPAD_START, 0x5814 }, { XAMINPUT_GAMEPAD_BACK, 0x5815 },
        { XAMINPUT_GAMEPAD_LEFT_THUMB, 0x5816 }, { XAMINPUT_GAMEPAD_RIGHT_THUMB, 0x5817 },
    };
    static std::mutex s_mutex;
    static uint16_t s_previous;
    static std::deque<std::pair<uint16_t, uint16_t>> s_queue; // (virtual key, flags)

    std::lock_guard lock(s_mutex);
    {
        static std::set<uint64_t> s_seenCalls;
        const uint64_t key = (uint64_t(flags) << 32) | (userIndex ? uint32_t(*userIndex) : 0xDEADu);
        if (s_seenCalls.insert(key).second)
            LOGFN("[input] XamInputGetKeystrokeEx caller: user {} flags 0x{:X}", userIndex ? int64_t(uint32_t(*userIndex)) : -1, flags);
    }
    if (userIndex && *userIndex != 0 && *userIndex != 0xFF)
        return X_ERROR_DEVICE_NOT_CONNECTED;

    // The game only asks for KEYBOARD keystrokes (flags 0x40000002: XINPUT_FLAG_KEYBOARD). Feeding it pad
    // buttons made its keyboard device look "used last", so the text showed PC bindings: "{CtlRStick}" had
    // no icon. A real console returns no pad keystrokes for this request. FS_PAD_KEYSTROKES=1: old behaviour.
    static const bool s_padKeystrokes = getenv("FS_PAD_KEYSTROKES") != nullptr;
    if (!(flags & 1) && !s_padKeystrokes)
        return X_ERROR_EMPTY;

    XAMINPUT_STATE state{};
    if (hid::IsInputAllowed() && hid::GetState(0, &state) == X_ERROR_SUCCESS)
    {
        const uint16_t now = state.Gamepad.wButtons & ~hid::g_prohibitedButtons;
        const uint16_t changed = now ^ s_previous;
        for (auto& [button, vk] : keys)
            if (changed & button)
                s_queue.emplace_back(vk, (now & button) ? 1 : 2); // XINPUT_KEYSTROKE_KEYDOWN / KEYUP
        s_previous = now;
    }

    if (s_queue.empty())
        return X_ERROR_EMPTY;
    LOG_FIRST(10, "[input] keystroke 0x{:04X} flags {}", s_queue.front().first, s_queue.front().second);

    auto* k = reinterpret_cast<Keystroke*>(keystroke);
    memset(k, 0, sizeof(*k));
    k->virtualKey = s_queue.front().first;
    k->flags = s_queue.front().second;
    k->userIndex = 0;
    s_queue.pop_front();
    if (userIndex)
        *userIndex = 0;
    return X_ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------------------
// Messages, loader, memory, misc

uint32_t XMsgInProcessCall(uint32_t app, uint32_t message, be<uint32_t>* param1, be<uint32_t>* param2)
{
    LOG_ONCE("XMsgInProcessCall(app 0x{:X}, msg 0x{:X}) (first call; later ones not logged)", app, message);
    if (message == 0x7001B && param1)
    {
        uint32_t* ptr = (uint32_t*)g_memory.Translate(param1[1]);
        ptr[0] = 0;
        ptr[1] = 0;
    }
    return 0;
}

uint32_t XMsgStartIORequest(uint32_t app, uint32_t message, XXOVERLAPPED* overlapped, void* buffer, uint32_t bufferSize)
{
    LOG_ONCE("XMsgStartIORequest(app 0x{:X}, msg 0x{:X}) (first call; later ones not logged)", app, message);
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

// Forgotten Sands: the Ex version (one extra flags argument) - completes the same way.
uint32_t XMsgStartIORequestEx(uint32_t app, uint32_t message, XXOVERLAPPED* overlapped, void* buffer, uint32_t bufferSize, uint32_t flags)
{
    LOG_ONCE("XMsgStartIORequestEx(app 0x{:X}, msg 0x{:X}) (first call; later ones not logged)", app, message);
    return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
}

// Forgotten Sands: profile settings (sensitivity, difficulty...). There is no Xbox profile, so every
// requested setting comes back as "not set" and the game uses its defaults.
// Result: { setting_count, settings_ptr } followed by setting_count X_USER_PROFILE_SETTING (0x28 bytes:
// from, user_index/xuid (8), setting_id, data (type + value, 16)).
uint32_t XamUserReadProfileSettings(uint32_t titleId, uint32_t userIndex, uint32_t xuidCount, be<uint64_t>* xuids,
    uint32_t settingCount, be<uint32_t>* settingIds, be<uint32_t>* bufferSize, uint8_t* buffer)
{
    // 9th argument (overlapped) is on the stack.
    auto& ctx = *GetPPCContext();
    auto* overlapped = reinterpret_cast<XXOVERLAPPED*>(g_memory.Translate(*reinterpret_cast<be<uint32_t>*>(g_memory.Translate(ctx.r1.u32 + 0x54))));
    const uint32_t needed = 8 + settingCount * 0x28;
    if (bufferSize == nullptr)
        return X_ERROR_INVALID_PARAMETER;
    if (buffer == nullptr || *bufferSize < needed)
    {
        *bufferSize = needed;
        return 0x7A; // ERROR_INSUFFICIENT_BUFFER
    }
    memset(buffer, 0, needed);
    auto header = reinterpret_cast<be<uint32_t>*>(buffer);
    header[0] = settingCount;
    header[1] = g_memory.MapVirtual(buffer + 8);
    for (uint32_t i = 0; i < settingCount; i++)
    {
        auto s = reinterpret_cast<be<uint32_t>*>(buffer + 8 + i * 0x28);
        s[0] = 0;                                  // from: none (default)
        s[1] = userIndex;
        s[3] = settingIds ? uint32_t(settingIds[i]) : 0;
    }
    LOG_ONCE("XamUserReadProfileSettings: {} settings reported as not set (game defaults)", settingCount);
    if (g_memory.MapVirtual(overlapped) != 0 && overlapped != reinterpret_cast<XXOVERLAPPED*>(g_memory.base))
        return CompleteOverlapped(overlapped, X_ERROR_SUCCESS);
    return X_ERROR_SUCCESS;
}

uint32_t XMsgCancelIORequest(XXOVERLAPPED* overlapped, uint32_t wait)
{
    return 0;
}

uint32_t XamAlloc(uint32_t flags, uint32_t size, be<uint32_t>* out)
{
    void* p = g_userHeap.Alloc(size);
    if (!p)
        return X_ERROR_FUNCTION_FAILED;
    memset(p, 0, size);
    *out = g_memory.MapVirtual(p);
    return X_ERROR_SUCCESS;
}

uint32_t XamFree(uint32_t address)
{
    if (address)
        g_userHeap.Free(g_memory.Translate(address));
    return X_ERROR_SUCCESS;
}

void XamLoaderLaunchTitle(const char* name, uint32_t flags)
{
    LOGFN("XamLoaderLaunchTitle(\"{}\") - the game wants to launch another title; exiting", name ? name : "");
    os::logger::Flush();
    std::_Exit(0);
}

void XamLoaderTerminateTitle()
{
    LOGFN("XamLoaderTerminateTitle - the game quit");
    os::logger::Flush();
    std::_Exit(0);
}

uint32_t XamGetExecutionId(be<uint32_t>* info)
{
    *info = RtlImageXexHeaderField(g_xex.headerGuest, 0x00040006); // XEX_HEADER_EXECUTION_INFO
    return X_STATUS_SUCCESS;
}

uint32_t XamGetSystemVersion() { return 0x20449700; }
uint32_t XGetAVPack() { return 6; }               // HDMI
uint32_t XGetGameRegion() { return 0x00FF; }      // NTSC-U
uint32_t XGetLanguage() { return Config::Language; }

uint32_t XamSessionCreateHandle(be<uint32_t>* handle)
{
    auto* e = CreateKernelObject<XamEnumerator>();
    *handle = GetKernelHandle(e);
    return X_STATUS_SUCCESS;
}

uint32_t XamSessionRefObjByHandle(uint32_t handle, be<uint32_t>* object)
{
    if (object)
        *object = handle;
    return X_STATUS_SUCCESS;
}

GUEST_IMPORT(XamNotifyCreateListener, XamNotifyCreateListener);
GUEST_IMPORT(XNotifyGetNext, XNotifyGetNext);
GUEST_IMPORT(XamContentCreateEx, XamContentCreateEx);
GUEST_IMPORT(XamContentClose, XamContentClose);
GUEST_IMPORT(XamContentFlush, XamContentFlush);
GUEST_IMPORT(XamContentDelete, XamContentDelete);
GUEST_IMPORT(XamContentGetCreator, XamContentGetCreator);
GUEST_IMPORT(XamContentGetLicenseMask, XamContentGetLicenseMask);
GUEST_IMPORT(XamContentCreateEnumerator, XamContentCreateEnumerator);
GUEST_IMPORT(XamCreateEnumeratorHandle, XamCreateEnumeratorHandle);
GUEST_IMPORT(XamGetPrivateEnumStructureFromHandle, XamGetPrivateEnumStructureFromHandle);
GUEST_IMPORT(XamEnumerate, XamEnumerate);
GUEST_IMPORT(XamUserCreateStatsEnumerator, XamUserCreateStatsEnumerator);
GUEST_IMPORT(XamUserGetSigninState, XamUserGetSigninState);
GUEST_IMPORT(XamUserGetSigninInfo, XamUserGetSigninInfo);
GUEST_IMPORT(XamUserGetXUID, XamUserGetXUID);
GUEST_IMPORT(XamUserGetDeviceContext, XamUserGetDeviceContext);
GUEST_IMPORT(XamWriteGamerTile, XamWriteGamerTile);
GUEST_IMPORT(XamShowMessageBoxUI, XamShowMessageBoxUI);
GUEST_IMPORT(XamShowMessageBoxUIEx, XamShowMessageBoxUIEx);
GUEST_IMPORT(XamShowDeviceSelectorUI, XamShowDeviceSelectorUI);
GUEST_IMPORT(XamShowSigninUI, XamShowSigninUI);
GUEST_IMPORT(XamShowKeyboardUI, XamShowKeyboardUI);
GUEST_IMPORT(XamShowAchievementsUI, XamShowAchievementsUI);
GUEST_IMPORT(XamShowMarketplaceUI, XamShowMarketplaceUI);
GUEST_IMPORT(XamShowMessageComposeUI, XamShowMessageComposeUI);
GUEST_IMPORT(XamShowGamerCardUIForXUID, XamShowGamerCardUIForXUID);
GUEST_IMPORT(XamInputGetCapabilities, XamInputGetCapabilities);
GUEST_IMPORT(XamInputGetState, XamInputGetState);
GUEST_IMPORT(XamInputSetState, XamInputSetState);
GUEST_IMPORT(XamInputGetKeystrokeEx, XamInputGetKeystrokeEx);
GUEST_IMPORT(XMsgInProcessCall, XMsgInProcessCall);
GUEST_IMPORT(XMsgStartIORequest, XMsgStartIORequest);
GUEST_IMPORT(XMsgStartIORequestEx, XMsgStartIORequestEx);
GUEST_IMPORT(XamUserReadProfileSettings, XamUserReadProfileSettings);
GUEST_IMPORT(XMsgCancelIORequest, XMsgCancelIORequest);
GUEST_IMPORT(XamAlloc, XamAlloc);
GUEST_IMPORT(XamFree, XamFree);
GUEST_IMPORT(XamLoaderLaunchTitle, XamLoaderLaunchTitle);
GUEST_IMPORT(XamLoaderTerminateTitle, XamLoaderTerminateTitle);
GUEST_IMPORT(XamGetExecutionId, XamGetExecutionId);
GUEST_IMPORT(XamGetSystemVersion, XamGetSystemVersion);
GUEST_IMPORT(XGetAVPack, XGetAVPack);
GUEST_IMPORT(XGetGameRegion, XGetGameRegion);
GUEST_IMPORT(XGetLanguage, XGetLanguage);
GUEST_IMPORT(XamSessionCreateHandle, XamSessionCreateHandle);
GUEST_IMPORT(XamSessionRefObjByHandle, XamSessionRefObjByHandle);

// ---------------------------------------------------------------------------------------
// Networking (xam NetDll_*): no network. Everything fails cleanly, logged once.
// The first argument of every NetDll call is the caller type.

constexpr uint32_t SOCKET_ERROR_VALUE = 0xFFFFFFFF;
constexpr uint32_t X_WSAENETDOWN = 10050;

GUEST_IMPORT_STUB(NetDll_WSAStartup, 0);
// Forgotten Sands (no network play on PC)
GUEST_IMPORT_STUB(NetDll_WSACreateEvent, 0);
GUEST_IMPORT_STUB(NetDll_WSAResetEvent, 1);
GUEST_IMPORT_STUB(XNotifyPositionUI, 0);
GUEST_IMPORT_STUB(NetDll_WSACleanup, 0);
GUEST_IMPORT_STUB(NetDll_WSAGetLastError, X_WSAENETDOWN);
GUEST_IMPORT_STUB(NetDll_socket, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_closesocket, 0);
GUEST_IMPORT_STUB(NetDll_ioctlsocket, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_setsockopt, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_bind, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_connect, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_listen, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_accept, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_select, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_recv, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_send, SOCKET_ERROR_VALUE);
GUEST_IMPORT_STUB(NetDll_inet_addr, 0xFFFFFFFF);
GUEST_IMPORT_STUB(NetDll___WSAFDIsSet, 0);
GUEST_IMPORT_STUB(NetDll_XNetStartup, 0);
GUEST_IMPORT_STUB(NetDll_XNetCleanup, 0);
GUEST_IMPORT_STUB(NetDll_XNetServerToInAddr, X_WSAENETDOWN);
GUEST_IMPORT_STUB(NetDll_XNetUnregisterInAddr, 0);
GUEST_IMPORT_STUB(NetDll_XNetConnect, X_WSAENETDOWN);
GUEST_IMPORT_STUB(NetDll_XNetGetConnectStatus, 3); // XNET_CONNECT_STATUS_LOST
GUEST_IMPORT_STUB(NetDll_XNetDnsLookup, X_WSAENETDOWN);
GUEST_IMPORT_STUB(NetDll_XNetDnsRelease, 0);

// int XNetInAddrToString(caller, IN_ADDR ina, char* buffer, int length)
PPC_FUNC(__imp__NetDll_XNetInAddrToString)
{
    static std::atomic<bool> s_logged;
    if (!s_logged.exchange(true)) ImportStub("NetDll_XNetInAddrToString", 0);
    uint32_t addr = ctx.r4.u32;
    char* buffer = ctx.r5.u32 ? (char*)g_memory.Translate(ctx.r5.u32) : nullptr;
    if (buffer && ctx.r6.u32)
        snprintf(buffer, ctx.r6.u32, "%u.%u.%u.%u", addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF);
    ctx.r3.u64 = 0;
}
