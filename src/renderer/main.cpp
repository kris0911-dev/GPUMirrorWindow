#include "producer.h"

#include "ipc.h"
#include "log.h"
#include "protocol.h"
#include "scene_ui.h"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

const char* argument(int argc, char** argv, const char* key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

double nowSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void sleepMs(int ms) {
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

uint32_t selfPid() {
#if defined(_WIN32)
    return GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

void usage() {
    std::fputs("Start mirror_video, mirror_image, or mirror_controls on their own.\nThe host lists a renderer after it connects.\n", stderr);
}

bool connectHost(IpcChannel* ipc) {
#if defined(_WIN32)
    return ipc->connectTcp("127.0.0.1", kHostPort);
#else
    char path[128];
    hostSocketPath(path, sizeof(path));
    return ipc->connectTo(path, 150);
#endif
}

void handleIncoming(Producer* producer, const Message& incoming) {
    if (incoming.type == kPointer) {
        producer->pointer(incoming.width, incoming.height, (int)incoming.generation);
    } else if (incoming.type == kHello && incoming.token != 0) {
        producer->attachSurface(incoming.token);
    }
}

}  // namespace

int main(int argc, char** argv) {
#if defined(MIRROR_APP_SCENE)
    int scene = MIRROR_APP_SCENE;
#else
    int scene = -1;
#endif
    if (const char* sceneText = argument(argc, argv, "--scene")) {
        scene = (int)std::strtol(sceneText, nullptr, 10);
    }
    if (scene < 0 || scene > 2) {
        usage();
        return 2;
    }

#if defined(_WIN32)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#else
    (void)argc;
#endif

    Producer producer;
    if (!producer.open(scene)) {
        return 1;
    }

    IpcChannel ipc;
    bool linked = false;
    double lastTry = -10.0;
    uint32_t announced = 0;
    uint64_t frame = 0;
    const double started = nowSeconds();
    const uint32_t pid = selfPid();
    logf("%s window is up. It connects to the host when mirror_host is running.", sceneTitle(scene));

    while (producer.pump()) {
        const double time = nowSeconds();
        if (!producer.draw(scene, (float)(time - started))) {
            sleepMs(2);
            continue;
        }
        if (!linked && time - lastTry >= 0.4) {
            lastTry = time;
            linked = connectHost(&ipc);
            if (linked) {
                announced = 0;
                logf("connected to host");
            }
        }
        if (linked && !ipc.isOpen()) {
            ipc.close();
            linked = false;
            announced = 0;
            logf("host disconnected");
        }
        if (!linked) {
            continue;
        }

        Message incoming{};
        while (ipc.poll(incoming)) {
            handleIncoming(&producer, incoming);
        }
        if (!ipc.isOpen()) {
            ipc.close();
            linked = false;
            announced = 0;
            continue;
        }
        if (producer.generation() == 0 || producer.width() < 1 || producer.height() < 1) {
            continue;
        }
        if (producer.generation() != announced) {
            Message hello{};
            hello.type = kHello;
            hello.tab = (uint32_t)scene;
            hello.width = producer.width();
            hello.height = producer.height();
            hello.generation = producer.generation();
            hello.pid = pid;
            if (!ipc.send(hello)) {
                ipc.close();
                linked = false;
                announced = 0;
                continue;
            }
            announced = producer.generation();
            logf("renderer %s pid %u  %dx%d", sceneTitle(scene), pid, hello.width, hello.height);
        }

        ++frame;
        Message notice{};
        notice.type = kFrame;
        notice.tab = (uint32_t)scene;
        notice.width = producer.width();
        notice.height = producer.height();
        notice.generation = producer.generation();
        notice.pid = pid;
        notice.frame = frame;
        if (!ipc.send(notice)) {
            ipc.close();
            linked = false;
            announced = 0;
            continue;
        }

#if defined(__APPLE__)
        const double deadline = nowSeconds() + 1.0;
        bool acked = false;
        while (nowSeconds() < deadline && ipc.isOpen()) {
            Message ack{};
            while (ipc.poll(ack)) {
                handleIncoming(&producer, ack);
                if (ack.type == kAck && ack.frame == frame) {
                    acked = true;
                }
            }
            if (acked) {
                break;
            }
            sleepMs(2);
        }
#endif
    }

    producer.close();
    ipc.close();
    return 0;
}
