#include <climits>
#include <cstdint>

extern "C" {
#include <arpa/inet.h>
#include <linux/nbd-netlink.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/netlink.h>
#include <poll.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
}

#include "nbd.hpp"
#include "util/log.hpp"
#include "util/socket.hpp"

#define NBD_OPT_EXPORT_NAME     (1)      /**< Client wants to select a named export (is followed by name of export) */
#define NBD_FLAG_FIXED_NEWSTYLE (1 << 0) /**< new-style export that actually supports extending */
#define NBD_FLAG_NO_ZEROES \
    (1 << 1) /**< we won't send the 128 bits of zeroes if the client sends NBD_FLAG_C_NO_ZEROES */

#define NBD_FLAG_READ_ONLY (1 << 1) /** Device is read-only */

const uint64_t nbd_magic = 0x4e42444d41474943UL;
const uint64_t opts_magic = 0x49484156454F5054UL;

struct TConnParams {
    std::vector<TSocket> socks;
    uint64_t size;
    int blocksize;
    uint16_t flags;
    int conn_timeout;
    int timeout;
    int dead_conn_timeout;
};

static TError SockError(const TError &error, const std::string &msg = "") {
    auto m = msg.empty() ? error.Text : fmt::format("{}: {}", error.Text, msg);

    switch (error.Errno) {
    case ECONNREFUSED:
    case ENOENT:
        return TError(EError::NbdSocketUnavaliable, m);
    case EAGAIN:
        return TError(EError::NbdSocketTimeout, m);
    default:
        return TError(EError::NbdSocketError, m);
    }
}

static std::shared_ptr<struct nl_sock> newNbdSock(int fd) {
    std::shared_ptr<struct nl_sock> sk(nl_socket_alloc(), [](struct nl_sock *sk) { nl_socket_free(sk); });

    if (!sk)
        return nullptr;

    int ret = fd < 0 ? genl_connect(sk.get()) : nl_socket_set_fd(sk.get(), -1, fd);

    if (ret < 0) {
        L_ERR("newNbdSock: {}", nl_geterror(ret));
        return nullptr;
    }

    return sk;
}

static struct nl_msg *newMsgConn(int driver_id, uint32_t seq, const TConnParams &params) {
    struct nlattr *sock_attr;
    struct nl_msg *msg = nlmsg_alloc();

    if (!msg)
        return NULL;

    genlmsg_put(msg, NL_AUTO_PORT, seq, driver_id, 0, 0, NBD_CMD_CONNECT, 0);
    NLA_PUT_U64(msg, NBD_ATTR_SIZE_BYTES, params.size);
    NLA_PUT_U64(msg, NBD_ATTR_BLOCK_SIZE_BYTES, params.blocksize);
    NLA_PUT_U64(msg, NBD_ATTR_SERVER_FLAGS, params.flags);

    if (params.timeout)
        NLA_PUT_U64(msg, NBD_ATTR_TIMEOUT, params.timeout);

    if (params.dead_conn_timeout)
        NLA_PUT_U64(msg, NBD_ATTR_DEAD_CONN_TIMEOUT, params.dead_conn_timeout);

    sock_attr = nla_nest_start(msg, NBD_ATTR_SOCKETS);

    if (!sock_attr)
        goto nla_put_failure;

    for (const auto &sock: params.socks) {
        struct nlattr *sock_opt = nla_nest_start(msg, NBD_SOCK_ITEM);

        if (!sock_opt)
            goto nla_put_failure;

        NLA_PUT_U32(msg, NBD_SOCK_FD, sock.Fd);
        nla_nest_end(msg, sock_opt);
    }

    nla_nest_end(msg, sock_attr);
    return msg;

nla_put_failure:
    free(msg);
    return NULL;
}

static TError parseConnMsg(struct nl_msg *msg, int &index) {
    struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(nlmsg_hdr(msg));
    struct nlattr *msg_attr[NBD_ATTR_MAX + 1];
    int ret;

    if (gnlh->cmd != NBD_CMD_CONNECT)
        return TError("gnlh->cmd: expected {} got {}", gnlh->cmd, NBD_CMD_CONNECT);

    ret = nla_parse(msg_attr, NBD_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);
    if (ret < 0)
        return TError("failed to parse nl_msg: {}", nl_geterror(ret));
    if (!msg_attr[NBD_ATTR_INDEX])
        return TError("missing device index");

    index = (int)nla_get_u32(msg_attr[NBD_ATTR_INDEX]);
    return OK;
}

