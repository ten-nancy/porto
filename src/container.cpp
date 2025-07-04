#include "container.hpp"

#include <algorithm>
#include <climits>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <mutex>

#include "cgroup.hpp"
#include "client.hpp"
#include "config.hpp"
#include "device.hpp"
#include "epoll.hpp"
#include "event.hpp"
#include "filesystem.hpp"
#include "kvalue.hpp"
#include "network.hpp"
#include "portod.hpp"
#include "property.hpp"
#include "rpc.hpp"
#include "task.hpp"
#include "util/cred.hpp"
#include "util/log.hpp"
#include "util/proc.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "volume.hpp"
#include "waiter.hpp"

extern "C" {
#include <fcntl.h>
#include <linux/magic.h>
#include <sched.h>
#include <sys/fsuid.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <unistd.h>
}

static const std::string LOCK_ACTION("LockAction");
static const std::string UPGRADE_LOCK_ACTION("UpgradeLockAction");
static const std::string LOCK_STATE_READ("LockStateRead");
static const std::string LOCK_STATE_WRITE("LockStateWrite");

MeasuredMutex ContainersMutex("containers");
static std::condition_variable ContainersCV;
std::shared_ptr<TContainer> RootContainer;
std::map<std::string, std::shared_ptr<TContainer>> Containers;
TPath ContainersKV;
TIdMap ContainerIdMap(1, CONTAINER_ID_MAX);
std::vector<ExtraProperty> ExtraProperties;
std::unordered_map<std::string, TSeccompProfile> SeccompProfiles;

std::unordered_set<std::string> SupportedExtraProperties = {
    "cgroupfs", "command", "max_respawns", "userns", "unshare_on_exec", "resolv_conf", "capabilities",
};

std::mutex CpuAffinityMutex;
static bool HyperThreadingEnabled = false;    /* hyperthreading is disabled in vms and sandbox tests */
static std::vector<unsigned> NeighborThreads; /* cpu -> neighbor hyperthread */

static TBitMap NumaNodes;
static std::vector<TBitMap> NodeThreads;  /* numa node -> list of cpus */
static std::vector<unsigned> ThreadsNode; /* cpu -> numa node */

// PORTO-914
static std::vector<unsigned> JailCpuPermutation;        /* 0,8,16,24,1,9,17,25,2,10,18,26,... */
static std::vector<unsigned> CpuToJailPermutationIndex; /* cpu -> index in JailCpuPermutation */
static std::vector<unsigned>
    JailCpuPermutationUsage; /* how many containers are jailed at JailCpuPermutation[cpu] core */

static TError CommitSubtreeCpus(const TCgroup &root, std::list<std::shared_ptr<TContainer>> &subtree);

/* return true if index specified for property */
static bool ParsePropertyName(std::string &name, std::string &idx) {
    if (name.size() && name.back() == ']') {
        auto lb = name.find('[');

        if (lb != std::string::npos) {
            idx = name.substr(lb + 1);
            idx.pop_back();
            name = name.substr(0, lb);

            return true;
        }
    }

    return false;
}

TError TContainer::ValidName(const std::string &name, bool superuser) {
    if (name.length() == 0)
        return TError(EError::InvalidValue, "container path too short");

    unsigned path_max = superuser ? CONTAINER_PATH_MAX_FOR_SUPERUSER : CONTAINER_PATH_MAX;
    if (name.length() > path_max)
        return TError(EError::InvalidValue, "container path too long, limit is {}", path_max);

    if (name[0] == '/') {
        if (name == ROOT_CONTAINER)
            return OK;
        return TError(EError::InvalidValue, "container path starts with '/': " + name);
    }

    for (std::string::size_type first = 0, i = 0; i <= name.length(); i++) {
        switch (name[i]) {
        case '/':
        case '\0':
            if (i == first)
                return TError(EError::InvalidValue, "double/trailing '/' in container path: " + name);
            if (i - first > CONTAINER_NAME_MAX)
                return TError(EError::InvalidValue, "container name component too long, limit is " +
                                                        std::to_string(CONTAINER_NAME_MAX) + ": '" +
                                                        name.substr(first, i - first) + "'");
            if (name.substr(first, i - first) == SELF_CONTAINER)
                return TError(EError::InvalidValue, "container name 'self' is reserved");
            if (name.substr(first, i - first) == DOT_CONTAINER)
                return TError(EError::InvalidValue, "container name '.' is reserved");
            first = i + 1;
        case 'a' ... 'z':
        case 'A' ... 'Z':
        case '0' ... '9':
        case '_':
        case '-':
        case '@':
        case ':':
        case '.':
            /* Ok */
            break;
        default:
            return TError(EError::InvalidValue, "forbidden character " + StringFormat("%#x", (unsigned char)name[i]));
        }
    }

    return OK;
}

std::string TContainer::ParentName(const std::string &name) {
    auto sep = name.rfind('/');
    if (sep == std::string::npos)
        return ROOT_CONTAINER;
    return name.substr(0, sep);
}

std::shared_ptr<TContainer> TContainer::Find(const std::string &name, bool strict) {
    PORTO_LOCKED(ContainersMutex);

    if (strict) {
        auto it = Containers.find(name);
        if (it == Containers.end())
            return nullptr;
        return it->second;
    }

    // Find nearest parent if not strict
    std::string ctName = name;

    while (!ctName.empty()) {
        auto it = Containers.find(ctName);
        if (it != Containers.end())
            return it->second;

        auto pos = ctName.rfind('/');
        if (pos == std::string::npos)
            break;

        ctName = ctName.substr(0, pos);
    }

    return nullptr;
}

TError TContainer::Find(const std::string &name, std::shared_ptr<TContainer> &ct, bool strict) {
    ct = Find(name, strict);
    if (ct)
        return OK;
    return TError(EError::ContainerDoesNotExist, "container " + name + " not found");
}

TError TContainer::FindTaskContainer(pid_t pid, std::shared_ptr<TContainer> &ct, bool strict) {
    TError error;
    std::unique_ptr<const TCgroup> cg;

    if (CgroupDriver.Cgroup2Subsystem->IsDisabled())
        error = CgroupDriver.FreezerSubsystem->TaskCgroup(pid, cg);
    else
        error = CgroupDriver.Cgroup2Subsystem->TaskCgroup(pid, cg);
    if (error)
        return error;

    if (cg->GetName() == PORTO_DAEMON_CGROUP || cg->GetName() == PORTO_HELPERS_CGROUP)
        return TError(EError::HelperError, "Read-only access");

    std::string prefix = std::string(PORTO_CGROUP_PREFIX) + "/";
    std::string name = cg->GetName();
    std::replace(name.begin(), name.end(), '%', '/');

    auto containers_lock = LockContainers();

    if (!StringStartsWith(name, prefix))
        return TContainer::Find(ROOT_CONTAINER, ct);

    return TContainer::Find(name.substr(prefix.length()), ct, strict);
}

/*
  Lock subtree shared or exclusive. Ancestors are always locked in shared mode.
  Container is locked in shared mode if:
    * ActionLocked > 0 -- this container is locked in shared mode
      by #ActionLocked readers.
    * SubtreeRead  > 0 -- some descendant containers are locked in shared mode
      by #SubtreeRead readers.
  Container is locked in exclusive mode if ActionLocked = -1.

  PendingWrite flag is set when writer is waiting for the lock. If this flag is set
  no reader can acquire lock. This prevents write-starvation.
*/

TError TContainer::LockAction(std::unique_lock<std::mutex> &containers_lock, bool shared) {
    LockTimer timer(LOCK_ACTION);
    L_DBG("LockAction{} {}", (shared ? "Shared" : ""), Slug);

    while (1) {
        if (State == EContainerState::Destroyed) {
            L_DBG("Lock failed, {} was destroyed", Slug);
            return TError(EError::ContainerDoesNotExist, "Container was destroyed");
        }
        bool busy;
        if (shared)
            busy = ActionLocked < 0 || PendingWrite;
        else
            busy = ActionLocked || SubtreeRead;
        for (auto ct = Parent.get(); !busy && ct; ct = ct->Parent.get())
            busy = ct->PendingWrite || (shared ? ct->ActionLocked < 0 : ct->ActionLocked);
        if (!busy)
            break;
        if (!shared)
            PendingWrite = true;
        ContainersCV.wait(containers_lock);
    }
    PendingWrite = false;
    ActionLocked += shared ? 1 : -1;
    LastActionPid = GetTid();
    for (auto ct = Parent.get(); ct; ct = ct->Parent.get())
        ct->SubtreeRead++;
    return OK;
}

void TContainer::UnlockAction(bool containers_locked) {
    L_DBG("UnlockAction{} {}", (ActionLocked > 0 ? "Shared" : ""), Slug);
    if (!containers_locked)
        ContainersMutex.lock();
    for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
        PORTO_ASSERT(ct->SubtreeRead > 0);
        ct->SubtreeRead--;
    }
    PORTO_ASSERT(ActionLocked);
    ActionLocked += (ActionLocked > 0) ? -1 : 1;
    /* not so effective and fair but simple */
    ContainersCV.notify_all();
    if (!containers_locked)
        ContainersMutex.unlock();
}

bool TContainer::IsActionLocked(bool shared) {
    for (auto ct = this; ct; ct = ct->Parent.get())
        if (ct->ActionLocked < 0 || (shared && ct->ActionLocked > 0))
            return true;
    return false;
}

void TContainer::DowngradeActionLock() {
    auto lock = LockContainers();
    PORTO_ASSERT(ActionLocked == -1);

    L_DBG("Downgrading exclusive to shared {}", Slug);

    ActionLocked = 1;
    ContainersCV.notify_all();
}

/* only after downgrade */
void TContainer::UpgradeActionLock() {
    LockTimer timer(UPGRADE_LOCK_ACTION);
    auto lock = LockContainers();

    L_DBG("Upgrading shared back to exclusive {}", Slug);

    PendingWrite = true;

    while (ActionLocked != 1)
        ContainersCV.wait(lock);

    ActionLocked = -1;
    LastActionPid = GetTid();

    PendingWrite = false;
}

void TContainer::LockStateRead() {
    LockTimer timer(LOCK_STATE_READ);
    auto lock = LockContainers();
    L_DBG("LockStateRead {}", Slug);
    while (StateLocked < 0)
        ContainersCV.wait(lock);
    StateLocked++;
    LastStatePid = GetTid();
}

void TContainer::LockStateWrite() {
    LockTimer timer(LOCK_STATE_WRITE);
    auto lock = LockContainers();
    L_DBG("LockStateWrite {}", Slug);
    while (StateLocked < 0)
        ContainersCV.wait(lock);
    StateLocked = -1 - StateLocked;
    while (StateLocked != -1)
        ContainersCV.wait(lock);
    LastStatePid = GetTid();
}

void TContainer::DowngradeStateLock() {
    auto lock = LockContainers();
    L_DBG("DowngradeStateLock {}", Slug);
    PORTO_ASSERT(StateLocked == -1);
    StateLocked = 1;
    ContainersCV.notify_all();
}

void TContainer::UnlockState() {
    auto lock = LockContainers();
    L_DBG("UnlockState {}", Slug);
    PORTO_ASSERT(StateLocked);
    if (StateLocked > 0)
        --StateLocked;
    else if (++StateLocked >= -1)
        ContainersCV.notify_all();
}

void TContainer::DumpLocks() {
    auto lock = LockContainers();
    for (auto &it: Containers) {
        auto &ct = it.second;
        if (ct->ActionLocked || ct->PendingWrite || ct->StateLocked || ct->SubtreeRead)
            L_SYS("{} StateLocked {} by {} ActionLocked {} by {} SubtreeRead {}{}", ct->Slug, ct->StateLocked,
                  ct->LastStatePid, ct->ActionLocked, ct->LastActionPid, ct->SubtreeRead,
                  (ct->PendingWrite ? " PendingWrite" : ""));
    }
}

void TContainer::Register() {
    PORTO_LOCKED(ContainersMutex);
    Containers[Name] = shared_from_this();
    if (Parent)
        Parent->Children.emplace_back(shared_from_this());
    Statistics->ContainersCreated++;
}

void TContainer::Unregister() {
    PORTO_LOCKED(ContainersMutex);
    Containers.erase(Name);
    if (Parent)
        Parent->Children.remove(shared_from_this());

    TError error = ContainerIdMap.Put(Id);
    if (error)
        L_WRN("Cannot put {} id: {}", Slug, error);

    PORTO_ASSERT(State == EContainerState::Stopped);
    State = EContainerState::Destroyed;
}

TContainer::TContainer(std::shared_ptr<TContainer> parent, int id, const std::string &name)
    : Parent(parent),
      Level(parent ? parent->Level + 1 : 0),
      Id(id),
      Name(name),
      Slug(fmt::format("CT{}:{}", id, name)),
      FirstName(!parent            ? ""
                : parent->IsRoot() ? name
                                   : name.substr(parent->Name.length() + 1)),
      Stdin(0),
      Stdout(1),
      Stderr(2),
      ClientsCount(0),
      ContainerRequests(0),
      OomEvents(0),
      NetLimitSoftValue(0)

{
    Statistics->ContainersCount++;

    memset(&TaintFlags, 0, sizeof(TaintFlags));

    std::fill(PropSet, PropSet + sizeof(PropSet), false);
    std::fill(PropDirty, PropDirty + sizeof(PropDirty), false);

    RealCreationTime = time(nullptr);
    CreationTime = GetCurrentTimeMs();
    SetProp(EProperty::CREATION_TIME);

    Stdin.SetOutside("/dev/null");
    Stdout.SetOutside("stdout");
    Stderr.SetOutside("stderr");
    Stdout.Limit = config().container().stdout_limit();
    Stderr.Limit = config().container().stdout_limit();
    Root = "/";
    RootPath = Parent ? Parent->RootPath : TPath("/");
    RootRo = false;
    Umask = 0002;
    Isolate = true;
    HostMode = IsRoot();

    NetProp = {{"inherited"}};
    NetIsolate = false;
    NetInherit = true;

    IpLimit = {{"any"}};
    IpPolicy = "any";

    Hostname = "";
    CapAmbient = NoCapabilities;
    if (Parent) {
        if (Level == 1)
            CapLimit = DefaultCapabilities;
        else
            CapLimit = Parent->CapLimit;
    } else
        CapLimit = AllCapabilities;
    CapBound = NoCapabilities;
    CapExtra = NoCapabilities;

    if (IsRoot())
        NsName = ROOT_PORTO_NAMESPACE;
    else if (config().container().default_porto_namespace())
        NsName = FirstName + "/";
    else
        NsName = "";
    SetProp(EProperty::PORTO_NAMESPACE);

    if (IsRoot())
        PlacePolicy = {PORTO_PLACE, "***"};
    else
        PlacePolicy = Parent->PlacePolicy;

    CpuPolicy = Parent ? Parent->CpuPolicy : "normal";
    SetProp(EProperty::CPU_POLICY);
    ChooseSchedPolicy();

    CpuPeriod = Parent ? Parent->CpuPeriod : config().container().cpu_period();

    if (Parent)
        CpuAffinity = Parent->CpuAffinity;

    if (CpuPeriod != 100000000) {
        /* Default cfs period is not configurable in kernel */
        SetProp(EProperty::CPU_PERIOD);
    }

    if (IsRoot()) {
        CpuLimitBound = CpuLimit = GetNumCores() * CPU_POWER_PER_SEC;
        CpuGuaranteeBound = CpuGuarantee = CpuLimit;
        SetProp(EProperty::CPU_LIMIT);
        SetProp(EProperty::MEM_LIMIT);
    } else {
        CpuLimitBound = Parent->CpuLimitBound;
        // cpu controller disabled by default, so we just inherit CpuGuaranteeBound
        CpuGuaranteeBound = Parent->CpuGuaranteeBound;
    }

    IoPolicy = "";
    IoPrio = 0;

    NetClass.DefaultTos = TNetwork::DefaultTos;
    NetClass.Fold = &NetClass;

    PressurizeOnDeath = config().container().pressurize_on_death();
    RunningChildren = 0;
    StartingChildren = 0;

    if (IsRoot()) {
        for (auto &extra: config().container().extra_env())
            EnvCfg += fmt::format("{}={};", extra.name(), extra.value());
    }

    Controllers |= CGROUP_FREEZER;

    if (CgroupDriver.Cgroup2Subsystem->Supported)
        Controllers |= CGROUP2;

    if (CgroupDriver.PerfSubsystem->Supported)
        Controllers |= CGROUP_PERF;

    if (CgroupDriver.CpuacctSubsystem->Controllers == CGROUP_CPUACCT)
        Controllers |= CGROUP_CPUACCT;

    if (Level <= 1) {
        Controllers |= CGROUP_MEMORY | CGROUP_CPU | CGROUP_CPUACCT | CGROUP_DEVICES;

        if (CgroupDriver.BlkioSubsystem->Supported)
            Controllers |= CGROUP_BLKIO;

        if (CgroupDriver.CpusetSubsystem->Supported)
            Controllers |= CGROUP_CPUSET;

        if (CgroupDriver.HugetlbSubsystem->Supported)
            Controllers |= CGROUP_HUGETLB;
    }

    if (Level == 1 && config().container().memory_limit_margin()) {
        uint64_t total = GetTotalMemory();

        MemLimit = total - std::min(total / 4, config().container().memory_limit_margin());
        SetPropDirty(EProperty::MEM_LIMIT);

        if (CgroupDriver.MemorySubsystem->SupportAnonLimit() && config().container().anon_limit_margin()) {
            AnonMemLimit = MemLimit - std::min(MemLimit / 4, config().container().anon_limit_margin());
            SetPropDirty(EProperty::ANON_LIMIT);
        }
    }

    if (Level == 1 && CgroupDriver.PidsSubsystem->Supported) {
        Controllers |= CGROUP_PIDS;

        if (config().container().default_thread_limit()) {
            ThreadLimit = config().container().default_thread_limit();
            SetProp(EProperty::THREAD_LIMIT);
        }
    }

    DirtyMemLimitBound = Parent ? Parent->DirtyMemLimitBound : 0;
    SetProp(EProperty::CONTROLLERS);

    RespawnDelay = config().container().respawn_delay_ms() * 1000000;

    Private = "";
    AgingTime = config().container().default_aging_time_s() * 1000;

    if (Parent && Parent->AccessLevel == EAccessLevel::None)
        AccessLevel = EAccessLevel::None;
    else if (Parent && Parent->AccessLevel <= EAccessLevel::ReadOnly)
        AccessLevel = EAccessLevel::ReadOnly;
    else
        AccessLevel = EAccessLevel::Normal;
    SetProp(EProperty::ENABLE_PORTO);

    for (auto p = Parent.get(); p; p = p->Parent.get()) {
        if (p->UserNs) {
            UserNsCred = p->TaskCred;
            break;
        }
    }

    if (config().container().has_default_coredump_filter()) {
        CoredumpFilter = config().container().default_coredump_filter();
        SetProp(EProperty::COREDUMP_FILTER);
    }
}

