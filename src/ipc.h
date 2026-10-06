#pragma once

#include "protocol.h"

// One duplex channel. The host listens, the renderer connects.
// A reader thread drains the socket so a full pipe cannot stall the writer.
class IpcChannel {
public:
    IpcChannel();
    ~IpcChannel();

    IpcChannel(const IpcChannel&) = delete;
    IpcChannel& operator=(const IpcChannel&) = delete;

#if defined(_WIN32)
    // Parent ends stay in this process. The two values are inheritable handles
    // the child passes to attach(). Call closeChildEnds() after CreateProcess.
    bool createPipes(uint64_t* childRead, uint64_t* childWrite);
    void closeChildEnds();
    bool attach(uint64_t readHandle, uint64_t writeHandle);
    bool connectTcp(const char* host, int port);
    bool adoptSocket(uint64_t socket);
#else
    bool listen(const char* name);
    bool waitForClient(int timeoutMs);
    bool connectTo(const char* name, int timeoutMs);
    bool adoptFd(int fd);
#endif

    bool send(const Message& message);
    bool poll(Message& message);
    bool isOpen() const;
    void close();

private:
    struct Impl;
    void startReader();
    Impl* impl_;
};

// The host listens. Each renderer connects on its own when the user starts it.
class IpcServer {
public:
    IpcServer();
    ~IpcServer();

    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;

    bool listen();
    bool take(IpcChannel* channel);
    void close();

private:
    struct Impl;
    Impl* impl_;
};
