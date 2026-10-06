#pragma once

#include <cstdint>

// Owns the renderer window and the shared surface the host mirrors.
class Producer {
public:
    Producer();
    ~Producer();

    Producer(const Producer&) = delete;
    Producer& operator=(const Producer&) = delete;

    bool open(int scene);
    // Dispatches the renderer window. Returns false when that window closes.
    bool pump();
    bool draw(int scene, float timeSeconds);
    int width() const;
    int height() const;
    uint32_t generation() const;
    // macOS: the host creates the IOSurface and sends its id after hello.
    void attachSurface(uint64_t token);
    // x and y are window pixels. action is PointerAction.
    void pointer(int x, int y, int action);
    void close();

private:
    struct Impl;
    Impl* impl_;
};