static struct nl_msg *newMsgReconn(int driverId, int index, uint32_t seq, const TConnParams &params) {
    struct nlattr *sock_attr;
    struct nl_msg *msg = nlmsg_alloc();

    if (!msg)
        return NULL;

    genlmsg_put(msg, NL_AUTO_PORT, seq, driverId, 0, 0, NBD_CMD_RECONFIGURE, 0);

    NLA_PUT_U32(msg, NBD_ATTR_INDEX, index);

    NLA_PUT_U64(msg, NBD_ATTR_SIZE_BYTES, params.size);
    NLA_PUT_U64(msg, NBD_ATTR_BLOCK_SIZE_BYTES, params.blocksize);
    NLA_PUT_U64(msg, NBD_ATTR_SERVER_FLAGS, params.flags);

    sock_attr = nla_nest_start(msg, NBD_ATTR_SOCKETS);
    if (!sock_attr)
        goto nla_put_failure;

    if (!sock_attr)
        goto nla_put_failure;

    for (const auto &sock: params.socks) {
        struct nlattr *sock_opt = nla_nest_start(msg, NBD_SOCK_ITEM);

        if (!sock_opt)
            goto nla_put_failure;

        NLA_PUT_U32(msg, NBD_SOCK_FD, sock.Fd);
        nla_nest_end(msg, sock_opt);
    }

    nla_nest_end(msg, sock_attr);
    return msg;

nla_put_failure:
    free(msg);
    return NULL;
}

static struct nl_msg *newMsgDisconn(int driverId, uint32_t seq, int index) {
    struct nl_msg *msg = nlmsg_alloc();

    if (!msg)
        return NULL;

    genlmsg_put(msg, NL_AUTO_PORT, seq, driverId, 0, 0, NBD_CMD_DISCONNECT, 0);
    NLA_PUT_U32(msg, NBD_ATTR_INDEX, index);
    return msg;

nla_put_failure:
    nlmsg_free(msg);
    return NULL;
}

static TError sendExportname(const TSocket &sock, const std::string &name) {
    struct {
        uint64_t magic;
        uint32_t opt;
        uint32_t size;
    } __attribute__((packed)) hdr = {
        be64toh(opts_magic),
        ntohl(NBD_OPT_EXPORT_NAME),
        ntohl(name.size()),
    };

    auto error = sock.Write(&hdr, sizeof(hdr));
    if (error)
        return error;

    return sock.Write(name.c_str(), name.size());
}

static TError negotiate(const TSocket &sock, uint64_t &size, uint16_t &flags, uint32_t client_flags,
                        const std::string &exportname) {
    TError error;
    uint64_t magic;
    uint16_t global_flags;

    if (error = sock.Read(&magic, sizeof(magic)))
        return SockError(error, "read magic failed");

    if (be64toh(magic) != nbd_magic)
        return TError(EError::NbdProtoError, "invalid magic: expected {}, got {}", nbd_magic, be64toh(magic));

    if (error = sock.Read(&magic, sizeof(magic)))
        return SockError(error, "read opts magic failed");

    if (be64toh(magic) != opts_magic)
        return TError(EError::NbdProtoError, "invalid magic: expected {}, got {}", opts_magic, be64toh(magic));

    if (error = sock.Read(&global_flags, sizeof(global_flags)))
        return SockError(error, "read global flags failed");

    global_flags = ntohs(global_flags);

    if (global_flags & NBD_FLAG_NO_ZEROES)
        client_flags |= NBD_FLAG_NO_ZEROES;

    client_flags = htonl(client_flags);

    if (error = sock.Write(&client_flags, sizeof(client_flags)))
        return SockError(error, "write client flags failed");

    if (error = sendExportname(sock, exportname))
        return SockError(error, "send export name failed");

    if (error = sock.Read(&size, sizeof(size))) {
        if (error.Errno == ECONNRESET)
            return TError(EError::NbdUnkownExport, "unknown export '{}'", exportname);
        return SockError(error, "read export size failed");
    }
    size = be64toh(size);

    if (error = sock.Read(&flags, sizeof(flags)))
        return SockError(error, "read export flags failed: {}");
    flags = ntohs(flags);

    if (!(global_flags & NBD_FLAG_NO_ZEROES)) {
        char buf[124];
        if (error = sock.Read(buf, 124)) {
            if (error.Errno == ECONNRESET)
                return TError(EError::NbdProtoError, "failed read trailing zeroes");
            return SockError(error);
        }
    }

    return OK;
}

