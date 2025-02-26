#include "event.hpp"

#include "client.hpp"
#include "config.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/unix.hpp"
#include "util/worker.hpp"

class TEventWorker: public TWorker<TEvent, std::priority_queue<TEvent>> {
protected:
    TClient client;

    TEvent Pop() override {
        auto request = Queue.top();
        Queue.pop();
        return request;
    }

    void Wait(TScopedLock &lock) override {
        if (!Valid)
            return;

        Statistics->QueuedEvents = Queue.size();

        if (Queue.size()) {
            auto now = GetCurrentTimeMs();
            if (Queue.top().DueMs <= now)
                return;
            auto timeout = Queue.top().DueMs - now;
            Cv.wait_for(lock, std::chrono::milliseconds(timeout));
        } else {
            TWorker::Wait(lock);
        }
    }

    bool Handle(TEvent &event) override {
        if (event.DueMs <= GetCurrentTimeMs()) {
            client.ClientContainer = RootContainer;
            client.StartRequest();
            TContainer::Event(event);
            client.FinishRequest();
            return true;
        }

        return false;
    }

public:
    TEventWorker(const size_t nr)
        : TWorker("portod-EV", nr),
          client("<event>")
    {}
};

std::string TEvent::GetMsg() const {
    switch (Type) {
    case EEventType::ChildExit:
        return "exit status " + std::to_string(Exit.Status) + " for child pid " + std::to_string(Exit.Pid);
    case EEventType::Exit:
        return "exit status " + std::to_string(Exit.Status) + " for pid " + std::to_string(Exit.Pid);
    case EEventType::RotateLogs:
        return "rotate logs";
    case EEventType::Respawn:
        return "respawn";
    case EEventType::OOM:
        return "OOM";
    case EEventType::WaitTimeout:
        return "wait timeout";
    case EEventType::DestroyAgedContainer:
        return "destroy aged container";
    case EEventType::DestroyWeakContainer:
        return "destroy weak container";
    default:
        return "unknown event";
    }
}

bool TEvent::operator<(const TEvent &rhs) const {
    return DueMs >= rhs.DueMs;
}

void TEventQueue::Add(uint64_t timeoutMs, const TEvent &e) {
    TEvent copy = e;
    copy.DueMs = GetCurrentTimeMs() + timeoutMs;
    Worker->Push(std::move(copy));
}

TEventQueue::TEventQueue() {
    Worker = std::make_shared<TEventWorker>(1);
}

void TEventQueue::Start() {
    Worker->Start();
}

void TEventQueue::Stop() {
    Worker->Stop();
}