TContainer::~TContainer() {
    PORTO_ASSERT(Net == nullptr);
    PORTO_ASSERT(!NetClass.Registered);
    Statistics->ContainersCount--;
    if (TaintFlags.TaintCounted && Statistics->ContainersTainted)
        Statistics->ContainersTainted--;
}

TError TContainer::Create(const std::string &name, std::shared_ptr<TContainer> &ct) {
    auto max_ct = config().container().max_total();
    TError error;
    int id = -1;

    if (CL->IsSuperUser())
        max_ct += NR_SUPERUSER_CONTAINERS;

    error = ValidName(name, CL->IsSuperUser());
    if (error)
        return error;

    auto lock = LockContainers();

    auto parent = TContainer::Find(TContainer::ParentName(name));
    if (parent) {
        if (parent->Level == CONTAINER_LEVEL_MAX)
            return TError(EError::InvalidValue, "You shall not go deeper! Maximum level is {}", CONTAINER_LEVEL_MAX);
        error = parent->LockActionShared(lock);
        if (error)
            return error;
        error = CL->CanControl(*parent, true);
        if (error)
            goto err;
    } else if (name != ROOT_CONTAINER)
        return TError(EError::ContainerDoesNotExist, "parent container not found for " + name);

    if (Containers.find(name) != Containers.end()) {
        error = TError(EError::ContainerAlreadyExists, "container " + name + " already exists");
        goto err;
    }

    if (Containers.size() >= max_ct + NR_SERVICE_CONTAINERS) {
        error = TError(EError::ResourceNotAvailable, "number of containers reached limit: " + std::to_string(max_ct));
        goto err;
    }

    error = ContainerIdMap.Get(id);
    if (error)
        goto err;

    ct = std::make_shared<TContainer>(parent, id, name);
    L_ACT("Create {}", ct->Slug);

    ct->OwnerCred = CL->Cred;
    ct->SetProp(EProperty::OWNER_USER);
    ct->SetProp(EProperty::OWNER_GROUP);

    /*
     * For sub-containers of client container use its task credentials.
     * This is safe because new container will have the same restrictions.
     */
    if (ct->IsChildOf(*CL->ClientContainer)) {
        if (ct->InUserNs() && CL->TaskCred.IsRootUser())
            ct->TaskCred = ct->UserNsCred;
        else
            ct->TaskCred = CL->TaskCred;
        (void)ct->TaskCred.InitGroups(ct->TaskCred.User());
    } else {
        if (ct->InUserNs() && CL->Cred.IsRootUser())
            ct->TaskCred = ct->UserNsCred;
        else
            ct->TaskCred = CL->Cred;
    }

    ct->SetProp(EProperty::USER);
    ct->SetProp(EProperty::GROUP);

    ct->SanitizeCapabilities();

    ct->SetProp(EProperty::STATE);

    ct->RespawnCount = 0;
    ct->SetProp(EProperty::RESPAWN_COUNT);

    error = ct->Save();
    if (error)
        goto err;

    ct->Register();

    if (parent)
        parent->UnlockAction(true);

    TContainerWaiter::ReportAll(*ct);

    return OK;

err:
    if (parent)
        parent->UnlockAction(true);
    if (id >= 0)
        ContainerIdMap.Put(id);
    ct = nullptr;
    return error;
}

TError TContainer::Restore(const TKeyValue &kv, std::shared_ptr<TContainer> &ct) {
    TError error;
    int id;

    error = StringToInt(kv.Get(P_RAW_ID), id);
    if (error)
        return error;

    L_ACT("Restore CT{}:{}", id, kv.Name);

    auto lock = LockContainers();

    if (Containers.find(kv.Name) != Containers.end())
        return TError(EError::ContainerAlreadyExists, kv.Name);

    std::shared_ptr<TContainer> parent;
    error = TContainer::Find(TContainer::ParentName(kv.Name), parent);
    if (error)
        return error;

    error = ContainerIdMap.GetAt(id);
    if (error)
        return error;

    ct = std::make_shared<TContainer>(parent, id, kv.Name);

    ct->Register();

    lock.unlock();

    error = CL->LockContainer(ct);
    if (error)
        goto err;

    error = ct->Load(kv);
    if (error)
        goto err;

    ct->SyncState();

    TNetwork::InitClass(*ct);

    /* Do not rewrite resolv.conf at restore */
    ct->TestClearPropDirty(EProperty::RESOLV_CONF);

    /* Restore cgroups only for running containers */
    if (ct->State != EContainerState::Stopped && ct->State != EContainerState::Dead) {
        error = TNetwork::RestoreNetwork(*ct);
        if (error)
            goto err;

        error = ct->PrepareCgroups(true);
        if (error)
            goto err;

        error = ct->PrepareOomMonitor();
        if (error)
            goto err;

        /* Kernel without group rt forbids moving RT tasks in to cpu cgroup */
        if (ct->Task.Pid && !CgroupDriver.CpuSubsystem->HasRtGroup) {
            auto cpuCg = CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.CpuSubsystem.get());
            std::unique_ptr<const TCgroup> cg;

            if (!CgroupDriver.CpuSubsystem->TaskCgroup(ct->Task.Pid, cg) && *cg != *cpuCg) {
                auto freezerCg = CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.FreezerSubsystem.get());
                if (!CgroupDriver.CpuSubsystem->HasRtGroup) {
                    std::vector<pid_t> prev, pids;
                    struct sched_param param;
                    param.sched_priority = 0;
                    bool retry;

                    /* Disable RT for all task in freezer cgroup */
                    do {
                        error = freezerCg->GetTasks(pids);
                        retry = false;
                        for (auto pid: pids) {
                            if (std::find(prev.begin(), prev.end(), pid) == prev.end() &&
                                sched_getscheduler(pid) == SCHED_RR && !sched_setscheduler(pid, SCHED_OTHER, &param))
                                retry = true;
                        }
                        prev = pids;
                    } while (retry);
                }

                /* Move tasks into correct cpu cgroup before enabling RT */
                if (!CgroupDriver.CpuSubsystem->HasRtGroup && ct->SchedPolicy == SCHED_RR) {
                    error = cpuCg->AttachAll(*freezerCg);
                    if (error)
                        L_WRN("Cannot move to corrent cpu cgroup: {}", error);
                }
            }
        }

        /* Disable memory guarantee in old cgroup */
        if (ct->MemGuarantee) {
            std::unique_ptr<const TCgroup> memCg;
            if (!CgroupDriver.MemorySubsystem->TaskCgroup(ct->Task.Pid, memCg) &&
                *memCg != *CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.MemorySubsystem.get()))
                CgroupDriver.MemorySubsystem->SetGuarantee(*memCg, 0);
        }

        error = ct->ApplyDynamicProperties(true);
        if (error)
            goto err;
    }

    if (ct->State == EContainerState::Dead && ct->AutoRespawn)
        ct->ScheduleRespawn();

    /* Do not apply dynamic properties to dead container */
    if (ct->State == EContainerState::Dead)
        memset(ct->PropDirty, 0, sizeof(ct->PropDirty));

    error = ct->Save();
    if (error)
        goto err;

    if (ct->State == EContainerState::Stopped)
        ct->RemoveWorkDir();

    CL->ReleaseContainer();

    return OK;

err:
    TNetwork::StopNetwork(*ct);
    ct->SetState(EContainerState::Stopped);
    ct->RemoveWorkDir();
    lock.lock();
    CL->ReleaseContainer(true);
    ct->Unregister();
    ct = nullptr;
    return error;
}

std::string TContainer::StateName(EContainerState state) {
    switch (state) {
    case EContainerState::Stopped:
        return "stopped";
    case EContainerState::Dead:
        return "dead";
    case EContainerState::Respawning:
        return "respawning";
    case EContainerState::Starting:
        return "starting";
    case EContainerState::Running:
        return "running";
    case EContainerState::Stopping:
        return "stopping";
    case EContainerState::Paused:
        return "paused";
    case EContainerState::Meta:
        return "meta";
    case EContainerState::Destroyed:
        return "destroyed";
    default:
        return "unknown";
    }
}

EContainerState TContainer::ParseState(const std::string &name) {
    if (name == "stopped")
        return EContainerState::Stopped;
    if (name == "dead")
        return EContainerState::Dead;
    if (name == "respawning")
        return EContainerState::Respawning;
    if (name == "starting")
        return EContainerState::Starting;
    if (name == "running")
        return EContainerState::Running;
    if (name == "stopping")
        return EContainerState::Stopping;
    if (name == "paused")
        return EContainerState::Paused;
    if (name == "meta")
        return EContainerState::Meta;
    return EContainerState::Destroyed;
}

TError TContainer::ValidLabel(const std::string &label, const std::string &value) {
    if (label.size() > PORTO_LABEL_NAME_LEN_MAX)
        return TError(EError::InvalidLabel, "Label name too log, max {} bytes", PORTO_LABEL_NAME_LEN_MAX);

    if (value.size() > PORTO_LABEL_VALUE_LEN_MAX)
        return TError(EError::InvalidLabel, "Label value too log, max {} bytes", PORTO_LABEL_VALUE_LEN_MAX);

    auto sep = label.find('.');
    if (sep == std::string::npos || sep < PORTO_LABEL_PREFIX_LEN_MIN || sep > PORTO_LABEL_PREFIX_LEN_MAX ||
        label.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) < sep ||
        label.find_first_not_of(PORTO_NAME_CHARS) != std::string::npos || StringStartsWith(label, "PORTO"))
        return TError(EError::InvalidLabel, "Invalid label name: {}", label);

    if (value.find_first_not_of(PORTO_NAME_CHARS) != std::string::npos)
        return TError(EError::InvalidLabel, "Invalid label value: {}", value);

    return OK;
}

TError TContainer::GetLabel(const std::string &label, std::string &value) const {
    if (label[0] == '.') {
        auto nm = label.substr(1);
        for (auto ct = this; ct; ct = ct->Parent.get()) {
            auto it = ct->Labels.find(nm);
            if (it != ct->Labels.end()) {
                value = it->second;
                return OK;
            }
        }
    } else {
        auto it = Labels.find(label);
        if (it != Labels.end()) {
            value = it->second;
            return OK;
        }
    }
    return TError(EError::LabelNotFound, "Label {} is not set", label);
}

void TContainer::SetLabel(const std::string &label, const std::string &value) {
    if (value.empty())
        Labels.erase(label);
    else
        Labels[label] = value;
    SetProp(EProperty::LABELS);
}

TError TContainer::IncLabel(const std::string &label, int64_t &result, int64_t add) {
    int64_t val;
    result = 0;

    auto it = Labels.find(label);
    if (it == Labels.end()) {
        if (!add)
            return TError(EError::LabelNotFound, "Label {} is not set", label);
        if (Labels.size() >= PORTO_LABEL_COUNT_MAX)
            return TError(EError::ResourceNotAvailable, "Too many labels");
        val = 0;
    } else {
        TError error = StringToInt64(it->second, val);
        if (error)
            return error;
    }

    result = val;

    if ((add > 0 && val > INT64_MAX - add) || (add < 0 && val < INT64_MIN - add))
        return TError(EError::InvalidValue, "Label {} overflow {} + {}", label, val, add);

    val += add;

    if (it == Labels.end())
        Labels[label] = std::to_string(val);
    else
        it->second = std::to_string(val);

    result = val;

    SetProp(EProperty::LABELS);
    return OK;
}

TPath TContainer::WorkDir() const {
    return TPath(PORTO_WORKDIR) / Name;
}

TError TContainer::CreateWorkDir() const {
    if (IsRoot())
        return OK;

    TFile dir;
    TError error;

    error = dir.OpenDir(PORTO_WORKDIR);
    if (error)
        return error;

    if (Level > 1) {
        for (auto &name: TPath(Parent->Name).Components()) {
            error = dir.OpenDirStrictAt(dir, name);
            if (error)
                return error;
        }
    }

    TPath name = FirstName;

    if (dir.ExistsAt(name)) {
        L_ACT("Remove stale working dir");
        error = dir.RemoveAt(name);
        if (error)
            L_ERR("Cannot remove working dir: {}", error);
    }

    error = dir.MkdirAt(name, 0775);
    if (!error) {
        error = dir.ChownAt(name, TaskCred);
        if (error)
            (void)dir.RemoveAt(name);
    }

    if (error) {
        if (error.Errno == ENOSPC || error.Errno == EROFS)
            L("Cannot create working dir: {}", error);
        else
            L_ERR("Cannot create working dir: {}", error);
    }

    return error;
}

void TContainer::RemoveWorkDir() const {
    if (IsRoot() || !WorkDir().Exists())
        return;

    TFile dir;
    TError error;

    error = dir.OpenDir(PORTO_WORKDIR);
    if (!error && Level > 1) {
        for (auto &name: TPath(Parent->Name).Components()) {
            error = dir.OpenDirStrictAt(dir, name);
            if (error)
                break;
        }
    }
    if (!error)
        error = dir.RemoveAt(FirstName);
    if (error)
        L_ERR("Cannot remove working dir: {}", error);
}

TPath TContainer::GetCwd() const {
    TPath cwd;

    for (auto ct = shared_from_this(); ct; ct = ct->Parent) {
        if (!ct->Cwd.IsEmpty())
            cwd = ct->Cwd / cwd;
        if (cwd.IsAbsolute())
            return cwd;
        if (ct->Root != "/")
            return TPath("/") / cwd;
    }

    if (IsRoot())
        return TPath("/");

    return WorkDir();
}

int TContainer::GetExitCode() const {
    if (State != EContainerState::Dead && State != EContainerState::Respawning)
        return 256;
    if (OomKilled)
        return -99;
    if (WIFSIGNALED(ExitStatus))
        return -WTERMSIG(ExitStatus);
    return WEXITSTATUS(ExitStatus);
}

TError TContainer::UpdateSoftLimit() {
    auto lock = LockContainers();
    TError error;

    for (auto ct = shared_from_this(); !ct->IsRoot(); ct = ct->Parent) {
        if (!(ct->Controllers & CGROUP_MEMORY))
            continue;

        int64_t limit = -1;

        /* Set memory soft limit for dead or hollow meta containers */
        if (ct->PressurizeOnDeath &&
            (ct->State == EContainerState::Dead ||
             (ct->State == EContainerState::Meta && !ct->RunningChildren && !ct->StartingChildren)))
            limit = config().container().dead_memory_soft_limit();

        if (ct->MemSoftLimit != limit) {
            auto cg = CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.MemorySubsystem.get());
            error = CgroupDriver.MemorySubsystem->SetSoftLimit(*cg, limit);
            if (error && error.Errno != ENOENT)
                return error;
            ct->MemSoftLimit = limit;
        }
    }

    return OK;
}

void TContainer::SetState(EContainerState next) {
    if (State == next)
        return;

    L_ACT("Change {} state {} -> {}", Slug, StateName(State), StateName(next));

    LockStateWrite();

    auto prev = State;
    State = next;

    if (prev == EContainerState::Starting || next == EContainerState::Starting) {
        for (auto p = Parent; p; p = p->Parent)
            p->StartingChildren += next == EContainerState::Starting ? 1 : -1;
    }

    if (prev == EContainerState::Running || next == EContainerState::Running) {
        for (auto p = Parent; p; p = p->Parent)
            p->RunningChildren += next == EContainerState::Running ? 1 : -1;
    }

    if (next == EContainerState::Dead && AutoRespawn)
        ScheduleRespawn();

    DowngradeStateLock();

    if (prev == EContainerState::Running || next == EContainerState::Running) {
        for (auto p = Parent; p; p = p->Parent)
            if (!p->RunningChildren && p->State == EContainerState::Meta)
                TContainerWaiter::ReportAll(*p);
    }

    std::string label, value;

    if ((State == EContainerState::Stopped || State == EContainerState::Dead || State == EContainerState::Respawning) &&
        StartError) {
        label = P_START_ERROR;
        value = StartError.ToString();
    } else if (State == EContainerState::Dead || State == EContainerState::Respawning) {
        label = P_EXIT_CODE;
        value = std::to_string(GetExitCode());
    }

    TContainerWaiter::ReportAll(*this, label, value);

    UnlockState();
}

TError TContainer::Destroy(std::list<std::shared_ptr<TVolume>> &unlinked) {
    TError error;

    L_ACT("Destroy {}", Slug);

    if (State != EContainerState::Stopped) {
        error = Stop(0);
        if (error)
            return error;
    }

    if (!Children.empty()) {
        for (auto &ct: Subtree()) {
            if (ct.get() != this) {
                TVolume::UnlinkAllVolumes(ct, unlinked);
                error = ct->Destroy(unlinked);
                if (error)
                    return error;
            }
        }
    }

    TVolume::UnlinkAllVolumes(shared_from_this(), unlinked);

    TPath path(ContainersKV / std::to_string(Id));
    error = path.Unlink();
    if (error)
        L_ERR("Can't remove key-value node {}: {}", path, error);

    auto lock = LockContainers();
    Unregister();
    lock.unlock();

    TContainerWaiter::ReportAll(*this);

    return OK;
}

bool TContainer::IsChildOf(const TContainer &ct) const {
    for (auto ptr = Parent.get(); ptr; ptr = ptr->Parent.get()) {
        if (ptr == &ct)
            return true;
    }
    return false;
}

/* Subtree in DFS post-order: childs first */
std::list<std::shared_ptr<TContainer>> TContainer::Subtree() {
    std::list<std::shared_ptr<TContainer>> subtree{shared_from_this()};
    auto lock = LockContainers();
    for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) {
        for (auto child: (*it)->Children)
            subtree.emplace(std::prev(it.base()), child);
    }
    return subtree;
}

/* Builds list of childs at this moment. */
std::list<std::shared_ptr<TContainer>> TContainer::Childs() {
    auto lock = LockContainers();
    auto childs(Children);
    return childs;
}

std::shared_ptr<TContainer> TContainer::GetParent() const {
    return Parent;
}

bool TContainer::HasPidFor(const TContainer &ct) const {
    auto ns = &ct;

    while (!ns->Isolate && ns->Parent)
        ns = ns->Parent.get();

    return ns == this || IsChildOf(*ns);
}

TError TContainer::GetPidFor(pid_t pidns, pid_t &pid) const {
    ino_t inode = TNamespaceFd::PidInode(pidns, "ns/pid");
    TError error;

    if (IsRoot()) {
        pid = 1;
    } else if (!Task.Pid) {
        error = TError(EError::InvalidState, "container isn't running");
    } else if (TNamespaceFd::PidInode(getpid(), "ns/pid") == inode) {
        pid = Task.Pid;
    } else if (WaitTask.Pid != Task.Pid && TNamespaceFd::PidInode(WaitTask.Pid, "ns/pid") == inode) {
        pid = TaskVPid;
    } else if (TNamespaceFd::PidInode(Task.Pid, "ns/pid") == inode) {
        if (!Isolate)
            pid = TaskVPid;
        else if (OsMode || IsMeta())
            pid = 1;
        else
            pid = 2;
    } else {
        error = TranslatePid(-Task.Pid, pidns, pid);
        if (!pid && !error)
            error = TError(EError::Permission, "pid is unreachable");
    }
    return error;
}

