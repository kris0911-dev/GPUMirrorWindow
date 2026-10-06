#pragma once

#include "protocol.h"

#include <algorithm>

// Cells for the renderers the user has selected. Origin is the host window's
// top left. Only listed, selected, living renderers get a cell.
struct MirrorCell {
    int slot = -1;
    float x = 0;
    float y = 0;
    float w = 0;
    float h = 0;

    bool contains(float px, float py) const {
        return px >= x && py >= y && px < x + w && py < y + h;
    }
};

inline int layoutMirrors(const bool* selected, const bool* alive, const int* srcW, const int* srcH, int slotCount, int windowW,
                          int windowH, MirrorCell* out, int maxOut) {
    int slots[kMaxRenderers];
    int n = 0;
    const int limit = slotCount < kMaxRenderers ? slotCount : kMaxRenderers;
    for (int i = 0; i < limit; ++i) {
        if (selected[i] && alive[i] && srcW && srcH && srcW[i] > 0 && srcH[i] > 0) {
            slots[n++] = i;
        }
    }
    if (n == 0 || maxOut <= 0 || windowW < 1 || windowH <= kChromeHeight) {
        return 0;
    }
    if (n > maxOut) {
        n = maxOut;
    }
    int cols = 1;
    if (n == 2) {
        cols = 2;
    } else if (n == 3) {
        cols = 3;
    } else if (n >= 4) {
        cols = 2;
    }
    const int rows = (n + cols - 1) / cols;
    const float pad = 12.f;
    const float gap = 10.f;
    const float top = (float)kChromeHeight + pad;
    const float availW = (float)windowW - pad * 2.f;
    const float availH = (float)windowH - top - pad;
    if (availW < 1.f || availH < 1.f) {
        return 0;
    }
    const float cellW = (availW - gap * (float)(cols - 1)) / (float)cols;
    const float cellH = (availH - gap * (float)(rows - 1)) / (float)rows;
    for (int i = 0; i < n; ++i) {
        const int col = i % cols;
        const int row = i / cols;
        const float cx = pad + (float)col * (cellW + gap);
        const float cy = top + (float)row * (cellH + gap);
        const int sourceW = srcW[slots[i]];
        const int sourceH = srcH[slots[i]];
        float scale = std::min(cellW / (float)sourceW, cellH / (float)sourceH);
        if (scale > 1.f) {
            scale = 1.f;
        }
        const float bw = (float)sourceW * scale;
        const float bh = (float)sourceH * scale;
        out[i].slot = slots[i];
        out[i].w = bw;
        out[i].h = bh;
        out[i].x = cx + (cellW - bw) * 0.5f;
        out[i].y = cy + (cellH - bh) * 0.5f;
    }
    return n;
}
