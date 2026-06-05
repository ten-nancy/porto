#pragma once

#include <map>
#include <memory>

#include "common.hpp"
#include "util/locks.hpp"
#include "util/path.hpp"

extern "C" {
#include <sys/epoll.h>
}

constexpr int EPOLL_EVENT_MEM = 1;

class TContainer;
class TEpollLoop;

class TEpollSource: public TNonCopyable {
public:
    int Fd;
    int Flags;
    uint32_t Events = EPOLLIN | EPOLLHUP;
    std::weak_ptr<TContainer> Container;

    TEpollSource(int fd, int flags, uint32_t events, std::weak_ptr<TContainer> container)
        : Fd(fd),
          Flags(flags),
          Events(events),
          Container(container)
    {}
    TEpollSource(int fd)
        : Fd(fd),
          Flags(0),
          Container()
    {}
    TEpollSource(const TFile &f)
        : TEpollSource(f.Fd)
    {}
    TEpollSource()
        : Fd(-1),
          Flags(0),
          Container()
    {}
};

class TEpollLoop: public TLockable, public TNonCopyable {
    TFile Fd;

    size_t MaxEvents = 0;
    struct epoll_event *Events = nullptr;

    std::vector<std::weak_ptr<TEpollSource>> Sources;

    TError ModifySourceEvents(int fd, uint32_t events) const;

    void RemoveSourceLocked(int fd);

public:
    TError Create();
    void Destroy();
    ~TEpollLoop();

    TError AddSource(std::shared_ptr<TEpollSource> source);
    void RemoveSource(int fd) {
        auto lock = ScopedLock();
        RemoveSourceLocked(fd);
    }
    void RemoveSource(const TEpollSource &source) {
        RemoveSource(source.Fd);
    }
    std::shared_ptr<TEpollSource> GetSource(int fd);

    TError StartInput(int fd) const;
    TError StopInput(int fd) const;
    TError StartOutput(int fd) const;

    TError GetEvents(std::vector<struct epoll_event> &evts, int timeout);
};
