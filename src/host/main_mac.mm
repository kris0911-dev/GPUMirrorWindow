#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>

#include "presenter.h"

#include "chrome.h"
#include "ipc.h"
#include "log.h"
#include "mirror_layout.h"
#include "protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mach-o/dyld.h>

extern char** environ;

namespace {

struct TabProcess {
    IpcChannel ipc;
    pid_t pid = 0;
    uint32_t reportedPid = 0;
    uint64_t frame = 0;
    uint64_t fpsBase = 0;
    double fpsStamp = 0;
    int fps = 0;
    int frameW = 0;
    int frameH = 0;
    uint32_t generation = 0;
    bool alive = false;
    bool listed = false;
    bool selected = false;
    bool surfaceOpen = false;
    bool connected = false;
    char label[16] = {};
};

struct App {
    NSWindow* window = nil;
    NSView* view = nil;
    Presenter presenter;
    IpcServer server;
    TabProcess tabs[kMaxRenderers];
    int hover = -1;
    int pointerSlot = -1;
    bool running = true;
    bool stopping = false;
    uint32_t parentPid = 0;
    id timer = nil;
};

App* g_app = nullptr;

double nowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void sendAck(int tab, uint64_t frame, void* user) {
    App* app = static_cast<App*>(user);
    if (!app || tab < 0 || tab >= kMaxRenderers) {
        return;
    }
    Message message{};
    message.type = kAck;
    message.tab = (uint32_t)tab;
    message.frame = frame;
    app->tabs[tab].ipc.send(message);
}

void stopRenderers(App* app) {
    if (!app || app->stopping) {
        return;
    }
    app->stopping = true;
    if (app->timer) {
        [(NSTimer*)app->timer invalidate];
        app->timer = nil;
    }
    app->server.close();
    for (TabProcess& tab : app->tabs) {
        tab.ipc.close();
        tab.alive = false;
        tab.listed = false;
        tab.connected = false;
    }
    app->presenter.shutdown();
}

void labelFor(App* app, int slot, uint32_t scene) {
    const char* base = scene == 0 ? "Video" : scene == 1 ? "Image" : scene == 2 ? "Controls" : "Renderer";
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
    logf("disconnected %s", tab.label[0] ? tab.label : "renderer");
    tab.connected = false;
    tab.listed = false;
    tab.selected = false;
    tab.alive = false;
    tab.surfaceOpen = false;
    tab.generation = 0;
    tab.frameW = 0;
    tab.frameH = 0;
    tab.reportedPid = 0;
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
                tab.reportedPid = message.pid;
            }
            if (!tab.listed) {
                labelFor(app, i, message.tab);
                tab.listed = true;
                tab.alive = true;
                logf("listed %s pid %u", tab.label, tab.reportedPid);
            }
            const bool changed = !tab.surfaceOpen || message.generation != tab.generation || message.width != tab.frameW ||
                                 message.height != tab.frameH;
            if (changed && message.width > 0 && message.height > 0) {
                uint64_t token = 0;
                if (app->presenter.createSurface(i, tab.reportedPid, message.generation, message.width, message.height, &token)) {
                    tab.frameW = message.width;
                    tab.frameH = message.height;
                    tab.generation = message.generation;
                    tab.surfaceOpen = true;
                    Message grant{};
                    grant.type = kHello;
                    grant.tab = (uint32_t)i;
                    grant.width = message.width;
                    grant.height = message.height;
                    grant.generation = message.generation;
                    grant.token = token;
                    tab.ipc.send(grant);
                }
            }
            if (message.type == kFrame && tab.surfaceOpen) {
                tab.frame = message.frame;
                app->presenter.onRendererFrame(i, message.frame, sendAck, app);
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

void tick(App* app) {
    if (!app->running || app->stopping) {
        return;
    }
    pumpIpc(app);
    CGFloat scale = app->window.backingScaleFactor;
    NSRect bounds = app->view.bounds;
    NSPoint mouse = [app->view convertPoint:app->window.mouseLocationOutsideOfEventStream fromView:nil];
    if (!NSPointInRect(mouse, bounds)) {
        app->hover = -1;
    } else {
        ChromeInfo chrome{};
        chrome.hover = app->hover;
        for (int i = 0; i < kMaxRenderers; ++i) {
            chrome.listed[i] = app->tabs[i].listed;
            chrome.selected[i] = app->tabs[i].selected;
            std::memcpy(chrome.label[i], app->tabs[i].label, sizeof(chrome.label[i]));
        }
        app->hover = hitTestTab((int)llround(mouse.x * scale), (int)llround(mouse.y * scale), chrome);
    }
    HostStatus status;
    status.hover = app->hover;
    status.windowWidth = (int)llround(bounds.size.width * scale);
    status.windowHeight = (int)llround(bounds.size.height * scale);
    for (int i = 0; i < kMaxRenderers; ++i) {
        status.listed[i] = app->tabs[i].listed;
        status.selected[i] = app->tabs[i].selected;
        status.alive[i] = app->tabs[i].alive;
        status.pid[i] = app->tabs[i].reportedPid;
        status.fps[i] = app->tabs[i].fps;
        status.frameW[i] = app->tabs[i].frameW;
        status.frameH[i] = app->tabs[i].frameH;
        std::memcpy(status.label[i], app->tabs[i].label, sizeof(status.label[i]));
    }
    app->presenter.render(status);
}

}  // namespace

@interface MirrorView : NSView
@end

@implementation MirrorView
- (BOOL)isFlipped {
    return YES;
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (CALayer*)makeBackingLayer {
    return [CAMetalLayer layer];
}
- (void)sendPointer:(NSEvent*)event action:(uint32_t)action {
    if (!g_app) {
        return;
    }
    NSPoint point = [self convertPoint:event.locationInWindow fromView:nil];
    CGFloat scale = self.window.backingScaleFactor;
    int px = (int)llround(point.x * scale);
    int py = (int)llround(point.y * scale);
    ChromeInfo chrome{};
    for (int i = 0; i < kMaxRenderers; ++i) {
        chrome.listed[i] = g_app->tabs[i].listed;
        chrome.selected[i] = g_app->tabs[i].selected;
        std::memcpy(chrome.label[i], g_app->tabs[i].label, sizeof(chrome.label[i]));
    }
    int tab = hitTestTab(px, py, chrome);
    if (action == kPointerDown && tab >= 0) {
        g_app->tabs[tab].selected = !g_app->tabs[tab].selected;
        return;
    }
    int windowW = (int)llround(self.bounds.size.width * scale);
    int windowH = (int)llround(self.bounds.size.height * scale);
    bool selected[kMaxRenderers];
    bool alive[kMaxRenderers];
    int frameW[kMaxRenderers];
    int frameH[kMaxRenderers];
    for (int i = 0; i < kMaxRenderers; ++i) {
        selected[i] = g_app->tabs[i].selected && g_app->tabs[i].listed;
        alive[i] = g_app->tabs[i].alive && g_app->tabs[i].listed;
        frameW[i] = g_app->tabs[i].frameW;
        frameH[i] = g_app->tabs[i].frameH;
    }
    MirrorCell cells[kMaxRenderers];
    const int count = layoutMirrors(selected, alive, frameW, frameH, kMaxRenderers, windowW, windowH, cells, kMaxRenderers);
    int slot = -1;
    int sx = -1;
    int sy = -1;
    for (int i = 0; i < count; ++i) {
        if (cells[i].contains((float)px, (float)py) && cells[i].w > 1.f && cells[i].h > 1.f) {
            slot = cells[i].slot;
            sx = (int)(((float)px - cells[i].x) / cells[i].w * (float)g_app->tabs[slot].frameW);
            sy = (int)(((float)py - cells[i].y) / cells[i].h * (float)g_app->tabs[slot].frameH);
            break;
        }
    }
    if (action == kPointerDown) {
        g_app->pointerSlot = slot;
    } else if (slot != g_app->pointerSlot) {
        slot = g_app->pointerSlot;
        sx = -1;
        sy = -1;
        g_app->pointerSlot = -1;
    } else {
        g_app->pointerSlot = -1;
    }
    if (slot < 0 || !g_app->tabs[slot].alive) {
        return;
    }
    Message message{};
    message.type = kPointer;
    message.tab = (uint32_t)slot;
    message.width = sx;
    message.height = sy;
    message.generation = action;
    g_app->tabs[slot].ipc.send(message);
}
- (void)mouseDown:(NSEvent*)event {
    [self sendPointer:event action:kPointerDown];
}
- (void)mouseUp:(NSEvent*)event {
    [self sendPointer:event action:kPointerUp];
}
- (void)keyDown:(NSEvent*)event {
    if (!g_app || event.charactersIgnoringModifiers.length == 0) {
        return;
    }
    unichar character = [event.charactersIgnoringModifiers characterAtIndex:0];
    if (character >= '1' && character <= '3') {
        int want = (int)character - '1';
        int seen = 0;
        for (int i = 0; i < kMaxRenderers; ++i) {
            if (!g_app->tabs[i].listed) {
                continue;
            }
            if (seen == want) {
                g_app->tabs[i].selected = !g_app->tabs[i].selected;
                break;
            }
            ++seen;
        }
    }
}
@end

@interface AppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@implementation AppDelegate
- (void)applicationDidFinishLaunching:(NSNotification*)notification {
    (void)notification;
    g_app = new App();
    g_app->parentPid = (uint32_t)getpid();

    NSRect visible = [NSScreen mainScreen].visibleFrame;
    CGFloat width = std::min(1200.0, visible.size.width - 48);
    CGFloat height = std::min(820.0, visible.size.height - 48);
    NSRect content = NSMakeRect(visible.origin.x + 16, visible.origin.y + visible.size.height - height - 16, width, height);
    g_app->window = [[NSWindow alloc] initWithContentRect:content
                                                 styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
    g_app->window.title = @"Mirror";
    g_app->window.delegate = self;
    MirrorView* view = [[MirrorView alloc] initWithFrame:NSMakeRect(0, 0, width, height)];
    view.wantsLayer = YES;
    g_app->view = view;
    g_app->window.contentView = view;
    [g_app->window makeKeyAndOrderFront:nil];
    [g_app->window makeFirstResponder:view];

    if (!g_app->presenter.init((__bridge void*)view)) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"Metal did not start";
        alert.informativeText = @"The page surface is an IOSurface. The main window samples it with Metal.";
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    if (!g_app->server.listen()) {
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = @"The host could not listen for renderers";
        [alert runModal];
        [NSApp terminate:nil];
        return;
    }
    g_app->timer = [NSTimer timerWithTimeInterval:1.0 / 60.0 repeats:YES block:^(NSTimer* timer) {
        (void)timer;
        tick(g_app);
    }];
    [[NSRunLoop mainRunLoop] addTimer:(NSTimer*)g_app->timer forMode:NSRunLoopCommonModes];
    logf("main window pid %u", g_app->parentPid);
    logf("start mirror_video, mirror_image, or mirror_controls yourself. Click one or more to mirror them.");
}
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender {
    (void)sender;
    return YES;
}
- (void)applicationWillTerminate:(NSNotification*)notification {
    (void)notification;
    stopRenderers(g_app);
    delete g_app;
    g_app = nullptr;
}
- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (g_app) {
        g_app->running = false;
    }
}
@end

int main(int argc, const char** argv) {
    (void)argc;
    (void)argv;
    std::signal(SIGPIPE, SIG_IGN);
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        AppDelegate* delegate = [AppDelegate new];
        app.delegate = delegate;
        [app activateIgnoringOtherApps:YES];
        [app run];
    }
    return 0;
}
