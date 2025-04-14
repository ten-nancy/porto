#pragma once

#include <string>

#include "common.hpp"
#include "util/error.hpp"

extern "C" {
#include <sys/socket.h>
#include <sys/types.h>
}

// TOOD(ovov): maybe rewrite with epoll/coroutine/greenthreads
class TSocket: public TNonCopyable {
    uint64_t DeadlineMs = 0;
    TError Connect(const sockaddr *addr, size_t len);
    TError Connect(const std::string &host, const std::string &port);

    TError SetTimeout(int64_t timeoutMs, int type) const;
    int64_t Timeout() const;

protected:
    TError ApplyDeadline(int type) const {
        auto timeout = Timeout();
        if (timeout < 0)
            return TError::System("socket timeout", EWOULDBLOCK);
        return SetTimeout(timeout, type);
    }
    TError ApplyReadDeadline() const {
        return ApplyDeadline(SO_RCVTIMEO);
    }
    TError ApplyWriteDeadline() const {
        return ApplyDeadline(SO_SNDTIMEO);
    }

public:
    union {
        const int Fd;
        int SetFd;
    };

    TSocket()
        : SetFd(-1)
    {};
    ~TSocket() {
        Close();
    }
    TSocket(TSocket &&other)
        : SetFd(other.Fd)
    {
        other.SetFd = -1;
    }

    TSocket(int fd)
        : SetFd(fd)
    {}
    void SetDeadline(uint64_t deadlineMs) {
        DeadlineMs = deadlineMs;
    }
    TError ResetDeadline() {
        SetDeadline(0);
        auto error = SetTimeout(0, SO_RCVTIMEO);
        if (!error)
            error = SetTimeout(0, SO_SNDTIMEO);
        return error;
    }

    TError Connect(const std::string &path);
    TError Connect(const std::string &host, int port);

    void Close();

    TError Read(void *buf, size_t len) const;
    TError Write(const void *buf, size_t len) const;
};
