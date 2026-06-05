#include <string>

#include "config.hpp"
#include "core.hpp"
#include "epoll.hpp"
#include "libporto.hpp"
#include "nbd.hpp"
#include "portod.hpp"
#include "util/log.hpp"
#include "util/path.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "version.hpp"
#include "volume.hpp"

extern "C" {
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
}

TPidFile MasterPidFile(PORTO_MASTER_PIDFILE, PORTOD_MASTER_NAME, "portod");
pid_t MasterPid;
bool RespawnPortod = true;
static bool NeedUpgrade = false;
static uint64_t StartServerShutdown = 0;

static std::string PreviousVersion;
static ino_t SocketIno = 0;

void ReopenMasterLog() {
    if (MasterPid)
        kill(MasterPid, SIGUSR1);
}

bool CheckPortoAlive() {
    Porto::Connection conn;
    if (conn.SetTimeout(1) != EError::Success)
        return false;
    std::string ver, rev;
    return !conn.GetVersion(ver, rev);
}

bool SanityCheck() {
    if (getuid() != 0) {
        std::cerr << "Need root privileges to start" << std::endl;
        return EXIT_FAILURE;
    }

    if (!MasterPidFile.Read() && MasterPidFile.Pid != getpid() && CheckPortoAlive()) {
        std::cerr << "Another instance of portod is running!" << std::endl;
        return EXIT_FAILURE;
    }

    if (CompareVersions(config().linux_version(), "3.18") < 0) {
        std::cerr << "Require Linux >= 3.18\n";
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int UpgradeMaster() {
    L_SYS("Updating master...");

    std::vector<const char *> args = {PORTO_BINARY_PATH};
    if (StdLog)
        args.push_back("--stdlog");
    if (Debug)
        args.push_back("--debug");
    else if (Verbose)
        args.push_back("--verbose");
    args.push_back(nullptr);

    execvp(args[0], (char **)args.data());

    args[0] = program_invocation_name;
    execvp(args[0], (char **)args.data());

    args[0] = "portod";
    execvp(args[0], (char **)args.data());

    args[0] = "/usr/sbin/portod";
    execvp(args[0], (char **)args.data());

    std::cerr << "Cannot exec " << args[0] << ": " << strerror(errno) << std::endl;
    return EXIT_FAILURE;
}

static inline void LogError(const TError &err, const char *msg) {
    if (err)
        L_ERR("{}: {}", msg, err);
}

class TServer {
    std::unordered_set<pid_t> Zombies;

    TError MakePipes(TFile &evtR, TFile &ackW) {
        auto error = Pipe(evtR, EvtW, O_NONBLOCK | O_CLOEXEC);
        if (error)
            return error;

        return Pipe(AckR, ackW, O_NONBLOCK | O_CLOEXEC);
    }

public:
    TFile EvtW, AckR;
    pid_t Pid = -1;
    TPidFd PidFd;
    int Status = 0;

    ~TServer() {
        LogError(Destroy(), "Cannot destroy server");
    }

    TError StartChild(std::function<void()> cleanup) {
        TFile evtR, ackW;
        auto error = MakePipes(evtR, ackW);
        if (error)
            return error;

        auto pid = fork();

        if (pid < 0)
            return TError::System("fork");
        else if (pid == 0) {
            cleanup();
            EvtW.Close();
            AckR.Close();
            if (dup2(evtR.Fd, REAP_EVT_FD) < 0) {
                L_ERR("Cannot dup evt read end: {}", TError::System("dup2"));
                _exit(EXIT_FAILURE);
            }
            evtR.Close();
            if (dup2(ackW.Fd, REAP_ACK_FD) < 0) {
                L_ERR("Cannot dup ack write end: {}", TError::System("dup2"));
                _exit(EXIT_FAILURE);
            }
            ackW.Close();
            _exit(Server());
        }

        evtR.Close();
        ackW.Close();

        error = PidFd.Open(pid);
        if (error) {
            if (kill(pid, SIGKILL) < 0)
                L_ERR("Cannot kill portod: {}", TError::System("kill"));
            int status;
            if (waitpid(pid, &status, 0) < 0)
                L_ERR("Cannot wait portod server: {}", TError::System("waitpid({})", pid));
            return TError(error, "Cannot open server pidfd");
        }
        Pid = pid;
        return OK;
    }

    bool HandleZombie(pid_t pid, int status, int code) {
        if (Zombies.count(pid) || pid == Pid)
            return false;

        if (code == CLD_KILLED) {
            // pass
        } else if (code == CLD_DUMPED) {
            status = status | (1 << 7);
        } else {  // CLD_EXITED
            status = status << 8;
        }

        L_VERBOSE("Report zombie pid={} status={}", pid, status);
        int report[2] = {pid, status};

        if (write(EvtW.Fd, report, sizeof(report)) != sizeof(report)) {
            L_WRN("Cannot report zombie: {}", TError::System("write"));
            return false;
        }

        Zombies.emplace(pid);
        Statistics->QueuedStatuses = Zombies.size();
        return true;
    }

    void ReportZombies() {
        while (true) {
            siginfo_t info;

            info.si_pid = 0;
            if (waitid(P_ALL, -1, &info, WNOHANG | WNOWAIT | WEXITED) || !info.si_pid)
                break;

            if (!HandleZombie(info.si_pid, info.si_status, info.si_code))
                break;
        }
    }

    int ReapZombies() {
        int pid;
        int nr = 0;

        while (read(AckR.Fd, &pid, sizeof(pid)) == sizeof(pid)) {
            if (pid <= 0)
                continue;

            if (Zombies.find(pid) == Zombies.end()) {
                L_WRN("Got ack for unknown zombie pid={}", pid);
            } else {
                L_VERBOSE("Reap zombie pid={}", pid);
                (void)waitpid(pid, NULL, 0);
                Zombies.erase(pid);
                Statistics->QueuedStatuses = Zombies.size();
            }

            nr++;
        }

        return nr;
    }

    TError Kill(int signo) {
        return PidFd.Kill(signo);
    }

    TError Wait(int timeoutMs) const {
        return PidFd.Wait(timeoutMs);
    }

    TError Reap() {
        siginfo_t info;
        auto error = PidFd.Reap(info);
        if (error)
            return error;
        Status = info.si_status;
        L_SYS("Portod {}", FormatExitStatus(Status));
        PidFd.Close();
        return OK;
    }

    TError Destroy() {
        if (!PidFd)
            return OK;

        L_SYS("Kill server");
        auto error = Kill(SIGKILL);
        if (error && error.Errno != ESRCH)
            return error;
        return Reap();
    }
};

static TError CreatePortoSocket() {
    TPath path(PORTO_SOCKET_PATH);
    struct stat fd_stat, sk_stat;
    struct sockaddr_un addr;
    TError error;
    TFile sock;

    if (dup2(PORTO_SK_FD, PORTO_SK_FD) == PORTO_SK_FD) {
        sock.SetFd = PORTO_SK_FD;
        if (!sock.Stat(fd_stat) && S_ISSOCK(fd_stat.st_mode) && !path.StatStrict(sk_stat) &&
            S_ISSOCK(sk_stat.st_mode)) {
            time_t now = time(nullptr);
            L_SYS(
                "Reuse porto socket: inode {} : {} "
                "age {} : {}", fd_stat.st_ino, sk_stat.st_ino, now - fd_stat.st_ctime, now - sk_stat.st_ctime);
            SocketIno = sk_stat.st_ino;
        } else {
            L_WRN("Unlinked porto socket. Recreating...");
            sock.SetFd = -1;
        }
    }

    if (!sock) {
        sock.SetFd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (sock.Fd < 0)
            return TError::System("socket()");

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);

        (void)path.Unlink();

        if (bind(sock.Fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
            return TError::System("bind()");

        error = path.StatStrict(sk_stat);
        if (error)
            return error;
        SocketIno = sk_stat.st_ino;
    }

    if (fchmod(sock.Fd, PORTO_SOCKET_MODE) < 0)
        return TError::System("fchmod()");

    error = path.Chown(RootUser, PortoGroup);
    if (error)
        return error;

    error = path.Chmod(PORTO_SOCKET_MODE);
    if (error)
        return error;

    if (listen(sock.Fd, config().daemon().max_clients()) < 0)
        return TError::System("listen()");

    if (sock.Fd == PORTO_SK_FD)
        sock.SetFd = -1;
    else if (dup2(sock.Fd, PORTO_SK_FD) != PORTO_SK_FD)
        return TError::System("dup2()");

    return OK;
}

static TError CreatePortoNlSocket() {
    struct stat st;
    if (!fstat(PORTO_NL_SK_FD, &st) && S_ISSOCK(st.st_mode)) {
        time_t now = time(nullptr);
        L_SYS("Reuse porto nl socket: inode {} age {}", st.st_ino, now - st.st_ctime);
        PortoNlSocketReused = true;
    } else {
        L_SYS("Create new porto nl socket");
        auto error = TNbdConn::MakeMcastSock(PORTO_NL_SK_FD);
        if (error)
            return error;
    }
    return OK;
}

void CheckPortoSocket() {
    struct stat fd_stat, sk_stat;
    TError error;

    if (fstat(PORTO_SK_FD, &fd_stat))
        error = TError::System("socket fd stat");
    else if (stat(PORTO_SOCKET_PATH, &sk_stat))
        error = TError::System("socket path stat");
    else if (!S_ISSOCK(fd_stat.st_mode) || !S_ISSOCK(sk_stat.st_mode))
        error = TError::System("not a socket");
    else if (sk_stat.st_ino != SocketIno)
        error = TError::System("different inode");
    else
        return;

    L_WRN("Porto socket: {}", error);
    kill(MasterPid, SIGHUP);
}

static void HandleSignal(TServer &server, const struct signalfd_siginfo &sigInfo) {
    int signo = sigInfo.ssi_signo;
    PrintSignalInfo(sigInfo);

    switch (signo) {
    case SIGINT:
    case SIGTERM: {
        L_SYS("Forward signal {} to portod", signo);
        LogError(server.Kill(signo), "Cannot send signal to server");
        RespawnPortod = false;

        L_SYS("Waiting for portod shutdown...");
        auto error = server.Wait(config().daemon().portod_stop_timeout() * 1000);
        if (error) {
            L_ERR("Server wait failed: {}", error);
            LogError(server.Kill(SIGKILL), "Cannot send SIGKILL to server");
        }
        return;
    }
    case SIGCHLD: {
        server.HandleZombie(sigInfo.ssi_pid, sigInfo.ssi_status, sigInfo.ssi_code);
        return;
    }
    case SIGUSR1: {
        OpenLog(PORTO_LOG);
        LogError(server.Kill(signo), "Cannot kill portod");
        return;
    }
    case SIGUSR2:
        DumpMallocInfo();
        return;
    case SIGHUP: {
        L_SYS("Updating server...");
        auto error = server.Kill(SIGHUP);
        if (error) {
            L_ERR("Cannot send SIGHUP to server: {}", error);
            return;
        }
        if (!NeedUpgrade) {
            StartServerShutdown = GetCurrentTimeMs();
            NeedUpgrade = true;
        }
        return;
    }
    default:
        /* Ignore other signals */
        return;
    }
}

static TError SpawnServer() {
    TFile sigFd;

    sigFd.SetFd = SignalFd();
    if (sigFd.Fd < 0)
        return TError::System("signalfd");

    auto error = CreatePortoSocket();
    if (error)
        return TError(error, "Cannot create porto socket");

    TServer server;
    error = server.StartChild([&]() { sigFd.Close(); });
    if (error)
        return TError(error, "Cannot start server");

    L_SYS("Start portod {}", server.Pid);
    Statistics->PortoStarts++;

    TEpollLoop loop;
    error = loop.Create();
    if (error)
        return TError(error, "Cannot create event loop");

    auto serverPidSource = std::make_shared<TEpollSource>(server.PidFd.PidFd);
    error = loop.AddSource(serverPidSource);
    if (error)
        return TError(error, "Cannot add server pidfd to epoll");

    auto ackSource = std::make_shared<TEpollSource>(server.AckR);
    error = loop.AddSource(ackSource);
    if (error)
        return TError(error, "Cannot add ack read end to epoll");

    auto sigSource = std::make_shared<TEpollSource>(sigFd);
    error = loop.AddSource(sigSource);
    if (error)
        return TError(error, "Cannot add signal fd to epoll");

    while (true) {
        std::vector<struct epoll_event> events;

        error = loop.GetEvents(events, -1);
        if (error)
            return error;

        for (auto ev: events) {
            auto source = loop.GetSource(ev.data.fd);
            if (!source)
                continue;

            if (source->Fd == server.PidFd.PidFd.Fd) {
                auto error = server.Reap();
                if (error)
                    return TError(error, "Cannot wait server");
                return OK;
            } else if (source->Fd == sigFd.Fd) {
                struct signalfd_siginfo sigInfo;
                while (read(sigFd.Fd, &sigInfo, sizeof(sigInfo)) == sizeof(sigInfo))
                    HandleSignal(server, sigInfo);
            } else if (source->Fd == server.AckR.Fd) {
                if (!server.ReapZombies()) {
                    L_SYS("Server pipe is inactive");
                    loop.RemoveSource(*ackSource);
                }
            } else {
                L_WRN("Unknown event {}", source->Fd);
                loop.RemoveSource(*source);
            }
        }
        server.ReportZombies();
    }
}

int PortodMaster() {
    TError error;
    int ret;

    if (SanityCheck())
        return EXIT_FAILURE;

    SetProcessName(PORTOD_MASTER_NAME);

    (void)close(STDIN_FILENO);
    int null = open("/dev/null", O_RDWR);
    PORTO_ASSERT(null == STDIN_FILENO);

    if (!StdLog || fcntl(STDOUT_FILENO, F_GETFD) < 0) {
        ret = dup2(null, STDOUT_FILENO);
        PORTO_ASSERT(ret == STDOUT_FILENO);
    }

    if (!StdLog || fcntl(STDERR_FILENO, F_GETFD) < 0) {
        ret = dup2(null, STDERR_FILENO);
        PORTO_ASSERT(ret == STDERR_FILENO);
    }

    OpenLog(PORTO_LOG);
    if (!LogFile)
        return EXIT_FAILURE;

    InitStatistics();

    Statistics->MasterStarted = GetCurrentTimeMs();

    ret = chdir("/");
    PORTO_ASSERT(!ret);

    CatchFatalSignals();

    MasterPid = getpid();
    error = MasterPidFile.Save(MasterPid);
    if (error)
        FatalError("Cannot save pid", error);

    ReadConfigs();
    error = ValidateConfig();
    if (error)
        FatalError("Invalid config", error);

    TPath pathVer(PORTO_VERSION_FILE);

    if (pathVer.ReadAll(PreviousVersion)) {
        (void)pathVer.Mkfile(0644);
        PreviousVersion = "";
    } else {
        if (PreviousVersion[0] == 'v')
            PreviousVersion = PreviousVersion.substr(1);
    }

    if (pathVer.WriteAll(PORTO_VERSION))
        L_ERR("Cannot update current version");

    TPath pathBin(PORTO_BINARY_PATH), prevBin;
    TPath procExe("/proc/self/exe"), thisBin;
    error = procExe.ReadLink(thisBin);
    if (error)
        FatalError("Cannot read /proc/self/exe", error);
    (void)pathBin.ReadLink(prevBin);

    if (prevBin != thisBin) {
        (void)pathBin.Unlink();
        error = pathBin.Symlink(thisBin);
        if (error)
            FatalError("Cannot update {}: {}", PORTO_BINARY_PATH, error);
    }

    L_SYS("{}", std::string(80, '-'));
    L_SYS("Started {} {} {} {}", PORTO_VERSION, PORTO_REVISION, GetPid(), thisBin);
    L_SYS("Previous version: {} {}", PreviousVersion, prevBin);

#ifndef PR_SET_CHILD_SUBREAPER
#define PR_SET_CHILD_SUBREAPER 36
#endif

    if (prctl(PR_SET_CHILD_SUBREAPER, 1) < 0) {
        TError error(EError::Unknown, errno, "prctl(PR_SET_CHILD_SUBREAPER)");
        L_ERR("Cannot set myself as a subreaper, make sure kernel version is at least 3.4: {}", error);
        return EXIT_FAILURE;
    }

    error = SetOomScoreAdj(-1000);
    if (error)
        L_ERR("Cannot adjust OOM score: {}", error);

    if (config().daemon().enable_nbd()) {
        error = LoadNbd();
        if (error) {
            L_ERR("Cannot load nbd module: {}", error);
            return EXIT_FAILURE;
        }

        error = CreatePortoNlSocket();
        if (error) {
            L_ERR("Cannot create porto netlink socket: {}", error);
            return EXIT_FAILURE;
        }
    }

    error = TCore::Register(thisBin);
    if (error) {
        L_ERR("Cannot setup core pattern: {}", error);
        return EXIT_FAILURE;
    }

    do {
        uint64_t started = GetCurrentTimeMs();
        uint64_t next = started + config().container().respawn_delay_ms();

        LogError(SpawnServer(), "Spawn server");
        if (NeedUpgrade) {
            uint64_t finish = GetCurrentTimeMs() - StartServerShutdown;
            Statistics->ShutdownTime += finish;
            UpgradeMaster();
        }
        if (next >= GetCurrentTimeMs())
            usleep((next - GetCurrentTimeMs()) * 1000);

        PreviousVersion = PORTO_VERSION;
    } while (RespawnPortod);

    error = TCore::Unregister();
    if (error)
        L_ERR("Cannot revert core pattern: {}", error);

    error = TPath(PORTO_SOCKET_PATH).Unlink();
    if (error)
        L_ERR("Cannot unlink socket file: {}", error);

    ServerPidFile.Remove();
    MasterPidFile.Remove();
    pathBin.Unlink();
    pathVer.Unlink();
    TPath(PORTO_CONTAINERS_KV).Rmdir();
    TPath(PORTO_VOLUMES_KV).Rmdir();
    if (config().daemon().enable_nbd())
        TPath(PORTO_NBD_KV).Rmdir();
    TPath("/run/porto").Rmdir();
    TPath(PORTOD_STAT_FILE).Unlink();

    L_SYS("Shutdown complete.");

    return EXIT_SUCCESS;
}
