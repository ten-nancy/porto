#pragma once

#include <atomic>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "cgroup.hpp"
#include "device.hpp"
#include "network.hpp"
#include "property.hpp"
#include "seccomp.hpp"
#include "stream.hpp"
#include "task.hpp"
#include "util/idmap.hpp"
#include "util/mutex.hpp"
#include "util/task.hpp"
#include "util/unix.hpp"

class TEpollSource;
class TEvent;
class TClient;
class TVolume;
class TVolumeLink;
class TKeyValue;
struct TBindMount;
class TVmStat;

struct TEnv;

enum class EContainerState {
    Stopped,
    Dead,
    Respawning,
    Starting,
    Running,
    Stopping,
    Paused,
    Meta,
    Destroyed,
};

enum class ECpuSetType {
    Inherit,
    Node,
    Absolute,
};

enum class ECgroupFs {
    None,
    Ro,
    Rw,
};

enum class EMemoryLockPolicy {
    Disabled = 0,
    Mlockall = 1,
    Executable = 2,
    Xattr = 3,
};

struct ExtraProperty {
    std::string Filter;

    struct Property {
        std::string Name;
        std::string Value;
    };
    std::vector<Property> Properties;
};

class TProperty;

class TContainer: public std::enable_shared_from_this<TContainer>, public TNonCopyable {
    friend class TProperty;

    int StateLocked = 0;
    int ActionLocked = 0;
    int SubtreeRead = 0;
    bool PendingWrite = false;
    pid_t LastStatePid = 0;
    pid_t LastActionPid = 0;

    TFile OomEvent;
    TFile OomKillEvent;
    std::vector<std::shared_ptr<TEpollSource>> Sources;

    // data
    TError UpdateSoftLimit();
    void SetState(EContainerState next);

    TError ApplyUlimits();
    TError ApplySchedPolicy();
    TError ApplyIoPolicy() const;
    TError ApplyDeviceConf();
    TError ApplyDynamicProperties(bool onRestore = false);

    TError PrepareOomMonitor();
    void ShutdownOom();
    TError PrepareCgroups(bool onRestore = false);
    TError PrepareTask(TTaskEnv &TaskEnv);

    TError PrepareResources();
    TError FreeResources(bool ignore = true);

    TError PrepareRuntimeResources();
    void FreeRuntimeResources();

    void Reap(bool oomKilled);
    void Exit(int status, bool oomKilled);

    TError BuildCpuTopology();
    TError ReserveCpus(unsigned nr_threads, unsigned nr_cores, TBitMap &threads, TBitMap &cores);
    void SetAffinity(const TBitMap &affinity);
    TError ApplyCpuSet();

    void UpdateJailCpuStateLocked(const TBitMap &affinity, bool release = false);
    TError NextJailCpu(TBitMap &affinity, int node = -1);
    TError JailCpus();
    void UnjailCpus(const TBitMap &affinity);
    void UnjailCpusLocked(const TBitMap &affinity);

    TError SetCpuGuarantee(uint64_t guarantee);
    TError ApplyCpuGuarantee();
    TError ApplyCpuShares();

    TError SetCpuLimit(uint64_t limit, uint64_t period);
    TError ApplyCpuLimit();
    TError ApplyExtraProperties();

    void CollectOoms();
    bool CollectOomsV1();
    void CollectOomsV2();

    void CollectOomKills();
    uint64_t GetOomKillsV1();
    uint64_t GetOomKillsV2();

public:
    const std::shared_ptr<TContainer> Parent;
    const int Level;  // 0 for root
    const int Id;
    const std::string Name;
    const std::string Slug;
    const std::string FirstName;

    EContainerState State = EContainerState::Stopped;
    std::atomic<int> RunningChildren;
    std::atomic<int> StartingChildren;

    bool HasResources() const {
        return State != EContainerState::Stopped && State != EContainerState::Destroyed;
    }

    bool IsRunningOrMeta() {
        return IsRunningOrMeta(State);
    }

    bool IsRunningOrMeta(EContainerState state) {
        return state == EContainerState::Running || state == EContainerState::Meta;
    }

    /* protected with ContainersMutex */
    std::list<std::shared_ptr<TContainer>> Children;

