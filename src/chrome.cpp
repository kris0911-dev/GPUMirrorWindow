#include "chrome.h"

#include "protocol.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

void putPixel(uint8_t* px, uint32_t argb) {
    px[0] = (uint8_t)(argb & 0xFF);
    px[1] = (uint8_t)((argb >> 8) & 0xFF);
    px[2] = (uint8_t)((argb >> 16) & 0xFF);
    px[3] = (uint8_t)((argb >> 24) & 0xFF);
}

void fillRect(uint8_t* base, int pitch, int width, int height, int x, int y, int w, int h, uint32_t argb) {
    if (w <= 0 || h <= 0 || width <= 0 || height <= 0) {
        return;
    }
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w;
    int y1 = y + h;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    if (x0 >= x1 || y0 >= y1) {
        return;
    }
    for (int yy = y0; yy < y1; ++yy) {
        uint8_t* row = base + yy * pitch + x0 * 4;
        for (int xx = x0; xx < x1; ++xx) {
            putPixel(row, argb);
            row += 4;
        }
    }
}

// 8x8 glyphs, MSB is the left pixel. Only the capitals the chrome actually draws.
const uint8_t* glyph(char c) {
    switch (c) {
    case '0': { static const uint8_t g[] = {0x3C, 0x66, 0x6E, 0x76, 0x66, 0x66, 0x3C, 0x00}; return g; }
    case '1': { static const uint8_t g[] = {0x18, 0x38, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}; return g; }
    case '2': { static const uint8_t g[] = {0x3C, 0x66, 0x06, 0x0C, 0x30, 0x60, 0x7E, 0x00}; return g; }
    case '3': { static const uint8_t g[] = {0x3C, 0x66, 0x06, 0x1C, 0x06, 0x66, 0x3C, 0x00}; return g; }
    case '4': { static const uint8_t g[] = {0x0C, 0x1C, 0x3C, 0x6C, 0x7E, 0x0C, 0x0C, 0x00}; return g; }
    case '5': { static const uint8_t g[] = {0x7E, 0x60, 0x7C, 0x06, 0x06, 0x66, 0x3C, 0x00}; return g; }
    case '6': { static const uint8_t g[] = {0x1C, 0x30, 0x60, 0x7C, 0x66, 0x66, 0x3C, 0x00}; return g; }
    case '7': { static const uint8_t g[] = {0x7E, 0x06, 0x0C, 0x18, 0x30, 0x30, 0x30, 0x00}; return g; }
    case '8': { static const uint8_t g[] = {0x3C, 0x66, 0x66, 0x3C, 0x66, 0x66, 0x3C, 0x00}; return g; }
    case '9': { static const uint8_t g[] = {0x3C, 0x66, 0x66, 0x3E, 0x06, 0x0C, 0x38, 0x00}; return g; }
    case 'A': { static const uint8_t g[] = {0x18, 0x3C, 0x66, 0x66, 0x7E, 0x66, 0x66, 0x00}; return g; }
    case 'C': { static const uint8_t g[] = {0x3C, 0x66, 0x60, 0x60, 0x60, 0x66, 0x3C, 0x00}; return g; }
    case 'D': { static const uint8_t g[] = {0x78, 0x6C, 0x66, 0x66, 0x66, 0x6C, 0x78, 0x00}; return g; }
    case 'E': { static const uint8_t g[] = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x7E, 0x00}; return g; }
    case 'F': { static const uint8_t g[] = {0x7E, 0x60, 0x60, 0x7C, 0x60, 0x60, 0x60, 0x00}; return g; }
    case 'G': { static const uint8_t g[] = {0x3C, 0x66, 0x60, 0x6E, 0x66, 0x66, 0x3C, 0x00}; return g; }
    case 'L': { static const uint8_t g[] = {0x60, 0x60, 0x60, 0x60, 0x60, 0x60, 0x7E, 0x00}; return g; }
    case 'M': { static const uint8_t g[] = {0xC6, 0xEE, 0xFE, 0xD6, 0xC6, 0xC6, 0xC6, 0x00}; return g; }
    case 'I': { static const uint8_t g[] = {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x7E, 0x00}; return g; }
    case 'N': { static const uint8_t g[] = {0x66, 0x76, 0x7E, 0x7E, 0x6E, 0x66, 0x66, 0x00}; return g; }
    case 'O': { static const uint8_t g[] = {0x3C, 0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x00}; return g; }
    case 'P': { static const uint8_t g[] = {0x7C, 0x66, 0x66, 0x7C, 0x60, 0x60, 0x60, 0x00}; return g; }
    case 'R': { static const uint8_t g[] = {0x7C, 0x66, 0x66, 0x7C, 0x6C, 0x66, 0x66, 0x00}; return g; }
    case 'S': { static const uint8_t g[] = {0x3C, 0x66, 0x60, 0x3C, 0x06, 0x66, 0x3C, 0x00}; return g; }
    case 'T': { static const uint8_t g[] = {0x7E, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x00}; return g; }
    case 'V': { static const uint8_t g[] = {0x66, 0x66, 0x66, 0x66, 0x66, 0x3C, 0x18, 0x00}; return g; }
    default: return nullptr;
    }
}

