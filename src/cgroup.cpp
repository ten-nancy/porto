#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <memory>
#include <string>
#include <unordered_set>
#include <mutex>

#include "cgroup.hpp"
#include "container.hpp"
#include "device.hpp"
#include "config.hpp"
#include "util/error.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "util/task.hpp"
#include "util/string.hpp"

extern "C" {
#include <fcntl.h>
#include <unistd.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/sysmacros.h>
}

const TFlagsNames ControllersName = {
    { CGROUP_FREEZER,   "freezer" },
    { CGROUP_MEMORY,    "memory" },
    { CGROUP_CPU,       "cpu" },
    { CGROUP_CPUACCT,   "cpuacct" },
    { CGROUP_NETCLS,    "net_cls" },
    { CGROUP_BLKIO,     "blkio" },
    { CGROUP_DEVICES,   "devices" },
    { CGROUP_HUGETLB,   "hugetlb" },
    { CGROUP_CPUSET,    "cpuset" },
    { CGROUP_PIDS,      "pids" },
    { CGROUP_PERF,      "perf_event" },
    { CGROUP_SYSTEMD,   "systemd" },
    { CGROUP2,          "cgroup2" },
};

static constexpr const char *CGROUP_PROCS = "cgroup.procs";
static constexpr const char *CGROUP_THREADS = "cgroup.threads";
static constexpr const char *TASKS = "tasks";

extern pid_t MasterPid;
extern pid_t ServerPid;
extern std::mutex TidsMutex;

TCgroupDriver CgroupDriver;

// TCgroup status

bool TCgroup::HasSubsystem() const {
    return Subsystem != nullptr;
}

bool TCgroup::Exists() const {
    return Path().IsDirectoryStrict();
}

bool TCgroup::IsEmpty() const {
    std::vector<pid_t> tasks;
    GetTasks(tasks);
    return tasks.empty();
}

bool TCgroup::IsRoot() const {
    return GetName() == "/";
}

bool TCgroup::IsCgroup2() const {
    return HasSubsystem() && Subsystem->IsCgroup2();
}

bool TCgroup::IsSecondary() const {
    return !HasSubsystem() || Subsystem->Hierarchy != Subsystem;
}

bool TCgroup::IsSubsystem(uint64_t kind) const {
    return HasSubsystem() && Subsystem->Kind == kind;
}

// TCgroup getters

std::string TCgroup::Type() const {
    return HasSubsystem() ? Subsystem->Type : "(null)";
}

TPath TCgroup::Path() const {
    if (!HasSubsystem())
        return TPath();

    return Subsystem->Root / Name;
}

std::string TCgroup::GetName() const {
    return Name;
}

const TSubsystem *TCgroup::GetSubsystem() const {
    return Subsystem;
}

std::unique_ptr<const TCgroup> TCgroup::GetUniquePtr() const {
    return HasSubsystem() ? GetSubsystem()->Cgroup(GetName()) : nullptr;
}

// TCgroup processes

TError TCgroup::SetPids(const std::string &knobPath, const std::vector<pid_t> &pids) const {
    if (!HasSubsystem())
        return TError("Cannot get from null cgroup");

    FILE *file;

    file = fopen(knobPath.c_str(), "w");
    if (!file)
        return TError::System("Cannot open knob " + knobPath);

    for (pid_t pid: pids)
        fprintf(file, "%d\n", pid);

    fclose(file);
    return OK;
}

TError TCgroup::GetPids(const std::string &knobPath, std::vector<pid_t> &pids) const {
    if (!HasSubsystem())
        return TError("Cannot get from null cgroup");

    FILE *file;
    int pid;

    pids.clear();
    file = fopen(knobPath.c_str(), "r");
    if (!file)
        return TError::System("Cannot open knob " + knobPath);

    while (fscanf(file, "%d", &pid) == 1)
        pids.push_back(pid);

    fclose(file);
    return OK;
}

TError TCgroup::GetProcesses(std::vector<pid_t> &pids) const {
    return GetPids(Knob(CGROUP_PROCS).ToString(), pids);
}

// TCgroup knobs

TPath TCgroup::Knob(const std::string &knob) const {
    if (!HasSubsystem())
        return TPath();
    return Path() / knob;
}

bool TCgroup::Has(const std::string &knob) const {
    if (!HasSubsystem())
        return false;
    return Knob(knob).IsRegularStrict();
}

TError TCgroup::Get(const std::string &knob, std::string &value) const {
    if (!HasSubsystem())
        return TError("Cannot get from null cgroup");
    return Knob(knob).ReadAll(value);
}

TError TCgroup::Set(const std::string &knob, const std::string &value) const {
    if (!HasSubsystem())
        return TError("Cannot set to null cgroup");
    L_CG("Set {} {} = {}", *this, knob, value);
    TError error = Knob(knob).WriteAll(value);
    if (error)
        error = TError(error, "Cannot set cgroup {} = {}", knob, value);
    return error;
}

TError TCgroup::GetInt64(const std::string &knob, int64_t &value) const {
    std::string string;
    TError error = Get(knob, string);
    if (!error)
        error = StringToInt64(string, value);
    return error;
}

TError TCgroup::SetInt64(const std::string &knob, int64_t value) const {
    return Set(knob, std::to_string(value));
}

TError TCgroup::GetUint64(const std::string &knob, uint64_t &value) const {
    std::string string;
    TError error = Get(knob, string);
    if (!error)
        error = StringToUint64(string, value);
    return error;
}

TError TCgroup::SetUint64(const std::string &knob, uint64_t value) const {
    return Set(knob, std::to_string(value));
}

TError TCgroup::GetBool(const std::string &knob, bool &value) const {
    std::string string;
    TError error = Get(knob, string);
    if (!error)
        value = StringTrim(string) != "0";
    return error;
}

TError TCgroup::SetBool(const std::string &knob, bool value) const {
    return Set(knob, value ? "1" : "0");
}

TError TCgroup::GetUintMap(const std::string &knob, TUintMap &value) const {
    if (!HasSubsystem())
        return TError("Cannot get from null cgroup");

    FILE *file = fopen(Knob(knob).c_str(), "r");
    char *key;
    unsigned long long val;

    if (!file)
        return TError::System("Cannot open knob " + knob);

    while (fscanf(file, "%ms %llu\n", &key, &val) == 2) {
        value[std::string(key)] = val;
        free(key);
    }

    fclose(file);
    return OK;
}

// TCgroup1

class TCgroup1 : public TCgroup {
    // cgroup1 specified
    static TError AbortFuse(pid_t pid);

public:
    // constructors and destructor
    // TCgroup1() = delete;
    TCgroup1() = default;
    TCgroup1(const TSubsystem *subsystem, const std::string &name): TCgroup(subsystem, name) {}
    ~TCgroup1() = default;

    // modifiers
    TError Create() const override;
    TError Remove() const override;

    // processes
    TError Attach(pid_t pid, bool thread = false) const override;
    TError AttachAll(const TCgroup &cg, bool thread = false) const override;
    TError KillAll(int signal, bool abortFuse = false) const override;

    TError GetTasks(std::vector<pid_t> &pids) const override;
    TError GetCount(uint64_t &count, bool thread = false) const override;
};

// TCgroup1 cgroup1 specified

TError TCgroup1::AbortFuse(pid_t pid) {
    TError error;
    std::vector<TMount> mounts;
    uint64_t deadline = GetCurrentTimeMs() + config().daemon().fuse_termination_timeout_s() * 1000;
    uint64_t sleep = config().daemon().fuse_termination_sleep_ms();

    if (!TTask::Exists(pid))
        return OK;

    do {
        error = TPath::ListFuseMounts(mounts, pid);
        if (error && error.Errno != ENOENT)
            return error;

        if (mounts.empty() || !TTask::Exists(pid))
            break;

        for (const TMount &m: mounts) {
            if (m.OptionalFields.size() > 0)
                continue;

            TPath connPath = "/sys/fs/fuse/connections/" + std::to_string(m.Device) + "/abort";

            L_ACT("Terminate via {} due to mount {} {} {} {}", connPath, m.Device, m.Source, m.Target, m.Type);

            error = connPath.WriteAll("1");
            if (error)
                return TError(error, "Cannot close fuse connections");
        }

        mounts.clear();

    } while (!WaitDeadline(deadline, sleep));

    return OK;
}

