#include <iostream>
#include <string>
#include <vector>

#include "cgroup.hpp"
#include "client.hpp"
#include "config.hpp"
#include "container.hpp"
#include "epoll.hpp"
#include "event.hpp"
#include "helpers.hpp"
#include "kvalue.hpp"
#include "libporto.hpp"
#include "nbd.hpp"
#include "netlimitsoft.hpp"
#include "network.hpp"
#include "portod.hpp"
#include "property.hpp"
#include "rpc.hpp"
#include "storage.hpp"
#include "util/log.hpp"
#include "util/signal.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "version.hpp"
#include "volume.hpp"

extern "C" {
#include <fcntl.h>
#include <poll.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
}

TPidFile ServerPidFile(PORTO_PIDFILE, PORTOD_NAME, "portod-slave");
pid_t ServerPid;

std::unique_ptr<TEpollLoop> EpollLoop;
std::unique_ptr<TEventQueue> EventQueue;

std::mutex TidsMutex;

bool DiscardState = false;

bool ShutdownPortod = false;
bool PortoNlSocketReused = false;
std::atomic_bool NeedStopHelpers(false);

bool SupportCgroupNs = false;
bool EnableOsModeCgroupNs = false;
bool EnableRwCgroupFs = false;
uint32_t RequestHandlingDelayMs = 0;

TNetLimitSoft NetLimitSoft;

extern std::vector<ExtraProperty> ExtraProperties;
extern std::unordered_set<std::string> SupportedExtraProperties;

static uint64_t ShutdownStart = 0;
static uint64_t ShutdownDeadline = 0;

void AckExitStatus(int pid) {
    if (!pid)
        return;

    L_DBG("Acknowledge exit status for {}", pid);
    int ret = write(REAP_ACK_FD, &pid, sizeof(pid));
    if (ret != sizeof(pid)) {
        L_ERR("Can't acknowledge exit status for {}: {}", pid, TError::System("write"));
        Crash();
    }
}

static int RecvExitEvents(int fd) {
    struct pollfd fds[1];
    int nr = 1000;

    fds[0].fd = fd;
    fds[0].events = POLLIN | POLLHUP;

    while (nr--) {
        int ret = poll(fds, 1, 0);
        if (ret < 0) {
            L_ERR("Failed receive exit events: {}", TError::System("epoll"));
            return ret;
        }

        if (!fds[0].revents || (fds[0].revents & POLLHUP))
            return 0;

        int pid, status;
        if (read(fd, &pid, sizeof(pid)) != sizeof(pid)) {
            L_ERR("Failed read exit pid: {}", TError::System("read"));
            return 0;
        }
    retry:
        if (read(fd, &status, sizeof(status)) != sizeof(status)) {
            if (errno == EAGAIN)
                goto retry;
            L_ERR("Failed read exit status: {}", TError::System("read"));
            return 0;
        }

        TEvent e(EEventType::Exit);
        e.Exit.Pid = pid;
        e.Exit.Status = status;
        EventQueue->Add(0, e);
    }

    return 0;
}

static std::map<int, std::shared_ptr<TClient>> Clients;

static TError DropIdleClient(std::shared_ptr<TContainer> from = nullptr) {
    uint64_t idle = config().daemon().client_idle_timeout() * 1000;
    uint64_t now = GetCurrentTimeMs();
    std::shared_ptr<TClient> victim;

    for (auto &it: Clients) {
        auto &client = it.second;

        if (client->Processing || client->Sending)
            continue;

        if (from && client->ClientContainer != from)
            continue;

        if (now - client->ActivityTimeMs > idle) {
            victim = client;
            idle = now - client->ActivityTimeMs;
        }
    }

    if (!victim)
        return TError(EError::ResourceNotAvailable, "All client slots are active: " + (from ? from->Name : "globally"));

    L_SYS("Kick client {} idle={} ms", victim->Id, idle);
    Clients.erase(victim->Fd);
    victim->CloseConnection();
    return OK;
}