    bool PropSet[(int)EProperty::NR_PROPERTIES];
    bool PropDirty[(int)EProperty::NR_PROPERTIES];
    uint64_t Controllers = 0;
    uint64_t RequiredControllers = 0;
    bool LinkMemoryWritebackBlkio = false;
    TCred OwnerCred;
    TCred TaskCred;
    std::vector<std::string> OwnerContainers;
    std::vector<std::string> EnabledExtraProperties;
    std::string Command;
    std::string CoreCommand;
    TTuple CommandArgv;
    TPath Cwd;
    TStdStream Stdin, Stdout, Stderr;
    std::string Root;
    bool RootRo;
    mode_t Umask;
    bool BindDns = false;  /* deprecated */
    bool Isolate;          /* New pid/ipc/utc/env namespace */
    bool OsMode = false;   /* Start as init process */
    bool HostMode = false; /* Preserve host capabilites */
    bool JobMode = false;  /* Process group */
    bool UserNs = false;
    TCred UserNsCred;
    bool UnshareOnExec = false;
    ECgroupFs CgroupFs = ECgroupFs::None;
    bool Fuse = false;
    TSeccompProfile Seccomp;
    std::string SeccompName;
    TSessionInfo SessionInfo;

    TMultiTuple NetProp;
    bool NetIsolate; /* Create new network namespace */
    bool NetInherit; /* Use parent network namespace */
    TMultiTuple NetXVlanSettings;

    std::string Hostname;
    std::string EnvCfg;
    std::string EnvSecret;

    std::vector<TBindMount> BindMounts;
    std::map<TPath, TPath> Symlink;

    TMultiTuple IpList;
    TMultiTuple IpLimit;
    std::string IpPolicy;

    /* CapLimit >= CapBound >= CapAmbient */
    /* Host mode and owner is root are exceptions for the first condition. */
    TCapabilities CapAmbient; /* get at start */
    TCapabilities CapLimit;   /* upper limit set by user */
    TCapabilities CapBound;   /* actual bounding set and can be set as ambient */
    TCapabilities CapExtra;   /* from extra properties */
    TMultiTuple DefaultGw;
    std::string ResolvConf;
    std::string EtcHosts;
    TDevices Devices;
    // We rely on restore to populate this,
    // so we dont need to dump it to KV.
    std::set<TPath> DevicesPath;
    TDevices FuseDevices;
    bool DevicesExplicit = false;
    TStringMap Sysctl;

    time_t RealCreationTime;
    time_t RealStartTime = 0;
    time_t RealDeathTime = 0;

    TError StartError;
    uint64_t CreationTime = 0;
    uint64_t StartTime = 0;
    uint64_t DeathTime = 0;
    uint64_t AgingTime;
    uint64_t ChangeTime = 0;

    TUlimit Ulimit;

    std::string NsName;

    uint64_t MemLimit = 0;
    uint64_t MemGuarantee = 0;
    uint64_t NewMemGuarantee = 0;
    int64_t MemSoftLimit = 0;
    EMemoryLockPolicy MemLockPolicy = EMemoryLockPolicy::Disabled;
    uint64_t AnonMemLimit = 0;
    uint64_t HugetlbLimit = 0;
    uint64_t ThreadLimit = 0;
    uint64_t DirtyMemLimit = 0;
    uint64_t DirtyMemLimitBound = 0;

    bool AnonOnly = false;
    bool RechargeOnPgfault = false;
    bool PressurizeOnDeath = false;

    std::string IoPolicy;
    int IoPrio;
    double IoWeight = 1;

    TUintMap IoBpsLimit;
    TUintMap IoBpsGuarantee;
    TUintMap IoOpsLimit;
    TUintMap IoOpsGuarantee;
    std::string CpuPolicy;

    int SchedPolicy;
    int SchedPrio;
    int SchedNice;
    bool SchedNoSmt = false;
    bool ExtSchedIdle = false;

    uint64_t CpuLimit = 0;
    uint64_t CpuLimitBound = 0;
    uint64_t CpuLimitCur = 0;
    uint64_t CpuPeriod;

    uint64_t CpuGuarantee = 0;
    uint64_t CpuGuaranteeBound = 0;
    uint64_t CpuGuaranteeCur = 0;
    uint64_t CpuWeight = 100;

    /* Under CpuAffinityMutex */
    ECpuSetType CpuSetType = ECpuSetType::Inherit;
    int CpuSetArg = 0;
    int CpuJail = 0;
    int NewCpuJail = 0;
    TBitMap CpuAffinity;

    bool AutoRespawn = false;
    int64_t RespawnLimit = -1;
    int64_t RespawnCount = 0;
    uint64_t RespawnDelay;

    TError MayRespawn();
    TError Respawn();
    TError ScheduleRespawn();

