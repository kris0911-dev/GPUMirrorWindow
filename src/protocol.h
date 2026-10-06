#pragma once

#include <cstdint>
#include <cstdio>
#include <cwchar>

// Fixed-size messages on a named pipe (Windows) or a unix socket (macOS).
// One write is one message. Unknown type values are ignored.

enum MessageType : uint32_t {
    kHello = 1,
    kFrame = 2,
    kShutdown = 3,
    kAck = 4,
    kPointer = 5,
};

// Carried in Message::generation. width/height are surface pixels.
enum PointerAction : uint32_t {
    kPointerMove = 0,
    kPointerDown = 1,
    kPointerUp = 2,
};

#pragma pack(push, 1)
struct Message {
    uint32_t type = 0;
    uint32_t tab = 0;
    int32_t width = 0;
    int32_t height = 0;
    uint32_t generation = 0;
    uint32_t pid = 0;
    uint64_t frame = 0;
    uint64_t token = 0;
};
#pragma pack(pop)

static_assert(sizeof(Message) == 40, "ipc message size drifted");

// Fallback size for a renderer that has not reported its window yet.
// Windows renderers publish a texture the size of their client area.
constexpr int kSurfaceWidth = 1280;
constexpr int kSurfaceHeight = 720;
constexpr int kTabCount = 3;
constexpr int kMaxRenderers = 8;
constexpr int kHostPort = 47631;

constexpr int kChromeHeight = 48;
constexpr int kTabX = 12;
constexpr int kTabY = 8;
constexpr int kTabW = 168;
constexpr int kTabH = 32;
constexpr int kTabGap = 8;

inline void sharedSurfaceName(wchar_t* out, size_t count, uint32_t rendererPid, uint32_t generation) {
    std::swprintf(out, count, L"Local\\gpumirror_%u_%u", rendererPid, generation);
}

inline void hostSocketPath(char* out, size_t count) {
    std::snprintf(out, count, "/tmp/gpumirror_host.sock");
}

inline void pipeName(wchar_t* out, size_t count, uint32_t parentPid, uint32_t tab) {
    std::swprintf(out, count, L"\\\\.\\pipe\\gpumirror_%u_%u", parentPid, tab);
}

inline void socketName(char* out, size_t count, uint32_t parentPid, uint32_t tab) {
    std::snprintf(out, count, "/tmp/gpumirror_%u_%u.sock", parentPid, tab);
}