// TCgroup1 modifiers

TError TCgroup1::Create() const {
    TError error;

    if (IsSecondary())
        return TError("Cannot create secondary cgroup " + Type());

    L_CG("Create cgroup {}", *this);
    error = Path().Mkdir(0755);
    if (error)
        L_ERR("Cannot create cgroup {} : {}", *this, error);

    for (auto subsys: CgroupDriver.Subsystems) {
        if (subsys->IsBound(*this)) {
            error = subsys->InitializeCgroup(*this);
            if (error)
                return error;
        }
    }

    return OK;
}

TError TCgroup1::Remove() const {
    struct stat st;
    TError error;

    if (IsSecondary())
        return TError("Cannot remove secondary cgroup " + Type());

    L_CG("Remove cgroup {}", *this);
    error = Path().Rmdir();

    std::vector<pid_t> startTasks;
    GetTasks(startTasks);
    /* workaround for bad synchronization */
    if (error && error.Errno == EBUSY && !Path().StatStrict(st) && st.st_nlink == 2) {
        uint64_t deadline = GetCurrentTimeMs() + config().daemon().cgroup_remove_timeout_s() * 1000;
        uint64_t interval = 1;
        do {
            error = KillAll(SIGKILL);
            if (error)
                L_WRN("Cannot kill tasks of cgroup {}: {}", *this, error);

            error = Path().Rmdir();
            if (!error || error.Errno != EBUSY)
                break;

            if (interval < 1000)
                interval *= 10;

        } while (!WaitDeadline(deadline, interval));
    }

    if (error && (error.Errno != ENOENT || Exists())) {
        std::vector<pid_t> tasks;
        GetTasks(tasks);
        L_CG_ERR("Cannot remove cgroup {} : {}, {} tasks inside",
              *this, error, tasks.size());

        L("Tasks before destroy:");
        for (auto task : startTasks)
            L("task: {}", task);

        L("Tasks after destroy:");
        for (size_t i = 0;
             i < tasks.size() && i < config().daemon().debug_hung_tasks_count();
             ++i) {
            auto task = tasks[i];
            PrintProc("status", task);
            PrintProc("wchan", task);
            PrintStack(task);
        }
    }

    return error;
}

// TCgroup1 processes

TError TCgroup1::Attach(pid_t pid, bool thread) const {
    if (IsSubsystem(CGROUP_NETCLS) && !config().network().enable_netcls_classid())
        return OK;

    if (IsSecondary())
        return TError("Cannot attach to secondary cgroup " + Type());

    if (thread && IsCgroup2())
        return OK;

    L_CG("Attach {} {} to {}", thread ? "thread" : "process", pid, *this);
    TError error;

    error = Knob(thread ? "tasks" : "cgroup.procs").WriteAll(std::to_string(pid));
    if (error)
        L_ERR("Cannot attach {} {} to {} : {}", thread ? "thread" : "process", pid, *this, error);

    return error;
}

TError TCgroup1::AttachAll(const TCgroup &cg,  bool thread) const {
    if (IsSubsystem(CGROUP_NETCLS) && !config().network().enable_netcls_classid())
        return OK;

    if (IsSecondary())
        return TError("Cannot attach to secondary cgroup " + Type());

    L_CG("Attach all processes from {} to {}", cg, *this);

    std::vector<pid_t> pids, prev;

    bool retry;
    TError error;

    error = cg.GetProcesses(pids);
    if (error)
        return error;

    if (thread && IsCgroup2())
        return OK;

    unsigned int now, startTime = GetCurrentTimeMs();
    do {
        error = thread ? cg.GetTasks(pids) : cg.GetProcesses(pids);
        if (error)
            return error;

        retry = false;
        for (auto pid: pids) {
            error = Knob(thread ? TASKS : CGROUP_PROCS).WriteAll(std::to_string(pid));
            if (error && error.Errno != ESRCH)
                return error;
            retry = retry || std::find(prev.begin(), prev.end(), pid) == prev.end();
        }
        prev = pids;

        now = GetCurrentTimeMs();
        if (startTime + 10000 < now) {
            startTime = now;
            L_WRN("Too long attachment of processes from {} to {}", cg, *this);
        }
    } while (retry);

    return OK;
}

TError TCgroup1::KillAll(int signal, bool abortFuse) const {
    std::vector<pid_t> tasks, killed;
    TError error, error2;
    bool retry;
    bool frozen = false;
    int iteration = 0;

    L_CG("KillAll {} {}", signal, *this);

    if (IsRoot())
        return TError(EError::Permission, "Bad idea");

    do {
        if (++iteration > 10 && !frozen &&
            CgroupDriver.FreezerSubsystem->IsBound(*this) &&
            !CgroupDriver.FreezerSubsystem->IsFrozen(*this))
        {
            error = CgroupDriver.FreezerSubsystem->Freeze(*this, false);
            if (error)
                L_ERR("Cannot freeze cgroup for killing {}: {}", *this, error);
            else
                frozen = true;
        }

        error = GetTasks(tasks);
        if (error)
            break;

        retry = false;
        for (auto pid: tasks) {
            if (pid == MasterPid || pid == ServerPid || pid <= 0) {
                L_TAINT(fmt::format("Cannot kill portod thread {}", pid));
                continue;
            }

            if (iteration > 10 && abortFuse) {
                error = TCgroup1::AbortFuse(pid);
                if (error)
                    L_ERR("Cannot abort fuse for {} of {}: {}", pid, *this, error);
            }

            if (std::find(killed.begin(), killed.end(), pid) == killed.end()) {
                if (kill(pid, signal) && errno != ESRCH && !error) {
                    error = TError::System("kill");
                    L_ERR("Cannot kill process {}: {}", pid, error);
                }
                retry = true;
            }
        }
        killed = tasks;
    } while (retry);

    // There is case when a child cgroup is frozen and tasks cannot be killed.
    // Cgroups are walked from leaves to root so it does not have to walk children in that case.
    // It is enough to thaw on its own.
    if (CgroupDriver.FreezerSubsystem->IsBound(*this) &&
        CgroupDriver.FreezerSubsystem->IsFrozen(*this)) {
        if (!frozen)
            L_TAINT(fmt::format("Rogue freeze in cgroup {}", *this));

        error = CgroupDriver.FreezerSubsystem->Thaw(*this, false);
        if (error)
            L_WRN("Cannot thaw cgroup {}: {}", *this, error);
    }

    return error;
}

TError TCgroup1::GetTasks(std::vector<pid_t> &pids) const {
    if (IsCgroup2())
        return GetPids(Knob(CGROUP_THREADS).ToString(), pids);
    return GetPids(Knob(TASKS).ToString(), pids);
}

TError TCgroup1::GetCount(uint64_t &count, bool thread) const {
    if (!HasSubsystem())
        TError("Cannot get from null cgroup");

    TError error;
    std::vector<pid_t> pids;

    error = GetPids(Knob(thread ? TASKS : CGROUP_PROCS).ToString(), pids);
    if (error)
        return error;

    count = pids.size();

    return OK;
}

std::unique_ptr<const TCgroup> TSubsystem::RootCgroup() const {
    return Cgroup("/");
}

std::unique_ptr<const TCgroup> TSubsystem::Cgroup(const std::string &name) const {
    PORTO_ASSERT(name[0] == '/');
    return std::unique_ptr<const TCgroup1>(new TCgroup1(this, name));
}

