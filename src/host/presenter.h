#pragma once

#include "protocol.h"

#include <cstdint>

struct HostStatus {
    int hover = -1;
    int windowWidth = 0;
    int windowHeight = 0;
    bool listed[kMaxRenderers] = {};
    bool selected[kMaxRenderers] = {};
    bool alive[kMaxRenderers] = {};
    uint32_t pid[kMaxRenderers] = {};
    int fps[kMaxRenderers] = {};
    int frameW[kMaxRenderers] = {};
    int frameH[kMaxRenderers] = {};
    char label[kMaxRenderers][16] = {};
};

// Draws the main window. The page pixels are copies of the renderers' shared
// GPU surfaces. This class does not draw the page with GDI or GDI+.
class Presenter {
public:
    Presenter();
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;

    bool init(void* nativeWindow);
    // On macOS, tokenOut receives the IOSurface id the renderer looks up.
    // width and height are the renderer window's client size.
    bool createSurface(int tab, uint32_t ownerPid, uint32_t generation, int width, int height, uint64_t* tokenOut);
    void releaseSurface(int tab);
    void pumpSurfaces();
    void render(const HostStatus& status);
    // macOS: blit the IOSurface, then call ack from the GPU completion handler.
    // Windows uses a keyed mutex inside pumpSurfaces, so this is unused there.
    void onRendererFrame(int tab, uint64_t frame, void (*ack)(int tab, uint64_t frame, void* user), void* user);
    void shutdown();

private:
    struct Impl;
    Impl* impl_;
};