TError TContainer::GetThreadCount(uint64_t &count) const {
    if (IsRoot()) {
        struct sysinfo si;
        if (sysinfo(&si) < 0)
            return TError::System("sysinfo");
        count = si.procs;
    } else if (Controllers & CGROUP_PIDS) {
        auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.PidsSubsystem.get());
        return CgroupDriver.PidsSubsystem->GetUsage(*cg, count);
    } else {
        auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
        return CgroupDriver.GetCgroupThreadCount(*cg, count);
    }
    return OK;
}

TError TContainer::GetProcessCount(uint64_t &count) const {
    TError error;
    if (IsRoot()) {
        struct stat st;
        error = TPath("/proc").StatStrict(st);
        if (error)
            return error;
        count = st.st_nlink > ProcBaseDirs ? st.st_nlink - ProcBaseDirs : 0;
    } else {
        auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
        return CgroupDriver.GetCgroupProcessCount(*cg, count);
    }
    return OK;
}

TError TContainer::GetVmStat(TVmStat &stat) const {
    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    std::vector<pid_t> pids;
    TError error;

    error = cg->GetProcesses(pids);
    if (error)
        return error;

    for (auto pid: pids)
        stat.Parse(pid);

    return OK;
}

TError TContainer::CheckMemGuarantee() const {
    uint64_t total = GetTotalMemory();
    uint64_t usage = RootContainer->GetTotalMemGuarantee();
    uint64_t old_usage = usage - std::min(NewMemGuarantee, usage);
    uint64_t reserve = config().daemon().memory_guarantee_reserve();
    uint64_t hugetlb = GetHugetlbMemory();

    if (usage + reserve + hugetlb > total) {
        /*
         * under overcommit allow to start containers without guarantee
         * for root user or nested containers
         */
        if (!NewMemGuarantee && (Level > 1 || CL->IsSuperUser()))
            return OK;

        return TError(
            EError::ResourceNotAvailable,
            "Memory guarantee overcommit: requested {}, available {}, guaranteed {} + reserved {} + hugetlb {} of {}",
            StringFormatSize(NewMemGuarantee), StringFormatSize(total - std::min(total, old_usage + reserve + hugetlb)),
            StringFormatSize(old_usage), StringFormatSize(reserve), StringFormatSize(hugetlb), StringFormatSize(total));
    }

    return OK;
}

uint64_t TContainer::GetTotalMemGuarantee(bool containers_locked) const {
    uint64_t sum = 0lu;

    /* Stopped container doesn't have memory guarantees */
    if (State == EContainerState::Stopped)
        return 0;

    if (!containers_locked)
        ContainersMutex.lock();

    for (auto &child: Children)
        sum += child->GetTotalMemGuarantee(true);

    sum = std::max(NewMemGuarantee, sum);

    if (!containers_locked)
        ContainersMutex.unlock();

    return sum;
}

uint64_t TContainer::GetMemLimit(bool effective) const {
    uint64_t lim = 0;

    for (auto ct = this; ct; ct = ct->Parent.get())
        if ((effective || ct->HasProp(EProperty::MEM_LIMIT)) && ct->MemLimit && (ct->MemLimit < lim || !lim))
            lim = ct->MemLimit;

    if (effective && !lim)
        lim = GetTotalMemory();

    return lim;
}

uint64_t TContainer::GetAnonMemLimit(bool effective) const {
    uint64_t lim = 0;

    for (auto ct = this; ct; ct = ct->Parent.get())
        if ((effective || ct->HasProp(EProperty::ANON_LIMIT)) && ct->AnonMemLimit && (ct->AnonMemLimit < lim || !lim))
            lim = ct->AnonMemLimit;

    if (effective && !lim)
        lim = GetTotalMemory();

    return lim;
}

TError TContainer::ChooseDirtyMemLimit() {
    if (Parent) {
        if (DirtyMemLimit > 0) {
            DirtyMemLimitBound = std::min(DirtyMemLimit, Parent->DirtyMemLimitBound);
            if (DirtyMemLimitBound == 0)
                DirtyMemLimitBound = DirtyMemLimit;
        } else
            DirtyMemLimitBound = Parent->DirtyMemLimitBound;
    } else
        DirtyMemLimitBound = DirtyMemLimit;

    if (Controllers & CGROUP_MEMORY) {
        auto memcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.MemorySubsystem.get());
        return CgroupDriver.MemorySubsystem->SetDirtyLimit(*memcg, DirtyMemLimit > 0 ? DirtyMemLimitBound : 0);
    }

    return OK;
}

void TContainer::PropagateDirtyMemLimit() {
    TError error;
    auto children = Subtree();
    children.reverse();
    for (auto &child: children) {
        error = child->ChooseDirtyMemLimit();
        if (error && (child->Id == Id || (error.Errno != EINVAL && error.Errno != ENOENT)))
            L_ERR("Cannot set {} {}: {}", P_DIRTY_LIMIT, Slug, error);
    }
}

TError TContainer::ApplyUlimits() {
    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    std::map<int, struct rlimit> map;
    std::vector<pid_t> prev, pids;
    TError error;
    bool retry;

    L_ACT("Apply ulimits");
    auto lim = GetUlimit();
    do {
        error = cg->GetTasks(pids);
        if (error)
            return error;
        retry = false;
        for (auto pid: pids) {
            if (std::find(prev.begin(), prev.end(), pid) != prev.end())
                continue;
            error = lim.Apply(pid);
            if (error && error.Errno != ESRCH)
                return error;
            retry = true;
        }
        prev = pids;
    } while (retry);

    return OK;
}

static constexpr const char *CPU_POLICY_IDLE = "idle";

void TContainer::ChooseSchedPolicy() {
    SchedPolicy = SCHED_OTHER;
    SchedPrio = 0;
    SchedNice = 0;
    SchedNoSmt = false;

    if (CpuPolicy == "rt") {
        SchedNice = config().container().rt_nice();
        if (config().container().rt_priority()) {
            SchedPolicy = SCHED_RR;
            SchedPrio = config().container().rt_priority();
        }
    } else if (CpuPolicy == "high") {
        SchedNice = config().container().high_nice();
    } else if (CpuPolicy == "batch") {
        SchedPolicy = SCHED_BATCH;
    } else if (CpuPolicy == CPU_POLICY_IDLE) {
        SchedPolicy = SCHED_IDLE;
    } else if (CpuPolicy == "iso") {
        SchedPolicy = 4;
        SchedNice = config().container().high_nice();
    } else if (CpuPolicy == "nosmt") {
        SchedNoSmt = HyperThreadingEnabled;
    }

    if (SchedPolicy != SCHED_RR) {
        /* -1 nice is a +10% cpu weight */
        SchedNice -= std::log(double(CpuWeight) / 100) / std::log(1.1);
        SchedNice = std::min(std::max(SchedNice, -20), 19);
    }
}

// check that srcset1 is subset of srcset2
static bool CPU_SUBSET(cpu_set_t *srcset1, cpu_set_t *srcset2) {
    cpu_set_t tmp;
    CPU_AND(&tmp, srcset1, srcset2);
    return CPU_EQUAL(&tmp, srcset1);
}

// TODO(ovov): crutch, remove it
static bool SkipIoUringThread(int pid) {
    std::string comm;
    auto error = TPath(fmt::format("/proc/{}/comm", pid)).ReadAll(comm);
    if (error.Errno != ESRCH && error.Errno != ENOENT && !StringStartsWith(comm, "iou-"))
        return false;
    return true;
}

TError TContainer::ApplySchedPolicy() {
    ChooseSchedPolicy();

    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    struct sched_param param;
    param.sched_priority = SchedPrio;
    TError error;

    std::vector<pid_t> prev, pids;
    bool retry;

    L_ACT("Set {} scheduler policy {}", *cg, CpuPolicy);

    // Extention of SCHED_IDLE to cgroups on kernel 5.15 and above. More details:
    // https://lore.kernel.org/lkml/20210608231132.32012-1-joshdon@google.com
    if (CgroupDriver.CpuSubsystem->HasIdle && Controllers & CGROUP_CPU) {
        auto cpucg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.CpuSubsystem.get());

        if (CgroupDriver.CpuSubsystem->EnableIdle && SchedPolicy == SCHED_IDLE) {
            CgroupDriver.CpuSubsystem->SetCpuIdle(*cpucg, true);
            ExtSchedIdle = true;
        } else {
            CgroupDriver.CpuSubsystem->SetCpuIdle(*cpucg, false);
            ExtSchedIdle = false;
        }
    }

    TBitMap taskAffinity;
    if (SchedNoSmt) {
        taskAffinity = GetNoSmtCpus();
        L("Task affinity set: {}", taskAffinity.Format());
    } else {
        taskAffinity.Set(CpuAffinity);
    }

    cpu_set_t taskMask;
    int schedPolicy = ExtSchedIdle ? SCHED_OTHER : SchedPolicy;
    taskAffinity.FillCpuSet(&taskMask);

    do {
        error = cg->GetTasks(pids);
        retry = false;

        for (auto pid: pids) {
            cpu_set_t current;

            if (std::find(prev.begin(), prev.end(), pid) != prev.end()) {
                // some io uring threads do not allow changing affinity
                // TODO(ovov): remove this after fixes in kernel
                if (!sched_getaffinity(pid, sizeof(current), &current) &&
                    // PORTO-993#627a4d9fcd10ac4784266ff7
                    (CPU_SUBSET(&current, &taskMask) || SkipIoUringThread(pid)) &&
                    sched_getscheduler(pid) == schedPolicy) {
                    continue;
                }
            }

            if (setpriority(PRIO_PROCESS, pid, SchedNice) && errno != ESRCH)
                return TError::System("setpriority");
            if (sched_setscheduler(pid, schedPolicy, &param) && errno != ESRCH)
                return TError::System("sched_setscheduler");
            if (sched_setaffinity(pid, sizeof(taskMask), &taskMask) && errno != ESRCH && !SkipIoUringThread(pid))
                return TError::System("sched_setaffinity({}, {})", pid, taskAffinity.Format());
            retry = true;
        }
        prev = pids;
    } while (retry);

    return OK;
}

TError TContainer::ApplyIoPolicy() const {
    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    TError error;

    std::vector<pid_t> prev, pids;
    bool retry;

    L_ACT("Set {} io policy {} ioprio {}", *cg, IoPolicy, IoPrio);
    do {
        error = cg->GetTasks(pids);
        retry = false;
        for (auto pid: pids) {
            if (std::find(prev.begin(), prev.end(), pid) != prev.end())
                continue;
            if (SetIoPrio(pid, IoPrio) && errno != ESRCH)
                return TError::System("ioprio");
            retry = true;
        }
        prev = pids;
    } while (retry);

    return OK;
}

TError TContainer::ApplyResolvConf() const {
    TError error;
    TFile file;

    if (HasProp(EProperty::RESOLV_CONF) ? !ResolvConf.size() : Root == "/")
        return OK;

    if (!Task.Pid)
        return TError(EError::InvalidState, "No container task pid");

    error = file.Open("/proc/" + std::to_string(Task.Pid) + "/root/etc/resolv.conf",
                      O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY);
    if (error)
        return error;

    if (file.FsType() != TMPFS_MAGIC)
        return TError(EError::NotSupported, "resolv.conf not on tmpfs");

    L_ACT("Apply resolv_conf for {}", Slug);
    error = file.Truncate(0);
    if (!error)
        error = file.WriteAll(ResolvConf.size() ? ResolvConf : RootContainer->ResolvConf);
    return error;
}

TContainer::TJailCpuState TContainer::GetJailCpuState() {
    auto lock = LockCpuAffinity();
    return {JailCpuPermutation, JailCpuPermutationUsage};
}

void TContainer::UpdateJailCpuStateLocked(const TBitMap &affinity, bool release) {
    PORTO_LOCKED(CpuAffinityMutex);

    for (unsigned cpu = 0; cpu < affinity.Size(); cpu++) {
        if (!affinity.Get(cpu))
            continue;

        unsigned index = CpuToJailPermutationIndex[cpu];
        if (release) {
            if (JailCpuPermutationUsage[index])
                JailCpuPermutationUsage[index]--;
        } else
            JailCpuPermutationUsage[index]++;
    }
}

TError TContainer::NextJailCpu(TBitMap &affinity, int node) {
    unsigned minUsage = UINT_MAX, minIndex;
    for (unsigned i = 0; i < JailCpuPermutationUsage.size(); ++i) {
        int cpu = JailCpuPermutation[i];

        if (affinity.Get(cpu))
            continue;

        if (node >= 0 && ThreadsNode[cpu] != (unsigned)node)
            continue;

        if (JailCpuPermutationUsage[i] < minUsage) {
            minIndex = i;
            minUsage = JailCpuPermutationUsage[i];
        }
    }

    if (minUsage == UINT_MAX)
        return TError("Failed allocate cpu to jail");

    JailCpuPermutationUsage[minIndex]++;
    affinity.Set(JailCpuPermutation[minIndex]);
    return OK;
}

void TContainer::UnjailCpus(const TBitMap &affinity) {
    auto lock = LockCpuAffinity();
    UnjailCpusLocked(affinity);
}

void TContainer::UnjailCpusLocked(const TBitMap &affinity) {
    UpdateJailCpuStateLocked(affinity, true);

    CpuJail = 0;
}

// TODO(ovov): kill this
TError TContainer::JailCpus() {
    TBitMap affinity;
    TError error;
    auto lock = LockCpuAffinity();

    if (NewCpuJail) {
        for (auto ct = Parent.get(); ct; ct = ct->Parent.get()) {
            if (ct->CpuJail)
                return TError(EError::ResourceNotAvailable, "Nested cpu jails are not supported for {}", Slug);
        }

        for (auto &ct: Subtree()) {
            if (ct.get() != this) {
                if (ct->CpuJail)
                    return TError(EError::ResourceNotAvailable, "Nested cpu jails are not supported for {}", Slug);
            }
        }
    }

    /* check desired jail value */
    if (CpuSetType == ECpuSetType::Node) {
        if (!NumaNodes.Get(CpuSetArg))
            return TError(EError::ResourceNotAvailable, "Numa node not found for {}", Slug);

        if ((unsigned)NewCpuJail >= NodeThreads[0].Weight())
            return TError(EError::ResourceNotAvailable, "Invalid jail with numa value {} (max {}) for {}", NewCpuJail,
                          NodeThreads[0].Weight() - 1, Slug);
    } else if ((unsigned)NewCpuJail >= JailCpuPermutation.size())
        return TError(EError::ResourceNotAvailable, "Invalid jail value {} (max {}) for {}", NewCpuJail,
                      JailCpuPermutation.size() - 1, Slug);

    /* read current cpus from cgroup */
    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.CpusetSubsystem.get());
    if (!cg->Exists())
        return OK;

    error = CgroupDriver.CpusetSubsystem->GetCpus(*cg, affinity);
    if (error) {
        L_ERR("Cannot get cpuset.cpus: {}", error);
        return error;
    }

    if (!NewCpuJail) {
        /* disable previously enabled jail */
        if (CpuJail)
            UnjailCpusLocked(affinity);

        return OK;
    }

    unsigned affinityWeight = affinity.Weight();

    /* try to detect previous jail affinity */

    int node = -1;
    bool nodeChanged = false;
    if (CpuSetType == ECpuSetType::Node) {
        /* check if node changed */
        node = CpuSetArg;
        for (unsigned cpu = 0; cpu < affinity.Size(); cpu++) {
            if (!affinity.Get(cpu))
                continue;

            if (ThreadsNode[cpu] != (unsigned)node) {
                nodeChanged = true;
                break;
            }
        }
    } else if (NumaNodes.Weight() > 1 && affinityWeight > 1 && affinityWeight < JailCpuPermutation.size()) {
        /* check previous pin to node, assume we was pinned */
        nodeChanged = true;
        for (unsigned cpu = 0; cpu < affinity.Size(); cpu++) {
            if (!affinity.Get(cpu))
                continue;

            if (node < 0)
                node = (int)ThreadsNode[cpu];
            else if (ThreadsNode[cpu] != (unsigned)node) {
                nodeChanged = false;
                break;
            }
        }
        /* we reuse node variable, so reset previous value */
        node = -1;
    }

    if ((unsigned)NewCpuJail != affinityWeight || nodeChanged) {
        if (node >= 0) {
            if (affinityWeight < NodeThreads[node].Weight())
                UpdateJailCpuStateLocked(affinity, true);
        } else if (affinityWeight < JailCpuPermutation.size())
            UpdateJailCpuStateLocked(affinity, true);

        affinity.Clear();

        /* main loop */
        for (int i = 0; i < NewCpuJail; i++) {
            error = NextJailCpu(affinity, node);
            if (error)
                return error;
        }

        auto subtree = Subtree();
        subtree.reverse();
        for (auto &ct: subtree)
            ct->CpuAffinity = affinity;

        error = CommitSubtreeCpus(*cg, subtree);
        if (error)
            return error;
    } else if (!CpuJail)
        /* case of portod reload, fill current usage */
        UpdateJailCpuStateLocked(affinity);

    CpuAffinity = affinity;
    CpuJail = NewCpuJail;

    return OK;
}

TBitMap TContainer::GetNoSmtCpus() {
    TBitMap taskAffinity;

    taskAffinity.Set(CpuAffinity);
    for (unsigned cpu = 0; cpu < taskAffinity.Size(); cpu++) {
        if (taskAffinity.Get(cpu)) {
            taskAffinity.Set(NeighborThreads[cpu], false);
        }
    }

    return taskAffinity;
}

class TCgroupAffinityIndex {
    /*
      qux/
      foo/bar/
      foo/
      foo-/
      baz/

      Trailing slash is mandatory for correct work of index!
      Without it index would look like this:

      qux
      foo/bar
      foo-
      foo
      baz

      In this index foo/bar is not followed by its parent foo
     */
    std::map<std::string, TBitMap, std::greater<std::string>> Index;

public:
    TCgroupAffinityIndex(const std::list<std::shared_ptr<TContainer>> &subtree) {
        for (auto &ct: subtree) {
            if (ct->State == EContainerState::Stopped || ct->State == EContainerState::Dead ||
                !(ct->Controllers & CGROUP_CPUSET))
                continue;

            auto cg = CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.CpusetSubsystem.get());
            if (!cg->Exists()) {
                L_WRN("{} with cpuset controller does not have cpuset cgroup", ct->Slug);
                continue;
            }

            Index.emplace(cg->GetName() + "/", ct->CpuAffinity);
        }
    }

    // Find target affinity for cgroup or its closest ancestor
    bool GetAffinity(const TCgroup &cg, TBitMap &affinity) const {
        auto name = cg.GetName() + "/";
        auto it = Index.lower_bound(name);
        if (it == Index.end())
            return false;
        // exact match or some other ancestor
        if (name != it->first && !StringStartsWith(cg.GetName(), it->first))
            return false;

        affinity = it->second;
        return true;
    }
};