TError TSubsystem::TaskCgroup(pid_t pid, std::unique_ptr<const TCgroup> &cgroup) const {
    std::vector<std::string> lines;
    auto cg_file = TPath("/proc/" + std::to_string(pid) + "/cgroup");
    auto type = TestOption();
    std::unordered_set<std::string> cgroupTypes;

    TError error = cg_file.ReadLines(lines);
    if (error)
        return error;

    bool found = false;

    for (auto &line : lines) {
        auto fields = SplitString(line, ':', 3);
        if (fields.size() < 3)
            continue;

        auto cgroups = SplitString(fields[1], ',');
        if (Kind == CGROUP2 && cgroups.empty())
            cgroups.push_back("");

        for (auto &cg : cgroups) {
            // check that we do not have fake cgroups created by cgroup with \n in name
            if (cgroupTypes.find(cg) != cgroupTypes.end())
                return TError(EError::Permission, "Fake cgroup found");
            cgroupTypes.insert(cg);

            if (!found && cg == type) {
                found = true;
                cgroup = Cgroup(fields[2]);
            }
        }
    }

    return found ? OK : TError("Cannot find {} cgroup for process {}", Type, pid);
}

bool TSubsystem::IsBound(const TCgroup &cg) const {
    return cg.HasSubsystem() && (cg.GetSubsystem()->Controllers & Kind);
}

// Memory

TError TMemorySubsystem::InitializeSubsystem() {
    auto cg = RootCgroup();

    HasWritebackBlkio = cg->Has(CgroupDriver.MemorySubsystem->WRITEBACK_BLKIO);
    if (HasWritebackBlkio)
        L_CG("Supports {}", CgroupDriver.MemorySubsystem->WRITEBACK_BLKIO);

    HasMemoryLockPolicy = cg->Has(CgroupDriver.MemorySubsystem->MEMORY_LOCK_POLICY);
    if (HasMemoryLockPolicy)
        L_CG("Supports {}", CgroupDriver.MemorySubsystem->MEMORY_LOCK_POLICY);

    return OK;
}

TError TMemorySubsystem::SetLimit(const TCgroup &cg, uint64_t limit) {
    uint64_t old_limit, old_high_limit = 0;
    std::string value;
    TError error;
    const std::string knobLimitName = LIMIT;
    const std::string knobHighLimitName = HIGH_LIMIT;
    const std::string knobSwapName = MEM_SWAP_LIMIT;
    /*
     * Maxumum value depends on arch, kernel version and bugs
     * "-1" works everywhere since 2.6.31
     */
    if (!limit) {
        std::string value = "-1";
        if (SupportSwap())
            (void)cg.Set(knobSwapName, value);
        return cg.Set(knobLimitName, value);
    }

    error = cg.GetUint64(knobLimitName, old_limit);
    if (error)
        return error;

    if (old_limit == limit)
        return OK;

    /* reduce high limit first to fail faster */
    if (limit < old_limit && SupportHighLimit()) {
        auto error2 = cg.GetUint64(knobHighLimitName, old_high_limit);
        if (!error2)
            error = cg.SetUint64(knobHighLimitName, limit);
        if (error)
            return error;
    }

    /* Memory limit cannot be bigger than Memory+Swap limit. */
    if (SupportSwap()) {
        uint64_t cur_limit;
        cg.GetUint64(knobSwapName, cur_limit);
        if (cur_limit < limit)
            (void)cg.SetUint64(knobSwapName, limit);
    }

    // this also updates high limit
    error = cg.SetUint64(knobLimitName, limit);
    if (!error && SupportSwap())
        error = cg.SetUint64(knobSwapName, limit);

    if (error) {
        (void)cg.SetUint64(knobLimitName, old_limit);
        if (old_high_limit)
            (void)cg.SetUint64(knobHighLimitName, old_high_limit);
    }

    return error;
}

TError TMemorySubsystem::GetCacheUsage(const TCgroup &cg, uint64_t &usage) const {
    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage = stat["total_inactive_file"] +
                stat["total_active_file"];
    return error;
}

TError TMemorySubsystem::GetShmemUsage(const TCgroup &cg, uint64_t &usage) const {
    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (error)
        return error;

    if (stat.count("total_shmem")) {
        usage = stat["total_shmem"];
    } else {
        if (cg.Has(ANON_USAGE))
            cg.GetUint64(ANON_USAGE, usage);
        else
            usage = stat["total_inactive_anon"] +
                    stat["total_active_anon"] +
                    stat["total_unevictable"];

        if (usage >= stat["total_rss"])
            usage -= stat["total_rss"];
        else
            usage = 0;
    }

    return error;
}

TError TMemorySubsystem::GetMLockUsage(const TCgroup &cg, uint64_t &usage) const {
    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage = stat["total_unevictable"];
    return error;
}

TError TMemorySubsystem::GetAnonUsage(const TCgroup &cg, uint64_t &usage) const {
    if (cg.Has(ANON_USAGE))
        return cg.GetUint64(ANON_USAGE, usage);

    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage = stat["total_inactive_anon"] +
                stat["total_active_anon"] +
                stat["total_unevictable"] +
                stat["total_swap"];
    return error;
}

bool TMemorySubsystem::SupportAnonLimit() const {
    return Cgroup(PORTO_DAEMON_CGROUP)->Has(ANON_LIMIT);
}

TError TMemorySubsystem::SetAnonLimit(const TCgroup &cg, uint64_t limit) const {
    if (cg.Has(ANON_LIMIT))
        return cg.Set(ANON_LIMIT, limit ? std::to_string(limit) : "-1");
    return OK;
}

bool TMemorySubsystem::SupportAnonOnly() const {
    return Cgroup(PORTO_DAEMON_CGROUP)->Has(ANON_ONLY);
}

TError TMemorySubsystem::SetAnonOnly(const TCgroup &cg, bool val) const {
    if (cg.Has(ANON_ONLY))
        return cg.SetBool(ANON_ONLY, val);
    return OK;
}

TError TMemorySubsystem::SetIoLimit(const TCgroup &cg, uint64_t limit) {
    if (!SupportIoLimit())
        return OK;
    return cg.SetUint64(FS_BPS_LIMIT, limit);
}

TError TMemorySubsystem::SetIopsLimit(const TCgroup &cg, uint64_t limit) {
    if (!SupportIoLimit())
        return OK;
    return cg.SetUint64(FS_IOPS_LIMIT, limit);
}

TError TMemorySubsystem::SetDirtyLimit(const TCgroup &cg, uint64_t limit) {
    if (!SupportDirtyLimit())
        return OK;
    if (limit || !cg.Has(DIRTY_RATIO))
        return cg.SetUint64(DIRTY_LIMIT, limit);
    return cg.SetUint64(DIRTY_RATIO, 50);
}

TError TMemorySubsystem::LinkWritebackBlkio(const TCgroup &memcg, const TCgroup &blkcg) const {
    TError error;
    TFile file;

    if (!HasWritebackBlkio || !CgroupDriver.BlkioSubsystem->Supported || (Controllers & CGROUP_BLKIO))
        return OK;

    error = file.OpenDir(blkcg.Path());
    if (error)
        return error;

    L_CG("Link writeback of {} with {}", memcg, blkcg);
    return memcg.SetUint64(CgroupDriver.MemorySubsystem->WRITEBACK_BLKIO, file.Fd);
}

TError TMemorySubsystem::SetupOOMEvent(const TCgroup &cg, TFile &event) {
    TError error;
    TFile knob;

    error = knob.OpenRead(cg.Knob(OOM_CONTROL));
    if (error)
        return error;

    PORTO_ASSERT(knob.Fd > 2);

    event.Close();
    event.SetFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event.Fd < 0)
        return TError::System("Cannot create eventfd");

    PORTO_ASSERT(event.Fd > 2);

    error = cg.Set(EVENT_CONTROL, std::to_string(event.Fd) + " " + std::to_string(knob.Fd));
    if (error)
        event.Close();
    return error;
}

