#include <stdafx.h>
#include <deque>
#include <sstream>
#include "memory.h"
#include <os/logger.h>
#include <user/config.h>
#include "xex_loader.h"
#include "achievements.h"

// Step 38f (the tester: "if its possible to add achievements that would be great"): the game's Xbox 360 achievements.
// - The list (names, texts, gamerscore, icons) comes from the XDBF data in the player's own default.xex at every start
//   and is written to <user>/achievements/list.json + icons/<id>.png (for the Android start screen's list).
// - The game unlocks through XUserWriteAchievements = XMsgStartIORequest(app 0xFB, message 0xB0008) with
//   { count, pointer to { user index, achievement id }[count] } (kernel/xam.cpp calls Achievements::Write).
// - Unlocks are kept in <user>/achievements/unlocked.txt (one "id<TAB>unix time" line each). Game saves are separate and
//   never touched. Reset = that file deleted (Android Settings > Reset achievements).
// - A new unlock is queued for the on-screen notice (Android: GameActivity polls nativePollAchievement; PC: the window
//   title shows it for a few seconds).
namespace
{
    struct Entry { uint32_t id = 0, gamerscore = 0, flags = 0, imageId = 0; std::string name, description, locked; bool hasIcon = false; };
    std::mutex g_mutex;
    std::vector<Entry> g_list;
    std::map<uint32_t, int64_t> g_unlocked;
    std::deque<std::string> g_notices;
    std::filesystem::path g_dir;

    std::string JsonEscape(const std::string& s)
    {
        std::string o;
        for (unsigned char c : s)
        {
            if (c == '"' || c == '\\') { o += '\\'; o += char(c); }
            else if (c == '\n') o += "\\n";
            else if (c < 0x20) o += fmt::format("\\u{:04x}", c);
            else o += char(c);
        }
        return o;
    }

    void SaveUnlocked()
    {
        std::error_code ec;
        std::filesystem::create_directories(g_dir, ec);
        const auto tmp = g_dir / "unlocked.txt.new";
        {
            std::ofstream f(tmp, std::ios::trunc);
            for (auto& [id, t] : g_unlocked)
                f << id << '\t' << t << '\n';
        }
        std::filesystem::rename(tmp, g_dir / "unlocked.txt", ec);
    }
}

static void Unlock(uint32_t id);

