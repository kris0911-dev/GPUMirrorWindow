#pragma once

#include "protocol.h"

#include <cstdint>

struct ChromeInfo {
    int width = 0;
    int height = 0;
    int hover = -1;
    bool listed[kMaxRenderers] = {};
    bool selected[kMaxRenderers] = {};
    char label[kMaxRenderers][16] = {};
};

// BGRA, top-down, tightly packed (pitch = width * 4).
void paintChrome(uint8_t* bgra, int pitch, const ChromeInfo& info);

// Client coordinates of the main window. Returns the renderer slot, or -1.
int hitTestTab(int x, int y, const ChromeInfo& info);