static TError WidenSubtreeCpus(const std::list<std::unique_ptr<const TCgroup>> &cgroups,
                               const TCgroupAffinityIndex &index, std::map<std::string, TBitMap> &cpusets) {
    for (auto &cg: cgroups) {
        TBitMap affinity;
        if (!index.GetAffinity(*cg, affinity))
            return TError("Cannot find target affinity for cgroup {}", *cg);

        TBitMap cur;
        auto error = CgroupDriver.CpusetSubsystem->GetCpus(*cg, cur);
        if (error)
            return error;

        if (!affinity.IsSubsetOf(cur)) {
            cur.Set(affinity);  // union

            error = CgroupDriver.CpusetSubsystem->SetCpus(*cg, cur);
            if (error)
                return error;
        }
        cpusets.emplace(cg->GetName(), std::move(cur));
    }
    return OK;
}

static TError NarrowSubtreeCpus(const std::list<std::unique_ptr<const TCgroup>> &cgroups,
                                const TCgroupAffinityIndex &index, std::map<std::string, TBitMap> &cpusets) {
    for (auto it = cgroups.rbegin(); it != cgroups.rend(); ++it) {
        auto &cg = *it;

        TBitMap affinity;
        if (!index.GetAffinity(*cg, affinity))
            return TError("Cannot find target affinity for cgroup {}", *cg);

        auto kt = cpusets.find(cg->GetName());  // use find just in case
        if (kt != cpusets.end() && kt->second.IsEqual(affinity))
            continue;

        auto error = CgroupDriver.CpusetSubsystem->SetCpus(*cg, affinity);
        if (error)
            return error;
    }
    return OK;
}

static TError ApplySubtreeCpus(const TCgroup &root, const std::list<std::shared_ptr<TContainer>> &subtree) {
    TCgroupAffinityIndex index(subtree);
    std::list<std::unique_ptr<const TCgroup>> cgroups;
    std::map<std::string, TBitMap> cpusets;

    auto error = CgroupDriver.CgroupSubtree(root, cgroups, true);
    if (error)
        return error;

    cgroups.push_front(root.GetUniquePtr());

    // Step 1: widen to union of current and target affinity
    error = WidenSubtreeCpus(cgroups, index, cpusets);
    if (error)
        return error;

    // Step 2: narrow to target affinity
    error = NarrowSubtreeCpus(cgroups, index, cpusets);
    if (error)
        return error;
    return OK;
}

static TError RevertSubtreeCpus(std::list<std::shared_ptr<TContainer>> &subtree) {
    for (auto &ct: subtree) {
        if (ct->State == EContainerState::Stopped || ct->State == EContainerState::Dead ||
            !(ct->Controllers & CGROUP_CPUSET))
            continue;

        auto cg = CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.CpusetSubsystem.get());
        if (!cg->Exists())
            continue;

        auto error = CgroupDriver.CpusetSubsystem->GetCpus(*cg, ct->CpuAffinity);
        if (error)
            return error;
    }
    return OK;
}

static TError CommitSubtreeCpus(const TCgroup &root, std::list<std::shared_ptr<TContainer>> &subtree) {
    auto error = ApplySubtreeCpus(root, subtree);

    if (error) {
        L_ERR("Failed to apply cpuset: {}", error);
        auto error2 = RevertSubtreeCpus(subtree);
        if (error2)
            L_ERR("Failed to revert cpuset: {}", error2);
        return error;
    }

    return OK;
}

TError TContainer::BuildCpuTopology() {
    PORTO_ASSERT(IsRoot());

    auto error = CpuAffinity.Read("/sys/devices/system/cpu/online");
    if (error)
        return error;

    NeighborThreads.clear();
    NeighborThreads.resize(CpuAffinity.Size());

    for (unsigned cpu = 0; cpu < CpuAffinity.Size(); cpu++) {
        if (!CpuAffinity.Get(cpu))
            continue;

        TBitMap neighbors;
        error = neighbors.Read(StringFormat("/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu));
        if (error)
            return error;

        if (neighbors.Weight() > 1) {
            HyperThreadingEnabled = true;
            NeighborThreads[cpu] = neighbors.FirstBitIndex(cpu + 1);
        }
    }

    error = NumaNodes.Read("/sys/devices/system/node/online");
    if (error)
        return error;

    NodeThreads.clear();
    NodeThreads.resize(NumaNodes.Size());

    ThreadsNode.clear();
    ThreadsNode.resize(CpuAffinity.Size());

    for (unsigned node = 0; node < NumaNodes.Size(); node++) {
        if (!NumaNodes.Get(node))
            continue;

        auto &nodeThreads = NodeThreads[node];

        error = nodeThreads.Read(StringFormat("/sys/devices/system/node/node%u/cpulist", node));
        if (error)
            return error;
        for (unsigned cpu = 0; cpu < nodeThreads.Size(); cpu++) {
            if (!nodeThreads.Get(cpu))
                continue;

            ThreadsNode[cpu] = node;
        }
    }

    JailCpuPermutation.clear();
    JailCpuPermutation.resize(CpuAffinity.Size());
    CpuToJailPermutationIndex.clear();
    CpuToJailPermutationIndex.resize(CpuAffinity.Size());

    std::vector<unsigned> nodeThreadsIter(NumaNodes.Size());
    for (unsigned i = 0; i != CpuAffinity.Size() / NumaNodes.Size(); i += HyperThreadingEnabled ? 2 : 1) {
        unsigned j = 0;
        for (unsigned node = 0; node < NumaNodes.Size(); node++) {
            if (!NumaNodes.Get(node))
                continue;

            int cpu = NodeThreads[node].FirstBitIndex(nodeThreadsIter[node]);
            unsigned index = i * NumaNodes.Size() + j;
            JailCpuPermutation[index] = cpu;
            CpuToJailPermutationIndex[cpu] = index;
            if (HyperThreadingEnabled) {
                index = (i + 1) * NumaNodes.Size() + j;
                JailCpuPermutation[index] = NeighborThreads[cpu];
                CpuToJailPermutationIndex[NeighborThreads[cpu]] = index;
            }

            nodeThreadsIter[node] = cpu + 1;
            ++j;
        }
    }

    /* don't clear JailCpuPermutationUsage */
    JailCpuPermutationUsage.resize(JailCpuPermutation.size());
    return OK;
}

TError TContainer::ApplyCpuSet() {
    auto lock = LockCpuAffinity();

    auto subtree = Subtree();
    subtree.reverse();

    for (auto &ct: subtree) {
        if (ct->CpuJail)
            continue;

        TBitMap affinity;

        switch (ct->CpuSetType) {
        case ECpuSetType::Inherit:
            affinity = ct->Parent->CpuAffinity;
            break;
        case ECpuSetType::Absolute:
            affinity = ct->CpuAffinity;
            break;
        case ECpuSetType::Node:
            if (!NumaNodes.Get(ct->CpuSetArg))
                return TError(EError::ResourceNotAvailable, "Numa node {} not found for {}", ct->CpuSetArg, ct->Slug);
            affinity = NodeThreads[ct->CpuSetArg];
            break;
        }

        L_VERBOSE("Assign CPUs {} for {}", affinity.Format(), ct->Slug);
        ct->CpuAffinity = affinity;
    }

    auto error =
        CommitSubtreeCpus(*(CgroupDriver.GetContainerCgroup(*this, CgroupDriver.CpusetSubsystem.get())), subtree);
    if (error)
        return error;

    return OK;
}

void TContainer::PropogateCpuGuarantee() {
    auto bound = Parent->CpuGuaranteeBound;
    if (Controllers & CGROUP_CPU)
        bound = std::min(CpuGuarantee, Parent->CpuGuaranteeBound);

    if (bound == CpuGuaranteeBound)
        return;

    CpuGuaranteeBound = bound;
    for (auto &ct: Children)
        ct->PropogateCpuGuarantee();
}

TError TContainer::SetCpuGuarantee(uint64_t guarantee) {
    if (IsRoot() || !(Controllers & CGROUP_CPU) || !HasResources())
        return OK;

    L_ACT("Set cpu guarantee {} {}", Slug, CpuPowerToString(guarantee));
    auto cpucg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.CpuSubsystem.get());
    auto error = CgroupDriver.CpuSubsystem->SetGuarantee(*cpucg, guarantee);
    if (error)
        L_ERR("Cannot set cpu guarantee: {}", error);
    return error;
}

TError TContainer::ApplyCpuGuarantee() {
    if (CpuGuaranteeBound == CpuGuaranteeCur)
        return OK;

    auto error = SetCpuGuarantee(CpuGuaranteeBound);
    if (error)
        return error;

    CpuGuaranteeCur = CpuGuaranteeBound;

    for (auto &ct: Children) {
        error = ct->ApplyCpuGuarantee();
        if (error)
            return error;
    }
    return OK;
}

TError TContainer::ApplyCpuShares() {
    if (IsRoot() || !(Controllers & CGROUP_CPU) || !HasResources() || ExtSchedIdle)
        return OK;

    auto cpucg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.CpuSubsystem.get());
    auto error = CgroupDriver.CpuSubsystem->SetShares(*cpucg, CpuPolicy, CpuWeight, CpuGuarantee);
    if (error)
        L_ERR("Cannot set cpu shares: {}", error);
    return error;
}

TError TContainer::SetCpuLimit(uint64_t limit, uint64_t period) {
    if (IsRoot() || !(Controllers & CGROUP_CPU) || !HasResources())
        return OK;

    auto cpucg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.CpuSubsystem.get());

    L_ACT("Set cpu limit {} {} period {}", Slug, CpuPowerToString(limit), period);

    auto error = CgroupDriver.CpuSubsystem->SetRtLimit(*cpucg, limit, period);
    if (error) {
        if (CpuPolicy == "rt")
            return error;
        L_WRN("Cannot set rt cpu limit: {}", error);
    }

    return CgroupDriver.CpuSubsystem->SetLimit(*cpucg, limit, period);
}

void TContainer::PropogateCpuLimit() {
    auto bound = CpuLimit ? std::min(CpuLimit, Parent->CpuLimitBound) : Parent->CpuLimitBound;
    if (bound == CpuLimitBound)
        return;

    CpuLimitBound = bound;
    for (auto &ct: Children)
        ct->PropogateCpuLimit();
}

TError TContainer::ApplyCpuLimit() {
    // Child cannot have limit greater than parent.
    // So in order to increase child limit, we first
    // must increase parent limit
    if (CpuLimitBound >= CpuLimitCur) {
        // change == 0, but period could be changed
        auto error = SetCpuLimit(CpuLimit ? CpuLimitBound : 0, CpuPeriod);
        if (error)
            return error;
    }

    if (CpuLimitBound != CpuLimitCur) {
        for (auto &ct: Children) {
            auto error = ct->ApplyCpuLimit();
            if (error)
                return error;
        }
    }

    if (CpuLimitBound < CpuLimitCur) {
        auto error = SetCpuLimit(CpuLimit ? CpuLimitBound : 0, CpuPeriod);
        if (error)
            return error;
    }
    CpuLimitCur = CpuLimitBound;

    return OK;
}

TError TContainer::ApplyExtraProperties() {
    TError error;
    std::unordered_set<std::string> caps;

    // clean old extra properties
    for (const auto &extraProp: EnabledExtraProperties) {
        std::string property = extraProp;
        std::string idx;

        if (ParsePropertyName(property, idx) && !idx.length())
            return TError(EError::InvalidProperty, "Empty property index");

        auto prop = ContainerProperties.find(property);
        if (prop != ContainerProperties.end()) {
            error = prop->second->Reset();
            if (error)
                return error;
            ClearProp(prop->second->Prop);
        }
    }

    EnabledExtraProperties.clear();
    ClearProp(EProperty::EXTRA_PROPS);

    for (const auto &extraProp: ExtraProperties) {
        if (!StringMatch(Name, extraProp.Filter))
            continue;

        for (const auto &extraProperty: extraProp.Properties) {
            std::string property = extraProperty.Name;
            std::string idx;

            if (ParsePropertyName(property, idx) && !idx.length())
                return TError(EError::InvalidProperty, "Empty property index");

            auto prop = ContainerProperties.find(property);
            if (prop != ContainerProperties.end()) {
                if (property == "capabilities") {
                    if (extraProperty.Value == "true") {
                        for (auto i: SplitEscapedString(idx, ';'))
                            caps.insert(i);
                    }
                } else if (!HasProp(prop->second->Prop)) {
                    if (idx.empty())
                        error = prop->second->Set(extraProperty.Value);
                    else
                        error = prop->second->SetIndexed(idx, extraProperty.Value);

                    if (error)
                        return error;

                    EnabledExtraProperties.emplace_back(extraProperty.Name);
                }
            }
        }
    }

    if (!caps.empty()) {
        std::string value;
        std::vector<std::string> extraCaps;
        auto prop = ContainerProperties.find("capabilities");
        bool hasCaps = HasProp(EProperty::CAPABILITIES);

        for (auto cap: caps) {
            if (hasCaps) {
                error = prop->second->GetIndexed(cap, value);
                if (error)
                    return error;

                if (value == "true") {
                    extraCaps.emplace_back(cap);
                    EnabledExtraProperties.emplace_back("capabilities[" + cap + "]");
                }

            } else {
                error = prop->second->SetIndexed(cap, "true");
                if (error)
                    return error;

                extraCaps.emplace_back(cap);
                EnabledExtraProperties.emplace_back("capabilities[" + cap + "]");
            }
        }

        CapExtra.Parse(MergeString(extraCaps, ';'));
    }

    if (!EnabledExtraProperties.empty())
        SetProp(EProperty::EXTRA_PROPS);

    return OK;
}

TError TContainer::CanSetSeccomp() const {
    for (auto ct = CT->Parent.get(); ct; ct = ct->Parent.get()) {
        if (!ct->Seccomp.Empty())
            return TError(EError::InvalidState, "seccomp already set for ancestor");
    }
    std::list<const TContainer *> subtree{this};
    for (auto it = subtree.begin(); it != subtree.end();) {
        for (const auto &child: (*it)->Children) {
            if (!child->Seccomp.Empty())
                return TError(EError::InvalidState, "seccomp already set for descendant");
            subtree.push_back(child.get());
        }
        subtree.erase(it++);
    }
    return OK;
}

TError TContainer::SetSeccomp(const seccomp::TProfile &profile) {
    TSeccompProfile p;
    TError error = p.Parse(profile);
    if (error)
        return error;

    CT->Seccomp = std::move(p);
    CT->SeccompName.clear();

    CT->SetProp(EProperty::SECCOMP);
    CT->ClearProp(EProperty::SECCOMP_NAME);
    return OK;
}

TError TContainer::SetSeccomp(const std::string &name) {
    auto it = SeccompProfiles.find(name);
    if (it == SeccompProfiles.end())
        return TError(EError::InvalidValue, "unkown seccomp profile {}", name);

    CT->Seccomp = it->second;
    CT->SeccompName = name;

    CT->SetProp(EProperty::SECCOMP_NAME);
    CT->ClearProp(EProperty::SECCOMP);
    return OK;
}