    TStringMap Labels;
    std::string Private;
    EAccessLevel AccessLevel;
    std::atomic<int> ClientsCount;
    std::atomic<uint64_t> ContainerRequests;

    bool IsWeak = false;
    bool OomIsFatal = true;
    int OomScoreAdj = 0;
    std::atomic<uint64_t> OomEvents;
    bool OomKilled = false;
    std::atomic<uint64_t> OomKills{0};
    std::atomic<uint64_t> OomKillsTotal{0};
    int ExitStatus = 0;

    struct {
        bool TaintCounted;
        bool RootOnLoop;
        bool BindWithSuid;
    } TaintFlags;

    /* OOM processing */
    void CollectMemoryEvents(int fd = -1);

    TPath RootPath; /* path in host namespace */
    std::vector<std::string> PlacePolicy;

    TTask Task;
    pid_t TaskVPid;
    TTask WaitTask;
    TTask SeizeTask;

    /* Protected with container state lock */
    std::shared_ptr<TNetwork> Net;

    /* Protected with NetStateMutex and container lock */
    TNetClass NetClass;

    uint64_t NetLimitSoftValue;

    TNetStat SockStat;
    std::unordered_map<ino_t, TSockStat> SocketsStats;

    uint32_t CoredumpFilter;

    TPath GetCwd() const;
    int GetExitCode() const;

    TPath WorkDir() const;
    TError CreateWorkDir() const;
    void RemoveWorkDir() const;

    bool IsMeta() const {
        return Command.empty() && !HasProp(EProperty::COMMAND_ARGV);
    }

    bool InUserNs() const {
        return !UserNsCred.IsUnknown();
    }

    TContainer(std::shared_ptr<TContainer> parent, int id, const std::string &name);
    ~TContainer();

    void Register();
    void Unregister();

    bool HasProp(EProperty prop) const {
        return PropSet[(int)prop];
    }

    void SetProp(EProperty prop) {
        PropSet[(int)prop] = true;
        PropDirty[(int)prop] = true;
    }

    void ClearProp(EProperty prop) {
        PropSet[(int)prop] = false;
        PropDirty[(int)prop] = true;
    }

    void SetPropDirty(EProperty prop) {
        PropDirty[(int)prop] = true;
    }

    bool TestPropDirty(EProperty prop) const {
        return PropDirty[(int)prop];
    }

    bool TestClearPropDirty(EProperty prop) {
        if (!PropDirty[(int)prop])
            return false;
        PropDirty[(int)prop] = false;
        return true;
    }

    template <typename... T>
    bool TestPropsDirty(T... props) {
        bool dirty = false;
        for (auto prop: {props...})
            dirty |= TestPropDirty(prop);
        return dirty;
    }

    template <typename... T>
    bool TestClearPropsDirty(T... props) {
        bool dirty = false;
        for (auto prop: {props...})
            dirty |= TestClearPropDirty(prop);
        return dirty;
    }

    TError CanSetSeccomp() const;
    TError SetSeccomp(const seccomp::TProfile &profile);
    TError SetSeccomp(const std::string &name);

    std::string GetPortoNamespace(bool write = false) const;

    TError LockAction(std::unique_lock<std::mutex> &containers_lock, bool shared = false);
    TError LockActionShared(std::unique_lock<std::mutex> &containers_lock) {
        return LockAction(containers_lock, true);
    }
    void UnlockAction(bool containers_locked = false);
    bool IsActionLocked(bool shared = false);
    void DowngradeActionLock();
    void UpgradeActionLock();

    void LockStateRead();
    void LockStateWrite();
    void DowngradeStateLock();
    void UnlockState();
    bool IsStateLockedRead() {
        return StateLocked != 0;
    }
    bool IsStateLockedWrite() {
        return StateLocked == -1;
    }

    static void DumpLocks();

    TTuple Taint();

    TUlimit GetUlimit() const;
    void SanitizeCapabilities();
    void SanitizeCapabilitiesAll();

    TError CheckMemGuarantee() const;
    uint64_t GetTotalMemGuarantee(bool containers_locked = false) const;
    uint64_t GetMemLimit(bool effective = true) const;
    uint64_t GetAnonMemLimit(bool effective = true) const;
    TError ChooseDirtyMemLimit();
    void PropagateDirtyMemLimit();

    void PropogateCpuGuarantee();
    void PropogateCpuLimit();

    bool IsRoot() const {
        return !Level;
    }
    bool IsChildOf(const TContainer &ct) const;

