#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include "presenter.h"

#include "chrome.h"
#include "ipc.h"
#include "log.h"
#include "mirror_layout.h"
#include "protocol.h"
#include "win_layout.h"

#include <windows.h>
#include <windowsx.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

namespace {

struct TabProcess {
    IpcChannel ipc;
    uint32_t pid = 0;
    uint64_t frame = 0;
    uint64_t fpsBase = 0;
    double fpsStamp = 0;
    int fps = 0;
    int frameW = 0;
    int frameH = 0;
    uint32_t generation = 0;
    bool connected = false;
    bool listed = false;
    bool selected = false;
    bool alive = false;
    bool surfaceOpen = false;
    char label[16] = {};
};

struct App {
    HWND hwnd = nullptr;
    Presenter presenter;
    TabProcess tabs[kMaxRenderers];
    IpcServer server;
    int hover = -1;
    int pointerSlot = -1;
    bool sizing = false;
    bool running = true;
    bool stopping = false;
    bool tracking = false;
    uint32_t parentPid = 0;
};

double nowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

App* appFrom(HWND hwnd) {
    return reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void stopRenderers(App* app) {
    if (app->stopping) {
        return;
    }
    app->stopping = true;
    app->server.close();
    for (int i = 0; i < kMaxRenderers; ++i) {
        app->tabs[i].ipc.close();
        app->tabs[i].alive = false;
        app->tabs[i].listed = false;
        app->tabs[i].connected = false;
    }
    app->presenter.shutdown();
}

void labelFor(App* app, int slot, uint32_t scene) {
    const char* base = "Renderer";
    if (scene == 0) {
        base = "Video";
    } else if (scene == 1) {
        base = "Image";
    } else if (scene == 2) {
        base = "Controls";
    }
    int copies = 1;
    const size_t baseLen = std::strlen(base);
    for (int i = 0; i < kMaxRenderers; ++i) {
        if (i == slot || !app->tabs[i].listed) {
            continue;
        }
        if (std::strncmp(app->tabs[i].label, base, baseLen) == 0) {
            copies += 1;
        }
    }
    if (copies == 1) {
        std::snprintf(app->tabs[slot].label, sizeof(app->tabs[slot].label), "%s", base);
    } else {
        std::snprintf(app->tabs[slot].label, sizeof(app->tabs[slot].label), "%s %d", base, copies);
    }
}

void dropSlot(App* app, int index) {
    TabProcess& tab = app->tabs[index];
    const char* name = tab.label[0] ? tab.label : "renderer";
    logf("disconnected %s", name);
    tab.connected = false;
    tab.listed = false;
    tab.selected = false;
    tab.alive = false;
    tab.surfaceOpen = false;
    tab.generation = 0;
    tab.frameW = 0;
    tab.frameH = 0;
    tab.pid = 0;
    tab.label[0] = 0;
    app->presenter.releaseSurface(index);
    tab.ipc.close();
}

void acceptRenderers(App* app) {
    for (;;) {
        int slot = -1;
        for (int i = 0; i < kMaxRenderers; ++i) {
            if (!app->tabs[i].connected) {
                slot = i;
                break;
            }
        }
        if (slot < 0 || !app->server.take(&app->tabs[slot].ipc)) {
            return;
        }
        app->tabs[slot].connected = true;
        logf("renderer connected");
    }
}

void pumpIpc(App* app) {
    acceptRenderers(app);
    const double now = nowSeconds();
    for (int i = 0; i < kMaxRenderers; ++i) {
        TabProcess& tab = app->tabs[i];
        if (!tab.connected) {
            continue;
        }
        Message message{};
        while (tab.ipc.poll(message)) {
            if (message.type != kHello && message.type != kFrame) {
                continue;
            }
            if (message.pid != 0) {
                tab.pid = message.pid;
            }
            if (!tab.listed) {
                labelFor(app, i, message.tab);
                tab.listed = true;
                tab.alive = true;
                logf("listed %s pid %u", tab.label, tab.pid);
            }
            const bool changed = !tab.surfaceOpen || message.generation != tab.generation || message.width != tab.frameW ||
                                 message.height != tab.frameH;
            if (changed && message.width > 0 && message.height > 0 && tab.pid != 0) {
                if (app->presenter.createSurface(i, tab.pid, message.generation, message.width, message.height, nullptr)) {
                    if (tab.surfaceOpen) {
                        logf("resized %s to %dx%d", tab.label, message.width, message.height);
                    }
                    tab.frameW = message.width;
                    tab.frameH = message.height;
                    tab.generation = message.generation;
                    tab.surfaceOpen = true;
                }
            }
            if (message.type == kFrame) {
                tab.frame = message.frame;
            }
        }
        if (!tab.ipc.isOpen()) {
            dropSlot(app, i);
            continue;
        }
        if (tab.fpsStamp == 0.0) {
            tab.fpsStamp = now;
            tab.fpsBase = tab.frame;
        } else if (now - tab.fpsStamp >= 0.5) {
            const double dt = now - tab.fpsStamp;
            tab.fps = (int)((double)(tab.frame - tab.fpsBase) / dt + 0.5);
            tab.fpsBase = tab.frame;
            tab.fpsStamp = now;
        }
    }
}

ChromeInfo chromeInfo(const App* app) {
    ChromeInfo info;
    info.hover = app->hover;
    for (int i = 0; i < kMaxRenderers; ++i) {
        info.listed[i] = app->tabs[i].listed;
        info.selected[i] = app->tabs[i].selected;
        std::memcpy(info.label[i], app->tabs[i].label, sizeof(info.label[i]));
    }
    return info;
}

bool cellAt(App* app, int x, int y, int* slot, int* sx, int* sy) {
    RECT client{};
    GetClientRect(app->hwnd, &client);
    const int windowW = client.right - client.left;
    const int windowH = client.bottom - client.top;
    bool selected[kMaxRenderers];
    bool alive[kMaxRenderers];
    int frameW[kMaxRenderers];
    int frameH[kMaxRenderers];
    for (int i = 0; i < kMaxRenderers; ++i) {
        selected[i] = app->tabs[i].selected && app->tabs[i].listed;
        alive[i] = app->tabs[i].alive && app->tabs[i].listed;
        frameW[i] = app->tabs[i].frameW;
        frameH[i] = app->tabs[i].frameH;
    }
    MirrorCell cells[kMaxRenderers];
    const int count = layoutMirrors(selected, alive, frameW, frameH, kMaxRenderers, windowW, windowH, cells, kMaxRenderers);
    for (int i = 0; i < count; ++i) {
        if (!cells[i].contains((float)x, (float)y) || cells[i].w <= 1.f || cells[i].h <= 1.f) {
            continue;
        }
        const int hit = cells[i].slot;
        *slot = hit;
        *sx = (int)(((float)x - cells[i].x) / cells[i].w * (float)app->tabs[hit].frameW);
        *sy = (int)(((float)y - cells[i].y) / cells[i].h * (float)app->tabs[hit].frameH);
        return true;
    }
    return false;
}

void sendPointer(App* app, int slot, int sx, int sy, uint32_t action) {
    if (slot < 0 || slot >= kMaxRenderers || !app->tabs[slot].alive) {
        return;
    }
    Message message{};
    message.type = kPointer;
    message.tab = (uint32_t)slot;
    message.width = sx;
    message.height = sy;
    message.generation = action;
    app->tabs[slot].ipc.send(message);
}

void toggleSlot(App* app, int slot) {
    if (slot < 0 || slot >= kMaxRenderers || !app->tabs[slot].listed) {
        return;
    }
    app->tabs[slot].selected = !app->tabs[slot].selected;
    logf("%s %s", app->tabs[slot].selected ? "mirroring" : "released", app->tabs[slot].label);
}

void tick(App* app) {
    if (!app->running || app->stopping || !app->hwnd) {
        return;
    }
    pumpIpc(app);
    app->presenter.pumpSurfaces();
    RECT client{};
    GetClientRect(app->hwnd, &client);
    HostStatus status;
    status.hover = app->hover;
    status.windowWidth = client.right - client.left;
    status.windowHeight = client.bottom - client.top;
    for (int i = 0; i < kMaxRenderers; ++i) {
        status.listed[i] = app->tabs[i].listed;
        status.selected[i] = app->tabs[i].selected;
        status.alive[i] = app->tabs[i].alive;
        status.pid[i] = app->tabs[i].pid;
        status.fps[i] = app->tabs[i].fps;
        status.frameW[i] = app->tabs[i].frameW;
        status.frameH[i] = app->tabs[i].frameH;
        std::memcpy(status.label[i], app->tabs[i].label, sizeof(status.label[i]));
    }
    app->presenter.render(status);
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        CREATESTRUCTW* created = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created->lpCreateParams));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    App* app = appFrom(hwnd);
    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint;
        BeginPaint(hwnd, &paint);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_TIMER:
        tick(app);
        return 0;
    case WM_ENTERSIZEMOVE:
        app->sizing = true;
        SetTimer(hwnd, 1, 16, nullptr);
        return 0;
    case WM_EXITSIZEMOVE:
        app->sizing = false;
        KillTimer(hwnd, 1);
        return 0;
    case WM_MOUSEMOVE: {
        if (!app->tracking) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            app->tracking = true;
        }
        app->hover = hitTestTab(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), chromeInfo(app));
        return 0;
    }
    case WM_MOUSELEAVE:
        app->tracking = false;
        app->hover = -1;
        return 0;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        int tab = hitTestTab(x, y, chromeInfo(app));
        if (tab >= 0) {
            toggleSlot(app, tab);
            return 0;
        }
        int slot = -1;
        int sx = 0;
        int sy = 0;
        if (cellAt(app, x, y, &slot, &sx, &sy)) {
            app->pointerSlot = slot;
            SetCapture(hwnd);
            sendPointer(app, slot, sx, sy, kPointerDown);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (app->pointerSlot < 0) {
            return 0;
        }
        const int slot = app->pointerSlot;
        app->pointerSlot = -1;
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        int hit = -1;
        int sx = -1;
        int sy = -1;
        if (!cellAt(app, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), &hit, &sx, &sy) || hit != slot) {
            sx = -1;
            sy = -1;
        }
        sendPointer(app, slot, sx, sy, kPointerUp);
        return 0;
    }
    case WM_KEYDOWN:
        if (wParam >= '1' && wParam <= '3') {
            int want = (int)wParam - '1';
            int seen = 0;
            for (int i = 0; i < kMaxRenderers; ++i) {
                if (!app->tabs[i].listed) {
                    continue;
                }
                if (seen == want) {
                    toggleSlot(app, i);
                    break;
                }
                ++seen;
            }
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;
    case WM_GETMINMAXINFO: {
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize.x = 720;
        info->ptMinTrackSize.y = 480;
        return 0;
    }
    case WM_DPICHANGED: {
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        SetWindowPos(hwnd, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY:
        app->running = false;
        stopRenderers(app);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    App* app = new App();
    app->parentPid = GetCurrentProcessId();

    HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    windowClass.lpszClassName = L"GPUMirrorWindow";
    if (!RegisterClassExW(&windowClass)) {
        logf("RegisterClassEx failed: %lu", GetLastError());
        delete app;
        return 1;
    }

    int windowX = 0;
    int windowY = 0;
    int windowW = 0;
    int windowH = 0;
    hostOuterRect(&windowX, &windowY, &windowW, &windowH);
    DWORD style = WS_OVERLAPPEDWINDOW;
    app->hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"Mirror", style, windowX, windowY, windowW, windowH, nullptr, nullptr,
                                instance, app);
    if (!app->hwnd) {
        logf("CreateWindowEx failed: %lu", GetLastError());
        delete app;
        return 1;
    }
    if (!app->presenter.init(app->hwnd)) {
        MessageBoxW(app->hwnd, L"Direct3D 11 did not start. The page surface is a shared GPU texture, so GDI+ cannot host it.",
                    L"GPU Mirror", MB_ICONERROR);
        DestroyWindow(app->hwnd);
        delete app;
        return 1;
    }
    if (!app->server.listen()) {
        MessageBoxW(app->hwnd, L"The host could not listen on 127.0.0.1:47631.", L"GPU Mirror", MB_ICONERROR);
        DestroyWindow(app->hwnd);
        delete app;
        return 1;
    }
    ShowWindow(app->hwnd, SW_SHOW);
    UpdateWindow(app->hwnd);
    logf("main window hwnd %p  pid %u", app->hwnd, app->parentPid);
    logf("start mirror_video, mirror_image, or mirror_controls yourself. Click one or more to mirror them.");

    while (app->running) {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                app->running = false;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (!app->running) {
            break;
        }
        if (!app->sizing) {
            tick(app);
        }
    }

    delete app;
    return 0;
}