TError TMemorySubsystem::GetOomKills(const TCgroup &cg, uint64_t &count) {
    TUintMap map;
    TError error = cg.GetUintMap(OOM_CONTROL, map);
    if (error)
        return error;
    if (!map.count("oom_kill"))
        return TError(EError::NotSupported, "no oom kill counter");
    count = map.at("oom_kill");
    return OK;
}

uint64_t TMemorySubsystem::GetOomEvents(const TCgroup &cg) {
    TUintMap stat;
    if (!Statistics(cg, stat))
        return stat["oom_events"];
    return 0;
}

TError TMemorySubsystem::GetReclaimed(const TCgroup &cg, uint64_t &count) const {
    TUintMap stat;
    Statistics(cg, stat);
    count = stat["total_pgpgout"] * 4096; /* Best estimation for now */
    return OK;
}

TError TMemorySubsystem::SetUseHierarchy(const TCgroup &cg, bool value) const {
    return cg.SetBool(USE_HIERARCHY, value);
}

// Freezer

TError TFreezerSubsystem::WaitState(const TCgroup &cg, const std::string &state) const {
    uint64_t deadline = GetCurrentTimeMs() + config().daemon().freezer_wait_timeout_s() * 1000;
    std::string cur;
    TError error;

    do {
        error = cg.Get("freezer.state", cur);
        if (error || StringTrim(cur) == state)
            return error;
    } while (!WaitDeadline(deadline));

    return TError("Freezer {} timeout waiting {}", cg.GetName(), state);
}

TError TFreezerSubsystem::Freeze(const TCgroup &cg, bool wait) const {
    TError error = cg.Set("freezer.state", "FROZEN");
    if (error || !wait)
        return error;
    error = WaitState(cg, "FROZEN");
    if (error)
        (void)cg.Set("freezer.state", "THAWED");
    return error;
}

TError TFreezerSubsystem::Thaw(const TCgroup &cg, bool wait) const {
    TError error = cg.Set("freezer.state", "THAWED");
    if (error || !wait)
        return error;
    if (IsParentFreezing(cg))
        return TError(EError::Busy, "parent cgroup is frozen");
    return WaitState(cg, "THAWED");
}

bool TFreezerSubsystem::IsFrozen(const TCgroup &cg) const {
    std::string state;
    return !cg.Get("freezer.state", state) && StringTrim(state) != "THAWED";
}

bool TFreezerSubsystem::IsSelfFreezing(const TCgroup &cg) const {
    bool val;
    return !cg.GetBool("freezer.self_freezing", val) && val;
}

bool TFreezerSubsystem::IsParentFreezing(const TCgroup &cg) const {
    bool val;
    return !cg.GetBool("freezer.parent_freezing", val) && val;
}

// Cpu

TError TCpuSubsystem::InitializeSubsystem() {
    auto cg = RootCgroup();

    HasShares = cg->Has(CPU_SHARES);
    if (HasShares && cg->GetUint64(CPU_SHARES, BaseShares))
        BaseShares = 1024;

    MinShares = 2; /* kernel limit MIN_SHARES */
    MaxShares = 1024 * 256; /* kernel limit MAX_SHARES */

    HasQuota = cg->Has(CPU_CFS_QUOTA_US) &&
               cg->Has(CPU_CFS_PERIOD_US);

    HasRtGroup = cg->Has(CPU_RT_RUNTIME_US) &&
                 cg->Has(CPU_RT_PERIOD_US);

    HasReserve = HasShares && HasQuota &&
                 cg->Has(CPU_CFS_RESERVE_US) &&
                 cg->Has(CPU_CFS_RESERVE_SHARES);

    HasIdle = cg->Has(CPU_IDLE);
    if (HasIdle)
        EnableIdle = config().container().enable_sched_idle();

    L_SYS("{} cores", GetNumCores());
    if (HasShares)
        L_CG("support shares {}", BaseShares);
    if (HasRtGroup)
        L_CG("support rt group");
    if (HasReserve)
        L_CG("support reserves");

    if (EnableIdle)
        L_CG("support idle cgroups");

    return OK;
}

TError TCpuSubsystem::InitializeCgroup(const TCgroup &cg) const {
    if (HasRtGroup && config().container().rt_priority())
        (void)cg.SetInt64(CPU_RT_RUNTIME_US, -1);
    return OK;
}


TError TCpuSubsystem::SetPeriod(const TCgroup &cg, uint64_t period) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasQuota) {
        if (period < 1000) /* 1ms */
            period = 1000;

        if (period > 1000000) /* 1s */
            period = 1000000;

        return cg.Set(CPU_CFS_PERIOD_US, std::to_string(period));
    }

    return OK;
}

TError TCpuSubsystem::SetLimit(const TCgroup &cg, uint64_t period, uint64_t limit) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasQuota) {
        int64_t quota = std::ceil((double)limit * period / CPU_POWER_PER_SEC);

        quota *= config().container().cpu_limit_scale();

        if (!quota)
            quota = -1;
        else if (quota < 1000) /* 1ms */
            quota = 1000;

        (void)cg.Set(CPU_CFS_QUOTA_US, std::to_string(quota));

        error = cg.Set(CPU_CFS_PERIOD_US, std::to_string(period));
        if (error)
            return error;

        error = cg.Set(CPU_CFS_QUOTA_US, std::to_string(quota));
        if (error)
            return error;
    }
    return OK;
}

TError TCpuSubsystem::SetGuarantee(const TCgroup &cg, uint64_t period, uint64_t guarantee) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasReserve && config().container().enable_cpu_reserve()) {
        uint64_t reserve = std::floor((double)guarantee * period / CPU_POWER_PER_SEC);

        error = cg.SetUint64(CPU_CFS_RESERVE_US, reserve);
        if (error)
            return error;
    }

    return OK;
}

TError TCpuSubsystem::SetShares(const TCgroup &cg, const std::string &policy, double weight, uint64_t guarantee) {
    TError error;

    if (HasReserve && config().container().enable_cpu_reserve()) {
        uint64_t shares = BaseShares, reserve_shares = BaseShares;

        shares *= weight;
        reserve_shares *= weight;

        if (policy == "rt" || policy == "high" || policy == "iso") {
            shares *= 16;
            reserve_shares *= 256;
        } else if (policy == "normal" || policy == "batch") {
            if (config().container().proportional_cpu_shares()) {
                double scale = (double)guarantee / CPU_POWER_PER_SEC;
                shares *= scale;
                reserve_shares *= scale;
            } else if (guarantee)
                reserve_shares *= 16;
        } else if (policy == "idle") {
            shares /= 16;
        }

        shares = std::min(std::max(shares, MinShares), MaxShares);
        reserve_shares = std::min(std::max(reserve_shares, MinShares), MaxShares);

        error = cg.SetUint64(CPU_SHARES, shares);
        if (error)
            return error;

        error = cg.SetUint64(CPU_CFS_RESERVE_SHARES, reserve_shares);
        if (error)
            return error;

    } else if (HasShares) {
        uint64_t shares = std::floor((double)guarantee * BaseShares / CPU_POWER_PER_SEC);

        /* default cpu_guarantee is 1c, shares < 1024 are broken */
        shares = std::max(shares, BaseShares);

        shares *= weight;

        if (policy == "rt")
            shares *= 256;
        else if (policy == "high" || policy == "iso")
            shares *= 16;
        else if (policy == "idle")
            shares /= 16;

        shares = std::min(std::max(shares, MinShares), MaxShares);

        error = cg.SetUint64(CPU_SHARES, shares);
        if (error)
            return error;
    }

    return OK;
}

TError TCpuSubsystem::SetCpuIdle(const TCgroup &cg, bool value) {
    return cg.SetBool(CPU_IDLE, value);
}

