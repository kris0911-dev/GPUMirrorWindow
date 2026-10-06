#include "ipc.h"

#include "log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

struct IpcChannel::Impl {
    HANDLE readPipe = INVALID_HANDLE_VALUE;
    HANDLE writePipe = INVALID_HANDLE_VALUE;
    HANDLE childRead = INVALID_HANDLE_VALUE;
    HANDLE childWrite = INVALID_HANDLE_VALUE;
    std::thread reader;
    std::mutex inboxMu;
    std::mutex writeMu;
    std::deque<Message> inbox;
    std::atomic<bool> open{false};
    std::atomic<bool> stop{false};
    bool socket = false;
    bool sameHandle = false;
};

struct IpcServer::Impl {
    SOCKET listen = INVALID_SOCKET;
    std::thread thread;
    std::mutex mu;
    std::deque<SOCKET> pending;
    std::atomic<bool> stop{false};
};

namespace {

bool readMessage(HANDLE pipe, Message* message, const std::atomic<bool>& stop) {
    auto* bytes = reinterpret_cast<uint8_t*>(message);
    size_t got = 0;
    while (got < sizeof(Message) && !stop.load()) {
        DWORD n = 0;
        BOOL ok = ReadFile(pipe, bytes + got, (DWORD)(sizeof(Message) - got), &n, nullptr);
        if (!ok || n == 0) {
            return false;
        }
        got += n;
    }
    return got == sizeof(Message);
}

bool readSocketMessage(SOCKET sock, Message* message, const std::atomic<bool>& stop) {
    auto* bytes = reinterpret_cast<char*>(message);
    size_t got = 0;
    while (got < sizeof(Message) && !stop.load()) {
        int n = recv(sock, bytes + got, (int)(sizeof(Message) - got), 0);
        if (n <= 0) {
            return false;
        }
        got += (size_t)n;
    }
    return got == sizeof(Message);
}

bool writeMessage(HANDLE pipe, const Message& message) {
    auto* bytes = reinterpret_cast<const uint8_t*>(&message);
    size_t sent = 0;
    while (sent < sizeof(Message)) {
        DWORD n = 0;
        if (!WriteFile(pipe, bytes + sent, (DWORD)(sizeof(Message) - sent), &n, nullptr) || n == 0) {
            return false;
        }
        sent += n;
    }
    return true;
}

bool writeSocketMessage(SOCKET sock, const Message& message) {
    auto* bytes = reinterpret_cast<const char*>(&message);
    size_t sent = 0;
    while (sent < sizeof(Message)) {
        int n = send(sock, bytes + sent, (int)(sizeof(Message) - sent), 0);
        if (n <= 0) {
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

void ensureWsa() {
    static bool started = false;
    if (started) {
        return;
    }
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) == 0) {
        started = true;
    }
}

void closeSocketHandle(HANDLE handle) {
    if (handle != INVALID_HANDLE_VALUE) {
        closesocket((SOCKET)handle);
    }
}

}  // namespace

void IpcChannel::startReader() {
    HANDLE pipe = impl_->readPipe;
    Impl* impl = impl_;
    impl_->reader = std::thread([impl, pipe] {
        while (!impl->stop.load()) {
            Message message{};
            const bool ok = impl->socket ? readSocketMessage((SOCKET)pipe, &message, impl->stop)
                                         : readMessage(pipe, &message, impl->stop);
            if (!ok) {
                impl->open = false;
                break;
            }
            std::lock_guard<std::mutex> lock(impl->inboxMu);
            impl->inbox.push_back(message);
            if (impl->inbox.size() > 32) {
                impl->inbox.pop_front();
            }
        }
    });
}

IpcChannel::IpcChannel() : impl_(new Impl) {}

IpcChannel::~IpcChannel() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool IpcChannel::createPipes(uint64_t* childRead, uint64_t* childWrite) {
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE parentRead = INVALID_HANDLE_VALUE;
    HANDLE parentWrite = INVALID_HANDLE_VALUE;
    HANDLE forChildRead = INVALID_HANDLE_VALUE;
    HANDLE forChildWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&parentRead, &forChildWrite, &inherit, 64 * 1024) ||
        !CreatePipe(&forChildRead, &parentWrite, &inherit, 64 * 1024)) {
        logf("CreatePipe failed: %lu", GetLastError());
        return false;
    }
    SetHandleInformation(parentRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentWrite, HANDLE_FLAG_INHERIT, 0);
    impl_->readPipe = parentRead;
    impl_->writePipe = parentWrite;
    impl_->childRead = forChildRead;
    impl_->childWrite = forChildWrite;
    *childRead = (uint64_t)forChildRead;
    *childWrite = (uint64_t)forChildWrite;
    impl_->stop = false;
    impl_->open = true;
    startReader();
    return true;
}

void IpcChannel::closeChildEnds() {
    if (impl_->childRead != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->childRead);
        impl_->childRead = INVALID_HANDLE_VALUE;
    }
    if (impl_->childWrite != INVALID_HANDLE_VALUE) {
        CloseHandle(impl_->childWrite);
        impl_->childWrite = INVALID_HANDLE_VALUE;
    }
}

bool IpcChannel::attach(uint64_t readHandle, uint64_t writeHandle) {
    impl_->readPipe = (HANDLE)readHandle;
    impl_->writePipe = (HANDLE)writeHandle;
    impl_->stop = false;
    impl_->open = true;
    startReader();
    return impl_->readPipe != INVALID_HANDLE_VALUE && impl_->writePipe != INVALID_HANDLE_VALUE;
}

