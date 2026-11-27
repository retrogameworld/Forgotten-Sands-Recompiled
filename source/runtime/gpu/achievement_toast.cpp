// Step 38f (PC): the picture of the "Achievement unlocked" notice that video.cpp draws over the game for a few seconds
// (the Android app shows its own notice, GameActivity). Text with Windows' GDI (Segoe UI), the achievement's icon
// (user/achievements/icons/<id>.png, from the player's own default.xex) decoded with stb_image.
#ifdef _WIN32
#include <stdafx.h>
#include <windows.h>
#include "achievement_toast.h"

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include <stb_image.h> // UnleashedRecomp thirdparty/stb (CMakeLists.txt)

namespace
{
    std::vector<std::string> Split(const std::string& s)
    {
        std::vector<std::string> f;
        size_t a = 0;
        for (size_t b; (b = s.find('\t', a)) != std::string::npos; a = b + 1) f.push_back(s.substr(a, b - a));
        f.push_back(s.substr(a));
        return f;
    }

    std::wstring Wide(const std::string& s)
    {
        std::wstring w(s.size() + 1, L'\0');
        const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
        w.resize(std::max(0, n));
        return w;
    }
}

bool BuildAchievementToast(const std::string& notice, uint32_t screenHeight, std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH)
{
    const auto f = Split(notice); // id, name, gamerscore, icon path
    const float s = std::clamp(float(screenHeight) / 1080.0f, 0.5f, 3.0f);
    auto px = [&](float v) { return std::max(1, int(v * s + 0.5f)); };
    const std::wstring title = L"Achievement unlocked";
    const std::wstring line = Wide((f.size() > 2 ? f[2] + " G  -  " : std::string()) + (f.size() > 1 ? f[1] : notice));

    HDC dc = CreateCompatibleDC(nullptr);
    if (dc == nullptr) return false;
    HFONT fontSmall = CreateFontW(-px(21), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    HFONT fontBig = CreateFontW(-px(29), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");

    // size: icon + the longer text line, within limits
    SIZE a{}, b{};
    HGDIOBJ old = SelectObject(dc, fontSmall);
    GetTextExtentPoint32W(dc, title.c_str(), int(title.size()), &a);
    SelectObject(dc, fontBig);
    GetTextExtentPoint32W(dc, line.c_str(), int(line.size()), &b);
    const int icon = px(76), pad = px(12), border = px(3);
    const int W = std::clamp(int(pad + icon + px(18) + std::max(a.cx, b.cx) + px(26)), px(380), px(980));
    const int H = icon + 2 * pad;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp == nullptr || bits == nullptr)
    {
        SelectObject(dc, old); DeleteObject(fontSmall); DeleteObject(fontBig); DeleteDC(dc);
        return false;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    uint32_t* p = static_cast<uint32_t*>(bits); // 0x00RRGGBB
    const uint32_t sand = 0xD8B060, panel = 0x1C1814;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            p[y * W + x] = (x < border || y < border || x >= W - border || y >= H - border) ? sand : panel;

    // the icon (64x64 PNG), scaled with GDI's halftone filter
    if (f.size() > 3 && !f[3].empty())
    {
        std::ifstream in(std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(f[3].c_str()))), std::ios::binary);
        std::vector<uint8_t> png((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        int iw = 0, ih = 0, comp = 0;
        if (uint8_t* img = png.empty() ? nullptr : stbi_load_from_memory(png.data(), int(png.size()), &iw, &ih, &comp, 4))
        {
            std::vector<uint32_t> bgra(size_t(iw) * ih);
            for (int i = 0; i < iw * ih; i++)
            {
                const uint8_t* q = img + i * 4; // over the panel colour where the icon is transparent
                const uint32_t al = q[3];
                auto mix = [&](uint32_t c, uint32_t bg) { return (c * al + bg * (255 - al)) / 255; };
                bgra[i] = mix(q[0], 0x1C) << 16 | mix(q[1], 0x18) << 8 | mix(q[2], 0x14);
            }
            stbi_image_free(img);
            BITMAPINFO ib{};
            ib.bmiHeader.biSize = sizeof(ib.bmiHeader);
            ib.bmiHeader.biWidth = iw;
            ib.bmiHeader.biHeight = -ih;
            ib.bmiHeader.biPlanes = 1;
            ib.bmiHeader.biBitCount = 32;
            ib.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, pad, pad, icon, icon, 0, 0, iw, ih, bgra.data(), &ib, DIB_RGB_COLORS, SRCCOPY);
        }
    }

    SetBkMode(dc, TRANSPARENT);
    const int tx = pad + icon + px(18);
    RECT r1{ tx, pad + px(6), W - px(14), pad + px(6) + px(28) };
    SelectObject(dc, fontSmall);
    SetTextColor(dc, RGB(0xD8, 0xB0, 0x60));
    DrawTextW(dc, title.c_str(), int(title.size()), &r1, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    RECT r2{ tx, pad + px(36), W - px(14), H - pad };
    SelectObject(dc, fontBig);
    SetTextColor(dc, RGB(0xFF, 0xFF, 0xFF));
    DrawTextW(dc, line.c_str(), int(line.size()), &r2, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    GdiFlush();

    rgba.resize(size_t(W) * H * 4);
    for (int i = 0; i < W * H; i++)
    {
        const uint32_t c = p[i];
        rgba[i * 4 + 0] = uint8_t(c >> 16);
        rgba[i * 4 + 1] = uint8_t(c >> 8);
        rgba[i * 4 + 2] = uint8_t(c);
        rgba[i * 4 + 3] = 255;
    }
    outW = uint32_t(W);
    outH = uint32_t(H);

    SelectObject(dc, oldBmp);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteObject(fontSmall);
    DeleteObject(fontBig);
    DeleteDC(dc);
    return true;
}
#endif