TError TContainer::ApplyDynamicProperties(bool onRestore) {
    auto memcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.MemorySubsystem.get());
    auto blkcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.BlkioSubsystem.get());
    TError error;

    if (TestClearPropDirty(EProperty::CPU_SET) && Parent) {
        if (NewCpuJail != CpuJail || CpuSetType == ECpuSetType::Node) {
            error = JailCpus();
            if (error)
                return error;
        }

        if (!CpuJail) {
            error = ApplyCpuSet();
            if (error)
                return error;
        }

        if ((Controllers & CGROUP_MEMORY) && CgroupDriver.MemorySubsystem->SupportNumaBalance()) {
            auto memcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.MemorySubsystem.get());
            if (config().container().enable_numa_migration() && !RootPath.IsRoot() && CpuSetType == ECpuSetType::Node)
                error = CgroupDriver.MemorySubsystem->SetNumaBalance(*memcg, 0, 0);
            else
                error = CgroupDriver.MemorySubsystem->SetNumaBalance(*memcg, 0, 4);

            if (error)
                return error;
        }
    }

    if (TestPropDirty(EProperty::CPU_GUARANTEE)) {
        error = ApplyCpuGuarantee();
        if (error)
            return error;
    }

    if (TestPropsDirty(EProperty::CPU_GUARANTEE, EProperty::CPU_WEIGHT, EProperty::CPU_POLICY)) {
        error = ApplyCpuShares();
        if (error)
            return error;
    }

    if (TestPropsDirty(EProperty::CPU_LIMIT, EProperty::CPU_PERIOD)) {
        error = ApplyCpuLimit();
        if (error)
            return error;
        if (TestPropDirty(EProperty::CPU_LIMIT) && CgroupDriver.MemorySubsystem->SupportHighLimit() &&
            (Controllers & CGROUP_MEMORY))
            SetPropDirty(EProperty::MEM_HIGH_LIMIT);
    }

    if ((!JobMode) && (TestPropsDirty(EProperty::CPU_POLICY, EProperty::CPU_WEIGHT))) {
        error = ApplySchedPolicy();
        if (error) {
            L_ERR("Cannot set scheduler policy: {}", error);
            return error;
        }
    }

    TestClearPropsDirty(EProperty::CPU_GUARANTEE, EProperty::CPU_LIMIT, EProperty::CPU_PERIOD, EProperty::CPU_WEIGHT,
                        EProperty::CPU_POLICY);

    if (TestClearPropDirty(EProperty::MEM_GUARANTEE)) {
        error = CgroupDriver.MemorySubsystem->SetGuarantee(*memcg, MemGuarantee);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_MEM_GUARANTEE, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::MEM_LIMIT)) {
        error = CgroupDriver.MemorySubsystem->SetLimit(*memcg, MemLimit);
        if (error) {
            if (error.Errno == EBUSY)
                return TError(EError::Busy, "Memory limit is too low for current memory usage");
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_MEM_LIMIT, error);
            return error;
        }
        if (CgroupDriver.MemorySubsystem->SupportHighLimit())
            SetPropDirty(EProperty::MEM_HIGH_LIMIT);
    }

    if (TestClearPropDirty(EProperty::MEM_HIGH_LIMIT)) {
        uint64_t highLimit = 0;
        if (MemLimit) {
            uint64_t numCores = std::min(uint64_t(GetNumCores()), CpuLimitBound / CPU_POWER_PER_SEC);
            uint64_t highMargin = 64 * std::max(1UL, numCores) * GetPageSize();
            highLimit = MemLimit - std::min(MemLimit / 4, highMargin);
        }

        error = CgroupDriver.MemorySubsystem->SetHighLimit(*memcg, highLimit);
        if (error) {
            L_ERR("Cannot set high_limit_in_bytes: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::MEM_LOCK_POLICY)) {
        error = memcg->SetUint64(CgroupDriver.MemorySubsystem->MLOCK_POLICY, static_cast<uint64_t>(MemLockPolicy));
        if (error) {
            L_ERR("Cannot set {}: {}", P_MEM_LOCK_POLICY, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ANON_LIMIT)) {
        error = CgroupDriver.MemorySubsystem->SetAnonLimit(*memcg, AnonMemLimit);
        if (error) {
            if (error.Errno == EBUSY)
                return TError(EError::Busy, "Memory limit is too low for current memory usage");
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_ANON_LIMIT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ANON_ONLY)) {
        error = CgroupDriver.MemorySubsystem->SetAnonOnly(*memcg, AnonOnly);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_ANON_ONLY, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::DIRTY_LIMIT))
        PropagateDirtyMemLimit();

    if (TestClearPropDirty(EProperty::RECHARGE_ON_PGFAULT)) {
        error = CgroupDriver.MemorySubsystem->RechargeOnPgfault(*memcg, RechargeOnPgfault);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Cannot set {}: {}", P_RECHARGE_ON_PGFAULT, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::PRESSURIZE_ON_DEATH)) {
        error = UpdateSoftLimit();
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Can't set {}: {}", P_PRESSURIZE_ON_DEATH, error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::IO_LIMIT)) {
        if (IoBpsLimit.count("fs")) {
            error = CgroupDriver.MemorySubsystem->SetIoLimit(*memcg, IoBpsLimit["fs"]);
            if (error) {
                if (error.Errno != EINVAL)
                    L_ERR("Can't set {}: {}", P_IO_LIMIT, error);
                return error;
            }
        }
        error = CgroupDriver.BlkioSubsystem->SetIoLimit(*blkcg, RootPath, IoBpsLimit);
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::IO_OPS_LIMIT)) {
        if (IoOpsLimit.count("fs")) {
            error = CgroupDriver.MemorySubsystem->SetIopsLimit(*memcg, IoOpsLimit["fs"]);
            if (error) {
                if (error.Errno != EINVAL)
                    L_ERR("Can't set {}: {}", P_IO_OPS_LIMIT, error);
                return error;
            }
        }
        error = CgroupDriver.BlkioSubsystem->SetIoLimit(*blkcg, RootPath, IoOpsLimit, true);
        if (error)
            return error;
    }

    if (TestClearPropDirty(EProperty::IO_WEIGHT) || TestPropDirty(EProperty::IO_POLICY)) {
        if (Controllers & CGROUP_BLKIO) {
            error = CgroupDriver.BlkioSubsystem->SetIoWeight(*blkcg, IoPolicy, IoWeight);
            if (error) {
                if (error.Errno != EINVAL)
                    L_ERR("Can't set {}: {}", P_IO_POLICY, error);
                return error;
            }
        }
    }

    if (TestClearPropDirty(EProperty::IO_POLICY)) {
        error = ApplyIoPolicy();
        if (error) {
            L_ERR("Cannot set io policy: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::HUGETLB_LIMIT)) {
        auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.HugetlbSubsystem.get());
        error = CgroupDriver.HugetlbSubsystem->SetHugeLimit(*cg, HugetlbLimit ?: -1);
        if (error) {
            if (error.Errno != EINVAL)
                L_ERR("Can't set {}: {}", P_HUGETLB_LIMIT, error);
            return error;
        }
        if (CgroupDriver.HugetlbSubsystem->SupportGigaPages()) {
            error = CgroupDriver.HugetlbSubsystem->SetGigaLimit(*cg, 0);
            if (error)
                L_WRN("Cannot forbid 1GB pages: {}", error);
        }
    }

    if (TestClearPropDirty(EProperty::NET_LIMIT) || TestClearPropDirty(EProperty::NET_GUARANTEE) ||
        TestClearPropDirty(EProperty::NET_RX_LIMIT)) {
        if (Net) {
            error = Net->SetupClasses(NetClass, true);
            if (error)
                return error;
        }
    }

    if (TestClearPropDirty(EProperty::NET_LIMIT_SOFT)) {
        if (Net) {
            error = Net->UpdateNetLimitSoft(NetLimitSoftValue);
            if (error)
                return error;
        }
    }

    if ((Controllers & CGROUP_NETCLS) && TestClearPropDirty(EProperty::NET_TOS)) {
        auto netcls = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.NetclsSubsystem.get());
        error = CgroupDriver.NetclsSubsystem->SetClass(*netcls, NetClass.LeafHandle | NetClass.DefaultTos);
        if (error) {
            L_ERR("Can't set classid: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::ULIMIT) && !onRestore) {
        for (auto &ct: Subtree()) {
            if (ct->State == EContainerState::Stopped || ct->State == EContainerState::Dead)
                continue;
            error = ct->ApplyUlimits();
            if (error) {
                L_ERR("Cannot update ulimit: {}", error);
                return error;
            }
        }
    }

    if (TestClearPropDirty(EProperty::THREAD_LIMIT)) {
        auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.PidsSubsystem.get());
        error = CgroupDriver.PidsSubsystem->SetLimit(*cg, ThreadLimit);
        if (error) {
            L_ERR("Cannot set thread limit: {}", error);
            return error;
        }
    }

    if (TestClearPropDirty(EProperty::RESOLV_CONF)) {
        error = ApplyResolvConf();
        if (error) {
            L_ERR("Cannot change /etc/resolv.conf contents: {}", error);
            return error;
        }
    }

    if (TestClearPropsDirty(EProperty::DEVICE_CONF, EProperty::DEVICE_CONF_EXPLICIT, EProperty::ENABLE_FUSE)) {
        error = ApplyDeviceConf();
        if (error) {
            if (error != EError::Permission && error != EError::DeviceNotFound)
                L_WRN("Cannot change allowed devices: {}", error);
            return error;
        }
    }

    return OK;
}

void TContainer::ShutdownOom() {
    for (auto &s: Sources)
        EpollLoop->RemoveSource(s->Fd);
    Sources.clear();

    OomEvent.Close();

    if (CgroupDriver.MemorySubsystem->IsCgroup2())
        OomKillEvent.Close();
}

TError TContainer::PrepareOomMonitor() {
    if (IsRoot() || !(Controllers & CGROUP_MEMORY))
        return OK;

    TError error;

    auto memcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.MemorySubsystem.get());
    error = CgroupDriver.MemorySubsystem->SetupOomEvent(*memcg, OomEvent);
    if (error)
        return error;

    auto type = CgroupDriver.MemorySubsystem->IsCgroup2() ? EPOLLPRI : EPOLLIN | EPOLLHUP;
    Sources.push_back(std::make_shared<TEpollSource>(OomEvent.Fd, EPOLL_EVENT_MEM, type, shared_from_this()));

    if (CgroupDriver.MemorySubsystem->IsCgroup2()) {
        auto leafMemcg = memcg->Leaf();
        error = CgroupDriver.MemorySubsystem->SetupOomEvent(*leafMemcg, OomKillEvent);
        if (error)
            goto out;
        Sources.push_back(std::make_shared<TEpollSource>(OomKillEvent.Fd, EPOLL_EVENT_MEM, type, shared_from_this()));
    }

    for (auto &s: Sources) {
        error = EpollLoop->AddSource(s);
        if (error)
            goto out;
    }

out:
    if (error)
        ShutdownOom();
    return error;
}

TDevices TContainer::EffectiveDevices(const TDevices &devices) const {
    if (IsRoot())
        return devices;

    auto base = DevicesExplicit && HasProp(EProperty::DEVICE_CONF) ? TDevices(RootContainer->Devices)
                                                                   : Parent->EffectiveDevices();

    return base.Merge(FuseDevices).Merge(devices);
}

TDevices TContainer::EffectiveDevices() const {
    return EffectiveDevices(Devices);
}

TError TContainer::SetControllers(uint64_t controllers) {
    if ((controllers & RequiredControllers) != RequiredControllers)
        return TError(EError::InvalidValue, "Cannot disable required controllers");

    auto needPropogateCpuGuarantee = (Controllers & CGROUP_CPU) != (controllers & CGROUP_CPU);
    Controllers = controllers;
    SetProp(EProperty::CONTROLLERS);

    if (needPropogateCpuGuarantee)
        PropogateCpuGuarantee();
    return OK;
}

TError TContainer::SetDeviceConf(const TDevices &devices, bool merge) {
    if (!Parent->HostMode && !(devices <= Parent->EffectiveDevices()))
        return TError(EError::Permission, "Device is not permitted for parent container");

    auto d = merge ? (Devices | devices) : devices;
    auto ed = EffectiveDevices(d);

    for (auto &ct: Subtree()) {
        if (ct.get() != this && ct->IsRunningOrMeta() && !(ct->Devices <= ed))
            return TError(EError::InvalidState, "Device is required in running subcontainer {}", ct->Name);
    }

    Devices = d;
    CT->SetProp(EProperty::DEVICE_CONF);
    return OK;
}

TError TContainer::ApplyDeviceConf() {
    if (IsRoot())
        return OK;

    TDevices devices = EffectiveDevices();

    bool unrestricted = HostMode;
    if ((Controllers & CGROUP_DEVICES) && !unrestricted) {
        auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.DevicesSubsystem.get());
        auto error = devices.Apply(*cg);
        if (error)
            return error;
    }

    /* We also setup devices during container mnt ns setup in task.cpp */
    if (Task.Pid && !TPath(Root).IsRoot()) {
        if (InUserNs())
            devices.PrepareForUserNs(UserNsCred);

        auto root = fmt::format("/proc/{}/root", Task.Pid);
        auto error = devices.Makedev(root);
        /* Ignore errors while recreating devices for recently died tasks */
        if (error && error.Errno != ENOENT)
            return error;

        auto newPaths = devices.AllowedPaths();
        for (auto &path: DevicesPath) {
            if (newPaths.find(path) != newPaths.end())
                continue;

            L_ACT("Remove {} device node {}", Slug, path);
            auto error = (root / path).Unlink();
            if (error && error.Errno != ENOENT)
                return error;
        }
        DevicesPath = newPaths;
    }

    for (auto &child: Children) {
        if (!child->Task.Pid)
            continue;

        auto error = child->ApplyDeviceConf();
        if (error)
            return error;
    }

    return OK;
}

TError TContainer::SetSymlink(const TPath &symlink, const TPath &target) {
    TError error;

    if (WaitTask.Pid) {
        TMountNamespace mnt;
        mnt.Cwd = GetCwd();
        mnt.Root = Root;
        mnt.BindCred = TaskCred;

        error = mnt.Enter(Task.Pid);
        if (error)
            return error;

        error = mnt.CreateSymlink(symlink, target);

        TError error2 = mnt.Leave();
        PORTO_ASSERT(!error2);
    }

    if (!error) {
        if (target)
            Symlink[symlink] = target;
        else
            Symlink.erase(symlink);
        SetProp(EProperty::SYMLINK);
    }

    return error;
}

TError TContainer::PrepareCgroups(bool onRestore) {
    TError error;

    if (JobMode) {
        Controllers = 0;
        if (RequiredControllers)
            return TError(EError::InvalidValue, "Cannot use cgroups in virt_mode=job");
        return OK;
    } else {
        Controllers |= CGROUP_FREEZER;
        RequiredControllers |= CGROUP_FREEZER;
        if (CgroupDriver.Cgroup2Subsystem->Supported) {
            Controllers |= CGROUP2;
            RequiredControllers |= CGROUP2;
        }
        if (Level == 1 && CgroupDriver.PidsSubsystem->Supported)
            Controllers |= CGROUP_PIDS;
    }

    if (Controllers & CGROUP_CPUSET)
        SetProp(EProperty::CPU_SET);

    if (OsMode && Isolate && config().container().detect_systemd() && CgroupDriver.SystemdSubsystem->Supported &&
        !(Controllers & CGROUP_SYSTEMD) && !RootPath.IsRoot()) {
        TPath cmd = RootPath / Command;
        TPath dst;
        if (!cmd.ReadLink(dst) && dst.BaseName() == "systemd") {
            L("Enable systemd cgroup for {}", Slug);
            Controllers |= CGROUP_SYSTEMD;
        }
    }

    error = CgroupDriver.CreateContainerCgroups(*this, onRestore);
    if (error)
        return error;

    if (!IsRoot() && (Controllers & CGROUP_MEMORY)) {
        auto memcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.MemorySubsystem.get());
        error = CgroupDriver.MemorySubsystem->SetUseHierarchy(*memcg, true);
        if (error)
            return error;

        /* Link memory cgroup writeback with related blkio cgroup */
        if (LinkMemoryWritebackBlkio) {
            auto blkcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.BlkioSubsystem.get());
            error = CgroupDriver.MemorySubsystem->LinkWritebackBlkio(*memcg, *blkcg);
            if (error)
                return error;
        }
    }

    if ((Controllers & CGROUP_DEVICES) && !onRestore) {
        // Case for OsMode+Root is dirty-dirty hack
        bool unrestricted = HostMode;
        if (!unrestricted) {
            auto devcg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.DevicesSubsystem.get());
            // We need to reset a *:* rwm before its too late:
            // if such cgroup has children we cannot change its devices
            if (CgroupDriver.DevicesSubsystem->Unbound(*devcg))
                SetPropDirty(EProperty::DEVICE_CONF);
        }
    }

    if (Controllers & CGROUP_NETCLS) {
        auto netcls = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.NetclsSubsystem.get());
        error = CgroupDriver.NetclsSubsystem->SetClass(*netcls, NetClass.LeafHandle | NetClass.DefaultTos);
        if (error)
            return error;
    }

    return OK;
}

TError TContainer::GetEnvironment(TEnv &env) const {
    env.ClearEnv();

    env.SetEnv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    env.SetEnv("HOME", GetCwd().ToString());
    env.SetEnv("USER", TaskCred.User());

    env.SetEnv("container", "lxc");

    /* lock these */
    env.SetEnv("PORTO_NAME", Name, true, true);
    env.SetEnv("PORTO_HOST", GetHostName(), true, true);
    env.SetEnv("PORTO_USER", OwnerCred.User(), true, true);

    for (auto &extra: config().container().extra_env())
        env.SetEnv(extra.name(), extra.value(), true, false);

    /* Inherit environment from containts in isolation domain */
    bool overwrite = true;
    for (auto ct = this; ct; ct = ct->Parent.get()) {
        TError error = env.Parse(ct->EnvCfg, overwrite);
        if (error && overwrite)
            return error;
        error = env.Parse(ct->EnvSecret, overwrite, true);
        if (error && overwrite)
            return error;
        overwrite = false;

        if (ct->Isolate)
            break;
    }

    return OK;
}

TError TContainer::ResolvePlace(TPath &place, bool strict) const {
    TError error;

    if (StringStartsWith(place.ToString(), "///"))
        place = RootPath / place.ToString().substr(2);

    if (place.IsAbsolute()) {
        for (auto &policy: PlacePolicy) {
            if (policy[0] == '/' || policy == "***") {
                if (StringMatch(place.ToString(), policy, strict))
                    goto found;
            } else {
                auto sep = policy.find('=');
                if (sep != std::string::npos && StringMatch(place.ToString(), policy.substr(sep + 1), strict))
                    goto found;
            }
        }
    } else {
        auto prefix = place.ToString() + "=";
        for (const auto &policy: PlacePolicy) {
            if (!place) {
                place = policy;
                goto found;
            }
            if (StringStartsWith(policy, prefix)) {
                place = policy.substr(prefix.size());
                goto found;
            }
        }
    }

    return TError(EError::Permission, "Place {} is not permitted", place);

found:
    while (StringEndsWith(place.ToString(), "***")) {
        if (place.ToString().size() > 3) {
            place = place.DirName();
        } else {
            place = PORTO_PLACE;
            break;
        }
    }

    if (!place.IsNormal())
        return TError(EError::InvalidPath, "Place path {} must be normalized", place);

    if (IsSystemPath(place))
        return TError(EError::InvalidPath, "Place path {} in system directory", place);

    return OK;
}

