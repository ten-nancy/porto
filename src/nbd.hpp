#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <unordered_map>

extern "C" {
#include <netlink/netlink.h>
}

#include "common.hpp"
#include "util/error.hpp"
#include "util/locks.hpp"
#include "util/path.hpp"

struct TNbdConnParams {
    TPath UnixPath;

    std::string Host;
    int Port;

    std::string ExportName;
    int NumConnections;
    int BlockSize;
    int BioTimeout;
    int ConnTimeout;
    int ReconnTimeout;
    bool ReadOnly;
};

struct TNbdConnCallback {
    std::promise<TError> Promise;
    std::function<TError(struct nl_msg *)> Callback;
};

class TNbdConnCallbacks: public std::unordered_map<uint32_t, TNbdConnCallback>, public TLockable {
public:
    ~TNbdConnCallbacks() {
        // just in case
        auto lock = ScopedLock();
        for (auto &p: *this)
            p.second.Promise.set_value(TError("interrupted"));
    }
};

class TNlFuture: public TNonCopyable {
    std::future<TError> Fut;
    std::function<bool()> Cleanup;

    bool DoCleanup() {
        auto f = Cleanup;
        if (!f)
            return false;
        Cleanup = nullptr;
        return f();
    }

public:
    TNlFuture(std::future<TError> &&fut, std::function<bool()> cleanup)
        : Fut(std::move(fut)),
          Cleanup(cleanup)
    {}

    TNlFuture(TNlFuture &&other)
        : Fut(std::move(other.Fut)),
          Cleanup(other.Cleanup)
    {}

    ~TNlFuture() {
        (void)DoCleanup();
    }

    TError WaitUntil(uint64_t deadlineMs);
    TError WaitFor(uint64_t timeoutMs);
};

class TNbdConn: public TNonCopyable {
    std::shared_ptr<struct nl_sock> Sock;
    std::shared_ptr<struct nl_sock> McastSock;
    int EventFd;
    std::thread McastThread;

    int DriverId = -1;
    std::atomic<std::uint32_t> Seq;

    TNbdConnCallbacks MsgCallbacks;
    TNbdConnCallbacks AckCallbacks;

    std::unordered_map<int, int> Disconnects;
    std::function<void(int, int)> DeadLinkCb;

public:
    TNbdConn() = default;
    ~TNbdConn() {
        Close();
    }
    void Close();
    TError Init(int fd);
    TError ConnectDevice(const TNbdConnParams &params, uint64_t deadlineMs, int &index);
    TError DisconnectDevice(int index, bool wait = false);
    TError ReconnectDevice(const TNbdConnParams &params, uint64_t deadlineMs, int index);
    void OnDeadLink(std::function<void(int, int)> cb) {
        DeadLinkCb = cb;
    }
    static TError MakeMcastSock(int targetFd);

private:
    uint32_t NextSeq();
    int GenCallback(struct nl_msg *msg, struct nlmsgerr *err, TNbdConnCallbacks &callbacks);
    int DeadLinkCallback(struct nl_msg *msg);

    TNlFuture RegisterCallback(uint32_t seq, std::function<TError(struct nl_msg *)> cb, TNbdConnCallbacks &callbacks);
    TNlFuture RegisterMsg(uint32_t seq, std::function<TError(struct nl_msg *)> cb);
    TNlFuture RegisterAck(uint32_t seq, std::function<TError(struct nl_msg *)> cb);

    static int ErrCallback(struct sockaddr_nl *msg, struct nlmsgerr *err, void *);
    static int MsgCallback(struct nl_msg *msg, void *);
    static int AckCallback(struct nl_msg *msg, void *);
};
