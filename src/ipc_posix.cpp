#include "ipc.h"

#include "log.h"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct IpcChannel::Impl {
    int fd = -1;
    int listenFd = -1;
    std::string path;
    std::thread reader;
    std::mutex inboxMu;
    std::mutex writeMu;
    std::deque<Message> inbox;
    std::atomic<bool> open{false};
    std::atomic<bool> stop{false};
};

namespace {

bool readExact(int fd, void* data, size_t size, const std::atomic<bool>& stop) {
    auto* bytes = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < size && !stop.load()) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (pr == 0) continue;
        ssize_t n = recv(fd, bytes + got, size - got, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        got += (size_t)n;
    }
    return got == size;
}

bool writeExact(int fd, const void* data, size_t size) {
    auto* bytes = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < size) {
        ssize_t n = send(fd, bytes + sent, size - sent, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += (size_t)n;
    }
    return true;
}

void setNoSigPipe(int fd) {
#if defined(__APPLE__)
    int value = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &value, sizeof(value));
#else
    (void)fd;
#endif
}

}  // namespace

IpcChannel::IpcChannel() : impl_(new Impl) {}

IpcChannel::~IpcChannel() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool IpcChannel::listen(const char* name) {
    ::unlink(name);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        logf("socket failed: %s", std::strerror(errno));
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", name);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        logf("bind %s failed: %s", name, std::strerror(errno));
        ::close(fd);
        return false;
    }
    if (::listen(fd, 1) < 0) {
        logf("listen failed: %s", std::strerror(errno));
        ::close(fd);
        ::unlink(name);
        return false;
    }
    impl_->listenFd = fd;
    impl_->path = name;
    return true;
}

bool IpcChannel::waitForClient(int timeoutMs) {
    if (impl_->listenFd < 0) {
        return false;
    }
    pollfd pfd{};
    pfd.fd = impl_->listenFd;
    pfd.events = POLLIN;
    int pr = ::poll(&pfd, 1, timeoutMs);
    if (pr <= 0) {
        logf("renderer did not connect");
        return false;
    }
    int fd = ::accept(impl_->listenFd, nullptr, nullptr);
    if (fd < 0) {
        logf("accept failed: %s", std::strerror(errno));
        return false;
    }
    setNoSigPipe(fd);
    ::close(impl_->listenFd);
    impl_->listenFd = -1;
    impl_->fd = fd;
    impl_->stop = false;
    impl_->open = true;
    startReader();
    return true;
}

bool IpcChannel::connectTo(const char* name, int timeoutMs) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", name);
    const int stepMs = 50;
    int waited = 0;
    while (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        if (waited >= timeoutMs) {
            logf("connect %s failed: %s", name, std::strerror(errno));
            ::close(fd);
            return false;
        }
        ::usleep(stepMs * 1000);
        waited += stepMs;
    }
    setNoSigPipe(fd);
    impl_->fd = fd;
    impl_->path = name;
    impl_->stop = false;
    impl_->open = true;
    startReader();
    return true;
}

void IpcChannel::startReader() {
    int fd = impl_->fd;
    Impl* impl = impl_;
    impl_->reader = std::thread([impl, fd] {
        while (!impl->stop.load()) {
            Message message{};
            if (!readExact(fd, &message, sizeof(message), impl->stop)) {
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

bool IpcChannel::adoptFd(int fd) {
    close();
    if (fd < 0) {
        return false;
    }
    setNoSigPipe(fd);
    impl_->fd = fd;
    impl_->stop = false;
    impl_->open = true;
    startReader();
    return true;
}

bool IpcChannel::send(const Message& message) {
    std::lock_guard<std::mutex> lock(impl_->writeMu);
    if (!impl_->open || impl_->fd < 0) {
        return false;
    }
    if (!writeExact(impl_->fd, &message, sizeof(message))) {
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
    if (impl_->fd >= 0) {
        ::shutdown(impl_->fd, SHUT_RDWR);
    }
    if (impl_->listenFd >= 0) {
        ::shutdown(impl_->listenFd, SHUT_RDWR);
    }
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    std::lock_guard<std::mutex> lock(impl_->writeMu);
    impl_->open = false;
    if (impl_->fd >= 0) {
        ::close(impl_->fd);
        impl_->fd = -1;
    }
    if (impl_->listenFd >= 0) {
        ::close(impl_->listenFd);
        impl_->listenFd = -1;
    }
    if (!impl_->path.empty()) {
        ::unlink(impl_->path.c_str());
        impl_->path.clear();
    }
    std::lock_guard<std::mutex> inbox(impl_->inboxMu);
    impl_->inbox.clear();
}

struct IpcServer::Impl {
    int listenFd = -1;
    std::string path;
    std::thread thread;
    std::mutex mu;
    std::deque<int> pending;
    std::atomic<bool> stop{false};
};

IpcServer::IpcServer() : impl_(new Impl) {}

IpcServer::~IpcServer() {
    close();
    delete impl_;
    impl_ = nullptr;
}

bool IpcServer::listen() {
    close();
    char path[128];
    hostSocketPath(path, sizeof(path));
    ::unlink(path);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        logf("socket failed: %s", std::strerror(errno));
        return false;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 || ::listen(fd, 8) < 0) {
        logf("listen %s failed: %s", path, std::strerror(errno));
        ::close(fd);
        ::unlink(path);
        return false;
    }
    impl_->listenFd = fd;
    impl_->path = path;
    impl_->stop = false;
    Impl* impl = impl_;
    impl_->thread = std::thread([impl] {
        while (!impl->stop.load()) {
            pollfd pfd{};
            pfd.fd = impl->listenFd;
            pfd.events = POLLIN;
            int pr = ::poll(&pfd, 1, 200);
            if (pr <= 0 || impl->stop.load()) {
                continue;
            }
            int client = ::accept(impl->listenFd, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            std::lock_guard<std::mutex> lock(impl->mu);
            impl->pending.push_back(client);
        }
    });
    logf("listening on %s", path);
    return true;
}

bool IpcServer::take(IpcChannel* channel) {
    if (!channel) {
        return false;
    }
    int client = -1;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->pending.empty()) {
            return false;
        }
        client = impl_->pending.front();
        impl_->pending.pop_front();
    }
    if (!channel->adoptFd(client)) {
        ::close(client);
        return false;
    }
    return true;
}

void IpcServer::close() {
    if (!impl_) {
        return;
    }
    impl_->stop = true;
    if (impl_->listenFd >= 0) {
        ::shutdown(impl_->listenFd, SHUT_RDWR);
        ::close(impl_->listenFd);
        impl_->listenFd = -1;
    }
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    while (!impl_->pending.empty()) {
        ::close(impl_->pending.front());
        impl_->pending.pop_front();
    }
    if (!impl_->path.empty()) {
        ::unlink(impl_->path.c_str());
        impl_->path.clear();
    }
}
