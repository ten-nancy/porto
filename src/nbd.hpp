#include <future>
#include <mutex>
#include <functional>
#include <unordered_map>
#include  <cstdint>

extern "C" {
#include <netlink/netlink.h>
}

#include "common.hpp"
#include "util/error.hpp"
#include "util/path.hpp"

using TNbdConnCallbacks = std::unordered_map<
    uint32_t,
    std::pair<
        std::promise<TError>, std::function<TError(struct nl_msg*)>
    >
>;

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

class TNbdConn : public TNonCopyable {
    std::shared_ptr<struct nl_sock> Sock;
    std::shared_ptr<struct nl_sock> McastSock;
    int EventFd;
    std::unique_ptr<std::thread> McastThread;

    int DriverId = -1;
    std::atomic<std::uint32_t> Seq;

    std::mutex MsgMutex;
    TNbdConnCallbacks MsgCallbacks;

    std::mutex AckMutex;
    TNbdConnCallbacks AckCallbacks;

    std::unordered_map<int, int> Disconnects;
    std::function<void(int, int)> DeadLinkCb;

public:
    TNbdConn() = default;
    ~TNbdConn() { Close(); }
    void Close();
    TError Init(int fd);
    TError ConnectDevice(const TNbdConnParams& params, uint64_t deadlineMs, int &index);
    TError DisconnectDevice(int index);
    TError ReconnectDevice(const TNbdConnParams& params, uint64_t deadlineMs, int index);
    void OnDeadLink(std::function<void(int, int)> cb) {
        DeadLinkCb = cb;
    }
    static TError MakeMcastSock(int targetFd);
private:
    uint32_t NextSeq();
    int GenCallback(struct nl_msg* msg, std::mutex &m, TNbdConnCallbacks& callbacks);
    int DeadLinkCallback(struct nl_msg* msg);
    std::future<TError> RegisterMsg(uint32_t seq, std::function<TError(struct nl_msg*)> f);
    std::future<TError> RegisterAck(uint32_t seq, std::function<TError(struct nl_msg*)> f);

    static int MsgCallback(struct nl_msg* msg, void*);
    static int AckCallback(struct nl_msg* msg, void*);
};