TError TCpuSubsystem::SetRtLimit(const TCgroup &cg, uint64_t period, uint64_t limit) {
    TError error;

    period = period / 1000; /* ns -> us */

    if (HasRtGroup && config().container().rt_priority()) {
        uint64_t max = GetNumCores() * CPU_POWER_PER_SEC;
        int64_t root_runtime, root_period, runtime;

        if (RootCgroup()->GetInt64(CPU_RT_PERIOD_US, root_period))
            root_period = 1000000;

        if (RootCgroup()->GetInt64(CPU_RT_RUNTIME_US, root_runtime))
            root_runtime = 950000;
        else if (root_runtime < 0)
            root_runtime = root_period;

        if (limit <= 0 || limit >= max ||
                (double)limit / max * root_period > root_runtime) {
            runtime = -1;
        } else {
            runtime = (double)limit * period / max;
            if (runtime < 1000)  /* 1ms */
                runtime = 1000;
        }

        error = cg.SetInt64(CPU_RT_PERIOD_US, period);
        if (error) {
            (void)cg.SetInt64(CPU_RT_RUNTIME_US, runtime);
            error = cg.SetInt64(CPU_RT_PERIOD_US, period);
        }
        if (!error)
            error = cg.SetInt64(CPU_RT_RUNTIME_US, runtime);
        if (error) {
            (void)cg.SetInt64(CPU_RT_RUNTIME_US, 0);
            return error;
        }
    }
    return OK;
}

// Cpuacct

TError TCpuacctSubsystem::Usage(const TCgroup &cg, uint64_t &value) const {
    std::string s;
    TError error = cg.Get("cpuacct.usage", s);
    if (error)
        return error;
    return StringToUint64(s, value);
}

TError TCpuacctSubsystem::SystemUsage(const TCgroup &cg, uint64_t &value) const {
    TUintMap stat;
    TError error = cg.GetUintMap("cpuacct.stat", stat);
    if (error)
        return error;
    value = stat["system"] * (1000000000 / sysconf(_SC_CLK_TCK));
    return OK;
}

// Cpuset

TError TCpusetSubsystem::SetCpus(const TCgroup &cg, const std::string &cpus) const {
    std::string val;
    TError error;
    TPath copy;

    if (cpus == "")
        copy = cg.Path().DirName() / "cpuset.cpus";

    if (cpus == "all")
        copy = TPath("/sys/devices/system/cpu/present");

    if (StringStartsWith(cpus, "node ")) {
        int id;
        error = StringToInt(cpus.substr(5), id);
        if (error)
            return error;
        copy = TPath("/sys/devices/system/node/node" + std::to_string(id) + "/cpulist");
    }

    // cgroup v2 root doesn't contain cpuset.cpus
    if (!copy.IsEmpty() && !IsCgroup2()) {
        error = copy.ReadAll(val);
        if (error)
            return error;
        val = StringTrim(val);
    } else
        val = cpus;

    return cg.Set("cpuset.cpus", val);
}

TError TCpusetSubsystem::SetMems(const TCgroup &cg, const std::string &mems) const {
    std::string val;
    TError error;
    TPath copy;

    if (mems == "")
        copy = cg.Path().DirName() / "cpuset.mems";

    if (mems == "all")
        copy = TPath("/sys/devices/system/node/online");

    if (!copy.IsEmpty()) {
        error = copy.ReadAll(val);
        if (error)
            return error;
        val = StringTrim(val);
    } else
        val = mems;

    return cg.Set("cpuset.mems", val);
}

TError TCpusetSubsystem::InitializeCgroup(const TCgroup &cg) const {
    TError error;

    error = SetCpus(cg, "");
    if (error)
        return error;

    error = SetMems(cg, "");
    if (error)
        return error;

    return OK;
}

TError TCpusetSubsystem::GetCpus(const TCgroup &cg, TBitMap &cpus) const {
    std::string s;
    auto error = GetCpus(cg, s);
    if (error)
        return error;
    return cpus.Parse(s);
}

TError TCpusetSubsystem::SetCpus(const TCgroup &cg, const TBitMap &cpus) const {
    return SetCpus(cg, cpus.Format());
}

// Netcls

TError TNetclsSubsystem::InitializeSubsystem() {
    HasPriority = config().network().enable_netcls_priority() &&
                  RootCgroup()->Has("net_cls.priority");
    if (HasPriority)
        L_CG("support netcls priority");
    return OK;
}

TError TNetclsSubsystem::SetClass(const TCgroup &cg, uint32_t classid) const {
    TError error;
    uint64_t cur;

    if (!config().network().enable_netcls_classid())
        return OK;

    if (HasPriority) {
        error = cg.GetUint64("net_cls.priority", cur);
        if (error || cur != classid) {
            error = cg.SetUint64("net_cls.priority", classid);
            if (error)
                return error;
        }
    }

    error = cg.GetUint64("net_cls.classid", cur);
    if (error || cur != classid) {
        error = cg.SetUint64("net_cls.classid", classid);
        if (error)
            return error;
    }

    return OK;
}

// Blkio

TError TBlkioSubsystem::DiskName(const std::string &disk, std::string &name) const {
    TPath sym("/sys/dev/block/" + disk), dev;
    TError error = sym.ReadLink(dev);
    if (!error)
        name = dev.BaseName();
    return error;
}

/* converts absolule path or disk or partition name into "major:minor" */
TError TBlkioSubsystem::ResolveDisk(const TPath &root, const std::string &key, std::string &disk) const {
    TError error;
    int tmp = 0;

    if (!sscanf(key.c_str(), "%*d:%*d%n", &tmp) && (unsigned)tmp == key.size()) {
        disk = key;
    } else {
        dev_t dev;

        if (key[0] == '/')
            dev = TPath(key).GetDev();
        else if (key[0] == '.')
            dev = TPath(root / key.substr(1)).GetDev();
        else
            dev = TPath("/dev/" + key).GetBlockDev();

        if (!dev)
            return TError(EError::InvalidValue, "Disk not found: " + key);

        disk = StringFormat("%d:%d", major(dev), minor(dev));
    }

    if (!TPath("/sys/dev/block/" + disk).Exists())
        return TError(EError::InvalidValue, "Disk not found:  " + disk);

    /* convert partition to disk */
    if (TPath("/sys/dev/block/" + disk + "/partition").Exists()) {
        TPath diskDev("/sys/dev/block/" + disk + "/../dev");
        if (diskDev.IsRegularStrict() && !diskDev.ReadAll(disk)) {
            disk = StringTrim(disk);
            if (sscanf(disk.c_str(), "%*d:%*d%n", &tmp) || (unsigned)tmp != disk.size())
                return TError(EError::InvalidValue, "Unexpected disk format: " + disk);
        }
    }

    return OK;
}

TError TDevicesSubsystem::SetAllow(const TCgroup &cg, const std::string &value) const {
    return cg.Set(DEVICES_ALLOW, value);
}

TError TDevicesSubsystem::SetDeny(const TCgroup &cg, const std::string &value) const {
    return cg.Set(DEVICES_DENY, value);
}

TError TBlkioSubsystem::InitializeSubsystem() {
    HasThrottler = RootCgroup()->Has("blkio.throttle.io_serviced");
    auto sched = HasThrottler ? "throttle." : "";
    auto recOpsKnob = fmt::format("blkio.{}io_serviced_recursive", sched);
    auto recur =  RootCgroup()->Has(recOpsKnob) ? "_recursive" : "";

    TimeKnob = fmt::format("blkio.{}io_service_time{}", sched, recur);
    OpsKnob = fmt::format("blkio.{}io_serviced{}", sched, recur);
    BytesKnob = fmt::format("blkio.{}io_service_bytes{}", sched, recur);

    return OK;
}

