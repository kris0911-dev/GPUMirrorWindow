#pragma once

#include "protocol.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

// Host on the left, one renderer window per tab stacked on the right.
// Sizes are outer window rectangles in physical pixels.

inline RECT mirrorWorkArea() {
    RECT work{};
    if (!SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) || work.right <= work.left || work.bottom <= work.top) {
        work.left = 0;
        work.top = 0;
        work.right = GetSystemMetrics(SM_CXSCREEN);
        work.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    return work;
}

inline void clientToOuter(int clientW, int clientH, int* outerW, int* outerH) {
    RECT bounds{0, 0, clientW, clientH};
    AdjustWindowRect(&bounds, WS_OVERLAPPEDWINDOW, FALSE);
    *outerW = bounds.right - bounds.left;
    *outerH = bounds.bottom - bounds.top;
}

inline void hostOuterRect(int* x, int* y, int* w, int* h) {
    const RECT work = mirrorWorkArea();
    const int workW = work.right - work.left;
    const int workH = work.bottom - work.top;
    int clientW = std::min(1200, std::max(800, workW - 48));
    int clientH = std::min(820, std::max(560, workH - 48));
    int outerW = 0;
    int outerH = 0;
    clientToOuter(clientW, clientH, &outerW, &outerH);
    *x = work.left + 12;
    *y = work.top + 12;
    *w = outerW;
    *h = outerH;
}

inline void rendererOuterRect(int tab, int* x, int* y, int* w, int* h) {
    const RECT work = mirrorWorkArea();
    int hostX = 0;
    int hostY = 0;
    int hostW = 0;
    int hostH = 0;
    hostOuterRect(&hostX, &hostY, &hostW, &hostH);
    const int gap = 8;
    const int left = hostX + hostW + gap;
    const int availW = work.right - 12 - left;
    const int availH = work.bottom - work.top - 24;
    if (tab < 0) {
        tab = 0;
    }
    if (tab > 2) {
        tab = 2;
    }

    if (availW >= 300 && availH >= 360) {
        int clientW = std::min(640, availW);
        int slot = (availH - gap * 2) / 3;
        int outerW = 0;
        int outerH = 0;
        int clientH = std::max(150, slot - 48);
        const int fitH = clientW * 9 / 16;
        if (clientH > fitH) {
            clientH = fitH;
        }
        clientToOuter(clientW, clientH, &outerW, &outerH);
        if (outerH > slot) {
            outerH = slot;
        }
        *x = left;
        *y = work.top + 12 + tab * (outerH + gap);
        *w = outerW;
        *h = outerH;
        return;
    }

    int outerW = 0;
    int outerH = 0;
    clientToOuter(480, 270, &outerW, &outerH);
    *w = outerW;
    *h = outerH;
    *x = std::max(work.left + 12, work.right - outerW - 16);
    *y = work.top + 16 + tab * 40;
}