TError TNlFuture::WaitFor(uint64_t timeoutMs) {
    auto status = Fut.wait_for(std::chrono::milliseconds(timeoutMs));
    if (status != std::future_status::ready)
        return DoCleanup() ? Fut.get() : TError("netlink timeout");

    Cleanup = nullptr;
    return Fut.get();
}

TError TNlFuture::WaitUntil(uint64_t deadlineMs) {
    uint64_t now = GetCurrentTimeMs();
    if (now >= deadlineMs)
        return DoCleanup() ? Fut.get() : TError("netlink timeout");

    return WaitFor(deadlineMs - now);
}

int TNbdConn::GenCallback(struct nl_msg *msg, struct nlmsgerr *err, TNbdConnCallbacks &callbacks) {
    struct nlmsghdr *hdr = msg ? nlmsg_hdr(msg) : &err->msg;
    uint32_t seq = hdr->nlmsg_seq;
    struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(hdr);

    if (gnlh->cmd == NBD_CMD_LINK_DEAD)
        return DeadLinkCallback(msg);

    auto lock = callbacks.ScopedLock();
    auto it = callbacks.find(seq);
    if (it != callbacks.end()) {
        auto cb = it->second.Callback;
        auto promise = std::move(it->second.Promise);

        callbacks.erase(it);
        lock.unlock();
        promise.set_value(msg ? cb(msg) : TError("netlink: {}", strerror(-err->error)));
    } else {
        lock.unlock();

        // if client timed out on waiting connection
        // we must disconnect it
        if (gnlh->cmd == NBD_CMD_CONNECT) {
            int index;
            auto error = parseConnMsg(msg, index);
            if (!error)
                error = DisconnectDevice(index, false);

            if (error)
                L_WRN("nbd: failed disconnect rogue device: {}", error);
        }
    }

    return NL_OK;
}

int TNbdConn::ErrCallback(struct sockaddr_nl *, struct nlmsgerr *err, void *arg) {
    TNbdConn *nbdConn = static_cast<TNbdConn *>(arg);
    return nbdConn->GenCallback(nullptr, err, nbdConn->AckCallbacks);
}

int TNbdConn::MsgCallback(struct nl_msg *msg, void *arg) {
    TNbdConn *nbdConn = static_cast<TNbdConn *>(arg);
    return nbdConn->GenCallback(msg, nullptr, nbdConn->MsgCallbacks);
}

int TNbdConn::AckCallback(struct nl_msg *msg, void *arg) {
    TNbdConn *nbdConn = static_cast<TNbdConn *>(arg);
    return nbdConn->GenCallback(msg, nullptr, nbdConn->AckCallbacks);
}

int TNbdConn::DeadLinkCallback(struct nl_msg *msg) {
    struct genlmsghdr *gnlh = (struct genlmsghdr *)nlmsg_data(nlmsg_hdr(msg));
    struct nlattr *msg_attr[NBD_ATTR_MAX + 1];
    int ret;

    ret = nla_parse(msg_attr, NBD_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), NULL);

    if (ret) {
        L_WRN("nbd: invalid message from kernel");
        return NL_SKIP;
    }

    if (!msg_attr[NBD_ATTR_INDEX]) {
        L_WRN("nbd: don't have the index set");
        return NL_SKIP;
    }

    ++Disconnects[nla_get_u32(msg_attr[NBD_ATTR_INDEX])];

    return NL_OK;
}

TNlFuture TNbdConn::RegisterCallback(uint32_t seq, std::function<TError(struct nl_msg *)> cb,
                                     TNbdConnCallbacks &callbacks) {
    std::promise<TError> promise;
    TNlFuture fut(promise.get_future(), [seq, &callbacks]() {
        auto lock = callbacks.ScopedLock();
        // seq is not in callbacks => pomise was fulfilled
        return !callbacks.erase(seq);
    });
    {
        auto lock = callbacks.ScopedLock();
        callbacks[seq] = {std::move(promise), cb};
    }

    return fut;
}