TError TBlkioSubsystem::GetIoStat(const TCgroup &cg, enum IoStat stat, TUintMap &map) const {
    std::vector<std::string> lines;
    std::string knob, prev, name;
    bool summ = false, hide = false;
    TError error;

    if (stat & IoStat::Time)
        knob = TimeKnob;
    else if (stat & IoStat::Iops)
        knob = OpsKnob;
    else
        knob = BytesKnob;

    error = cg.Knob(knob).ReadLines(lines);
    if (error)
        return error;

    uint64_t total = 0;
    for (auto &line: lines) {
        auto word = SplitString(line, ' ');
        if (word.size() != 3)
            continue;

        if (word[1] == "Read") {
            if (stat & IoStat::Write)
                continue;
        } else if (word[1] == "Write") {
            if (stat & IoStat::Read)
                continue;
        } else
            continue;

        if (word[0] != prev) {
            if (DiskName(word[0], name))
                continue;
            prev = word[0];
            summ = StringStartsWith(name, "sd") ||
                   StringStartsWith(name, "nvme") ||
                   StringStartsWith(name, "vd");
            hide = StringStartsWith(name, "ram");
        }

        if (hide)
            continue;

        uint64_t val;
        if (!StringToUint64(word[2], val) && val) {
            map[name] += val;
            if (summ)
                total += val;
        }
    }
    map["hw"] = total;

    return OK;
}

TError TBlkioSubsystem::SetIoLimit(const TCgroup &cg, const TPath &root,
                                   const TUintMap &map, bool iops) {
    std::string knob[2] = {
        iops ? "blkio.throttle.read_iops_device" : "blkio.throttle.read_bps_device",
        iops ? "blkio.throttle.write_iops_device" : "blkio.throttle.write_bps_device",
    };
    TError error, result;
    TUintMap plan[2];
    std::string disk;
    int dir;

    /* load current limits */
    for (dir = 0; dir < 2; dir++) {
        std::vector<std::string> lines;
        error = cg.Knob(knob[dir]).ReadLines(lines);
        if (error)
            return error;
        for (auto &line: lines) {
            auto sep = line.find(' ');
            if (sep != std::string::npos)
                plan[dir][line.substr(0, sep)] = 0;
        }
    }

    for (auto &it: map) {
        auto key = it.first;
        auto sep = key.rfind(' ');

        dir = 2;
        if (sep != std::string::npos) {
            if (sep != key.size() - 2 || ( key[sep+1] != 'r' && key[sep+1] != 'w'))
                return TError(EError::InvalidValue, "Invalid io limit key: " + key);
            dir = key[sep+1] == 'r' ? 0 : 1;
            key = key.substr(0, sep);
        }

        if (key == "fs")
            continue;

        error = ResolveDisk(root, key, disk);
        if (error)
            return error;

        if (dir == 0 || dir == 2)
            plan[0][disk] = it.second;
        if (dir == 1 || dir == 2)
            plan[1][disk] = it.second;
    }

    for (dir = 0; dir < 2; dir++) {
        for (auto &it: plan[dir]) {
            error = cg.Set(knob[dir], it.first + " " + std::to_string(it.second));
            if (error && !result)
                result = error;
        }
    }

    return result;
}

TError TBlkioSubsystem::SetIoWeight(const TCgroup &cg, const std::string &policy,
                                    double weight) const {
    double bfq_weight = weight;
    TError error;

    /*
     * Cgroup v1 mess:
     * CFQ: 10..1000 default 500
     * BFQ: 1..1000 default 100
     */

    if (policy == "rt" || policy == "high") {
        weight *= 1000;
        bfq_weight *= 1000;
    } else if (policy == "" || policy == "none" || policy == "normal") {
        weight *= 500;
        bfq_weight *= 100;
    } else if (policy == "batch" || policy == "idle") {
        weight *= 10;
        bfq_weight *= 1;
    } else
        return TError(EError::InvalidValue, "unknown policy: " + policy);

    if (cg.Has(CFQ_WEIGHT)) {
        error = cg.SetUint64(CFQ_WEIGHT, std::min(std::max(weight, 10.), 1000.));
        if (error)
            return error;
    }

    if (cg.Has(BFQ_WEIGHT)) {
        error = cg.SetUint64(BFQ_WEIGHT, std::min(std::max(bfq_weight, 1.), 1000.));
        if (error)
            return error;
    }

    return OK;
}

// Devices

// Pids

TError TPidsSubsystem::GetUsage(const TCgroup &cg, uint64_t &usage) const {
    return cg.GetUint64("pids.current", usage);
}

TError TPidsSubsystem::SetLimit(const TCgroup &cg, uint64_t limit) const {
    if (!limit)
        return cg.Set("pids.max", "max");
    return cg.SetUint64("pids.max", limit);
}

// Systemd

TError TSystemdSubsystem::InitializeSubsystem() {
    TError error = TaskCgroup(getpid(), PortoService);
    if (!error)
        L_CG("porto service: {}", *PortoService);
    return error;
}

// Cgroup v2

// Nothing for now

TCgroupDriver::TCgroupDriver() {
    MemorySubsystem = std::unique_ptr<TMemorySubsystem>(new TMemorySubsystem());
    FreezerSubsystem = std::unique_ptr<TFreezerSubsystem>(new TFreezerSubsystem());
    CpuSubsystem = std::unique_ptr<TCpuSubsystem>(new TCpuSubsystem());
    CpuacctSubsystem = std::unique_ptr<TCpuacctSubsystem>(new TCpuacctSubsystem());
    CpusetSubsystem = std::unique_ptr<TCpusetSubsystem>(new TCpusetSubsystem());
    NetclsSubsystem = std::unique_ptr<TNetclsSubsystem>(new TNetclsSubsystem());
    BlkioSubsystem = std::unique_ptr<TBlkioSubsystem>(new TBlkioSubsystem());
    DevicesSubsystem = std::unique_ptr<TDevicesSubsystem>(new TDevicesSubsystem());
    HugetlbSubsystem = std::unique_ptr<THugetlbSubsystem>(new THugetlbSubsystem());
    PidsSubsystem = std::unique_ptr<TPidsSubsystem>(new TPidsSubsystem());
    PerfSubsystem = std::unique_ptr<TPerfSubsystem>(new TPerfSubsystem());
    SystemdSubsystem = std::unique_ptr<TSystemdSubsystem>(new TSystemdSubsystem());
    Cgroup2Subsystem = std::unique_ptr<TCgroup2Subsystem>(new TCgroup2Subsystem());

    AllSubsystems.push_back(MemorySubsystem.get());
    AllSubsystems.push_back(FreezerSubsystem.get());
    AllSubsystems.push_back(CpuSubsystem.get());
    AllSubsystems.push_back(CpuacctSubsystem.get());
    AllSubsystems.push_back(CpusetSubsystem.get());
    AllSubsystems.push_back(NetclsSubsystem.get());
    AllSubsystems.push_back(BlkioSubsystem.get());
    AllSubsystems.push_back(DevicesSubsystem.get());
    AllSubsystems.push_back(HugetlbSubsystem.get());
    AllSubsystems.push_back(PidsSubsystem.get());
    AllSubsystems.push_back(PerfSubsystem.get());
    AllSubsystems.push_back(SystemdSubsystem.get());
    AllSubsystems.push_back(Cgroup2Subsystem.get());
}