static TError AcceptConnection(int listenFd) {
    struct sockaddr_un peer_addr;
    socklen_t peer_addr_size;
    TError error;
    int clientFd;

    peer_addr_size = sizeof(struct sockaddr_un);
    clientFd = accept4(listenFd, (struct sockaddr *)&peer_addr, &peer_addr_size, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (clientFd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return OK; /* client already gone */
        return TError::System("accept4()");
    }

    Statistics->ClientsConnected++;

    auto client = std::make_shared<TClient>(clientFd);
    error = client->IdentifyClient();
    if (error)
        return error;

    unsigned max_clients = config().daemon().max_clients_in_container();
    if (client->IsSuperUser())
        max_clients += NR_SUPERUSER_CLIENTS;

    if (client->ClientContainer->ClientsCount > (int)max_clients) {
        error = DropIdleClient(client->ClientContainer);
        if (error)
            return error;
    }

    max_clients = config().daemon().max_clients();
    if (client->IsSuperUser())
        max_clients += NR_SUPERUSER_CLIENTS;

    if (Statistics->ClientsCount > max_clients) {
        error = DropIdleClient();
        if (error)
            return error;
    }

    error = EpollLoop->AddSource(client);
    if (error)
        return error;

    client->InEpoll = true; /* FIXME cleanup this crap */
    Clients[client->Fd] = client;

    return OK;
}

static void StartShutdown() {
    ShutdownPortod = true;
    ShutdownStart = GetCurrentTimeMs();
    ShutdownDeadline = ShutdownStart + config().daemon().portod_shutdown_timeout() * 1000;

    /* Stop accepting new clients */
    EpollLoop->RemoveSource(PORTO_SK_FD);

    /* Kick idle clients */
    for (auto it = Clients.begin(); it != Clients.end();) {
        auto client = it->second;

        if (client->IsBlockShutdown()) {
            L_SYS("Client blocks shutdown: {}", client->Id);
            ++it;
        } else {
            client->CloseConnection();
            it = Clients.erase(it);
        }
    }
}

static void ServerLoop() {
    TError error;

    auto AcceptSource = std::make_shared<TEpollSource>(PORTO_SK_FD);
    error = EpollLoop->AddSource(AcceptSource);
    if (error) {
        L_ERR("Can't add RPC server fd to epoll: {}", error);
        return;
    }

    auto MasterSource = std::make_shared<TEpollSource>(REAP_EVT_FD);
    error = EpollLoop->AddSource(MasterSource);
    if (error) {
        L_ERR("Can't add master fd to epoll: {}", error);
        return;
    }

    /* Don't disturb threads. Deliver signals via signalfd. */
    int sigFd = SignalFd();

    auto sigSource = std::make_shared<TEpollSource>(sigFd);
    error = EpollLoop->AddSource(sigSource);
    if (error) {
        L_ERR("Can't add sigSource to epoll: {}", error);
        return;
    }

    if (config().daemon().enable_nbd()) {
        error = StartNbd();
        if (error) {
            L_ERR("Can't start nbd: {}", error);
            return;
        }
    }
    StartStatFsLoop();
    StartRpcQueue();
    TStorage::StartAsyncRemover();
    EventQueue->Start();

    error = StartMetricsServer(PORTO_METRICS_SOCKET_PATH);
    if (error)
        L_WRN("Failed start metrics server: {}", error);

    if (config().daemon().log_rotate_ms()) {
        TEvent ev(EEventType::RotateLogs);
        EventQueue->Add(config().daemon().log_rotate_ms(), ev);
    }

    std::vector<struct epoll_event> events;

    while (true) {
        error = EpollLoop->GetEvents(events, 1000);
        if (error) {
            L_ERR("epoll error {}", error);
            goto exit;
        }

        if (RecvExitEvents(REAP_EVT_FD))
            goto exit;

        for (auto ev: events) {
            auto source = EpollLoop->GetSource(ev.data.fd);
            if (!source)
                continue;

            if (source->Fd == sigFd) {
                struct signalfd_siginfo sigInfo;

                if (read(sigFd, &sigInfo, sizeof sigInfo) != sizeof sigInfo) {
                    L_ERR("SignalFd read failed");
                    continue;
                }

                PrintSignalInfo(sigInfo);
                switch (sigInfo.ssi_signo) {
                case SIGINT:
                    DiscardState = true;
                    L_SYS("Shutdown...");
                    StartShutdown();
                    break;
                case SIGTERM:
                    L_SYS("Shutdown...");
                    StartShutdown();
                    break;
                case SIGHUP:
                    L_SYS("Updating...");
                    StartShutdown();
                    break;
                case SIGUSR1:
                    OpenLog(PORTO_LOG);
                    break;
                case SIGUSR2:
                    DumpMallocInfo();
                    TContainer::DumpLocks();
                    break;
                case SIGCHLD:
                    if (!TTask::Deliver(sigInfo.ssi_pid, sigInfo.ssi_code, sigInfo.ssi_status)) {
                        TEvent e(EEventType::ChildExit);
                        e.Exit.Pid = sigInfo.ssi_pid;
                        e.Exit.Status = sigInfo.ssi_status;
                        EventQueue->Add(0, e);
                    }
                    break;
                default:
                    L_WRN("Unexpected signal: {}", sigInfo.ssi_signo);
                    break;
                }
            } else if (source->Fd == PORTO_SK_FD) {
                error = AcceptConnection(source->Fd);
                if (error && Verbose)
                    L_SYS("Cannot accept connection: {}", error);
            } else if (source->Fd == REAP_EVT_FD) {
                // we handled all events from the master before events
                // from the clients (so clients see updated view of the
                // world as soon as possible)
                continue;
            } else if (source->Flags & EPOLL_EVENT_MEM) {
                auto container = source->Container.lock();

                if (!container) {
                    L_WRN("Container not found for OOM fd {}", source->Fd);
                    EpollLoop->StopInput(source->Fd);
                } else {
                    container->CollectMemoryEvents(source->Fd);
                    if (container->OomIsFatal && container->OomEvents > 0) {
                        TEvent e(EEventType::OOM, container);
                        EventQueue->Add(0, e);
                    }
                }
            } else if (Clients.find(source->Fd) != Clients.end()) {
                auto client = Clients[source->Fd];
                error = client->Event(ev.events);
                if (error) {
                    Clients.erase(source->Fd);
                    client->CloseConnection();
                }
            } else {
                L_WRN("Unknown event {}", source->Fd);
                EpollLoop->RemoveSource(source->Fd);
            }
        }

        if (ShutdownPortod) {
            if (Clients.empty()) {
                L_SYS("All clients are gone");
                break;
            }
            if (int64_t(ShutdownDeadline - GetCurrentTimeMs()) < 0) {
                L_SYS("Shutdown timeout exceeded");
                TContainer::DumpLocks();
                break;
            }
        }
    }

exit:

    for (auto c: Clients)
        c.second->CloseConnection(true);
    Clients.clear();
    NeedStopHelpers = true;

    L_SYS("Stop threads...");
    EventQueue->Stop();
    StopRpcQueue();
    StopStatFsLoop();
    TStorage::StopAsyncRemover();
}

static TError NetLimitSoftInitializeUlimit(TUlimit &ulimit) {
    TError error;

    if (config().network().network_limit_soft_bpf_elf_path().empty())
        return OK;

    if (CompareVersions(config().linux_version(), "5.11") >= 0)
        return OK;

    // we need to bump MEMLOCK to infinity for older kernels for bpf to work

    L_SYS("Bumping RLIMIT_MEMLOCK to RLIM_INFINITY to make bpf machinery work on kernel versions below 5.11");
    ulimit.Set(RLIMIT_MEMLOCK, RLIM_INFINITY, RLIM_INFINITY);

    error = ulimit.Apply();
    if (error)
        return error;

    error = ulimit.Load();
    if (error)
        return error;

    return OK;
}

static TError TuneLimits() {
    TUlimit ulimit;
    TError error;

    /*
     * two FDs for each container: OOM event and netlink
     * twenty for each thread
     * one for each client
     * plus some extra
     */
    int maxFd = config().container().max_total() * 2 + NR_SUPERUSER_CONTAINERS * 2 +
                (config().daemon().ro_threads() + config().daemon().rw_threads() + config().daemon().io_threads() +
                 config().daemon().vl_threads()) *
                    20 +
                config().daemon().max_clients() + NR_SUPERUSER_CLIENTS + 10000;

    L_SYS("Estimated portod file descriptor limit: {}", maxFd);

    ulimit.Set(RLIMIT_NOFILE, maxFd, maxFd);

    /*
     * Old make set unlimited stack
     */
    ulimit.Set(RLIMIT_STACK, 8 << 20, RLIM_INFINITY);

    error = ulimit.Apply();
    if (error)
        return error;

    error = ulimit.Load();
    if (error)
        return error;

    error = NetLimitSoftInitializeUlimit(ulimit);
    if (error)
        return error;

    for (auto &res: ulimit.Resources)
        L_SYS("Ulimit {}", res.Format());

    return OK;
}

static TError CreateRootContainer() {
    TError error;

    error = TContainer::Create(ROOT_CONTAINER, RootContainer);
    if (error)
        return error;

    PORTO_ASSERT(RootContainer->Id == ROOT_CONTAINER_ID);
    PORTO_ASSERT(RootContainer->IsRoot());

    RootContainer->Isolate = false;

    error = RootContainer->Ulimit.Parse(config().container().default_ulimit());
    if (error)
        return error;

    uint64_t pids_max, threads_max;
    std::string str;
    if (!GetSysctl("kernel.pid_max", str) && !StringToUint64(str, pids_max) && !GetSysctl("kernel.threads-max", str) &&
        !StringToUint64(str, threads_max)) {
        uint64_t lim = std::min(pids_max, threads_max) / 2;
        L_SYS("Default nproc ulimit: {}", lim);
        RootContainer->Ulimit.Set(TUlimit::GetType("nproc"), lim, lim, false);
    }

    error = RootContainer->Devices.InitDefault();
    if (error)
        return error;

    error = SystemClient.LockContainer(RootContainer);
    if (error)
        return error;

    error = RootContainer->Start();
    if (error)
        return error;

    error = ContainerIdMap.GetAt(DEFAULT_CONTAINER_ID);
    if (error)
        return error;

    error = ContainerIdMap.GetAt(LEGACY_CONTAINER_ID);
    if (error)
        return error;

    SystemClient.ReleaseContainer();

    TNetwork::SyncResolvConf();

    return OK;
}

static void CleanupWorkdir() {
    TPath temp(PORTO_WORKDIR);
    std::vector<std::string> list;
    TError error;

    error = temp.ReadDirectory(list);
    if (error)
        L_ERR("Cannot list temp dir: {}", error);

    for (auto &name: list) {
        auto it = Containers.find(name);
        if (it != Containers.end() && it->second->State != EContainerState::Stopped)
            continue;
        TPath path = temp / name;
        error = path.RemoveAll();
        if (error)
            L_WRN("Cannot remove workdir {}: {}", path, error);
    }
}

static void DestroyContainers(bool weak) {
    std::list<std::shared_ptr<TVolume>> unlinked;

    SystemClient.LockContainer(RootContainer);

    /* leaves first */
    for (auto &ct: RootContainer->Subtree()) {
        if (ct->IsRoot() || (weak && !ct->IsWeak))
            continue;

        TError error = ct->Destroy(unlinked);
        if (error)
            L_ERR("Cannot destroy container {}: {}", ct->Name, error);
    }

    SystemClient.ReleaseContainer();

    TVolume::DestroyUnlinked(unlinked);
}

static void NetLimitSoftInitialize() {
    if (config().network().network_limit_soft_bpf_elf_path().empty()) {
        L_SYS(
            "Setting up netlimit soft... no `network {{ network_limit_soft_bpf_elf_path: <value> }}` set in the portod "
            "config");
        return;
    }

    const std::string path = config().network().network_limit_soft_bpf_elf_path();

    L_SYS("Setting up netlimit soft... bpf elf path '{}'", path);
    TError error = NetLimitSoft.Setup(path);
    if (error)
        L_ERR("Cannot setup netlimit soft {}", error);
}

void PrepareServer() {
    TError error;

    SetDieOnParentExit(SIGKILL);

    ResetStatistics();

    Statistics->PortoStarted = GetCurrentTimeMs();

    SetProcessName(PORTOD_NAME);

    OpenLog(PORTO_LOG);
    if (!LogFile)
        FatalError("log is not opened");

    ServerPid = getpid();
    error = ServerPidFile.Save(ServerPid);
    if (error)
        FatalError("Cannot save pid", error);

    ReadConfigs();

    for (const auto &seccompProfile: config().container().seccomp_profiles()) {
        TSeccompProfile p;
        auto error = p.Parse(seccompProfile.profile());
        if (error) {
            L_ERR("Failed parse {} seccomp profile", seccompProfile.name());
            continue;
        }
        SeccompProfiles[seccompProfile.name()] = std::move(p);
    }

    SupportCgroupNs = CompareVersions(config().linux_version(), "4.6") >= 0;
    if (SupportCgroupNs) {
        EnableRwCgroupFs = config().container().enable_rw_cgroupfs();
        EnableOsModeCgroupNs = EnableRwCgroupFs || config().container().use_os_mode_cgroupns();
    }
    RequestHandlingDelayMs = config().daemon().request_handling_delay_ms();

    for (const auto &extraProp: config().container().extra_properties()) {
        ExtraProperty properties;
        properties.Filter = extraProp.filter();

        for (const auto &prop: extraProp.properties()) {
            const auto &propName = prop.name();
            // the second condition allows checking indexed properties, e.g. capabilitiesj
            if (SupportedExtraProperties.find(propName) == SupportedExtraProperties.end() &&
                SupportedExtraProperties.find(propName.substr(0, propName.find('['))) == SupportedExtraProperties.end())
            {
                L_ERR("Extra property {} not supported", propName);
                Statistics->Fatals++;
                continue;
            }
            properties.Properties.push_back({propName, prop.value()});
        }
        if (!properties.Properties.empty())
            ExtraProperties.emplace_back(properties);
    }

    if (config().daemon().enable_fuse()) {
        error = RunCommand({"modprobe", "fuse"});
        if (error)
            FatalError("Cannot load fuse module: {}", error);
    }

    InitPortoGroups();
    InitCapabilities();
    InitIpcSysctl();
    InitProcBaseDirs();
    TNetwork::InitializeConfig();

    L_SYS("Portod config:\n{}", config().DebugString());

    SetPtraceProtection(config().daemon().ptrace_protection());

    error = TuneLimits();
    if (error)
        FatalError("Cannot set correct limits", error);

    if (fcntl(PORTO_SK_FD, F_SETFD, FD_CLOEXEC) < 0)
        FatalError("Can't set close-on-exec flag on PORTO_SK_FD: {}", TError::System("fcntl"));

    if (fcntl(REAP_EVT_FD, F_SETFD, FD_CLOEXEC) < 0)
        FatalError("Can't set close-on-exec flag on REAP_EVT_FD: {}", TError::System("fcntl"));

    if (fcntl(REAP_ACK_FD, F_SETFD, FD_CLOEXEC) < 0)
        FatalError("Can't set close-on-exec flag on REAP_ACK_FD: {}", TError::System("fcntl"));

    umask(0);

    error = SetOomScoreAdj(0);
    if (error)
        FatalError("Can't adjust OOM score", error);

    error = CgroupDriver.Initialize();
    if (error)
        FatalError("Cannot initalize cgroup driver: {}", error);

    InitContainerProperties();
    TStorage::Init();

    ContainersKV = TPath(PORTO_CONTAINERS_KV);
    error = TKeyValue::Mount(ContainersKV);
    if (error)
        FatalError("Cannot mount containers keyvalue", error);

    VolumesKV = TPath(PORTO_VOLUMES_KV);
    error = TKeyValue::Mount(VolumesKV);
    if (error)
        FatalError("Cannot mount volumes keyvalue", error);

    if (config().daemon().enable_nbd()) {
        NbdKV = TPath(PORTO_NBD_KV);
        error = TKeyValue::Mount(NbdKV);
        if (error)
            FatalError("Cannot mount nbd keyvalue", error);
    }

    if (error)
        FatalError("Cannot mount secure binds", error);

    TPath root("/");
    error = root.Chdir();
    if (error)
        FatalError("Cannot chdir to /", error);

    // We want propagate mounts into containers
    error = root.Remount(MS_SHARED | MS_REC);
    if (error)
        FatalError("Cannot remount / recursively as shared", error);

    TPath tracefs = "/sys/kernel/tracing";
    if (config().container().enable_tracefs() && tracefs.Exists()) {
        error = tracefs.Mount("none", "tracefs", MS_NOEXEC | MS_NOSUID | MS_NODEV, {"mode=755"});
        if (error && error.Errno != EBUSY)
            L_SYS("Cannot mount tracefs: {}", error);
    }

    EpollLoop = std::unique_ptr<TEpollLoop>(new TEpollLoop());
    EventQueue = std::unique_ptr<TEventQueue>(new TEventQueue());

    error = EpollLoop->Create();
    if (error)
        FatalError("Cannot initialize epoll", error);

    error = StartAsyncUmounter();
    if (error)
        FatalError("Cannot start async umount worker", error);

    TPath tmp_dir(PORTO_WORKDIR);
    if (!tmp_dir.IsDirectoryFollow()) {
        (void)tmp_dir.Unlink();
        error = tmp_dir.MkdirAll(0755);
        if (error)
            FatalError("Cannot create tmp_dir", error);
    }

    NetLimitSoftInitialize();

    {
        std::map<const std::string, const std::string> labels;
        if (config().has_metrics()) {
            for (auto &pair: config().metrics().global_tags())
                labels.emplace(pair.first, pair.second);
        }
        MetricsRegistry = std::unique_ptr<TMetricsRegistry>(new TMetricsRegistry(labels));
        MetricsRegistry->Version.WithLabels({{"version", PORTO_VERSION}}) = 1;
    }
}

class TRestoreWorker: public TWorker<TKeyValue *> {
    std::atomic<size_t> WorkSize;
    std::unordered_map<std::string, std::vector<TKeyValue *>> ChildMap;

protected:
    bool Handle(TKeyValue *&node) override {
        TClient client("<restore>");
        client.ClientContainer = RootContainer;
        client.StartRequest();

        std::shared_ptr<TContainer> ct;
        auto error = TContainer::Restore(*node, ct);
        if (error) {
            L_ERR("Cannot restore {}: {}", node->Name, error);
            Statistics->ContainerLost++;
            MetricsRegistry->ContainerLost++;
            node->Path.Unlink();
        }
        client.FinishRequest();

        // If k is not presented in map than map[k] is write operation.
        // Use find here to avoid write to map from multiple threads.
        auto it = ChildMap.find(node->Name);
        if (it != ChildMap.end()) {
            for (auto child: it->second)
                Push(std::move(child));
        }
        if (!--WorkSize)
            Shutdown();

        return true;
    }

    TKeyValue *Pop() override {
        auto node = Queue.front();
        Queue.pop();
        return node;
    }

public:
    TRestoreWorker(size_t workers, std::unordered_map<std::string, std::vector<TKeyValue *>> childMap)
        : TWorker("portod-RS", workers),
          WorkSize(0),
          ChildMap(childMap)
    {}

    void Execute() {
        Start();
        Join();
    }

    void Push(TKeyValue *&&node) override {
        ++WorkSize;
        TWorker::Push(std::move(node));
    }
};

static void RestoreContainers() {
    TIdMap ids(4, CONTAINER_ID_MAX - 4);
    std::list<TKeyValue> nodes;

    TError error = TKeyValue::ListAll(ContainersKV, nodes);
    if (error)
        FatalError("Cannot list container kv", error);

    for (auto node = nodes.begin(); node != nodes.end();) {
        error = node->Load();
        if (!error) {
            if (!node->Has(P_RAW_ID))
                error = TError("id not found");
            if (!node->Has(P_RAW_NAME))
                error = TError("name not found");
            if (!error && (StringToInt(node->Get(P_RAW_ID), node->Id) || (node->Id > 3 && ids.GetAt(node->Id))))
                node->Id = 0;
        }
        if (error) {
            L_ERR("Cannot load {}: {}", node->Path, error);
            (void)node->Path.Unlink();
            node = nodes.erase(node);
            continue;
        }
        /* name for dependency building */
        node->Name = node->Get(P_RAW_NAME);
        ++node;
    }

    std::unordered_map<std::string, std::vector<TKeyValue *>> childMap;

    for (auto &node: nodes) {
        if (node.Name[0] != '/' && !node.Id) {
            error = ids.Get(node.Id);
            if (!error) {
                L("Replace container {} id {}", node.Name, node.Id);
                TPath path = ContainersKV / std::to_string(node.Id);
                node.Path.Rename(path);
                node.Path = path;
                node.Set(P_RAW_ID, std::to_string(node.Id));
                node.Save();
            }
        }

        if (node.Name[0] == '/')
            continue;

        auto parent = TContainer::ParentName(node.Name);

        auto it = childMap.find(parent);
        if (it == childMap.end()) {
            childMap[parent] = {&node};
        } else
            it->second.push_back(&node);
    }

    auto &slots = childMap["/"];
    if (slots.empty())
        return;

    TRestoreWorker restoreQueue(std::max(1, get_nprocs() / 4), childMap);

    for (auto node: slots) {
        restoreQueue.Push(std::move(node));
    }
    restoreQueue.Execute();
}

void RestoreState() {
    SystemClient.StartRequest();

    auto error = CreateRootContainer();
    if (error)
        FatalError("Cannot create root container", error);

    SystemClient.ClientContainer = RootContainer;

    L_SYS("Restore containers...");
    RestoreContainers();

    L_SYS("Restore statistics...");
    TContainer::SyncPropertiesAll();

    L_SYS("Restore volumes...");
    TVolume::RestoreAll();

    DestroyContainers(true);

    if (DiscardState) {
        DiscardState = false;

        L_SYS("Destroy containers...");
        DestroyContainers(false);

        L_SYS("Destroy volumes...");
        TVolume::DestroyAll();
    }

    SystemClient.FinishRequest();

    L_SYS("Cleanup cgroup...");
    CgroupDriver.CleanupCgroups();

    L_SYS("Cleanup workdir...");
    CleanupWorkdir();

    L_SYS("Restore complete. time={} ms", GetCurrentTimeMs() - Statistics->PortoStarted);
}

void CleanupServer() {
    TError error;

    if (DiscardState) {
        DiscardState = false;

        SystemClient.StartRequest();

        L_SYS("Stop containers...");

        SystemClient.LockContainer(RootContainer);
        error = RootContainer->Stop(0);
        SystemClient.ReleaseContainer();
        if (error)
            L_ERR("Failed to stop root container and its children {}", error);

        L_SYS("Destroy containers...");
        DestroyContainers(false);

        L_SYS("Destroy volumes...");
        TVolume::DestroyAll();

        std::list<std::shared_ptr<TVolume>> unlinked;

        SystemClient.LockContainer(RootContainer);
        error = RootContainer->Destroy(unlinked);
        SystemClient.ReleaseContainer();
        if (error)
            L_ERR("Cannot destroy root container{}", error);

        TVolume::DestroyUnlinked(unlinked);

        SystemClient.FinishRequest();

        RootContainer = nullptr;

        error = ContainersKV.UmountAll();
        if (error)
            L_ERR("Can't destroy key-value storage: {}", error);

        error = VolumesKV.UmountAll();
        if (error)
            L_ERR("Can't destroy volume key-value storage: {}", error);

        if (config().daemon().enable_nbd()) {
            error = NbdKV.UmountAll();
            if (error)
                L_ERR("Can't destroy volume key-value storage: {}", error);
        }
    }

    StopMetricsServer();

    StopAsyncUmounter();
    if (config().daemon().enable_nbd())
        StopNbd();

    ServerPidFile.Remove();

    // move master to root, otherwise older version will kill itself
    CgroupDriver.FreezerSubsystem->RootCgroup()->Attach(MasterPid);

    L_SYS("Shutdown complete. time={} ms", GetCurrentTimeMs() - ShutdownStart);
}

int Server() {
    uint64_t start = GetCurrentTimeMs();
    PrepareServer();
    RestoreState();
    Statistics->RestoreTime += GetCurrentTimeMs() - start;
    ServerLoop();

    CleanupServer();

    return EXIT_SUCCESS;
}