TNlFuture TNbdConn::RegisterMsg(uint32_t seq, std::function<TError(struct nl_msg *)> cb) {
    return RegisterCallback(seq, cb, MsgCallbacks);
}

TNlFuture TNbdConn::RegisterAck(uint32_t seq, std::function<TError(struct nl_msg *)> cb) {
    return RegisterCallback(seq, cb, AckCallbacks);
}

inline uint32_t TNbdConn::NextSeq() {
    while (true) {
        uint32_t seq = ++Seq;
        if (seq != NL_AUTO_SEQ)
            return seq;
    }
}

TError TNbdConn::MakeMcastSock(int targetFd) {
    auto sk = newNbdSock(-1);
    if (!sk)
        return TError("failed create netlink socket");

    int mcast_grp = genl_ctrl_resolve_grp(sk.get(), NBD_GENL_FAMILY_NAME, NBD_GENL_MCAST_GROUP_NAME);
    if (mcast_grp < 0)
        return TError("failed resolve netlink mcast group: {}", nl_geterror(mcast_grp));

    int ret = nl_socket_add_memberships(sk.get(), mcast_grp, 0);
    if (ret < 0)
        return TError("failed add mcast group: {}", nl_geterror(ret));

    // set rx buffer size to 4MiB
    ret = nl_socket_set_buffer_size(sk.get(), 1 << 22, 0);
    if (ret < 0)
        return TError("failed set netlink socket buffer size: {}", nl_geterror(ret));

    if (dup2(nl_socket_get_fd(sk.get()), targetFd) < 0)
        return TError::System("dup2");

    return OK;
}

TError TNbdConn::Init(int mcastFd) {
    auto sock = newNbdSock(-1);
    if (!sock)
        return TError("failed create nbd socket");

    int ret = genl_ctrl_resolve(sock.get(), "nbd");
    if (ret < 0)
        return TError("failed resolve nbd driver_id: {}", nl_geterror(ret));
    DriverId = ret;

    ret = nl_socket_set_nonblocking(sock.get());
    if (ret < 0)
        return TError("failed set socket non-blocking: {}", nl_geterror(ret));

    nl_socket_disable_seq_check(sock.get());
    nl_socket_modify_err_cb(sock.get(), NL_CB_CUSTOM, TNbdConn::ErrCallback, this);
    nl_socket_modify_cb(sock.get(), NL_CB_VALID, NL_CB_CUSTOM, TNbdConn::MsgCallback, this);
    nl_socket_modify_cb(sock.get(), NL_CB_ACK, NL_CB_CUSTOM, TNbdConn::AckCallback, this);

    auto mcastSock = newNbdSock(mcastFd);
    if (!mcastSock)
        return TError("failed create nbd socket");
    nl_socket_disable_seq_check(mcastSock.get());
    nl_socket_modify_cb(mcastSock.get(), NL_CB_VALID, NL_CB_CUSTOM, TNbdConn::MsgCallback, this);

    EventFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (EventFd < 0)
        return TError::System("eventfd");

    McastThread = std::thread([this, sock, mcastSock] {
        SetProcessName("portod-NS");

        struct pollfd fds[] = {{
                                   .fd = EventFd,
                                   .events = POLLIN,
                                   .revents = 0,
                               },
                               {
                                   .fd = nl_socket_get_fd(sock.get()),
                                   .events = POLLIN,
                                   .revents = 0,
                               },
                               {
                                   .fd = nl_socket_get_fd(mcastSock.get()),
                                   .events = POLLIN,
                                   .revents = 0,
                               }};

        while (true) {
            if (ppoll(fds, sizeof(fds) / sizeof(*fds), NULL, NULL) < 0)
                L_WRN("nbd: {}", TError::System("mcast poll"));

            if (fds[0].revents)
                break;

            if (fds[1].revents) {
                int ret = nl_recvmsgs_default(sock.get());
                if (ret < 0)
                    L_WRN("nbd: nl_recvmsgs_default: {}", nl_geterror(ret));
            }

            if (fds[2].revents) {
                int ret = nl_recvmsgs_default(mcastSock.get());
                if (ret < 0)
                    L_WRN("nbd: nl_recvmsgs_default: {}", nl_geterror(ret));
                for (auto &pair: Disconnects)
                    DeadLinkCb(pair.first, pair.second);
                Disconnects.clear();
            }
        }
    });

    Sock = sock;
    McastSock = mcastSock;

    return OK;
}