TError TCgroupDriver::InitializeCgroups() {
    TPath root("/sys/fs/cgroup");
    TPath cgroup2root = root / "porto";
    std::vector<TMount> mounts;
    TMount mount;
    TError error;

    error = root.FindMount(mount);
    if (error) {
        L_ERR("Cannot find cgroups root mount: {}", error);
        return error;
    }

    if (mount.Target != root) {
        error = root.Mount("cgroup", "tmpfs", 0, {});
        if (error) {
            L_ERR("Cannot mount cgroups root: {}", error);
            return error;
        }
    } else if (StringStartsWith(mount.Options, "ro,")) {
        error = root.Remount(MS_NODEV | MS_NOSUID | MS_NOEXEC | MS_ALLOW_WRITE);
        if (error) {
            L_ERR("Cannot remount cgroups root: {}", error);
            return error;
        }
    }

    error = TPath::ListAllMounts(mounts);
    if (error) {
        L_ERR("Can't create mount snapshot: {}", error);
        return error;
    }

    for (auto subsys: AllSubsystems) {
        for (auto &mnt: mounts) {
            if (mnt.Type == subsys->MountType && mnt.HasOption(subsys->TestOption())) {
                subsys->Root = mnt.Target;
                L_CG("Found cgroup subsystem {} mounted at {}", subsys->Type, subsys->Root);
                break;
            }
        }
    }

    if (config().daemon().merge_memory_blkio_controllers() &&
            !CgroupDriver.MemorySubsystem->IsDisabled() && !CgroupDriver.BlkioSubsystem->IsDisabled() &&
            CgroupDriver.MemorySubsystem->Root.IsEmpty() && CgroupDriver.BlkioSubsystem->Root.IsEmpty()) {
        TPath path = root / "memory,blkio";

        if (!path.Exists())
            (void)path.Mkdir(0755);
        error = path.Mount("cgroup", "cgroup", 0, {"memory", "blkio"});
        if (!error) {
            (root / "memory").Symlink("memory,blkio");
            (root / "blkio").Symlink("memory,blkio");
            CgroupDriver.MemorySubsystem->Root = path;
            CgroupDriver.BlkioSubsystem->Root = path;
        } else {
            L_CG("Cannot merge memory and blkio {}", error);
            (void)path.Rmdir();
        }
    }

    for (auto subsys: AllSubsystems) {
        if (subsys->IsDisabled() || subsys->Root)
            continue;

        TPath path = root / subsys->Type;

        L_CG("Mount cgroup subsysem {} at {}", subsys->Type, path);
        if (!path.Exists()) {
            error = path.Mkdir(0755);
            if (error) {
                L_ERR("Cannot create cgroup mountpoint: {}", error);
                continue;
            }
        }

        error = path.Mount(subsys->MountType, subsys->MountType, 0, subsys->MountOptions() );
        if (error) {
            (void)path.Rmdir();
            L_ERR("Cannot mount cgroup: {}", error);
            continue;
        }

        subsys->Root = path;
    }

    for (auto subsys: AllSubsystems) {
        if (!subsys->Root)
            continue;

        error = subsys->InitializeSubsystem();
        if (error) {
            L_ERR("Cannot initialize cgroup subsystem{}: {}", subsys->Type, error);
            subsys->Root = "";
        }
    }

    for (auto subsys: AllSubsystems) {
        if (subsys->IsDisabled()) {
            L_CG("Cgroup subsysem {} is disabled", subsys->Type);
            continue;
        }

        if (!subsys->Root) {
            if (subsys->IsOptional()) {
                L_CG("Cgroup subsystem {} is not supported", subsys->Type);
                continue;
            }
            return TError(EError::NotSupported, "Cgroup {} is not supported", subsys->Type);
        }

        error = subsys->Base.OpenDir(subsys->Root);
        if (error) {
            L_ERR("Cannot open cgroup {} root directory: {}", subsys->Type, error);
            return error;
        }

        Subsystems.push_back(subsys);

        subsys->Hierarchy = subsys;
        subsys->Controllers |= subsys->Kind;
        for (auto hy: Hierarchies) {
            if (subsys->Root == hy->Root) {
                L_CG("Cgroup subsystem {} bound to hierarchy {}", subsys->Type, hy->Type);
                subsys->Hierarchy = hy;
                hy->Controllers |= subsys->Kind;
                break;
            }
        }

        if (subsys->Hierarchy == subsys)
            Hierarchies.push_back(subsys);

        subsys->Supported = true;
    }

    for (auto subsys: AllSubsystems)
        if (subsys->Hierarchy)
            subsys->Controllers |= subsys->Hierarchy->Controllers;

    /* This piece of code should never be executed. */
    TPath("/usr/sbin/cgclear").Chmod(0);

    return error;
}

TError TCgroupDriver::InitializeDaemonCgroups() {
    std::vector<const TSubsystem *> DaemonSubsystems = {
        FreezerSubsystem.get(),
        MemorySubsystem.get(),
        CpuacctSubsystem.get(),
        PerfSubsystem.get()
    };

    if (Cgroup2Subsystem->Supported)
        DaemonSubsystems.push_back(Cgroup2Subsystem.get());

    for (auto subsys : DaemonSubsystems) {
        auto hy = subsys->Hierarchy;
        TError error;

        if (!hy)
            continue;

        auto cg = hy->Cgroup(PORTO_DAEMON_CGROUP);
        error = RecreateCgroup(*cg);
        if (error)
            return error;

        // portod
        error = cg->Attach(GetPid());
        if (error)
            return error;

        // portod master
        error = cg->Attach(GetPPid());
        if (error)
            return error;
    }

    auto cg = MemorySubsystem->Cgroup(PORTO_DAEMON_CGROUP);
    TError error = MemorySubsystem->SetLimit(*cg, config().daemon().memory_limit());
    if (error)
        return error;

    cg = MemorySubsystem->Cgroup(PORTO_HELPERS_CGROUP);
    error = RecreateCgroup(*cg);
    if (error)
        return error;

    error = MemorySubsystem->SetLimit(*cg, config().daemon().helpers_memory_limit());
    if (error)
        return error;

    error = MemorySubsystem->SetDirtyLimit(*cg, config().daemon().helpers_dirty_limit());
    if (error)
        L_ERR("Cannot set portod-helpers dirty limit: {}", error);

    cg = FreezerSubsystem->Cgroup(PORTO_CGROUP_PREFIX);
    if (!cg->Exists()) {
        error = cg->Create();
        if (error)
            return error;
    }

    if (Cgroup2Subsystem->Supported) {
        cg = Cgroup2Subsystem->Cgroup(PORTO_CGROUP_PREFIX);
        if (!cg->Exists()) {
            error = cg->Create();
            if (error)
                return error;
        }
    }

    return OK;
}

void TCgroupDriver::CleanupCgroups() {
    TError error;
    int pass = 0;
    bool retry;

again:
    retry = false;

    /* freezer must be first */
    for (auto hy: Hierarchies) {
        std::list<std::unique_ptr<const TCgroup>> cgroups;

        error = CgroupChildren(*(hy->RootCgroup()), cgroups);
        if (error)
            L_ERR("Cannot dump porto {} cgroups : {}", hy->Type, error);

        for (auto it = cgroups.rbegin(); it != cgroups.rend(); ++it) {
            auto& cg = *it;
            if (!StringStartsWith(cg->GetName(), PORTO_CGROUP_PREFIX))
                continue;

            if (cg->GetName() == PORTO_DAEMON_CGROUP &&
                    (hy->Controllers & (CGROUP_FREEZER | CGROUP_MEMORY | CGROUP_CPUACCT | CGROUP2 | CGROUP_PERF)))
                continue;

            if (cg->GetName() == PORTO_HELPERS_CGROUP &&
                    (hy->Controllers & CGROUP_MEMORY))
                continue;

            if (cg->GetName() == PORTO_CGROUP_PREFIX &&
                    (hy->Controllers & (CGROUP_FREEZER | CGROUP2)))
                continue;

            bool found = false;
            for (auto &it: Containers) {
                auto ct = it.second;
                auto ctCg = GetContainerCgroup(*(it.second), hy);
                // child cgroups should not be removed in case of cgroupfs=rw
                if (ct->State != EContainerState::Stopped &&
                    (*ctCg == *cg || (ct->CgroupFs == ECgroupFs::Rw &&
                                    StringStartsWith(cg->GetName(), ctCg->GetName() + "/"))))
                {
                    found = true;
                    break;
                }
            }
            if (found)
                continue;

            if (*hy == *FreezerSubsystem && FreezerSubsystem->IsFrozen(*cg)) {
                (void)FreezerSubsystem->Thaw(*cg, false);
                if (FreezerSubsystem->IsParentFreezing(*cg)) {
                    retry = true;
                    continue;
                }
            }

            error = RemoveCgroup(*cg);
            if (error)
                L_WRN("Cannot cleanup cgroup {}: {}", *cg, error);
        }
    }

    if (retry && pass++ < 3)
        goto again;
}

