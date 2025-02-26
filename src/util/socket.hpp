#pragma once

#include <string>

#include "common.hpp"
#include "util/error.hpp"
#include "util/unix.hpp"

extern "C" {
#include <sys/socket.h>
#include <sys/types.h>
}

// TOOD(ovov): maybe rewrite with epoll/coroutine/greenthreads
class TSocket: public TNonCopyable {
    uint64_t DeadlineMs = 0;
    TError Connect(const sockaddr *addr, size_t len);
    TError Connect(const std::string &host, const std::string &port);

    TError SetReadTimeout(int64_t timeoutMs) const;
    TError SetWriteTimeout(int64_t timeoutMs) const;
    int64_t Timeout() const {
        if (!DeadlineMs)
            return 0;
        uint64_t now = GetCurrentTimeMs();
        if (now >= DeadlineMs)
            return -1;
        return DeadlineMs - now;
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

    void SetDeadline(uint64_t deadlineMs) {
        DeadlineMs = deadlineMs;
    }
    TError ResetDeadline() {
        SetDeadline(0);
        auto error = SetReadTimeout(0);
        if (!error)
            error = SetWriteTimeout(0);
        return error;
    }

    TError Connect(const std::string &path);
    TError Connect(const std::string &host, int port);

    void Close();

    TError Read(void *buf, size_t len) const;
    TError Write(const void *buf, size_t len) const;
};