TError TContainer::PrepareTask(TTaskEnv &TaskEnv) {
    TError error;

    TaskEnv.CT = shared_from_this();
    TaskEnv.Client = CL;

    TaskEnv.Cgroups = CgroupDriver.GetContainerCgroups(*this);

    TaskEnv.Mnt.Cwd = GetCwd();

    TaskEnv.Mnt.Root = Root;
    TaskEnv.Mnt.HostRoot = RootPath;

    TaskEnv.Mnt.RootRo = RootRo;

    TaskEnv.Mnt.RunSize = GetMemLimit() / 2;

    TaskEnv.Mnt.BindCred = Parent->RootPath.IsRoot() ? CL->TaskCred : TCred(RootUser, RootGroup);

    if (OsMode && Isolate && (Controllers & CGROUP_SYSTEMD))
        TaskEnv.Mnt.Systemd = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.SystemdSubsystem.get())->GetName();

    TaskEnv.Cred = TaskCred;

    TaskEnv.LoginUid = OsMode ? -1 : OwnerCred.GetUid();

    error = GetEnvironment(TaskEnv.Env);
    if (error)
        return error;

    /* one more fork for creating nested pid-namespace */
    TaskEnv.TripleFork =
        Isolate && TaskEnv.PidFd.GetFd() >= 0 && TaskEnv.PidFd.Inode() != TNamespaceFd::PidInode(getpid(), "ns/pid");

    TaskEnv.QuadroFork = !JobMode && !OsMode && !IsMeta();

    TaskEnv.Mnt.BindMounts = BindMounts;
    TaskEnv.Mnt.Symlink = Symlink;

    /* legacy kludge */
    if (BindDns && !TaskEnv.Mnt.Root.IsRoot()) {
        TBindMount bm;
        bm.Source = "/etc/hosts";
        bm.Target = "/etc/hosts";
        bm.MntFlags |= MS_RDONLY;
        TaskEnv.Mnt.BindMounts.push_back(bm);
    }

    /* Resolve paths in parent namespace and check volume ownership */
    for (auto &bm: TaskEnv.Mnt.BindMounts) {
        if (!bm.Source.IsAbsolute())
            bm.Source = Parent->GetCwd() / bm.Source;

        auto src = TVolume::ResolveOrigin(Parent->RootPath / bm.Source);
        bm.ControlSource = src && !CL->CanControl(src->Volume->VolumeOwner);

        if (!bm.Target.IsAbsolute())
            bm.Target = TaskEnv.Mnt.Cwd / bm.Target;

        auto dst = TVolume::ResolveOrigin(Parent->RootPath / TaskEnv.Mnt.Root / bm.Target);
        bm.ControlTarget = dst && !CL->CanControl(dst->Volume->VolumeOwner);

        /* allow suid inside by default */
        bm.MntFlags |= MS_ALLOW_SUID;

        /* this allows to inject suid binaries into host */
        if (Parent->RootPath.IsRoot() && !TaskEnv.Mnt.Root.IsRoot() && bm.Source.IsDirectoryFollow()) {
            TStatFS stat;
            if (!bm.Source.StatFS(stat) && (stat.MntFlags & MS_ALLOW_SUID)) {
                L("Bindmount source {} allows suid in host", bm.Source);
                TaintFlags.BindWithSuid = true;
            }
        }

        if (bm.MntFlags & MS_ALLOW_DEV) {
            if (!OwnerCred.IsRootUser())
                return TError(EError::Permission, "Not enough permissions to allow devices at bind mount");
        } else
            bm.MntFlags |= MS_NODEV;
    }

    TaskEnv.Mnt.BindPortoSock = AccessLevel != EAccessLevel::None;

    if (IsMeta() || TaskEnv.TripleFork || TaskEnv.QuadroFork) {
        TPath path = TPath(PORTO_HELPERS_PATH) / "portoinit";
        TError error = TaskEnv.PortoInit.OpenRead(path);
        if (error) {
            TPath exe("/proc/self/exe");
            error = exe.ReadLink(path);
            if (error)
                return error;
            path = path.DirName() / "portoinit";
            error = TaskEnv.PortoInit.OpenRead(path);
            if (error)
                return error;
        }
    }

    TaskEnv.Mnt.IsolateRun = TaskEnv.Mnt.Root.IsRoot() && OsMode && Isolate && !InUserNs();

    // Create new mount namespaces if we have to make any changes
    TaskEnv.NewMountNs = Isolate || TaskEnv.Mnt.IsolateRun || (Level == 1 && !HostMode) ||
                         TaskEnv.Mnt.BindMounts.size() || Hostname.size() || ResolvConf.size() || EtcHosts.size() ||
                         !TaskEnv.Mnt.Root.IsRoot() || TaskEnv.Mnt.RootRo || !TaskEnv.Mnt.Systemd.empty() || Fuse;

    if (TaskEnv.NewMountNs && (HostMode || JobMode))
        return TError(EError::InvalidValue, "Cannot change mount-namespace in this virt_mode");

    return OK;
}

void TContainer::SanitizeCapabilities() {
    bool pidns = false;
    bool memcg = false;
    bool netns = false;
    bool found = false;

    CapBound = CapLimit;

    for (auto ct = this; ct; ct = ct->Parent.get()) {
        pidns |= ct->Isolate;
        // TODO(kndrvt): change to Controllers later
        memcg |= ct->MemLimit && ct->HasProp(EProperty::MEM_LIMIT);
        netns |= ct->NetIsolate;

        if (!found && !HasProp(EProperty::CAPABILITIES) && (ct->HasProp(EProperty::CAPABILITIES) || ct->Level == 1)) {
            CapLimit = ct->CapLimit;
            CapBound = CapLimit;
            found = true;
        }

        if (ct->HasProp(EProperty::CAPABILITIES) || ct->Level == 1)
            CapBound &= ct->CapLimit;
    }

    if (!HasProp(EProperty::CAPABILITIES)) {
        if (HostMode) {
            CapBound = AllCapabilities;
            return;
        } else if (OwnerCred.IsRootUser()) {
            CapBound = RootCapabilities;
            return;
        }
    }

    TCapabilities remove;
    if (!pidns)
        remove |= PidNsCapabilities;
    if (!memcg)
        remove |= MemCgCapabilities;
    if (!netns)
        remove |= NetNsCapabilities;

    if (HasProp(EProperty::CAPABILITIES))
        remove &= ~CapLimit;
    if (HasProp(EProperty::CAPABILITIES_AMBIENT))
        remove &= ~CapAmbient;

    CapBound &= ~remove;
    // Extra property capabilities (old hack)
    CapBound |= (CapLimit & CapExtra);
}

void TContainer::SanitizeCapabilitiesAll() {
    for (auto &ct: Subtree())
        ct->SanitizeCapabilities();
}

TUlimit TContainer::GetUlimit() const {
    TUlimit res = Ulimit;

    for (auto p = Parent.get(); p; p = p->Parent.get())
        res.Merge(p->Ulimit, false);

    uint64_t memlock = 0;
    if (config().container().memlock_margin()) {
        memlock = GetMemLimit(false); /* limit must be set explicitly */
        memlock -= std::min(memlock, config().container().memlock_margin());
    }
    if (memlock < config().container().memlock_minimal())
        memlock = config().container().memlock_minimal();
    if (memlock)
        res.Set(RLIMIT_MEMLOCK, memlock, RLIM_INFINITY, false);

    return res;
}

TError TContainer::StartTask() {
    TTaskEnv TaskEnv;
    TError error;

    if (!UserNs) {
        error = TNetwork::StartNetwork(*this, TaskEnv);
        if (error)
            return error;
    }

    if (IsRoot())
        return OK;

    /* After restart apply all set dynamic properties */
    memcpy(PropDirty, PropSet, sizeof(PropDirty));

    /* Applied by starting task */
    TestClearPropDirty(EProperty::RESOLV_CONF);

    error = ApplyDynamicProperties();
    if (error)
        return error;

    error = TaskEnv.OpenNamespaces(*this);
    if (error)
        return error;

    error = PrepareTask(TaskEnv);
    if (error)
        return error;

    /* Meta container without namespaces don't need task */
    if (IsMeta() && !Isolate && NetInherit && !TaskEnv.NewMountNs)
        return OK;

    error = TaskEnv.Start();
    DevicesPath = EffectiveDevices().AllowedPaths();

    /* Always report OOM situation if any */
    if (error) {
        CollectMemoryEvents();
        if (OomKills || OomEvents)
            error = TError(EError::ResourceNotAvailable, "OOM at container {} start: {}", Name, error);
    }

    return error;
}

TError TContainer::StartParents() {
    TError error;

    if (ActionLocked >= 0 || CL->LockedContainer.get() != this) {
        L_ERR("Container is not locked");
        return TError(EError::Busy, "Container is not locked");
    }

    if (!Parent)
        return OK;

    auto cg = CgroupDriver.GetContainerCgroup(*Parent, CgroupDriver.FreezerSubsystem.get());
    if (CgroupDriver.FreezerSubsystem->IsFrozen(*cg))
        return TError(EError::InvalidState, "Parent container is frozen");

    if (IsRunningOrMeta(Parent->State))
        return OK;

    if (!config().container().enable_start_parents())
        return TError(EError::InvalidState, "Parent container is stopped");

    L_TAINT("Starting with stopped parent is deprecated");

    /* In case client is portod, we don't start parents
     * PORTO-1010
     */
    if (CL->IsPortod())
        return TError(EError::Unknown, "Cannot start parents of portod's container");

    auto current = CL->LockedContainer;

    std::shared_ptr<TContainer> target;
    do {
        target = Parent;
        while (target->Parent && !IsRunningOrMeta(target->Parent->State))
            target = target->Parent;

        CL->ReleaseContainer();

        error = CL->LockContainer(target);
        if (error)
            return error;

        if (!IsRunningOrMeta(target->State)) {
            error = target->Start();
            if (error)
                return error;
        }
    } while (target != Parent);

    CL->ReleaseContainer();

    return CL->LockContainer(current);
}

TError TContainer::PrepareStart() {
    TError error;

    error = CL->CanControl(OwnerCred);
    if (error)
        return error;

    if (Parent) {
        CT = std::const_pointer_cast<TContainer>(shared_from_this());
        LockStateWrite();

        error = ApplyExtraProperties();

        if (!error) {
            for (auto &knob: ContainerProperties) {
                error = knob.second->Start();
                if (error)
                    break;
            }
        }

        UnlockState();
        CT = nullptr;
        if (error)
            return error;
    }

    if (HasProp(EProperty::ROOT) && RootPath.IsRegularFollow()) {
        std::shared_ptr<TVolume> vol;
        rpc::TVolumeSpec spec;

        L("Emulate deprecated loop root={} for {}", RootPath, Slug);
        TaintFlags.RootOnLoop = true;

        spec.set_backend("loop");
        spec.set_storage(RootPath.ToString());
        spec.set_read_only(RootRo);
        spec.add_links()->set_container(ROOT_PORTO_NAMESPACE + Name);

        auto current = CL->LockedContainer;
        CL->ReleaseContainer();

        error = TVolume::Create(spec, vol);
        if (error) {
            L_ERR("Cannot create root volume: {}", error);
            return error;
        }

        error = CL->LockContainer(current);
        if (error)
            return error;

        L("Replace root={} with volume {}", Root, vol->Path);
        LockStateWrite();
        RootPath = vol->Path;
        Root = vol->Path.ToString();
        UnlockState();
    }

    (void)TaskCred.InitGroups(TaskCred.User());

    SanitizeCapabilities();

    if (TaintFlags.TaintCounted && Statistics->ContainersTainted)
        Statistics->ContainersTainted--;

    TaintFlags.TaintCounted = 0;
    TaintFlags.BindWithSuid = false;

    /* Check target task credentials */
    error = CL->CanControl(TaskCred);
    if (!error && !OwnerCred.IsMemberOf(TaskCred.GetGid()) && !CL->IsSuperUser()) {
        TCred cred;
        cred.Init(TaskCred.User());
        if (!cred.IsMemberOf(TaskCred.GetGid()))
            error = TError(EError::Permission, "Cannot control group " + TaskCred.Group());
    }

    /*
     * Allow any user:group in chroot
     * FIXME: non-racy chroot validation is impossible for now
     */
    if (error && !RootPath.IsRoot())
        error = OK;

    /* Allow any user:group in sub-container if client can change uid/gid */
    if (error && CL->CanSetUidGid() && IsChildOf(*CL->ClientContainer))
        error = OK;

    if (error)
        return error;

    /* Even without capabilities user=root require chroot */
    if (RootPath.IsRoot() && TaskCred.IsRootUser() && !OwnerCred.IsRootUser())
        return TError(EError::Permission, "user=root requires chroot");

    if (HostMode && Level) {
        if (!Parent->HostMode)
            return TError(EError::Permission, "For virt_mode=host parent must be in the same mode");
        if (!CL->ClientContainer->HostMode)
            return TError(EError::Permission, "For virt_mode=host client must be in the same mode");
    }

    if (JobMode && Level == 1)
        return TError(EError::InvalidValue, "Container in virt_mode=job without parent");

    if (Parent && Parent->JobMode)
        return TError(EError::InvalidValue, "Parent container in virt_mode=job");

    if (JobMode && RequiredControllers)
        return TError(EError::InvalidValue, "Cannot use cgroups in virt_mode=job");

    if (JobMode && IsMeta())
        return TError(EError::InvalidValue, "Job container without command considered useless");

    if (HasProp(EProperty::USERNS)) {
        if (UserNs && (HostMode || JobMode))
            return TError(EError::InvalidValue, "userns=true incompatible with virt_mode");
        // TODO: perhaps remove Fuse here later
    } else if (Fuse) {
        LockStateWrite();
        UserNs = true;
        UnshareOnExec = true;
        SetProp(EProperty::USERNS);
        UnlockState();
    }

    /* CapLimit >= CapBound */
    if (CapBound & ~CapLimit) {
        TCapabilities cap = CapBound;
        cap &= ~CapLimit;
        if ((!OwnerCred.IsRootUser() && !HostMode) || HasProp(EProperty::CAPABILITIES))
            return TError(EError::Permission, "Capabilities out of bounds: " + cap.Format());
    }

    /* CapBound >= CapAmbient */
    if (CapAmbient & ~CapBound) {
        TCapabilities cap = CapAmbient;
        cap &= ~CapBound;
        return TError(EError::Permission, "Ambient capabilities out of bounds: " + cap.Format());
    }

    if (!Parent) {
        /* Root container */
    } else if (!HasProp(EProperty::PLACE)) {
        /* Inherit parent policy */
        PlacePolicy = Parent->PlacePolicy;
    } else {
        /* Enforce place restictions */
        for (const auto &policy: PlacePolicy) {
            TPath place = policy;
            const auto wildcardPos = policy.find("***");
            if (wildcardPos != std::string::npos && wildcardPos < policy.size() - 3) {
                auto policyEndPart = policy.substr(wildcardPos, policy.size() - wildcardPos);
                auto otherChar = find_if(policyEndPart.begin(), policyEndPart.end(),
                                         [](const char &c) { return c != '*' && c != '/'; });
                if (otherChar != policyEndPart.end())
                    return TError(EError::Permission, "Wildcard allowed only at the end of the place");
            }

            if (!place.IsAbsolute()) {
                if (policy == "***") {
                    if (std::find(Parent->PlacePolicy.begin(), Parent->PlacePolicy.end(), policy) ==
                        Parent->PlacePolicy.end())
                        return TError(EError::Permission, "Place {} is not allowed by parent container", policy);
                    continue;
                }
                auto sep = policy.find('=');
                if (sep != std::string::npos && policy[sep + 1] == '/')
                    place = policy.substr(sep + 1);
            }
            if (Parent->ResolvePlace(place))
                return TError(EError::Permission, "Place {} is not allowed by parent container", policy);
        }
    }

    return OK;
}

TError TContainer::Start() {
    TError error;

    if (State != EContainerState::Stopped)
        return TError(EError::InvalidState, "Cannot start container {} in state {}", Name, StateName(State));

    error = StartParents();
    if (error)
        return error;

    /*
     * Container can already be started (and even dead) due to non-atomical lock
     * transfers between parents and children in StartParents() above
     */
    if (State != EContainerState::Stopped) {
        if (IsRunningOrMeta()) {
            return OK;
        } else {
            return TError(EError::InvalidState, "Cannot start container {} in state {}", Name, StateName(State));
        }
    }

    StartError = OK;

    error = PrepareStart();
    if (error) {
        error = TError(error, "Cannot prepare start for container {}", Name);
        goto err_prepare;
    }

    L_ACT("Start {}", Slug);

    SetState(EContainerState::Starting);

    StartTime = GetCurrentTimeMs();
    RealStartTime = time(nullptr);
    SetProp(EProperty::START_TIME);

    error = PrepareResources();
    if (error)
        goto err_prepare;

    error = PrepareRuntimeResources();
    if (error)
        goto err;

    /* Complain about insecure misconfiguration */
    for (auto &taint: Taint()) {
        L_TAINT(taint);
        if (!TaintFlags.TaintCounted) {
            TaintFlags.TaintCounted = true;
            Statistics->ContainersTainted++;
        }
    }

    DowngradeActionLock();

    error = StartTask();

    UpgradeActionLock();

    if (error) {
        SetState(EContainerState::Stopping);
        (void)Terminate(0);
        goto err;
    }

    if (IsMeta())
        SetState(EContainerState::Meta);
    else
        SetState(EContainerState::Running);

    SetProp(EProperty::ROOT_PID);

    error = Save();
    if (error) {
        L_ERR("Cannot save state after start {}", error);
        (void)Reap(false);
        goto err;
    }

    Statistics->ContainersStarted++;

    return OK;

err:
    TNetwork::StopNetwork(*this);
    FreeRuntimeResources();
    (void)FreeResources();
err_prepare:
    StartError = error;
    SetState(EContainerState::Stopped);
    Statistics->ContainersFailedStart++;

    return error;
}

TError TContainer::PrepareResources() {
    TError error;

    if (IsRoot()) {
        error = BuildCpuTopology();
        if (error)
            return error;
    }

    error = CheckMemGuarantee();
    if (error) {
        Statistics->FailMemoryGuarantee++;
        return error;
    }

    error = CreateWorkDir();
    if (error)
        return error;

    TNetwork::InitClass(*this);

    error = PrepareCgroups();
    if (error) {
        L_ERR("Can't prepare task cgroups: {}", error);
        goto undo;
    }

    if (RequiredVolumes.size()) {
        error = TVolume::CheckRequired(*this);
        if (error)
            goto undo;
    }

    return OK;

undo:
    (void)FreeResources();
    return error;
}

/* Some resources are not required in dead state */

TError TContainer::PrepareRuntimeResources() {
    TError error;

    error = PrepareOomMonitor();
    if (error) {
        L_ERR("Cannot prepare OOM monitor: {}", error);
        return error;
    }

    error = UpdateSoftLimit();
    if (error) {
        L_ERR("Cannot update memory soft limit: {}", error);
        return error;
    }

    return OK;
}

void TContainer::FreeRuntimeResources() {
    TError error;

    CollectMemoryEvents();
    ShutdownOom();

    error = UpdateSoftLimit();
    if (error)
        L_ERR("Cannot update memory soft limit: {}", error);

    if (CpuJail)
        UnjailCpus(CpuAffinity);
}

TError TContainer::FreeResources(bool ignore) {
    /*
     * The ignore case is used on startup path,
     * when the user thing hasn't even started
     */

    TError error;

    if (IsRoot())
        return OK;

    OomKills = 0;
    ClearProp(EProperty::OOM_KILLS);

    if (!JobMode) {
        error = CgroupDriver.RemoveContainerCgroups(*this, ignore);
        if (error)
            return error;
    }

    RemoveWorkDir();

    Stdout.Remove(*this);
    Stderr.Remove(*this);

    return OK;
}

TError TContainer::Kill(int sig) {
    if (State != EContainerState::Running)
        return TError(EError::InvalidState, "invalid container state={}", TContainer::StateName(State));

    L_ACT("Kill task {} in {}", Task.Pid, Slug);
    return Task.Kill(sig);
}

TError TContainer::Terminate(uint64_t deadline) {
    TError error;

    if (IsRoot())
        return TError(EError::Permission, "Cannot terminate root container");

    L_ACT("Terminate tasks in {}", Slug);

    if (!(Controllers & CGROUP_FREEZER) && !JobMode) {
        if (Task.Pid)
            return TError(EError::NotSupported, "Cannot terminate without freezer");
        return OK;
    }

    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    if (cg->IsEmpty())
        return OK;

    if (Task.Pid && deadline && !IsMeta()) {
        int sig = SIGTERM;

        if (OsMode && Isolate) {
            uint64_t mask = TaskHandledSignals(Task.Pid);
            if (mask & BIT(SIGPWR - 1))
                sig = SIGPWR;
            else if (!(mask & BIT(SIGTERM - 1)))
                sig = 0;
        }

        if (sig) {
            if (JobMode)
                error = Task.KillPg(sig);
            else
                error = Task.Kill(sig);
            if (!error) {
                L_ACT("Wait task {} after signal {} in {}", Task.Pid, sig, Slug);
                while (Task.Exists() && !Task.IsZombie() && !WaitDeadline(deadline))
                    ;
            }
        }
    }

    if (JobMode)
        return WaitTask.KillPg(SIGKILL);

    if (WaitTask.Pid && Isolate) {
        error = WaitTask.Kill(SIGKILL);
        if (error)
            return error;
    }

    if (cg->IsEmpty())
        return OK;

    error = cg->KillAll(SIGKILL, Fuse);
    if (error)
        return error;

    return OK;
}