void drawText(uint8_t* base, int pitch, int width, int height, int x, int y, const char* text, int scale, uint32_t color) {
    if (scale < 1) scale = 1;
    for (const char* p = text; *p; ++p) {
        const uint8_t* rows = glyph(*p);
        if (rows) {
            for (int row = 0; row < 8; ++row) {
                for (int col = 0; col < 8; ++col) {
                    if (rows[row] & (0x80u >> col)) {
                        fillRect(base, pitch, width, height, x + col * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
        x += 8 * scale;
    }
}

int textWidth(const char* text, int scale) {
    return (int)std::strlen(text) * 8 * scale;
}

}  // namespace

int chipX(const ChromeInfo& info, int slot) {
    int shown = 0;
    for (int i = 0; i < kMaxRenderers; ++i) {
        if (!info.listed[i]) {
            continue;
        }
        if (i == slot) {
            return kTabX + shown * (kTabW + kTabGap);
        }
        ++shown;
    }
    return -1;
}

int hitTestTab(int x, int y, const ChromeInfo& info) {
    if (y < kTabY || y >= kTabY + kTabH) {
        return -1;
    }
    for (int i = 0; i < kMaxRenderers; ++i) {
        if (!info.listed[i]) {
            continue;
        }
        const int tx = chipX(info, i);
        if (x >= tx && x < tx + kTabW) {
            return i;
        }
    }
    return -1;
}

#if defined(_WIN32)
void paintChromeGdi(uint8_t* bgra, int pitch, const ChromeInfo& info) {
    BITMAPINFO header{};
    header.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    header.bmiHeader.biWidth = info.width;
    header.bmiHeader.biHeight = -info.height;
    header.bmiHeader.biPlanes = 1;
    header.bmiHeader.biBitCount = 32;
    header.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(dc, &header, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        DeleteDC(dc);
        ReleaseDC(nullptr, screen);
        return;
    }
    HGDIOBJ oldBitmap = SelectObject(dc, dib);
    static HFONT tabFont = CreateFontW(-16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    static HFONT statusFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    RECT background{0, 0, info.width, info.height};
    HBRUSH backgroundBrush = CreateSolidBrush(RGB(18, 20, 28));
    FillRect(dc, &background, backgroundBrush);
    DeleteObject(backgroundBrush);
    HBRUSH lineBrush = CreateSolidBrush(RGB(44, 49, 64));
    RECT line{0, info.height - 1, info.width, info.height};
    FillRect(dc, &line, lineBrush);
    DeleteObject(lineBrush);

    static const COLORREF kAccent[] = {RGB(255, 90, 106), RGB(106, 164, 255), RGB(61, 220, 151)};
    static const COLORREF kSelected[] = {RGB(142, 42, 56), RGB(30, 76, 136), RGB(26, 104, 72)};
    SetBkMode(dc, TRANSPARENT);
    SelectObject(dc, tabFont);
    int shown = 0;
    for (int i = 0; i < kMaxRenderers; ++i) {
        if (!info.listed[i]) {
            continue;
        }
        const int tx = kTabX + shown * (kTabW + kTabGap);
        ++shown;
        const bool on = info.selected[i];
        COLORREF fill = RGB(42, 46, 59);
        if (on) {
            fill = kSelected[i % 3];
        } else if (i == info.hover) {
            fill = RGB(58, 65, 84);
        }
        HBRUSH tabBrush = CreateSolidBrush(fill);
        RECT tab{tx, kTabY, tx + kTabW, kTabY + kTabH};
        FillRect(dc, &tab, tabBrush);
        DeleteObject(tabBrush);
        if (on) {
            HBRUSH accent = CreateSolidBrush(kAccent[i % 3]);
            RECT bar{tx, kTabY + kTabH - 3, tx + kTabW, kTabY + kTabH};
            FillRect(dc, &bar, accent);
            DeleteObject(accent);
        }
        wchar_t label[16];
        MultiByteToWideChar(CP_UTF8, 0, info.label[i], -1, label, 16);
        SetTextColor(dc, on ? RGB(242, 244, 248) : RGB(197, 202, 214));
        DrawTextW(dc, label, -1, &tab, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    const wchar_t* status = nullptr;
    if (shown == 0) {
        status = L"Waiting for renderers";
    } else {
        bool any = false;
        for (int i = 0; i < kMaxRenderers; ++i) {
            if (info.listed[i] && info.selected[i]) {
                any = true;
            }
        }
        if (!any) {
            status = L"Select to mirror";
        }
    }
    if (status) {
        SelectObject(dc, statusFont);
        SetTextColor(dc, RGB(183, 190, 204));
        SIZE measured{};
        GetTextExtentPoint32W(dc, status, (int)wcslen(status), &measured);
        const int tabsRight = kTabX + shown * (kTabW + kTabGap);
        const int sx = info.width - measured.cx - 16;
        if (sx > tabsRight) {
            RECT statusRect{sx, 0, info.width - 12, info.height};
            DrawTextW(dc, status, -1, &statusRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
    }

    auto* pixels = static_cast<uint8_t*>(bits);
    for (int y = 0; y < info.height; ++y) {
        std::memcpy(bgra + y * pitch, pixels + y * info.width * 4, (size_t)info.width * 4);
    }
    SelectObject(dc, oldBitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
#endif

void paintChrome(uint8_t* bgra, int pitch, const ChromeInfo& info) {
    if (!bgra || info.width <= 0 || info.height <= 0 || pitch < info.width * 4) {
        return;
    }
#if defined(_WIN32)
    paintChromeGdi(bgra, pitch, info);
    return;
#endif
    fillRect(bgra, pitch, info.width, info.height, 0, 0, info.width, info.height, 0xFF12141Cu);
    fillRect(bgra, pitch, info.width, info.height, 0, info.height - 1, info.width, 1, 0xFF2C3140u);

    static const uint32_t kAccent[] = {0xFFFF5A6Au, 0xFF6AA4FFu, 0xFF3DDC97u};
    static const uint32_t kSelected[] = {0xFF8E2A38u, 0xFF1E4C88u, 0xFF1A6848u};
    int shown = 0;
    bool any = false;
    for (int i = 0; i < kMaxRenderers; ++i) {
        if (!info.listed[i]) {
            continue;
        }
        if (info.selected[i]) {
            any = true;
        }
        const int tx = kTabX + shown * (kTabW + kTabGap);
        ++shown;
        uint32_t fill = 0xFF2A2E3Bu;
        if (info.selected[i]) {
            fill = kSelected[i % 3];
        } else if (i == info.hover) {
            fill = 0xFF3A4154u;
        }
        fillRect(bgra, pitch, info.width, info.height, tx, kTabY, kTabW, kTabH, fill);
        if (info.selected[i]) {
            fillRect(bgra, pitch, info.width, info.height, tx, kTabY + kTabH - 3, kTabW, 3, kAccent[i % 3]);
        }
        char upper[16];
        int n = 0;
        for (const char* p = info.label[i]; *p && n < 15; ++p) {
            char c = *p;
            if (c >= 'a' && c <= 'z') {
                c = (char)(c - 'a' + 'A');
            }
            upper[n++] = c;
        }
        upper[n] = 0;
        const int scale = 2;
        const int tw = textWidth(upper, scale);
        drawText(bgra, pitch, info.width, info.height, tx + (kTabW - tw) / 2, kTabY + (kTabH - 8 * scale) / 2 - 1, upper, scale,
                 info.selected[i] ? 0xFFF2F4F8u : 0xFFC5CAD6u);
    }
    const char* status = shown == 0 ? "STARTING" : any ? nullptr : "SELECT";
    if (status) {
        const int sw = textWidth(status, 1);
        const int sx = info.width - sw - 16;
        const int tabsRight = kTabX + shown * (kTabW + kTabGap);
        if (sx > tabsRight) {
            drawText(bgra, pitch, info.width, info.height, sx, (info.height - 8) / 2, status, 1, 0xFFB7BECCu);
        }
    }
}