void TNbdConn::Close() {
    Sock.reset();
    McastSock.reset();

    if (McastThread.joinable()) {
        uint64_t data = 1;
        if (write(EventFd, &data, sizeof(data)) < 0)
            L_ERR("nbd: {}", TError::System("eventfd write"));
        McastThread.join();
        close(EventFd);
    }
}

TError MakeConnections(const TNbdConnParams &params, uint64_t deadlineMs, TConnParams &cparams) {
    if (params.NumConnections <= 0)
        return TError("num_connections <= 0");

    cparams.socks.reserve(params.NumConnections);

    for (int i = 0; i < params.NumConnections; ++i) {
        TSocket sock;
        TError error;
        uint16_t flags = 0;
        uint64_t size = 0;

        sock.SetDeadline(deadlineMs);

        if (params.UnixPath)
            error = sock.Connect(params.UnixPath.ToString());
        else
            error = sock.Connect(params.Host, params.Port);

        if (error)
            return SockError(error);

        error = negotiate(sock, size, flags, NBD_FLAG_FIXED_NEWSTYLE, params.ExportName);
        if (error)
            return TError(error, "negotiate failed");
        if (!i) {
            cparams.size = size;
            cparams.flags = flags;
        } else {
            if (size != cparams.size)
                return TError("device size mismatch: {} != {}", size, cparams.size);
            if (flags != cparams.flags)
                return TError("device flags mismatch: {} != {}", flags, cparams.flags);
        }

        sock.ResetDeadline();
        cparams.socks.push_back(std::move(sock));
    }

    if (params.ReadOnly)
        cparams.flags |= NBD_FLAG_READ_ONLY;

    return OK;
}

TError TNbdConn::ConnectDevice(const TNbdConnParams &params, uint64_t deadlineMs, int &index) {
    TError error;
    TConnParams cparams;
    cparams.blocksize = params.BlockSize;
    cparams.timeout = params.BioTimeout;
    cparams.dead_conn_timeout = params.ReconnTimeout;

    auto sk = Sock;
    if (!sk)
        return TError("nbd connection is closed");

    error = MakeConnections(params, deadlineMs, cparams);
    if (error)
        return error;

    auto seq = NextSeq();
    auto msg = newMsgConn(DriverId, seq, cparams);
    if (!msg)
        return TError("failed create msg");

    auto ackFut = RegisterAck(seq, [](struct nl_msg *) { return OK; });
    auto msgFut = RegisterMsg(seq, [&index](struct nl_msg *msg) { return parseConnMsg(msg, index); });

    int ret = nl_send_auto(sk.get(), msg);
    nlmsg_free(msg);
    if (ret < 0)
        return TError("nl_send failed");

    error = ackFut.WaitUntil(deadlineMs);
    if (error)
        return error;
    return msgFut.WaitUntil(deadlineMs);
}

TError TNbdConn::ReconnectDevice(const TNbdConnParams &params, uint64_t deadlineMs, int index) {
    TError error;
    TConnParams cparams;
    cparams.blocksize = params.BlockSize;
    cparams.timeout = params.BioTimeout;
    cparams.dead_conn_timeout = params.ReconnTimeout;

    auto sk = Sock;
    if (!sk)
        return TError("nbd connection is closed");

    error = MakeConnections(params, deadlineMs, cparams);
    if (error)
        return error;

    auto seq = NextSeq();
    auto msg = newMsgReconn(DriverId, index, seq, cparams);
    if (!msg)
        return TError("failed create msg");

    auto fut = RegisterAck(seq, [](struct nl_msg *) { return OK; });

    int ret = nl_send_auto(sk.get(), msg);
    nlmsg_free(msg);
    if (ret < 0)
        return TError("nl_send failed");

    return fut.WaitUntil(deadlineMs);
}

TError TNbdConn::DisconnectDevice(int index, bool wait) {
    auto sk = Sock;
    if (!sk)
        return TError("nbd connection is closed");

    uint32_t seq = NextSeq();
    struct nl_msg *msg = newMsgDisconn(DriverId, seq, index);

    auto fut = RegisterAck(seq, [](struct nl_msg *) { return OK; });

    int ret = nl_send_auto(sk.get(), msg);
    nlmsg_free(msg);
    if (ret < 0)
        return TError("nl_send failed");

    if (wait)
        return fut.WaitFor(5000);
    return OK;
}