void TContainer::ForgetPid() {
    Task.Pid = 0;
    TaskVPid = 0;
    WaitTask.Pid = 0;
    ClearProp(EProperty::ROOT_PID);
    SeizeTask.Pid = 0;
    ClearProp(EProperty::SEIZE_PID);
}

TError TContainer::Stop(uint64_t timeout) {
    uint64_t deadline = timeout ? GetCurrentTimeMs() + timeout : 0;
    auto freezer = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    TError error;

    if (State == EContainerState::Stopped)
        return OK;

    if (!(Controllers & CGROUP_FREEZER) && !JobMode) {
        if (Task.Pid)
            return TError(EError::NotSupported, "Cannot stop without freezer");
    } else if (CgroupDriver.FreezerSubsystem->IsParentFreezing(*freezer))
        return TError(EError::InvalidState, "Parent container is paused");

    auto subtree = Subtree();

    /* Downgrade explusive lock if we are going to wait. */
    if (timeout)
        CL->LockedContainer->DowngradeActionLock();

    if (!timeout) {
        L_ACT("Killing spree");
        for (auto it = subtree.rbegin(); it != subtree.rend(); ++it)
            if ((*it)->Isolate && (*it)->WaitTask.Pid)
                (void)(*it)->WaitTask.Kill(SIGKILL);
    }

    for (auto &ct: subtree) {
        if (ct->IsRoot() || ct->State == EContainerState::Stopped)
            continue;

        if (JobMode && (ct->State == EContainerState::Dead)) {
            ct->SetState(EContainerState::Stopping);
            continue;
        }

        ct->SetState(EContainerState::Stopping);

        error = ct->Terminate(deadline);
        if (error)
            L_ERR("Cannot terminate tasks in {}: {}", Slug, error);

        auto cg = CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.FreezerSubsystem.get());
        if (CgroupDriver.FreezerSubsystem->IsSelfFreezing(*cg) && !JobMode) {
            L_ACT("Thaw terminated paused {}", ct->Slug);
            error = CgroupDriver.FreezerSubsystem->Thaw(*cg, false);
            if (error)
                L_ERR("Cannot thaw {}: {}", ct->Slug, error);
        }
    }

    if (timeout)
        CL->LockedContainer->UpgradeActionLock();

    for (auto &ct: subtree) {
        if (ct->State == EContainerState::Stopped)
            continue;

        L_ACT("Stop {}", Slug);

        TNetwork::StopNetwork(*ct);
        ct->FreeRuntimeResources();
        error = ct->FreeResources(false);

        ct->LockStateWrite();

        ct->ForgetPid();

        if (error) {
            // Prevent deletion because of aging
            ct->StartTime = ct->DeathTime;
            ct->RealStartTime = ct->RealDeathTime;
            ct->DeathTime = GetCurrentTimeMs();
            ct->RealDeathTime = time(nullptr);
            ct->UnlockState();

            ct->SetState(EContainerState::Dead);
            (void)ct->Save();

            return error;
        }

        ct->StartTime = 0;
        ct->RealStartTime = 0;
        ct->ClearProp(EProperty::START_TIME);

        ct->DeathTime = 0;
        ct->RealDeathTime = 0;
        ct->ClearProp(EProperty::DEATH_TIME);

        ct->ExitStatus = 0;
        ct->ClearProp(EProperty::EXIT_STATUS);

        ct->OomEvents = 0;
        ct->OomKilled = false;
        ct->ClearProp(EProperty::OOM_KILLED);

        ct->UnlockState();

        ct->SetState(EContainerState::Stopped);

        error = ct->Save();
        if (error)
            return error;
    }

    return OK;
}

void TContainer::Reap(bool oomKilled) {
    TError error;

    error = Terminate(0);
    if (error)
        L_WRN("Cannot terminate {} : {}", Slug, error);

    LockStateWrite();

    if (!HasProp(EProperty::DEATH_TIME)) {
        DeathTime = GetCurrentTimeMs();
        RealDeathTime = time(nullptr);
        SetProp(EProperty::DEATH_TIME);
    }

    if (oomKilled) {
        OomKilled = oomKilled;
        SetProp(EProperty::OOM_KILLED);
    }

    ForgetPid();

    UnlockState();

    Stdout.Rotate(*this);
    Stderr.Rotate(*this);

    SetState(EContainerState::Dead);

    FreeRuntimeResources();

    error = Save();
    if (error)
        L_WRN("Cannot save container state after exit: {}", error);
}

void TContainer::Exit(int status, bool oomKilled) {
    if (State == EContainerState::Stopped)
        return;

    /*
     * SIGKILL could be delivered earlier than OOM event.
     * Any non-zero exit code or signal might be casuesd by OOM.
     */
    CollectMemoryEvents(OomEvent.Fd);
    if (status && OomIsFatal && OomEvents > 0)
        oomKilled = true;

    /* Detect fatal signals: portoinit cannot kill itself */
    if (WaitTask.Pid != Task.Pid && WIFEXITED(status) && WEXITSTATUS(status) > 128 &&
        WEXITSTATUS(status) < 128 + SIGRTMIN * 2)
        status = WEXITSTATUS(status) - ((WEXITSTATUS(status) > 128 + SIGRTMIN) ? SIGRTMIN : 128);

    L_EVT("Exit {} {} {}", Slug, FormatExitStatus(status), (oomKilled ? "invoked by OOM" : ""));

    LockStateWrite();
    ExitStatus = status;
    SetProp(EProperty::EXIT_STATUS);
    UnlockState();

    for (auto &ct: Subtree()) {
        if (ct->State != EContainerState::Stopped && ct->State != EContainerState::Dead)
            ct->Reap(oomKilled);
    }
}

TError TContainer::Pause() {
    if (State != EContainerState::Running && State != EContainerState::Meta)
        return TError(EError::InvalidState, "Contaner not running");

    if (!(Controllers & CGROUP_FREEZER))
        return TError(EError::NotSupported, "Cannot pause without freezer");

    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    TError error = CgroupDriver.FreezerSubsystem->Freeze(*cg);
    if (error)
        return error;

    for (auto &ct: Subtree()) {
        if (ct->State == EContainerState::Running || ct->State == EContainerState::Meta) {
            ct->SetState(EContainerState::Paused);
            error = ct->Save();
            if (error)
                L_ERR("Cannot save state after pause: {}", error);
        }
    }

    return OK;
}

TError TContainer::Resume() {
    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    if (!(Controllers & CGROUP_FREEZER))
        return TError(EError::NotSupported, "Cannot resume without freezer");

    if (CgroupDriver.FreezerSubsystem->IsParentFreezing(*cg))
        return TError(EError::InvalidState, "Parent container is paused");

    if (!CgroupDriver.FreezerSubsystem->IsSelfFreezing(*cg))
        return TError(EError::InvalidState, "Container not paused");

    TError error = CgroupDriver.FreezerSubsystem->Thaw(*cg);
    if (error)
        return error;

    for (auto &ct: Subtree()) {
        auto cg = CgroupDriver.GetContainerCgroup(*ct, CgroupDriver.FreezerSubsystem.get());
        if (CgroupDriver.FreezerSubsystem->IsSelfFreezing(*cg))
            CgroupDriver.FreezerSubsystem->Thaw(*cg, false);
        if (ct->State == EContainerState::Paused) {
            ct->SetState(IsMeta() ? EContainerState::Meta : EContainerState::Running);
        }
        error = ct->Save();
        if (error)
            L_ERR("Cannot save state after resume: {}", error);
    }

    return OK;
}

TError TContainer::MayRespawn() {
    if (State != EContainerState::Dead && State != EContainerState::Respawning)
        return TError(EError::InvalidState, "Cannot respawn: container in state={}", TContainer::StateName(State));

    if (Parent->State != EContainerState::Running && Parent->State != EContainerState::Meta &&
        Parent->State != EContainerState::Respawning)
        return TError(EError::InvalidState, "Cannot respawn: parent container in state={}",
                      TContainer::StateName(Parent->State));

    if (RespawnLimit >= 0 && RespawnCount >= RespawnLimit)
        return TError(EError::ResourceNotAvailable, "Cannot respawn: reached max_respawns={}", RespawnLimit);

    return OK;
}

TError TContainer::ScheduleRespawn() {
    TError error = MayRespawn();
    if (!error) {
        L_ACT("Change {} state {} -> {}", Slug, StateName(State), StateName(EContainerState::Respawning));
        State = EContainerState::Respawning;
        if (Parent->State == EContainerState::Respawning) {
            L_ACT("Respawn {} after respawning parent", Slug);
        } else {
            auto delay = std::max(50UL, RespawnDelay / 1000000);
            L_ACT("Respawn {} after {} ms", Slug, delay);
            TEvent e(EEventType::Respawn, shared_from_this());
            EventQueue->Add(delay, e);
        }
    }
    return error;
}

TError TContainer::Respawn() {
    TError error;

    error = MayRespawn();
    if (error) {
        if (State == EContainerState::Respawning)
            SetState(EContainerState::Dead);
        return error;
    }

    if (Parent->State == EContainerState::Respawning) {
        if (State != EContainerState::Respawning)
            SetState(EContainerState::Respawning);
        L_ACT("Respawn {} after respawning parent", Slug);
        return OK;
    }

    LockStateWrite();
    RespawnCount++;
    SetProp(EProperty::RESPAWN_COUNT);
    UnlockState();

    L_ACT("Respawn {} try {}", Slug, RespawnCount);

    TNetwork::StopNetwork(*this);

    StartError = OK;

    error = PrepareStart();
    if (error) {
        error = TError(error, "Cannot prepare respawn for container {}", Name);
        goto err_prepare;
    }

    StartTime = GetCurrentTimeMs();
    RealStartTime = time(nullptr);
    SetProp(EProperty::START_TIME);

    error = PrepareRuntimeResources();
    if (error)
        goto err;

    CL->LockedContainer->DowngradeActionLock();

    error = StartTask();

    CL->LockedContainer->UpgradeActionLock();

    if (error) {
        (void)Terminate(0);
        goto err;
    }

    if (IsMeta())
        SetState(EContainerState::Meta);
    else
        SetState(EContainerState::Running);

    SetProp(EProperty::ROOT_PID);

    error = Save();
    if (error) {
        L_ERR("Cannot save state after respawn {}", error);
        (void)Reap(false);
        goto err;
    }

    Statistics->ContainersStarted++;

    for (auto &child: Childs()) {
        child->LockStateWrite();
        if (child->State == EContainerState::Respawning || child->AutoRespawn)
            child->ScheduleRespawn();
        child->UnlockState();
    }

    return OK;

err:
    TNetwork::StopNetwork(*this);
    FreeRuntimeResources();

err_prepare:
    StartError = error;
    DeathTime = GetCurrentTimeMs();
    RealDeathTime = time(nullptr);
    SetProp(EProperty::DEATH_TIME);

    Statistics->ContainersFailedStart++;
    L("Cannot respawn {} - {}", Slug, error);
    AccountErrorType(error);
    SetState(EContainerState::Dead);
    (void)Save();
    return error;
}

void TContainer::SyncProperty(const std::string &name) {
    PORTO_ASSERT(IsStateLockedRead());
    if (StringStartsWith(name, "net_") && Net)
        Net->SyncStat();
    if (StringStartsWith(name, "oom_kills"))
        for (auto &ct: Subtree())
            ct->CollectOomKills();
}

void TContainer::SyncPropertiesAll() {
    TNetwork::SyncAllStat();
    for (auto &ct: RootContainer->Subtree())
        ct->CollectOomKills();
}

TError TContainer::HasProperty(const std::string &property) const {
    std::string name = property, index;
    TError error;

    if (!ParsePropertyName(name, index)) {
        auto dot = name.find('.');
        if (dot != std::string::npos) {
            std::string type = property.substr(0, dot);

            if (type.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) == std::string::npos) {
                auto lock = LockContainers();
                return GetLabel(property, type);
            }

            if (State == EContainerState::Stopped)
                return TError(EError::InvalidState, "Not available in stopped state");

            if (!CgroupDriver.HasContainerCgroupsKnob(*this, property))
                return TError(EError::InvalidProperty, "Unknown cgroup attribute: {}", property);

            return OK;
        }
    }

    auto it = ContainerProperties.find(name);
    if (it == ContainerProperties.end())
        return TError(EError::InvalidProperty, "Unknown property");

    auto prop = it->second;

    if (!prop->IsSupported)
        return TError(EError::NotSupported, "Not supported");

    if (prop->Prop != EProperty::NONE && !HasProp(prop->Prop))
        return TError(EError::NoValue, "Property not set");

    if (prop->RequireControllers) {
        if (State == EContainerState::Stopped)
            return TError(EError::InvalidState, "Not available in stopped state");
        if (!(prop->RequireControllers & Controllers))
            return TError(EError::NoValue, "Controllers is disabled");
    }

    CT = std::const_pointer_cast<TContainer>(shared_from_this());
    error = prop->Has();
    CT = nullptr;

    return error;
}

TError TContainer::GetProperty(const std::string &origProperty, std::string &value) const {
    TError error;
    std::string property = origProperty;
    std::string idx;

    if (!ParsePropertyName(property, idx)) {
        auto dot = property.find('.');

        if (dot != std::string::npos) {
            std::string type = property.substr(0, dot);

            if (type.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) == std::string::npos) {
                auto lock = LockContainers();
                return GetLabel(property, value);
            }

            if (State == EContainerState::Stopped)
                return TError(EError::InvalidState, "Not available in stopped state: " + property);

            return CgroupDriver.GetContainerCgroupsKnob(*this, property, value);
        }
    } else if (!idx.length()) {
        return TError(EError::InvalidProperty, "Empty property index");
    }

    auto it = ContainerProperties.find(property);
    if (it == ContainerProperties.end())
        return TError(EError::InvalidProperty, "Unknown container property: " + property);
    auto prop = it->second;

    CT = std::const_pointer_cast<TContainer>(shared_from_this());
    error = prop->CanGet();
    if (!error) {
        if (idx.length())
            error = prop->GetIndexed(idx, value);
        else
            error = prop->Get(value);
    }
    CT = nullptr;

    return error;
}

TError TContainer::SetProperty(const std::string &origProperty, const std::string &origValue) {
    if (IsRoot())
        return TError(EError::Permission, "System containers are read only");

    std::string property = origProperty;
    std::string idx;

    if (ParsePropertyName(property, idx) && !idx.length())
        return TError(EError::InvalidProperty, "Empty property index");

    std::string value = StringTrim(origValue);
    TError error;

    auto it = ContainerProperties.find(property);
    if (it == ContainerProperties.end()) {
        auto dot = property.find('.');

        if (dot != std::string::npos) {
            std::string type = property.substr(0, dot);

            if (type.find_first_not_of(PORTO_LABEL_PREFIX_CHARS) == std::string::npos) {
                error = TContainer::ValidLabel(property, value);
                if (error)
                    return error;
                auto lock = LockContainers();
                SetLabel(property, value);
                lock.unlock();
                TContainerWaiter::ReportAll(*this, property, value);
                return Save();
            }
        }

        return TError(EError::InvalidProperty, "Invalid property " + property);
    }
    auto prop = it->second;

    CT = std::const_pointer_cast<TContainer>(shared_from_this());

    error = prop->CanSet();

    if (!error && prop->RequireControllers)
        error = EnableControllers(prop->RequireControllers);

    std::string oldValue;
    if (!error)
        error = prop->Get(oldValue);

    if (!error) {
        if (idx.length())
            error = prop->SetIndexed(idx, value);
        else
            error = prop->Set(value);
    }

    if (!error && HasResources()) {
        error = ApplyDynamicProperties();
        if (error) {
            (void)prop->Set(oldValue);
            (void)TestClearPropDirty(prop->Prop);
        }
    }

    CT = nullptr;

    if (!error) {
        auto it = std::find(EnabledExtraProperties.begin(), EnabledExtraProperties.end(), origProperty);
        if (it != EnabledExtraProperties.end()) {
            EnabledExtraProperties.erase(it);
            if (EnabledExtraProperties.empty())
                ClearProp(EProperty::EXTRA_PROPS);
        }

        error = Save();
    }

    return error;
}

bool TContainer::MatchLabels(const rpc::TStringMap &labels) const {
    bool matchLabels = true;
    for (const auto &label: labels.map()) {
        bool matchLabel = false;
        for (const auto &it: Labels) {
            if (it.first == label.key() && it.second == label.val()) {
                matchLabel = true;
                break;
            }
        }
        if (!matchLabel) {
            matchLabels = false;
            break;
        }
    }
    return matchLabels;
}

TError TContainer::Load(const rpc::TContainerSpec &spec, bool restoreOnError) {
    TError error;
    rpc::TContainerSpec oldSpec;

    PORTO_ASSERT(!CT);
    CT = std::const_pointer_cast<TContainer>(shared_from_this());
    LockStateWrite();
    ChangeTime = time(nullptr);
    for (auto &it: ContainerProperties) {
        auto prop = it.second;
        if (!prop->Has(spec))
            continue;
        error = prop->CanSet();
        if (!error && prop->RequireControllers)
            error = EnableControllers(prop->RequireControllers);
        if (!error) {
            if (restoreOnError)
                prop->Dump(oldSpec);
            error = prop->Load(spec);
        }
        if (error)
            break;
    }
    SanitizeCapabilities();
    UnlockState();
    CT = nullptr;

    if (!error && HasResources())
        error = ApplyDynamicProperties();

    if (!error)
        error = Save();

    if (error && restoreOnError)
        Load(oldSpec, false);

    return error;
}

void TContainer::Dump(const std::vector<std::string> &props, std::unordered_map<std::string, std::string> &propsOps,
                      rpc::TContainer &spec) {
    PORTO_ASSERT(!CT);
    CT = std::const_pointer_cast<TContainer>(shared_from_this());
    LockStateRead();
    if (props.empty()) {
        for (auto &it: ContainerProperties) {
            auto prop = it.second;
            if (!prop->CanGet()) {
                if (prop->IsReadOnly)
                    prop->Dump(*spec.mutable_status());
                else
                    prop->Dump(*spec.mutable_spec());
            }
        }
    } else {
        for (auto &p: props) {
            auto it = ContainerProperties.find(p);
            if (it == ContainerProperties.end()) {
                TError(EError::InvalidProperty, "Unknown property {}", p).Dump(*spec.mutable_status()->add_error());
                continue;
            }
            auto prop = it->second;
            auto index = propsOps.find(prop->Name);

            if (!prop->CanGet()) {
                if (prop->IsReadOnly) {
                    if (index == propsOps.end())
                        prop->Dump(*spec.mutable_status());
                    else
                        prop->DumpIndexed(index->second, *spec.mutable_status());
                } else {
                    if (index == propsOps.end())
                        prop->Dump(*spec.mutable_spec());
                    else
                        prop->DumpIndexed(index->second, *spec.mutable_spec());
                }
            }
        }
    }
    UnlockState();
    CT = nullptr;
}

