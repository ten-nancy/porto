#pragma once

#include <condition_variable>
#include <thread>
#include <queue>

#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/locks.hpp"
#include "util/thread.hpp"

template<typename T,
         typename Q = std::queue<T>>
class TWorker : public TLockable {
protected:
    volatile bool Valid = true;
    std::condition_variable Cv;
    std::vector<std::shared_ptr<std::thread>> Threads;
    size_t Seq = 0;
    const std::string Name;
    size_t Nr;

    virtual void Wait(TScopedLock &lock) {
        if (!Valid)
            return;

        Cv.wait(lock);
    }

    void WorkerFn(const std::string &name) {
        SetProcessName(name);
        auto lock = ScopedLock();
        while (Valid) {
            if (Queue.empty())
                Wait(lock);

            while (Valid && !Queue.empty()) {
                T request = Pop();

                size_t seq = Seq;
                lock.unlock();
                bool handled = Handle(request);
                lock.lock();
                bool haveNewData = seq != Seq;

                if (!handled) {
                    Queue.push(std::move(request));
                    if (!haveNewData)
                        Wait(lock);
                }
            }
        }
    }

    void Shutdown() {
        auto lock = ScopedLock();
        Valid = false;
        Cv.notify_all();
    }

    void Join() {
        for (auto thread : Threads)
            thread->join();
        Threads.clear();
    }

    virtual T Pop() =0;
    virtual bool Handle(T &elem) =0;

public:
    Q Queue;

    TWorker(const std::string &name, size_t nr) : Name(name), Nr(nr) {}

    void Start() {
        for (size_t i = 0; i < Nr; i++)
            Threads.push_back(std::shared_ptr<std::thread>(NewThread(&TWorker::WorkerFn, this, Name + std::to_string(i))));
    }

    virtual void Stop() {
        if (Valid) {
            Shutdown();
            Join();
        }
    }

    virtual void Push(T &&elem) {
        auto lock = ScopedLock();
        Queue.push(std::move(elem));
        Seq++;
        Cv.notify_one();
    }
};