void Achievements::Init(const std::filesystem::path& userDir)
{
    std::lock_guard lock(g_mutex);
    g_dir = userDir / "achievements";
    std::error_code ec;
    std::filesystem::create_directories(g_dir / "icons", ec);

    // Step 38f: settings reset_achievements = true (the PC's way; Android: Settings > Reset achievements deletes the
    // file itself): all unlocks cleared, the setting back to false. Game saves are not touched.
    if (Config::ResetAchievements)
    {
        std::filesystem::remove(g_dir / "unlocked.txt", ec);
        Config::ResetAchievements = false;
        const bool ok = Config::SetSettingLine("reset_achievements", "false");
        LOGFN("[achievements] reset (reset_achievements = true in the settings){}", ok ? "" : " - could not set it back to false");
    }

    // what was unlocked before (cleared first: on Android a second game start can run in the same process, after a
    // reset in Settings)
    g_unlocked.clear();
    g_notices.clear();
    if (std::ifstream f(g_dir / "unlocked.txt"); f)
    {
        uint32_t id; int64_t t;
        while (f >> id >> t)
            g_unlocked[id] = t;
    }

    const uint8_t* x = nullptr;
    uint32_t size = 0;
    if (!XexGetXdbf(&x, &size))
    {
        LOGN_WARNING("[achievements] no XDBF data in the game - no list");
        return;
    }
    auto u16 = [&](uint32_t o) { return uint32_t(x[o]) << 8 | x[o + 1]; };
    auto u32 = [&](uint32_t o) { return uint32_t(x[o]) << 24 | uint32_t(x[o + 1]) << 16 | uint32_t(x[o + 2]) << 8 | x[o + 3]; };
    const uint32_t entryTable = u32(8), entries = u32(12), freeTable = u32(16);
    const uint32_t dataStart = 24 + entryTable * 18 + freeTable * 8;
    auto blob = [&](uint32_t ns, uint64_t id, uint32_t* len) -> const uint8_t* {
        for (uint32_t k = 0; k < entries && 24 + (k + 1) * 18 <= size; k++)
        {
            const uint32_t e = 24 + k * 18;
            const uint64_t eid = uint64_t(u32(e + 2)) << 32 | u32(e + 6);
            const uint32_t off = u32(e + 10), l = u32(e + 14);
            if (u16(e) == ns && eid == id && dataStart + off + l <= size) { *len = l; return x + dataStart + off; }
        }
        return nullptr;
    };
    // English strings (namespace 3, locale 1)
    std::map<uint32_t, std::string> strings;
    uint32_t len = 0;
    if (const uint8_t* st = blob(3, 1, &len); st && len >= 14)
    {
        const uint32_t count = uint32_t(st[12]) << 8 | st[13];
        uint32_t p = 14;
        for (uint32_t i = 0; i < count && p + 4 <= len; i++)
        {
            const uint32_t sid = uint32_t(st[p]) << 8 | st[p + 1], sl = uint32_t(st[p + 2]) << 8 | st[p + 3];
            p += 4;
            if (p + sl > len) break;
            strings[sid] = std::string(reinterpret_cast<const char*>(st + p), sl);
            p += sl;
        }
    }
    const uint8_t* ach = blob(1, 0x58414348, &len); // "XACH"
    if (ach == nullptr || len < 14)
    {
        LOGN_WARNING("[achievements] no achievement table in the game's XDBF data");
        return;
    }
    const uint32_t count = uint32_t(ach[12]) << 8 | ach[13];
    g_list.clear();
    for (uint32_t i = 0; i < count && 14 + (i + 1) * 0x24 <= len; i++)
    {
        const uint8_t* e = ach + 14 + i * 0x24;
        auto r16 = [&](uint32_t o) { return uint32_t(e[o]) << 8 | e[o + 1]; };
        auto r32 = [&](uint32_t o) { return uint32_t(e[o]) << 24 | uint32_t(e[o + 1]) << 16 | uint32_t(e[o + 2]) << 8 | e[o + 3]; };
        Entry a;
        a.id = r16(0);
        a.name = strings[r16(2)];
        a.description = strings[r16(4)];
        a.locked = strings[r16(6)];
        const uint32_t image = r32(8);
        a.gamerscore = r16(12);
        a.flags = r32(16);
        a.imageId = image;
        uint32_t pl = 0;
        if (const uint8_t* png = blob(2, image, &pl); png && pl > 8)
        {
            const auto icon = g_dir / "icons" / fmt::format("{}.png", a.id);
            if (!std::filesystem::exists(icon, ec) || std::filesystem::file_size(icon, ec) != pl)
                std::ofstream(icon, std::ios::binary | std::ios::trunc).write(reinterpret_cast<const char*>(png), pl);
            a.hasIcon = true;
        }
        g_list.push_back(std::move(a));
    }
    std::ofstream f(g_dir / "list.json", std::ios::trunc);
    f << "[\n";
    for (size_t i = 0; i < g_list.size(); i++)
    {
        const auto& a = g_list[i];
        f << fmt::format("  {{\"id\": {}, \"name\": \"{}\", \"description\": \"{}\", \"locked\": \"{}\", \"gamerscore\": {}, \"secret\": {}, \"icon\": {}}}{}\n",
            a.id, JsonEscape(a.name), JsonEscape(a.description), JsonEscape(a.locked), a.gamerscore, (a.flags & 8) ? "false" : "true",
            a.hasIcon ? fmt::format("\"icons/{}.png\"", a.id) : std::string("null"), i + 1 < g_list.size() ? "," : "");
    }
    f << "]\n";
    LOGFN("[achievements] {} in the game, {} unlocked", g_list.size(), g_unlocked.size());

    // tests: FS_TEST_UNLOCK=<id>[,<id>...]@<seconds> unlocks those achievements that many seconds after the start,
    // as if the game had (to see the notice, the list and the file without playing to one)
    if (const char* t = getenv("FS_TEST_UNLOCK"); t && *t)
    {
        std::string v = t;
        const size_t at = v.find('@');
        const double secs = at == std::string::npos ? 30.0 : atof(v.c_str() + at + 1);
        std::vector<uint32_t> ids;
        std::stringstream list(v.substr(0, at));
        for (std::string id; std::getline(list, id, ','); )
            if (!id.empty()) ids.push_back(uint32_t(strtoul(id.c_str(), nullptr, 10)));
        std::thread([ids, secs] {
            std::this_thread::sleep_for(std::chrono::milliseconds(int64_t(secs * 1000)));
            for (uint32_t id : ids) { Unlock(id); std::this_thread::sleep_for(std::chrono::milliseconds(500)); }
        }).detach();
    }
}

static void Unlock(uint32_t id)
{
    {
        std::lock_guard lock(g_mutex);
        if (g_unlocked.count(id))
            return;
        g_unlocked[id] = int64_t(std::time(nullptr));
        SaveUnlocked();
        const Entry* a = nullptr;
        for (auto& e : g_list) if (e.id == id) a = &e;
        LOGFN("[achievements] UNLOCKED {}: {} ({} G)", id, a ? a->name : std::string("?"), a ? a->gamerscore : 0);
        g_notices.push_back(fmt::format("{}\t{}\t{}\t{}", id, a ? a->name : fmt::format("Achievement {}", id), a ? a->gamerscore : 0,
            a && a->hasIcon ? (const char*)(g_dir / "icons" / fmt::format("{}.png", id)).u8string().c_str() : ""));
    }
}

void Achievements::Write(uint32_t count, uint32_t pointer)
{
    for (uint32_t i = 0; i < count && i < 64; i++)
        Unlock(uint32_t(*reinterpret_cast<be<uint32_t>*>(g_memory.Translate(pointer + i * 8 + 4))));
}

std::vector<Achievements::Detail> Achievements::Details()
{
    std::lock_guard lock(g_mutex);
    std::vector<Detail> out;
    for (const auto& a : g_list)
    {
        const auto u = g_unlocked.find(a.id);
        out.push_back({ a.id, a.imageId, a.gamerscore, a.flags, a.name, a.description, a.locked, u == g_unlocked.end() ? 0 : u->second });
    }
    return out;
}

bool Achievements::PollNotice(std::string& out)
{
    std::lock_guard lock(g_mutex);
    if (g_notices.empty())
        return false;
    out = g_notices.front();
    g_notices.pop_front();
    return true;
}

#ifdef __ANDROID__
#include <jni.h>
// "id \t name \t gamerscore \t icon path" of the next new unlock, or null (GameActivity polls it)
extern "C" JNIEXPORT jstring JNICALL Java_com_fsrecomp_forgottensands_GameActivity_nativePollAchievement(JNIEnv* env, jclass)
{
    std::string s;
    return Achievements::PollNotice(s) ? env->NewStringUTF(s.c_str()) : nullptr;
}
#endif