std::unique_ptr<const TCgroup> TCgroupDriver::GetContainerCgroup(const TContainer &container, TSubsystem *subsystem) const {
    if (container.IsRoot())
        return subsystem->RootCgroup();

    if (subsystem->Controllers & (CGROUP_FREEZER | CGROUP2)) {
        if (container.JobMode)
            return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "/" + container.Parent->Name);
        return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "/" + container.Name);
    }

    if (subsystem->Controllers & CGROUP_SYSTEMD) {
        if (container.Controllers & CGROUP_SYSTEMD)
            return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "%" +
                                    StringReplaceAll(container.Name, "/", "%"));

        return SystemdSubsystem->PortoService->GetUniquePtr();
    }

    std::string cg;
    for (auto ct = &container; !ct->IsRoot(); ct = ct->Parent.get()) {
        auto enabled = ct->Controllers & subsystem->Controllers;

        if (!cg.empty())
            cg = (enabled ? "/" : "%") + cg;
        if (!cg.empty() || enabled)
            cg = ct->FirstName + cg;
    }

    if (cg.empty())
        return subsystem->RootCgroup();

    return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "%" + cg);
}

std::list<std::unique_ptr<const TCgroup>> TCgroupDriver::GetContainerCgroups(const TContainer &container) const {
    std::list<std::unique_ptr<const TCgroup>> cgroups;
    for (auto hy: Hierarchies)
        cgroups.push_back(GetContainerCgroup(container, hy));
    return cgroups;
}

std::unique_ptr<const TCgroup> TCgroupDriver::GetContainerCgroupByKnob(const TContainer &container, const std::string &knob) const {
    const std::string type = knob.substr(0, knob.find('.'));
    for (auto subsys: Subsystems) {
        if (subsys->Type != type)
            continue;

        return GetContainerCgroup(container, subsys);
    }

    return nullptr;
}

bool TCgroupDriver::HasContainerCgroupsKnob(const TContainer &container, const std::string &knob) const {
    auto cg = GetContainerCgroupByKnob(container, knob);
    if (cg == nullptr)
        return false;

    return cg->Has(knob);
}

TError TCgroupDriver::GetContainerCgroupsKnob(const TContainer &container, const std::string &knob, std::string &value) const {
    auto cg = GetContainerCgroupByKnob(container, knob);
    if (cg == nullptr)
        return TError(EError::InvalidValue, "Cgroup does not found by {}", knob);

    if (!cg->Has(knob))
        return TError(EError::InvalidValue, "Unknown cgroup attribute: {}", knob);

    return cg->Get(knob, value);
}


TError TCgroupDriver::CreateCgroup(const TCgroup &cg) const {
    TError error;

    error = cg.Create();
    if (error)
        return error;

    return OK;
}

TError TCgroupDriver::RemoveCgroup(const TCgroup &cg) const {
    TError error;
    std::list<std::unique_ptr<const TCgroup>> children;

    error = CgroupChildren(cg, children, true);
    if (error)
        return error;

    for (auto it = children.rbegin(); it != children.rend(); ++it) {
        error = (*it)->Remove();
        if (error)
            return error;
    }

    error = cg.Remove();
    if (error)
        return error;

    return OK;
}

/* Note that knobs will stay in default values, thus use is limited */
TError TCgroupDriver::RecreateCgroup(const TCgroup &cg) const {
    if (!cg.Exists())
        return cg.Create();

    TError error;
    auto tmpcg = cg.GetSubsystem()->Cgroup(cg.GetName() + "_tmp");
    auto cleanup = [&](const TError &error) -> TError {
        (void)cg.AttachAll(*tmpcg);
        (void)tmpcg->KillAll(SIGKILL);
        (void)tmpcg->Remove();
        return error;
    };

    if (tmpcg->Exists())
        cleanup(OK);

    L_CG("Recreate cgroup {}", cg);
    error = tmpcg->Create();
    if (error)
        return error;

    error = tmpcg->AttachAll(cg);
    if (error)
        return cleanup(error);

    error = cg.Remove();
    if (error)
        return cleanup(error);

    error = cg.Create();
    if (error)
        return cleanup(error);

    return cleanup(OK);
}

TError TCgroupDriver::CreateContainerCgroups(TContainer &container, bool onRestore) const {
    TError error;

    auto missing = container.Controllers | container.RequiredControllers;

    for (auto hy: Hierarchies) {
        auto cg = GetContainerCgroup(container, hy);

        if (!(container.Controllers & hy->Controllers))
            continue;

        if ((container.Controllers & hy->Controllers) != hy->Controllers) {
            container.Controllers |= hy->Controllers;
            container.SetProp(EProperty::CONTROLLERS);
        }

        missing &= ~hy->Controllers;

        if (cg->Exists())
            continue;

        error = CreateCgroup(*cg);
        if (error)
            return error;

        if (onRestore) {
            L_WRN("There is no {} cgroup for CT{}:{}", TSubsystem::Format(hy->Controllers), container.Id, container.Name);
            if (hy->Controllers & CGROUP_FREEZER)
                continue;

            // freezer is first in Hierarchies
            auto freezer = GetContainerCgroup(container, FreezerSubsystem.get());
            error = cg->AttachAll(*freezer, true);
            if (error)
                return error;
        }
    }

    if (missing) {
        std::string types;
        for (auto subsys: Subsystems)
            if (subsys->Kind & missing)
                types += " " + subsys->Type;
        return TError(EError::NotSupported, "Some cgroup controllers are not available:" + types);
    }

    return OK;
}

TError TCgroupDriver::RemoveContainerCgroups(const TContainer &container, bool ignore) const {
    TError error;

    for (auto hy: Hierarchies) {
        if (!(container.Controllers & hy->Controllers) || hy->Controllers & CGROUP_FREEZER)
            continue;

        error = RemoveCgroup(*GetContainerCgroup(container, hy));
        if (!ignore && error && error.Errno != ENOENT)
            return error;
    }

    /* Dispose freezer at last because of recovery */
    error = RemoveCgroup(*GetContainerCgroup(container, FreezerSubsystem.get()));
    if (!ignore && error && error.Errno != ENOENT)
        return error;

    return OK;
}

TError TCgroupDriver::CgroupChildren(const TCgroup &cg, std::list<std::unique_ptr<const TCgroup>> &cgroups, bool all) const {
    TPathWalk walk;
    TError error;

    cgroups.clear();

    error = walk.OpenList(cg.Path());
    if (error)
        return TError(error, "Cannot get childs for {}", cg);

    while (1) {
        error = walk.Next();
        if (error)
            return TError(error, "Cannot get childs for {}", cg);

        if (!walk.Path)
            break;

        if (!S_ISDIR(walk.Stat->st_mode) || walk.Postorder || walk.Level() == 0)
            continue;

        std::string name = cg.GetSubsystem()->Root.InnerPath(walk.Path).ToString();

        /* Ignore non-porto cgroups */
        if (!StringStartsWith(name, PORTO_CGROUP_PREFIX) && !all)
            continue;

        cgroups.push_back(cg.GetSubsystem()->Cgroup(name));
    }

    return OK;
}

TError TCgroupDriver::GetCgroupCount(const TCgroup &cgroup, uint64_t &count, bool thread) const {
    TError error;
    std::list<std::unique_ptr<const TCgroup>> cgroups;

    error = CgroupChildren(cgroup, cgroups);
    if (error)
        return error;

    cgroups.push_back(cgroup.GetUniquePtr());

    count = 0;
    uint64_t buf;
    for (auto& cg: cgroups) {
        error = cg->GetCount(buf, thread);
        if (error) {
            count = 0;
            return error;
        }
        count += buf;
    }

    return OK;
}

TError TCgroupDriver::GetCgroupThreadCount(const TCgroup &cg, uint64_t &count) const {
    return GetCgroupCount(cg, count, true);
}

TError TCgroupDriver::GetCgroupProcessCount(const TCgroup &cg, uint64_t &count) const {
    return GetCgroupCount(cg, count);
}
