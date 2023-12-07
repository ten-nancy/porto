#include "util/socket.hpp"

extern "C" {
#include <unistd.h>
#include <sys/un.h>
#include <netdb.h>
}

TError TSocket::Connect(const sockaddr *addr, size_t len) {
    int64_t timeout = Timeout();
    if (timeout < 0)
        return TError::System("connect socket failed", EWOULDBLOCK);

    auto error = SetWriteTimeout(timeout);
    int ret = connect(Fd, addr, len);
    if (ret == 0)
        return OK;
    return TError::System("connect socket failed");
}

TError TSocket::Connect(const std::string &path) {
    TError error;
    struct sockaddr_un un_addr = {
        .sun_family = AF_UNIX,
        .sun_path = {},
    };

    if (sizeof(un_addr.sun_path) <= path.size())
        return TError("path to unix socket is too long");

    if ((SetFd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0)
        return TError::System("create unix socket failed");

    strncpy(un_addr.sun_path, path.c_str(), path.size());

    return Connect((const sockaddr*)&un_addr, sizeof(un_addr));
}

TError TSocket::Connect(const std::string &name, int port) {
    return Connect(name, std::to_string(port));
}

TError TSocket::Connect(const std::string &name, const std::string &port) {
    TError error = TError::System("connect socket failed", EWOULDBLOCK);
    int ret;
    struct addrinfo *ai = NULL;
    struct addrinfo hints = {};
    hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    ret = getaddrinfo(name.c_str(), port.c_str(), &hints, &ai);

    if (ret != 0)
        return TError::System("resolve name {} failed: {}", name, gai_strerror(ret));

    for (auto rp = ai; rp != NULL && Timeout() >= 0; rp = rp->ai_next) {
        SetFd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if (Fd == -1)
            continue;

        error = Connect(rp->ai_addr, rp->ai_addrlen);
        if (!error)
            break;
        Close();
    }
    freeaddrinfo(ai);

    return error;
}

void TSocket::Close() {
    if (Fd >= 0)  {
        close(Fd);
        SetFd = -1;
    }
}

TError TSocket::Read(void *buf, size_t len) const {
    char *p = (char*)buf;

    while (len > 0) {
        int64_t timeout = Timeout();
        if (timeout < 0)
            return TError::System("read from socket failed", EWOULDBLOCK);

        auto error = SetReadTimeout(timeout);
        if (error)
            return error;

        ssize_t res = read(Fd, p, len);

        if (res <= 0) {
            if (!res)
                errno = ECONNRESET;

            return TError::System("read from socket failed");
        }

        len -= res;
        p += res;
    }
    return OK;
}

TError TSocket::Write(const void *buf, size_t len) const {
    const char *p = (const char*)buf;

    while (len > 0) {
        int64_t timeout = Timeout();
        if (timeout < 0)
            return TError::System("write to socket failed", EWOULDBLOCK);

        auto error = SetWriteTimeout(timeout);
        if (error)
            return error;

        ssize_t res = write(Fd, p, len);

        if (res < 0) {
            if (errno == EAGAIN)
                continue;
            return TError::System("write to socket failed");
        }

        len -= res;
        p += res;
    }
    return OK;
}

TError TSocket::SetReadTimeout(int64_t timeout_ms) const {
    struct timeval tv;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(Fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv))
        return TError::System("setsockopt(SO_RCVTIMEO)");

    return OK;

}

TError TSocket::SetWriteTimeout(int64_t timeout_ms) const {
    struct timeval tv;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (setsockopt(Fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv))
        return TError::System("setsockopt(SO_SNDTIMEO)");

    return OK;
}