    std::list<std::shared_ptr<TContainer>> Subtree();
    std::list<std::shared_ptr<TContainer>> Childs();

    std::shared_ptr<TContainer> GetParent() const;

    bool HasPidFor(const TContainer &ct) const;
    TError GetPidFor(pid_t pidns, pid_t &pid) const;

    TError GetThreadCount(uint64_t &count) const;
    TError GetProcessCount(uint64_t &count) const;
    TError GetVmStat(TVmStat &stat) const;

    TError StartTask();
    TError StartParents();
    TError PrepareStart();
    TError Start();

    TError Stop(uint64_t timeout);
    TError Pause();
    TError Resume();
    TError Terminate(uint64_t deadline);
    TError Kill(int sig);
    TError Destroy(std::list<std::shared_ptr<TVolume>> &unlinked);

    /* Refresh cached counters */
    void SyncProperty(const std::string &name);
    static void SyncPropertiesAll();

    TError ApplyResolvConf() const;
    TError SetSymlink(const TPath &symlink, const TPath &target);

    TError SetControllers(uint64_t controllers);
    TError SetDeviceConf(const TDevices &devices, bool merge);
    TError EnableFuse(bool value);
    TError EnableControllers(uint64_t controllers);
    TError HasProperty(const std::string &property) const;
    TError GetProperty(const std::string &property, std::string &value) const;
    TError SetProperty(const std::string &property, const std::string &value);

    bool MatchLabels(const rpc::TStringMap &labels) const;

    TError Load(const rpc::TContainerSpec &spec, bool restoreOnError = false);
    void Dump(const std::vector<std::string> &props, std::unordered_map<std::string, std::string> &propsOps,
              rpc::TContainer &spec);

    /* Protected with ContainersLock */
    static TError ValidLabel(const std::string &label, const std::string &value);
    TError GetLabel(const std::string &label, std::string &value) const;
    void SetLabel(const std::string &label, const std::string &value);
    TError IncLabel(const std::string &label, int64_t &result, int64_t add = 1);

    void ForgetPid();
    void SyncState();
    TError Seize();

    TError Save(void);
    TError Load(const TKeyValue &node);

    void ChooseSchedPolicy();
    TBitMap GetNoSmtCpus();

    /* protected with VolumesLock and container lock */
    std::list<std::shared_ptr<TVolumeLink>> VolumeLinks;
    int VolumeMounts = 0;

    /* protected with VolumesLock */
    std::list<std::shared_ptr<TVolume>> OwnedVolumes;
    std::vector<std::string> RequiredVolumes;

    TError GetEnvironment(TEnv &env) const;

    TError ResolvePlace(TPath &place, bool strict = false) const;
    TDevices EffectiveDevices() const;
    TDevices EffectiveDevices(const TDevices &devices) const;

    static TError ValidName(const std::string &name, bool superuser);
    static std::string ParentName(const std::string &name);

    static std::string StateName(EContainerState state);
    static EContainerState ParseState(const std::string &name);

    static std::shared_ptr<TContainer> Find(const std::string &name, bool strict = true);
    static TError Find(const std::string &name, std::shared_ptr<TContainer> &ct, bool strict = true);
    static TError FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &ct, bool strict = true);

    static TError Create(const std::string &name, std::shared_ptr<TContainer> &ct);
    static TError Restore(const TKeyValue &kv, std::shared_ptr<TContainer> &ct);

    static void Event(const TEvent &event);

    struct TJailCpuState {
        std::vector<unsigned> Permutation;
        std::vector<unsigned> Usage;

        TJailCpuState(const std::vector<unsigned> &permutation, std::vector<unsigned> usage)
            : Permutation(permutation),
              Usage(usage)
        {}
    };

    static TJailCpuState GetJailCpuState();

private:
    TError SetupNetLimitSoft(uint64_t kbs);
};

extern MeasuredMutex ContainersMutex;
extern std::shared_ptr<TContainer> RootContainer;
extern std::map<std::string, std::shared_ptr<TContainer>> Containers;
extern TPath ContainersKV;
extern TIdMap ContainerIdMap;
extern std::unordered_map<std::string, TSeccompProfile> SeccompProfiles;

static inline std::unique_lock<std::mutex> LockContainers() {
    return ContainersMutex.UniqueLock();
}

extern std::mutex CpuAffinityMutex;

static inline std::unique_lock<std::mutex> LockCpuAffinity() {
    return std::unique_lock<std::mutex>(CpuAffinityMutex);
}