bool IpcChannel::send(const Message& message) {
    std::lock_guard<std::mutex> lock(impl_->writeMu);
    if (!impl_->open || impl_->writePipe == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (impl_->socket) {
        if (!writeSocketMessage((SOCKET)impl_->writePipe, message)) {
            impl_->open = false;
            return false;
        }
        return true;
    }
    if (!writeMessage(impl_->writePipe, message)) {
        impl_->open = false;
        return false;
    }
    return true;
}

bool IpcChannel::poll(Message& message) {
    std::lock_guard<std::mutex> lock(impl_->inboxMu);
    if (impl_->inbox.empty()) {
        return false;
    }
    message = impl_->inbox.front();
    impl_->inbox.pop_front();
    return true;
}

bool IpcChannel::isOpen() const {
    return impl_->open.load();
}

void IpcChannel::close() {
    if (!impl_) {
        return;
    }
    impl_->stop = true;
    if (impl_->readPipe != INVALID_HANDLE_VALUE) {
        if (impl_->socket) {
            shutdown((SOCKET)impl_->readPipe, SD_BOTH);
            closeSocketHandle(impl_->readPipe);
        } else {
            CloseHandle(impl_->readPipe);
        }
        impl_->readPipe = INVALID_HANDLE_VALUE;
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    std::lock_guard<std::mutex> lock(impl_->writeMu);
    impl_->open = false;
    closeChildEnds();
    if (!impl_->sameHandle && impl_->writePipe != INVALID_HANDLE_VALUE) {
        if (impl_->socket) {
            closeSocketHandle(impl_->writePipe);
        } else {
            CloseHandle(impl_->writePipe);
        }
    }
    impl_->writePipe = INVALID_HANDLE_VALUE;
    impl_->socket = false;
    impl_->sameHandle = false;
    std::lock_guard<std::mutex> inbox(impl_->inboxMu);
    impl_->inbox.clear();
}

bool IpcChannel::adoptSocket(uint64_t socket) {
    close();
    SOCKET incoming = (SOCKET)socket;
    if (incoming == INVALID_SOCKET) {
        return false;
    }
    u_long blocking = 0;
    ioctlsocket(incoming, FIONBIO, &blocking);
    BOOL nodelay = TRUE;
    setsockopt(incoming, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof(nodelay));
    // One socket handle. recv and send run at the same time; a duplicated
    // socket handle makes the read side fail as soon as it starts.
    impl_->readPipe = (HANDLE)incoming;
    impl_->writePipe = (HANDLE)incoming;
    impl_->sameHandle = true;
    impl_->socket = true;
    impl_->stop = false;
    impl_->open = true;
    startReader();
    return true;
}

bool IpcChannel::connectTcp(const char* host, int port) {
    close();
    ensureWsa();
    SOCKET sock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (sock == INVALID_SOCKET) {
        return false;
    }
    u_long nonblocking = 1;
    ioctlsocket(sock, FIONBIO, &nonblocking);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        closesocket(sock);
        return false;
    }
    int rc = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc != 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(sock);
        return false;
    }
    fd_set writeSet;
    fd_set errorSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&errorSet);
    FD_SET(sock, &writeSet);
    FD_SET(sock, &errorSet);
    timeval wait{};
    wait.tv_sec = 0;
    wait.tv_usec = 150000;
    int selected = select(0, nullptr, &writeSet, &errorSet, &wait);
    int soerr = 0;
    int length = sizeof(soerr);
    getsockopt(sock, SOL_SOCKET, SO_ERROR, (char*)&soerr, &length);
    if (selected <= 0 || soerr != 0) {
        closesocket(sock);
        return false;
    }
    return adoptSocket((uint64_t)sock);
}

IpcServer::IpcServer() : impl_(new Impl) {}

IpcServer::~IpcServer() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool IpcServer::listen() {
    close();
    ensureWsa();
    SOCKET sock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (sock == INVALID_SOCKET) {
        logf("socket failed: %d", WSAGetLastError());
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)kHostPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(sock, 8) != 0) {
        logf("listen 127.0.0.1:%d failed: %d", kHostPort, WSAGetLastError());
        closesocket(sock);
        return false;
    }
    impl_->stop = false;
    impl_->listen = sock;
    Impl* impl = impl_;
    impl_->thread = std::thread([impl] {
        while (!impl->stop.load()) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(impl->listen, &readSet);
            timeval wait{};
            wait.tv_sec = 0;
            wait.tv_usec = 200000;
            int selected = select(0, &readSet, nullptr, nullptr, &wait);
            if (selected <= 0 || impl->stop.load()) {
                continue;
            }
            SOCKET client = accept(impl->listen, nullptr, nullptr);
            if (client == INVALID_SOCKET) {
                continue;
            }
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->pending.push_back(client);
        }
    });
    logf("listening on 127.0.0.1:%d", kHostPort);
    return true;
}

bool IpcServer::take(IpcChannel* channel) {
    if (!channel) {
        return false;
    }
    SOCKET client = INVALID_SOCKET;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->pending.empty()) {
            return false;
        }
        client = impl_->pending.front();
        impl_->pending.pop_front();
    }
    if (!channel->adoptSocket((uint64_t)client)) {
        closesocket(client);
        return false;
    }
    return true;
}

void IpcServer::close() {
    if (!impl_) {
        return;
    }
    impl_->stop = true;
    if (impl_->listen != INVALID_SOCKET) {
        closesocket(impl_->listen);
        impl_->listen = INVALID_SOCKET;
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    while (!impl_->pending.empty()) {
        closesocket(impl_->pending.front());
        impl_->pending.pop_front();
    }
}