TError TContainer::Save(void) {
    TKeyValue node(ContainersKV / std::to_string(Id));
    TError error;

    ChangeTime = time(nullptr);

    /* These are not properties */
    node.Set(P_RAW_ID, std::to_string(Id));
    node.Set(P_RAW_NAME, Name);

    auto prev_ct = CT;
    CT = std::const_pointer_cast<TContainer>(shared_from_this());

    for (auto knob: ContainerProperties) {
        std::string value;

        /* Skip knobs without a value */
        if (knob.second->Prop == EProperty::NONE || !HasProp(knob.second->Prop))
            continue;

        error = knob.second->Save(value);
        if (error)
            break;

        /* Temporary hack for backward migration */
        if (knob.second->Prop == EProperty::STATE && State == EContainerState::Respawning)
            value = "dead";

        node.Set(knob.first, value);
    }

    CT = prev_ct;

    if (error)
        return error;

    return node.Save();
}

TError TContainer::Load(const TKeyValue &node) {
    EContainerState state = EContainerState::Destroyed;
    uint64_t controllers = 0;
    TError error;

    CT = std::const_pointer_cast<TContainer>(shared_from_this());
    LockStateWrite();

    OwnerCred = CL->Cred;

    for (auto &kv: node.Data) {
        std::string key = kv.first;
        std::string value = kv.second;

        if (key == P_STATE) {
            /*
             * We need to set state at the last moment
             * because properties depends on the current value
             */
            state = ParseState(value);
            continue;
        }

        if (key == P_RAW_ID || key == P_RAW_NAME)
            continue;

        auto it = ContainerProperties.find(key);
        if (it == ContainerProperties.end()) {
            L_WRN("Unknown property: {}, skipped", key);
            continue;
        }
        auto prop = it->second;

        controllers |= prop->RequireControllers;

        error = prop->Load(value);

        if (error.Error == EError::NotSupported) {
            L_WRN("Unsupported property: {}, skipped", key);
            continue;
        }

        if (error) {
            L_ERR("Cannot load {} : {}", key, error);
            state = EContainerState::Dead;
            break;
        }

        SetProp(prop->Prop);
    }

    if (state != EContainerState::Destroyed) {
        UnlockState();
        SetState(state);
        LockStateWrite();
        SetProp(EProperty::STATE);
    } else
        error = TError("Container has no state");

    if (!node.Has(P_CONTROLLERS) && State != EContainerState::Stopped)
        Controllers = RootContainer->Controllers;

    if (Level == 1 && CgroupDriver.CpusetSubsystem->Supported)
        Controllers |= CGROUP_CPUSET;

    if (!JobMode) {
        if (CgroupDriver.PerfSubsystem->Supported)
            Controllers |= CGROUP_PERF;

        if (CgroupDriver.Cgroup2Subsystem->Supported)
            Controllers |= CGROUP2;
    }

    if (controllers & ~Controllers)
        L_WRN("Missing cgroup controllers {}", TSubsystem::Format(controllers & ~Controllers));

    if (!node.Has(P_OWNER_USER) || !node.Has(P_OWNER_GROUP))
        OwnerCred = TaskCred;

    SanitizeCapabilities();

    UnlockState();
    CT = nullptr;

    return error;
}

TError TContainer::Seize() {
    if (SeizeTask.Pid) {
        if (GetTaskName(SeizeTask.Pid) == "portoinit") {
            pid_t ppid = SeizeTask.GetPPid();
            if (ppid == getpid() || ppid == getppid())
                return OK;
            while (!kill(SeizeTask.Pid, SIGKILL))
                usleep(100000);
        }
        SeizeTask.Pid = 0;
    }

    auto pidStr = std::to_string(WaitTask.Pid);
    const char *argv[] = {
        "portoinit", "--container", Name.c_str(), "--seize", pidStr.c_str(), NULL,
    };

    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());
    TPath path = TPath(PORTO_HELPERS_PATH) / "portoinit";

    TError error = SeizeTask.Fork(true);
    if (error)
        return error;

    if (SeizeTask.Pid) {
        SetProp(EProperty::SEIZE_PID);
        return OK;
    }

    if (cg->Attach(GetPid()))
        _exit(EXIT_FAILURE);

    execv(path.c_str(), (char *const *)argv);

    TPath exe("/proc/self/exe");
    error = exe.ReadLink(path);
    if (error)
        _exit(EXIT_FAILURE);

    path = path.DirName() / "portoinit";

    execv(path.c_str(), (char *const *)argv);

    _exit(EXIT_FAILURE);
}

void TContainer::SyncState() {
    TError error;
    std::unique_ptr<const TCgroup> taskCg;
    auto freezerCg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.FreezerSubsystem.get());

    L_ACT("Sync {} state {}", Slug, StateName(State));

    if (!freezerCg->Exists()) {
        if (State != EContainerState::Stopped && State != EContainerState::Stopping)
            L("Freezer not found");
        ForgetPid();
        SetState(EContainerState::Stopped);
        return;
    }

    if (State == EContainerState::Starting || State == EContainerState::Respawning)
        SetState(IsMeta() ? EContainerState::Meta : EContainerState::Running);

    if (CgroupDriver.FreezerSubsystem->IsFrozen(*freezerCg)) {
        if (State != EContainerState::Paused)
            CgroupDriver.FreezerSubsystem->Thaw(*freezerCg);
    } else if (State == EContainerState::Paused)
        SetState(IsMeta() ? EContainerState::Meta : EContainerState::Running);

    if (State == EContainerState::Stopped || State == EContainerState::Stopping) {
        if (State == EContainerState::Stopped)
            L("Found unexpected freezer");
        Stop(0);
    } else if (State == EContainerState::Meta && !WaitTask.Pid && !Isolate) {
        /* meta container */
    } else if (!WaitTask.Exists()) {
        if (State != EContainerState::Dead)
            L("Task not found");
        Reap(false);
    } else if (WaitTask.IsZombie()) {
        L("Task is zombie");
        Task.Pid = 0;
    } else if (CgroupDriver.FreezerSubsystem->TaskCgroup(WaitTask.Pid, taskCg)) {
        L("Cannot check freezer");
        Reap(false);
    } else if (*taskCg != *freezerCg) {
        L("Task in wrong freezer {} but must be in {}", *taskCg, *freezerCg);
        if (WaitTask.GetPPid() == getppid()) {
            if (Task.Pid != WaitTask.Pid && Task.GetPPid() == WaitTask.Pid)
                Task.Kill(SIGKILL);
            WaitTask.Kill(SIGKILL);
        }
        Reap(false);
    } else {
        pid_t ppid = WaitTask.GetPPid();
        if (ppid != getppid()) {
            L("Task reparented to {} ({}). Seize.", ppid, GetTaskName(ppid));
            error = Seize();
            if (error) {
                L("Cannot seize reparented task: {}", error);
                Reap(false);
            }
        }
    }

    switch (Parent ? Parent->State : EContainerState::Meta) {
    case EContainerState::Stopped:
        if (State != EContainerState::Stopped)
            Stop(0); /* Also stop paused */
        break;
    case EContainerState::Dead:
        if (State != EContainerState::Dead && State != EContainerState::Stopped)
            Reap(false);
        break;
    case EContainerState::Running:
    case EContainerState::Meta:
    case EContainerState::Starting:
    case EContainerState::Stopping:
    case EContainerState::Respawning:
        /* Any state is ok */
        break;
    case EContainerState::Paused:
        if (State == EContainerState::Running || State == EContainerState::Meta)
            SetState(EContainerState::Paused);
        break;
    case EContainerState::Destroyed:
        L_ERR("Destroyed parent?");
        break;
    }

    if (State != EContainerState::Stopped && !HasProp(EProperty::START_TIME)) {
        StartTime = GetCurrentTimeMs();
        RealStartTime = time(nullptr);
        SetProp(EProperty::START_TIME);
    }

    if (State == EContainerState::Dead && !HasProp(EProperty::DEATH_TIME)) {
        DeathTime = GetCurrentTimeMs();
        RealDeathTime = time(nullptr);
        SetProp(EProperty::DEATH_TIME);
    }
}

TError TContainer::EnableFuse(bool value) {
    if (Fuse == value)
        return OK;

    if (value) {
        TDevices devices;
        auto error = devices.Parse("/dev/fuse rw", CL->Cred);
        if (error)
            return error;
        FuseDevices = devices;
    } else
        FuseDevices = TDevices();

    CT->Fuse = value;
    CT->SetProp(EProperty::ENABLE_FUSE);

    return OK;
}

TError TContainer::EnableControllers(uint64_t controllers) {
    if (State == EContainerState::Stopped) {
        Controllers |= controllers;
        RequiredControllers |= controllers;
    } else if ((Controllers & controllers) != controllers)
        return TError(EError::NotSupported, "Cannot enable controllers in runtime");
    return OK;
}

static TUintMap getMemoryEvents(const TFile &f) {
    TUintMap events;
    auto error = TCgroup::GetUintMap(f, events);
    if (error) {
        if (error.Errno != ENOENT)
            L_WRN("Cannot get memory events {}: {}", f.ProcPath(), error);
        return {};
    }
    return events;
}

static uint64_t getDelta(std::atomic<uint64_t> &v, uint64_t newv) {
    uint64_t cur = v;
    while (true) {
        if (newv <= cur)
            return 0;

        auto delta = newv - cur;
        if (v.compare_exchange_weak(cur, newv))
            return delta;
    }
}

void TContainer::CollectMemoryEvents(int fd) {
    if (CgroupDriver.MemorySubsystem->IsCgroup2()) {
        if (fd == OomEvent.Fd || fd < 0)
            CollectOomsV2();

        if (fd == OomKillEvent.Fd || fd < 0)
            CollectOomKills();
    } else {
        if (CollectOomsV1() || fd < 0)
            CollectOomKills();
    }
}

void TContainer::CollectOomsV2() {
    auto events = getMemoryEvents(OomEvent);
    auto it = events.find("oom");
    if (it == events.end()) {
        L_WRN("No oom field in memory.events");
        return;
    }
    auto ooms = it->second;
    auto delta = getDelta(OomEvents, ooms);
    if (delta > 0) {
        Statistics->ContainersOOM += delta;
        L_EVT("OOM in {}", Slug);
    }
}

bool TContainer::CollectOomsV1() {
    uint64_t ooms = CgroupDriver.MemorySubsystem->NotifyOomEvents(OomEvent);
    if (!ooms)
        return false;

    OomEvents += ooms;
    Statistics->ContainersOOM += ooms;
    // counter increments after event, so recheck it after 3s
    EventQueue->Add(3000, {EEventType::CollectOOM, shared_from_this()});
    L_EVT("OOM in {}", Slug);

    return true;
}

void TContainer::CollectOomKills() {
    if (!HasResources() || !(Controllers & CGROUP_MEMORY))
        return;

    uint64_t kills = 0;
    if (CgroupDriver.MemorySubsystem->IsCgroup2())
        kills = GetOomKillsV2();
    else
        kills = GetOomKillsV1();

    auto delta = getDelta(OomKills, kills);
    if (!delta)
        return;

    L_EVT("OOM kill in {}", Slug);

    SetProp(EProperty::OOM_KILLS);

    for (auto ct = shared_from_this(); ct; ct = ct->Parent) {
        ct->OomKillsTotal += delta;
        ct->SetProp(EProperty::OOM_KILLS_TOTAL);
        ct->Save();
    }
}

uint64_t TContainer::GetOomKillsV2() {
    auto events = getMemoryEvents(OomKillEvent);
    auto it = events.find("oom_kill");
    if (it == events.end()) {
        L_WRN("No oom_kill in {} memory.events", Slug);
        return 0;
    }
    return it->second;
}

uint64_t TContainer::GetOomKillsV1() {
    uint64_t kills = 0;

    auto cg = CgroupDriver.GetContainerCgroup(*this, CgroupDriver.MemorySubsystem.get());
    auto error = CgroupDriver.MemorySubsystem->GetOomKills(*cg, kills);
    if (error) {
        if (error.Errno != ENOENT)
            L_WRN("Cannot get OOM kills: {}", error);
        return 0;
    }
    return kills;
}

void TContainer::Event(const TEvent &event) {
    TError error;

    auto ct = event.Container.lock();

    switch (event.Type) {
    case EEventType::OOM: {
        if (ct && !CL->LockContainer(ct) && ct->OomIsFatal)
            ct->Exit(SIGKILL, true);
        break;
    }

    case EEventType::CollectOOM: {
        if (ct)
            ct->CollectOomKills();
        break;
    }

    case EEventType::Respawn: {
        if (ct && !CL->LockContainer(ct))
            ct->Respawn();
        break;
    }

    case EEventType::Exit:
    case EEventType::ChildExit: {
        bool delivered = false;

        auto lock = LockContainers();
        for (auto &it: Containers) {
            if (it.second->WaitTask.Pid != event.Exit.Pid && it.second->SeizeTask.Pid != event.Exit.Pid)
                continue;
            ct = it.second;
            break;
        }
        lock.unlock();

        if (ct && !CL->LockContainer(ct)) {
            if (ct->WaitTask.Pid == event.Exit.Pid || ct->SeizeTask.Pid == event.Exit.Pid) {
                ct->Exit(event.Exit.Status, false);
                delivered = true;
            }
            CL->ReleaseContainer();
        }

        if (event.Type == EEventType::Exit) {
            AckExitStatus(event.Exit.Pid);
        } else {
            if (!delivered)
                L("Unknown zombie {} {}", event.Exit.Pid, event.Exit.Status);
            (void)waitpid(event.Exit.Pid, NULL, 0);
        }

        break;
    }

    case EEventType::WaitTimeout: {
        auto waiter = event.WaitTimeout.Waiter.lock();
        if (waiter)
            waiter->Timeout();
        break;
    }

    case EEventType::DestroyAgedContainer: {
        if (ct && !CL->LockContainer(ct)) {
            std::list<std::shared_ptr<TVolume>> unlinked;

            if (ct->State == EContainerState::Dead && GetCurrentTimeMs() >= ct->DeathTime + ct->AgingTime) {
                Statistics->RemoveDead++;
                ct->Destroy(unlinked);
            }
            CL->ReleaseContainer();

            TVolume::DestroyUnlinked(unlinked);
        }
        break;
    }

    case EEventType::DestroyWeakContainer:
        if (ct && !CL->LockContainer(ct)) {
            std::list<std::shared_ptr<TVolume>> unlinked;

            if (ct->IsWeak)
                ct->Destroy(unlinked);
            CL->ReleaseContainer();

            TVolume::DestroyUnlinked(unlinked);
        }
        break;

    case EEventType::RotateLogs: {
        for (auto &ct: RootContainer->Subtree()) {
            if (ct->State == EContainerState::Dead && GetCurrentTimeMs() >= ct->DeathTime + ct->AgingTime) {
                TEvent ev(EEventType::DestroyAgedContainer, ct);
                EventQueue->Add(0, ev);
            }
            if (ct->State == EContainerState::Running) {
                ct->Stdout.Rotate(*ct);
                ct->Stderr.Rotate(*ct);
            }
        }

        struct stat st;
        if (!StdLog && LogFile && !LogFile.Stat(st) && !st.st_nlink)
            ReopenMasterLog();

        CheckPortoSocket();

        EventQueue->Add(config().daemon().log_rotate_ms(), event);
        break;
    }
    }
}

std::string TContainer::GetPortoNamespace(bool write) const {
    std::string ns;
    for (auto ct = this; ct && !ct->IsRoot(); ct = ct->Parent.get()) {
        if (ct->AccessLevel == EAccessLevel::Isolate || ct->AccessLevel == EAccessLevel::ReadIsolate ||
            ct->AccessLevel == EAccessLevel::SelfIsolate || (write && ct->AccessLevel == EAccessLevel::ChildOnly))
            return ct->Name + "/" + ns;
        ns = ct->NsName + ns;
    }
    return ns;
}

TTuple TContainer::Taint() {
    TTuple taint;

    if (OwnerCred.IsRootUser() && Level && !HasProp(EProperty::CAPABILITIES))
        taint.push_back("Container owned by root has unrestricted capabilities.");

    if (NetIsolate && Hostname == "")
        taint.push_back("Container with network namespace without hostname is confusing.");

    if (BindDns)
        taint.push_back("Property bind_dns is deprecated and will be removed soon.");

    if (!OomIsFatal)
        taint.push_back(
            "Containers with oom_is_fatal=false often stuck in broken state after OOM, you have been warned.");

    if (OsMode && Isolate && HasProp(EProperty::COMMAND) && Command != "/sbin/init")
        taint.push_back(
            "Containers virt_mode=os and custom command often infected with zombies, use virt_mode=app user=root "
            "group=root.");

    if (CpuPolicy == "rt" && CpuLimit)
        taint.push_back("RT scheduler works really badly when usage hits cpu_limit, use cpu_policy=high");

    if (Level == 1) {
        if (!MemLimit || !HasProp(EProperty::MEM_LIMIT))
            taint.push_back("First level container without memory_limit.");
        if (!CpuLimit)
            taint.push_back("First level container without cpu_limit.");
        if (!Isolate)
            taint.push_back("First level container without pid namespace.");
        if (!(Controllers & CGROUP_DEVICES))
            taint.push_back("First level container without devices cgroup.");
    }

    if (Root != "/") {
        if (!(Controllers & CGROUP_DEVICES))
            taint.push_back("Container with chroot and without devices cgroup.");
    }

    if (AccessLevel >= EAccessLevel::Normal) {
        if (Root != "/")
            taint.push_back("Container could escape chroot with enable_porto=true.");
        if (Isolate)
            taint.push_back("Container could escape pid namespace with enable_porto=true.");
        if (NetIsolate)
            taint.push_back("Container could escape network namespace with enable_porto=true.");
    }

    if (AccessLevel > EAccessLevel::ReadOnly) {
        if (NetIsolate && IpPolicy == "any")
            taint.push_back("Container could escape network namespace without ip_limit.");
    }

    if (TaintFlags.RootOnLoop)
        taint.push_back("Container with deprecated root=loop.img");

    if (TaintFlags.BindWithSuid)
        taint.push_back("Container with bind mount source which allows suid in host");

    return taint;
}
