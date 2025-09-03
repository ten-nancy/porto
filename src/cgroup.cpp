#include "cgroup.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>

#include "config.hpp"
#include "container.hpp"
#include "device.hpp"
#include "util/error.hpp"
#include "util/log.hpp"
#include "util/string.hpp"
#include "util/task.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/sysmacros.h>
#include <unistd.h>
}

const TFlagsNames ControllersName = {
    {CGROUP_FREEZER, "freezer"}, {CGROUP_MEMORY, "memory"}, {CGROUP_CPU, "cpu"},         {CGROUP_CPUACCT, "cpuacct"},
    {CGROUP_NETCLS, "net_cls"},  {CGROUP_BLKIO, "blkio"},   {CGROUP_DEVICES, "devices"}, {CGROUP_HUGETLB, "hugetlb"},
    {CGROUP_CPUSET, "cpuset"},   {CGROUP_PIDS, "pids"},     {CGROUP_PERF, "perf_event"}, {CGROUP_SYSTEMD, "systemd"},
    {CGROUP2, "cgroup2"},
};

static constexpr const char *CGROUP_PROCS = "cgroup.procs";
static constexpr const char *CGROUP_SUBTREE_CONTROL = "cgroup.subtree_control";
static constexpr const char *CGROUP_THEADS = "cgroup.threads";
static constexpr const char *CGROUP_KILL = "cgroup.kill";
static constexpr const char *TASKS = "tasks";

static constexpr const char *LEAF = "leaf";

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
    return HasSubsystem() ? GetSubsystem()->Cgroup(GetName(), HasLeaf) : nullptr;
}

// TCgroup processes

TError TCgroup::SetPids(const std::string &knob, const std::vector<pid_t> &pids) const {
    if (!HasSubsystem())
        return TError("Cannot get from null cgroup");

    TError error;
    TFile file;

    error = file.OpenWrite(Knob(knob));
    if (error)
        return TError::System("Cannot set pids to knob " + knob);

    for (pid_t pid: pids)
        file.WriteAll(std::to_string(pid));

    return OK;
}

TError TCgroup::GetPids(const std::string &knob, std::vector<pid_t> &pids) const {
    if (!HasSubsystem())
        return TError("Cannot get from null cgroup");

    FILE *file;
    int pid;

    pids.clear();
    file = fopen(Knob(knob).ToString().c_str(), "r");
    if (!file)
        return TError::System("Cannot open knob " + knob);

    while (fscanf(file, "%d", &pid) == 1)
        pids.push_back(pid);

    fclose(file);
    return OK;
}

TError TCgroup::GetProcesses(std::vector<pid_t> &pids) const {
    return GetPids(CGROUP_PROCS, pids);
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

static void getUintMap(FILE *stream, TUintMap &value) {
    char *key;
    unsigned long long val;

    while (fscanf(stream, "%ms %llu\n", &key, &val) == 2) {
        value[std::string(key)] = val;
        free(key);
    }
}

TError TCgroup::GetUintMap(const std::string &knob, TUintMap &value) const {
    if (!HasSubsystem())
        return TError("Cannot get from null cgroup");

    FILE *stream = fopen(Knob(knob).c_str(), "r");
    if (!stream)
        return TError::System("Cannot open knob {}", knob);

    getUintMap(stream, value);
    fclose(stream);
    return OK;
}

TError TCgroup::GetUintMap(const TFile &file, TUintMap &value) {
    auto error = file.Lseek(0, SEEK_SET);
    if (error)
        return error;

    TFile other;
    error = other.Dup(file);
    if (error)
        return error;

    FILE *stream = fdopen(other.Fd, "r");
    if (!stream)
        return TError::System("Cannot open knob");
    other.SetFd = -1;

    getUintMap(stream, value);
    fclose(stream);
    return OK;
}

// TCgroup1

class TCgroup1: public TCgroup {
    // cgroup1 specified
    static TError AbortFuse(pid_t pid);

public:
    // constructors and destructor
    // TCgroup1() = delete;
    TCgroup1() = default;
    TCgroup1(const TSubsystem *subsystem, const std::string &name, bool hasLeaf)
        : TCgroup(subsystem, name, hasLeaf)
    {}
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

    std::unique_ptr<const TCgroup> Leaf() const override {
        return std::unique_ptr<const TCgroup>(new TCgroup1(Subsystem, Name, HasLeaf));
    }
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
        L_CG_ERR("Cannot remove cgroup {} : {}, {} tasks inside", *this, error, tasks.size());

        L("Tasks before destroy:");
        for (auto task: startTasks)
            L("task: {}", task);

        L("Tasks after destroy:");
        for (size_t i = 0; i < tasks.size() && i < config().daemon().debug_hung_tasks_count(); ++i) {
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

    error = SetPids(thread ? TASKS : CGROUP_PROCS, {pid});
    if (error)
        L_ERR("Cannot attach {} {} to {} : {}", thread ? "thread" : "process", pid, *this, error);

    return error;
}

TError TCgroup1::AttachAll(const TCgroup &cg, bool thread) const {
    if (IsSubsystem(CGROUP_NETCLS) && !config().network().enable_netcls_classid())
        return OK;

    if (IsSecondary())
        return TError("Cannot attach to secondary cgroup " + Type());

    L_CG("Attach all processes from {} to {}", cg, *this);

    TError error;
    std::vector<pid_t> pids, prev;
    bool retry;

    error = cg.GetProcesses(pids);
    if (error)
        return error;

    unsigned int now, startTime = GetCurrentTimeMs();
    do {
        error = thread ? cg.GetTasks(pids) : cg.GetProcesses(pids);
        if (error)
            return error;

        retry = false;
        for (auto pid: pids) {
            error = SetPids(thread ? TASKS : CGROUP_PROCS, {pid});
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
        if (++iteration > 10 && !frozen && CgroupDriver.FreezerSubsystem->IsBound(*this) &&
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
    if (CgroupDriver.FreezerSubsystem->IsBound(*this) && CgroupDriver.FreezerSubsystem->IsFrozen(*this)) {
        if (!frozen)
            L_TAINT(fmt::format("Rogue freeze in cgroup {}", *this));

        error = CgroupDriver.FreezerSubsystem->Thaw(*this, false);
        if (error)
            L_WRN("Cannot thaw cgroup {}: {}", *this, error);
    }

    return error;
}

TError TCgroup1::GetTasks(std::vector<pid_t> &pids) const {
    return GetPids(TASKS, pids);
}

TError TCgroup1::GetCount(uint64_t &count, bool thread) const {
    if (!HasSubsystem())
        TError("Cannot get from null cgroup");

    TError error;
    std::vector<pid_t> pids;

    error = GetPids(thread ? TASKS : CGROUP_PROCS, pids);
    if (error)
        return error;

    count = pids.size();

    return OK;
}

// TCgroup2

class TCgroup2: public TCgroup {
    // cgroup2 specified
    TPath LeafPath() const;
    static std::string LeafKnob(const std::string &knob, bool hasLeaf);

public:
    // constructors and destructor
    TCgroup2() = delete;
    TCgroup2(const TSubsystem *subsystem, const std::string &name, bool hasLeaf)
        : TCgroup(subsystem, name, hasLeaf)
    {}
    ~TCgroup2() = default;

    // modifiers
    TError Create() const override;
    TError Remove() const override;

    // processes
    TError Attach(pid_t pid, bool thread = false) const override;
    TError AttachAll(const TCgroup &cg, bool thread = false) const override;
    TError KillAll(int signal, bool abortFuse = false) const override;

    TError GetProcesses(std::vector<pid_t> &pids) const override;
    TError GetTasks(std::vector<pid_t> &pids) const override;
    TError GetCount(uint64_t &count, bool thread = false) const override;

    std::unique_ptr<const TCgroup> Leaf() const override {
        if (!HasLeaf)
            return std::unique_ptr<const TCgroup>(new TCgroup2(Subsystem, Name, HasLeaf));

        return std::unique_ptr<const TCgroup>(new TCgroup2(Subsystem, Name + "/leaf", HasLeaf));
    }

    // controllers
    static inline std::string ConvertControllerToV2(const std::string &type);
    bool HasEmptyControllers() const;

    TError SetController(const std::string &type, bool add) const;
    TError AddController(const std::string &type) const;
    TError DelController(const std::string &type) const;

    TError GetControllers(std::vector<std::string> &controllers) const;
    TError GetControllers(std::string &controllers) const;

    TError SetControllers(const std::vector<std::string> &controllers) const;
    TError SetControllers(const std::string &controllers) const;

    TError InitializeControllers() const;
    TError InitializeSubsystems() const;
};

// TCgroup2 cgroup2 specified

TPath TCgroup2::LeafPath() const {
    if (!HasLeaf)
        return Path();

    return Path() / LEAF;
}

std::string TCgroup2::LeafKnob(const std::string &knob, bool hasLeaf) {
    if (!hasLeaf)
        return knob;

    return fmt::format("{}/{}", LEAF, knob);
}

// TCgroup2 modifiers

TError TCgroup2::Create() const {
    TError error;

    if (Exists() && IsSecondary())
        return OK;

    if (StringEndsWith(GetName(), LEAF))
        return TError("Cgroup name cannot be {}", LEAF);

    L_CG("Create cgroup {}", *this);
    error = Path().Mkdir(0755);
    if (error)
        return TError(error, "Cannot create cgroup {}", *this);

    if (HasLeaf) {
        L_CG("Create leaf cgroup {}", LeafPath());
        error = LeafPath().Mkdir(0755);
        if (error) {
            (void)Path().Rmdir();
            return TError(error, "Cannot create leaf of cgroup {}", *this);
        }
    }

    error = InitializeControllers();
    if (error) {
        if (HasLeaf)
            (void)LeafPath().Rmdir();
        (void)Path().Rmdir();
        return TError(error, "Cannot initialize controllers of cgroup {}", *this);
    }

    error = InitializeSubsystems();
    if (error) {
        if (HasLeaf)
            (void)LeafPath().Rmdir();
        (void)Path().Rmdir();
        return TError(error, "Cannot initialize cgroup {}", *this);
    }

    return OK;
}

TError TCgroup2::Remove() const {
    TError error;
    struct stat st;

    if (IsSecondary())
        return OK;

    L_CG("Remove cgroup {}", *this);
    error = LeafPath().Rmdir();

    std::vector<pid_t> startTasks;
    GetTasks(startTasks);
    /* workaround for bad synchronization */
    if (error && error.Errno == EBUSY && !LeafPath().StatStrict(st) && st.st_nlink == 2) {
        uint64_t deadline = GetCurrentTimeMs() + config().daemon().cgroup_remove_timeout_s() * 1000;
        uint64_t interval = 1;
        do {
            error = KillAll(SIGKILL);
            if (error)
                L_WRN("Cannot kill tasks of cgroup {}: {}", *this, error);

            error = LeafPath().Rmdir();
            if (!error || error.Errno != EBUSY)
                break;

            if (interval < 1000)
                interval *= 10;

        } while (!WaitDeadline(deadline, interval));
    }

    // TODO(kndrvt): return ENOENT and process his case later
    if (error && (/*error.Errno != ENOENT || */ Exists())) {
        std::vector<pid_t> tasks;
        GetTasks(tasks);
        L_CG_ERR("Cannot remove cgroup {}: {}, {} tasks inside", *this, error, tasks.size());

        L("Tasks before destroy:");
        for (auto task: startTasks)
            L("task: {}", task);

        L("Tasks after destroy:");
        for (size_t i = 0; i < tasks.size() && i < config().daemon().debug_hung_tasks_count(); ++i) {
            auto task = tasks[i];
            PrintProc("status", task);
            PrintProc("wchan", task);
            PrintStack(task);
        }
    }

    if (HasLeaf) {
        error = Path().Rmdir();
        if (error && error.Errno != ENOENT)
            L_WRN("Cannot remove cgroup {}: {}", *this, error);
    }

    return OK;
}

// TCgroup2 processes

TError TCgroup2::Attach(pid_t pid, bool thread) const {
    L_CG("Attach {} {} to {}", thread ? "thread" : "process", pid, *this);
    TError error;

    if (HasLeaf) {
        // TODO(kndrvt): remove this backward compatibility later
        TPath leafPath = LeafPath();
        if (!leafPath.PathExists()) {
            L_CG("Create leaf cgroup {}", leafPath);
            error = leafPath.Mkdir(0755);
            if (error)
                L_ERR("Cannot create leaf of cgroup {}", *this);
        }
    }

    error = SetPids(LeafKnob(thread ? CGROUP_THEADS : CGROUP_PROCS, HasLeaf), {pid});
    if (error)
        L_ERR("Cannot attach {} {} to {} : {}", thread ? "thread" : "process", pid, *this, error);

    return error;
}

TError TCgroup2::AttachAll(const TCgroup &cg, bool thread) const {
    L_CG("Attach all processes from {} to {}", cg, *this);

    TError error;
    if (HasLeaf) {
        // TODO(kndrvt): remove this backward compatibility later
        TPath leafPath = LeafPath();
        if (!leafPath.PathExists()) {
            L_CG("Create leaf cgroup {}", leafPath);
            error = leafPath.Mkdir(0755);
            if (error)
                L_ERR("Cannot create leaf of cgroup {}", *this);
        }
    }

    std::vector<pid_t> pids, prev;
    bool retry;

    error = cg.GetProcesses(pids);
    if (error)
        return error;

    unsigned int now, startTime = GetCurrentTimeMs();
    do {
        error = thread ? cg.GetTasks(pids) : cg.GetProcesses(pids);
        if (error)
            return error;

        retry = false;
        for (auto pid: pids) {
            error = SetPids(LeafKnob(thread ? CGROUP_THEADS : CGROUP_PROCS, HasLeaf), {pid});
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

TError TCgroup2::KillAll(int signal, bool) const {
    TError error;

    L_CG("KillAll {} {}", signal, *this);

    if (IsRoot())
        return TError(EError::Permission, "Bad idea");

    if (Has(CGROUP_KILL)) {
        error = Set(CGROUP_KILL, "1");
        if (error)
            return error;
        return OK;
    }

    L_CG("Downgrade to cgroup v1 killing, cause there is no {} in {}", CGROUP_KILL, *this);

    std::vector<pid_t> tasks, killed;
    TError error2;
    bool retry;
    bool frozen = false;
    int iteration = 0;

    do {
        if (++iteration > 10 && !frozen && CgroupDriver.FreezerSubsystem->IsBound(*this) &&
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

    if (CgroupDriver.FreezerSubsystem->IsBound(*this) && CgroupDriver.FreezerSubsystem->IsFrozen(*this)) {
        if (!frozen)
            L_TAINT(fmt::format("Rogue freeze in cgroup {}", *this));

        error = CgroupDriver.FreezerSubsystem->Thaw(*this, false);
        if (error)
            L_WRN("Cannot thaw cgroup {}: {}", *this, error);
    }

    return error;
}

TError TCgroup2::GetProcesses(std::vector<pid_t> &pids) const {
    return GetPids(LeafKnob(CGROUP_PROCS, HasLeaf), pids);
}

TError TCgroup2::GetTasks(std::vector<pid_t> &pids) const {
    return GetPids(LeafKnob(CGROUP_THEADS, HasLeaf), pids);
}

TError TCgroup2::GetCount(uint64_t &count, bool thread) const {
    if (!GetSubsystem())
        TError("Cannot get from null cgroup");

    TError error;
    std::vector<pid_t> pids;

    error = GetPids(LeafKnob(thread ? CGROUP_THEADS : CGROUP_PROCS, HasLeaf), pids);
    if (error)
        return error;

    count = pids.size();

    return OK;
}

// TCgroup2 controllers

std::string TCgroup2::ConvertControllerToV2(const std::string &type) {
    if (type == "blkio")
        return "io";
    else if (type == "unified" || type == "cgroup2")
        return "";
    else
        return type;
}

bool TCgroup2::HasEmptyControllers() const {
    TError error;
    std::string value;

    error = Get(CGROUP_SUBTREE_CONTROL, value);
    if (error)
        return false;

    return value.empty();
}

TError TCgroup2::SetController(const std::string &type, bool add) const {
    std::string t = ConvertControllerToV2(type);
    if (t.empty())
        return OK;
    return Set(CGROUP_SUBTREE_CONTROL, (add ? "+" : "-") + t);
}

TError TCgroup2::AddController(const std::string &type) const {
    return SetController(type, true);
}

TError TCgroup2::DelController(const std::string &type) const {
    return SetController(type, false);
}

TError TCgroup2::GetControllers(std::vector<std::string> &controllers) const {
    TError error;
    std::string buf;

    error = GetControllers(buf);
    if (error)
        return error;

    controllers = SplitString(buf, ' ');

    return OK;
}

TError TCgroup2::GetControllers(std::string &controllers) const {
    return Get(CGROUP_SUBTREE_CONTROL, controllers);
}

TError TCgroup2::SetControllers(const std::vector<std::string> &controllers) const {
    TError error;
    std::string value;

    for (auto &c: controllers) {
        std::string type = ConvertControllerToV2(c);
        if (!type.empty())
            value += " +" + type;
    }

    error = Set(CGROUP_SUBTREE_CONTROL, value);
    if (error)
        return error;

    return OK;
}

TError TCgroup2::SetControllers(const std::string &controllers) const {
    return SetControllers(SplitString(controllers, ' '));
}

TError TCgroup2::InitializeControllers() const {
    TError error;
    std::vector<std::string> controllers;

    for (auto subsys: CgroupDriver.Cgroup2Subsystems) {
        if (CgroupDriver.Cgroup2Subsystem->Controllers & subsys->Kind)
            controllers.push_back(subsys->Type);
    }

    error = SetControllers(controllers);
    if (error)
        return error;

    return OK;
}

TError TCgroup2::InitializeSubsystems() const {
    TError error;

    for (auto subsys: CgroupDriver.Cgroup2Subsystems) {
        if (CgroupDriver.Cgroup2Subsystem->Controllers & subsys->Kind) {
            error = subsys->InitializeCgroup(*this);
            if (error)
                return error;
        }
    }

    return OK;
}

std::unique_ptr<const TCgroup> TSubsystem::RootCgroup() const {
    return Cgroup(IsCgroup2() ? "/porto" : "/", false);
}

std::unique_ptr<const TCgroup> TSubsystem::Cgroup(const std::string &name, bool hasLeaf) const {
    PORTO_ASSERT(name[0] == '/');
    if (CgroupDriver.UseCgroup2() && IsCgroup2())
        return std::unique_ptr<const TCgroup2>(new TCgroup2(this, name, hasLeaf));
    else
        return std::unique_ptr<const TCgroup1>(new TCgroup1(this, name, hasLeaf));
}

// TODO(kndrvt): remove leaf if exists
TError TSubsystem::TaskCgroup(pid_t pid, std::unique_ptr<const TCgroup> &cgroup, bool hasLeaf) const {
    std::vector<std::string> lines;
    auto cg_file = TPath("/proc/" + std::to_string(pid) + "/cgroup");
    auto type = CgroupDriver.UseCgroup2() && IsCgroup2() ? "" : TestOption();
    std::unordered_set<std::string> cgroupTypes;

    TError error = cg_file.ReadLines(lines);
    if (error)
        return error;

    bool found = false;

    for (auto &line: lines) {
        auto fields = SplitString(line, ':', 3);
        if (fields.size() < 3)
            continue;

        auto cgroups = SplitString(fields[1], ',');
        if (cgroups.empty())
            cgroups.push_back("");

        for (auto &cg: cgroups) {
            // check that we do not have fake cgroups created by cgroup with \n in name
            if (cgroupTypes.find(cg) != cgroupTypes.end())
                return TError(EError::Permission, "Fake cgroup found");
            cgroupTypes.insert(cg);

            if (!found && cg == type) {
                TPath cgroup_path = fields[2];
                if (type == "" && cgroup_path.BaseName() == LEAF)
                    cgroup_path = cgroup_path.DirName();
                found = true;
                cgroup = Cgroup(cgroup_path.ToString(), hasLeaf);
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
    TError error;
    auto cg = RootCgroup();

    HasWritebackBlkio = cg->Has(WRITEBACK_BLKIO);
    if (HasWritebackBlkio)
        L_CG("Supports {}", std::string(WRITEBACK_BLKIO));

    HasMemoryLockPolicy = cg->Has(MLOCK_POLICY);
    if (HasMemoryLockPolicy)
        L_CG("Supports {}", std::string(MLOCK_POLICY));

    return OK;
}

TError TMemorySubsystem::Statistics(const TCgroup &cg, TUintMap &stat) const {
    return cg.GetUintMap(STAT, stat);
}

TError TMemorySubsystem::GetCacheUsage(const TCgroup &cg, uint64_t &usage) const {
    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage = stat["total_inactive_file"] + stat["total_active_file"];
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
            usage = stat["total_inactive_anon"] + stat["total_active_anon"] + stat["total_unevictable"];

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

TError TMemorySubsystem::GetReclaimed(const TCgroup &cg, uint64_t &value) const {
    TUintMap stat;
    Statistics(cg, stat);
    value = stat["total_pgpgout"] * 4096; /* Best estimation for now */
    return OK;
}

TError TMemorySubsystem::Usage(const TCgroup &cg, uint64_t &value) const {
    return cg.GetUint64(IsCgroup2() ? CURRENT : USAGE, value);
}

bool TMemorySubsystem::SupportSoftLimit() const {
    return RootCgroup()->Has(SOFT_LIMIT);
}

TError TMemorySubsystem::SetSoftLimit(const TCgroup &cg, int64_t limit) const {
    if (!SupportSoftLimit())
        return OK;
    return cg.SetInt64(SOFT_LIMIT, limit);
}

TError TMemorySubsystem::GetSoftLimit(const TCgroup &cg, int64_t &limit) const {
    if (!SupportSoftLimit())
        return OK;
    return cg.GetInt64(SOFT_LIMIT, limit);
}

bool TMemorySubsystem::SupportGuarantee() const {
    return RootCgroup()->Has(IsCgroup2() ? LOW : LOW_LIMIT);
}

TError TMemorySubsystem::SetGuarantee(const TCgroup &cg, uint64_t guarantee) const {
    if (!SupportGuarantee())
        return OK;
    return cg.SetUint64(IsCgroup2() ? LOW : LOW_LIMIT, guarantee);
}

bool TMemorySubsystem::SupportHighLimit() const {
    return RootCgroup()->Has(IsCgroup2() ? HIGH : HIGH_LIMIT);
}

TError TMemorySubsystem::SetHighLimit(const TCgroup &cg, uint64_t limit) const {
    if (!SupportHighLimit())
        return OK;
    if (!limit) {
        if (IsCgroup2())
            return cg.Set(HIGH, "max");
        else
            return cg.Set(HIGH_LIMIT, "-1");
    }
    return cg.SetUint64(IsCgroup2() ? HIGH : HIGH_LIMIT, limit);
}

bool TMemorySubsystem::SupportRechargeOnPgfault() const {
    return RootCgroup()->Has(RECHARGE_ON_PAGE_FAULT);
}

TError TMemorySubsystem::RechargeOnPgfault(const TCgroup &cg, bool enable) const {
    if (!SupportRechargeOnPgfault())
        return OK;
    return cg.SetBool(RECHARGE_ON_PAGE_FAULT, enable);
}

bool TMemorySubsystem::SupportNumaBalance() const {
    return RootCgroup()->Has(NUMA_BALANCE_VMPROT);
}
TError TMemorySubsystem::SetNumaBalance(const TCgroup &cg, uint64_t flag, uint64_t mask) {
    if (!SupportNumaBalance())
        return OK;
    return cg.Set(NUMA_BALANCE_VMPROT, fmt::format("{} {}", flag, mask));
}

bool TMemorySubsystem::SupportDirtyLimit() const {
    return RootCgroup()->Has(DIRTY_LIMIT);
}

TError TMemorySubsystem::SetDirtyLimit(const TCgroup &cg, uint64_t limit) {
    if (!SupportDirtyLimit())
        return OK;
    if (limit || !cg.Has(DIRTY_RATIO))
        return cg.SetUint64(DIRTY_LIMIT, limit);
    return cg.SetUint64(DIRTY_RATIO, 50);
}

bool TMemorySubsystem::SupportAnonLimit() const {
    return RootCgroup()->Has(ANON_LIMIT);
}

TError TMemorySubsystem::SetAnonLimit(const TCgroup &cg, uint64_t limit) const {
    if (!SupportAnonLimit())
        return OK;
    return cg.Set(ANON_LIMIT, limit ? std::to_string(limit) : "-1");
}

TError TMemorySubsystem::ResetAnonMaxUsage(const TCgroup &cg) const {
    if (!cg.Has(ANON_MAX_USAGE))
        return OK;
    return cg.SetUint64(ANON_MAX_USAGE, 0);
}

TError TMemorySubsystem::GetAnonMaxUsage(const TCgroup &cg, uint64_t &usage) const {
    if (!cg.Has(ANON_MAX_USAGE))
        return OK;
    return cg.GetUint64(ANON_MAX_USAGE, usage);
}

TError TMemorySubsystem::GetAnonUsage(const TCgroup &cg, uint64_t &usage) const {
    if (cg.Has(ANON_USAGE))
        return cg.GetUint64(ANON_USAGE, usage);

    TUintMap stat;
    TError error = Statistics(cg, stat);
    if (!error)
        usage =
            stat["total_inactive_anon"] + stat["total_active_anon"] + stat["total_unevictable"] + stat["total_swap"];
    return error;
}

bool TMemorySubsystem::SupportAnonOnly() const {
    return Cgroup(PORTO_DAEMON_CGROUP, false)->Has(ANON_ONLY);
}

TError TMemorySubsystem::SetAnonOnly(const TCgroup &cg, bool val) const {
    if (cg.Has(ANON_ONLY))
        return cg.SetBool(ANON_ONLY, val);
    return OK;
}

bool TMemorySubsystem::SupportSwap() const {
    return RootCgroup()->Has(IsCgroup2() ? SWAP_MAX : MEM_SWAP_LIMIT);
}

constexpr auto HIGH_LIMIT_RETRIES = 3;

TError TMemorySubsystem::ReclaimLimit(const TCgroup &cg, uint64_t limit, std::string &old_high_limit) {
    const auto knob = IsCgroup2() ? HIGH : HIGH_LIMIT;

    auto error = cg.Get(knob, old_high_limit);
    if (error)
        return error;

    for (int i = 0; i < HIGH_LIMIT_RETRIES; ++i) {
        error = cg.SetUint64(knob, limit);
        if (error) {
            if (i > 0)
                goto fail;
            return error;
        }

        uint64_t usage;
        error = Usage(cg, usage);
        if (error)
            goto fail;

        if (usage <= limit)
            return OK;
    }

    error = TError(EError::Unknown, EBUSY, "high_limit");
fail:
    // try to revert high limit
    auto error2 = cg.Set(knob, old_high_limit);
    if (error2)
        L_WRN("Failed to rollback high limit: {}", error2);
    return error;
}

TError TMemorySubsystem::SetLimit(const TCgroup &cg, uint64_t limit) {
    TError error;
    uint64_t old_limit;
    std::string old_high_limit;
    std::string value;
    const std::string maxKnob = IsCgroup2() ? MAX : LIMIT;
    const std::string highKnob = IsCgroup2() ? HIGH : HIGH_LIMIT;
    const std::string swapKnob = IsCgroup2() ? SWAP_MAX : MEM_SWAP_LIMIT;
    /*
     * Maxumum value depends on arch, kernel version and bugs
     * "-1" works everywhere since 2.6.31
     */
    if (!limit) {
        std::string value = IsCgroup2() ? "max" : "-1";
        if (SupportSwap()) {
            error = cg.Set(swapKnob, value);
            if (error)
                L_WRN("Cannot set {}: {}", swapKnob, value);
        }
        return cg.Set(maxKnob, value);
    }

    if (IsCgroup2()) {
        error = cg.Get(maxKnob, value);
        if (value == "max")
            old_limit = INT64_MAX;
    } else
        error = cg.GetUint64(maxKnob, old_limit);
    if (error)
        return error;

    if (old_limit == limit)
        return OK;

    /* reduce high limit first to fail faster */
    if (limit < old_limit && SupportHighLimit()) {
        error = ReclaimLimit(cg, limit, old_high_limit);
        if (error)
            return error;
    }

    /* Memory limit cannot be bigger than Memory+Swap limit. */
    if (SupportSwap()) {
        uint64_t cur_limit;
        cg.GetUint64(swapKnob, cur_limit);
        if (cur_limit < limit) {
            error = cg.SetUint64(swapKnob, limit);
            if (error)
                L_WRN("Cannot set {}: {}", swapKnob, value);
        }
    }

    // this also updates high limit
    error = cg.SetUint64(maxKnob, limit);
    if (!error && SupportSwap())
        error = cg.SetUint64(swapKnob, limit);

    if (error) {
        (void)cg.SetUint64(maxKnob, old_limit);
        if (!old_high_limit.empty())
            (void)cg.Set(highKnob, old_high_limit);
    }

    return error;
}

bool TMemorySubsystem::SupportIoLimit() const {
    return RootCgroup()->Has(FS_BPS_LIMIT);
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

TError TMemorySubsystem::LinkWritebackBlkio(const TCgroup &memcg, const TCgroup &blkcg) const {
    TError error;
    TFile file;

    if (!HasWritebackBlkio || !CgroupDriver.BlkioSubsystem->Supported || (Controllers & CGROUP_BLKIO))
        return OK;

    error = file.OpenDir(blkcg.Path());
    if (error)
        return error;

    L_CG("Link writeback of {} with {}", memcg, blkcg);
    return memcg.SetUint64(WRITEBACK_BLKIO, file.Fd);
}

TError TMemorySubsystem::SetupOomEvent(const TCgroup &cg, TFile &event) const {
    event.Close();

    if (!IsCgroup2())
        return SetupOomEventLegacy(cg, event);

    auto error = event.OpenRead(cg.Knob(EVENTS_LOCAL));
    if (error)
        return error;

    PORTO_ASSERT(event.Fd > 2);

    return OK;
}

TError TMemorySubsystem::SetupOomEventLegacy(const TCgroup &cg, TFile &event) const {
    TError error;
    TFile knob;

    error = knob.OpenRead(cg.Knob(OOM_CONTROL));
    if (error)
        return error;

    PORTO_ASSERT(knob.Fd > 2);

    event.SetFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (event.Fd < 0)
        return TError::System("Cannot create eventfd");

    PORTO_ASSERT(event.Fd > 2);

    error = cg.Set(EVENT_CONTROL, std::to_string(event.Fd) + " " + std::to_string(knob.Fd));
    if (error) {
        event.Close();
        return error;
    }

    return OK;
}

uint64_t TMemorySubsystem::NotifyOomEvents(TFile &event) const {
    if (!event)
        return 0;

    uint64_t value = 0;
    int ret = read(event.Fd, &value, sizeof(value));
    if (ret != sizeof(value))
        return 0;

    return value;
}

TError TMemorySubsystem::GetOomKills(const TCgroup &cg, uint64_t &value) const {
    TUintMap map;
    auto error = cg.GetUintMap(IsCgroup2() ? EVENTS_LOCAL : OOM_CONTROL, map);
    if (error)
        return error;

    auto it = map.find("oom_kill");
    if (it == map.end())
        return TError(EError::NotSupported, "field oom_kill not found");

    value = it->second;
    return OK;
}

TError TMemorySubsystem::SetUseHierarchy(const TCgroup &cg, bool value) const {
    if (IsCgroup2())
        return OK;
    return cg.SetBool(USE_HIERARCHY, value);
}

// Freezer

TError TFreezerSubsystem::WaitState(const TCgroup &cg, const std::string &state) const {
    uint64_t deadline = GetCurrentTimeMs() + config().daemon().freezer_wait_timeout_s() * 1000;
    std::string cur;
    TError error;

    do {
        error = cg.Get(STATE, cur);
        if (error || StringTrim(cur) == state)
            return error;
    } while (!WaitDeadline(deadline));

    return TError("Freezer {} timeout waiting {}", cg.GetName(), state);
}

TError TFreezerSubsystem::Freeze(const TCgroup &cg, bool wait) const {
    TError error = cg.Set(STATE, "FROZEN");
    if (error || !wait)
        return error;
    error = WaitState(cg, "FROZEN");
    if (error)
        (void)cg.Set(STATE, "THAWED");
    return error;
}

TError TFreezerSubsystem::Thaw(const TCgroup &cg, bool wait) const {
    TError error = cg.Set(STATE, "THAWED");
    if (error || !wait)
        return error;
    if (IsParentFreezing(cg))
        return TError(EError::Busy, "parent cgroup is frozen");
    return WaitState(cg, "THAWED");
}

bool TFreezerSubsystem::IsFrozen(const TCgroup &cg) const {
    std::string state;
    return !cg.Get(STATE, state) && StringTrim(state) != "THAWED";
}

bool TFreezerSubsystem::IsSelfFreezing(const TCgroup &cg) const {
    bool val;
    return !cg.GetBool(SELF_FREEZING, val) && val;
}

bool TFreezerSubsystem::IsParentFreezing(const TCgroup &cg) const {
    bool val;
    return !cg.GetBool(PARENT_FREEZING, val) && val;
}

// Cpu

TError TCpuSubsystem::InitializeSubsystem() {
    auto cg = RootCgroup();

    L_SYS("{} cores", GetNumCores());

    // shares / weight
    if (IsCgroup2()) {
        HasShares = cg->Has(WEIGHT);
        if (HasShares && cg->GetUint64(WEIGHT, BaseShares))
            BaseShares = 100; /* kernel deafult CGROUP_WEIGHT_DFL */

        MinShares = 1;     /* kernel limit CGROUP_WEIGHT_MIN */
        MaxShares = 10000; /* kernel limit CGROUP_WEIGHT_MAX */
    } else {
        HasShares = cg->Has(SHARES);
        if (HasShares && cg->GetUint64(SHARES, BaseShares))
            BaseShares = 1024;

        MinShares = 2;          /* kernel limit MIN_SHARES */
        MaxShares = 1024 * 256; /* kernel limit MAX_SHARES */
    }

    if (HasShares)
        L_CG("support shares {}", BaseShares);

    // quota, rt, reserve
    if (IsCgroup2()) {
        HasQuota = cg->Has(MAX);
    } else {
        HasQuota = cg->Has(CFS_QUOTA_US) && cg->Has(CFS_PERIOD_US);

        HasRtGroup = cg->Has(RT_RUNTIME_US) && cg->Has(RT_PERIOD_US);

        HasReserve = cg->Has(CFS_RESERVE_US);
        HasReserveRqs = cg->Has(CFS_RESERVE_RQS);
    }

    if (HasRtGroup)
        L_CG("support rt group");
    if (HasReserve)
        L_CG("support reserves");

    // idle
    HasIdle = cg->Has(IDLE);
    if (HasIdle)
        EnableIdle = config().container().enable_sched_idle();

    if (EnableIdle)
        L_CG("support idle cgroups");

    return OK;
}

TError TCpuSubsystem::InitializeCgroup(const TCgroup &cg) const {
    if (HasRtGroup)
        (void)cg.SetInt64(RT_RUNTIME_US, -1);
    return OK;
}

TError TCpuSubsystem::Statistics(const TCgroup &cg, TUintMap &stat) const {
    return cg.GetUintMap(STAT, stat);
}

inline std::string TCpuSubsystem::ChooseThrottledKnob() const {
    return IsCgroup2() ? THROTTLED_USEC : THROTTLED_TIME;
}

bool TCpuSubsystem::SupportThrottled() const {
    return RootCgroup()->Has(STAT);
}

TError TCpuSubsystem::GetThrottled(const TCgroup &cg, uint64_t &value) const {
    TError error;
    TUintMap stat;

    error = cg.GetUintMap(STAT, stat);
    if (error)
        return error;

    value = stat[ChooseThrottledKnob()];

    return OK;
}

bool TCpuSubsystem::SupportUnconstrainedWait() const {
    return RootCgroup()->Has(CgroupDriver.CpuacctSubsystem->WAIT) && RootCgroup()->Has(CFS_BURST_USAGE) &&
           RootCgroup()->Has(CFS_BURST_LOAD) && RootCgroup()->Has(CFS_THROTTLED);
}

TError TCpuSubsystem::GetUnconstrainedWait(const TCgroup &cg, uint64_t &value) const {
    TError error;
    TUintMap stat;
    int64_t wait, burstLoad, burstUsage, throttled;

    error = cg.GetInt64(CgroupDriver.CpuacctSubsystem->WAIT, wait);
    if (error)
        return error;

    error = cg.GetUintMap(STAT, stat);
    if (error)
        return error;

    burstLoad = static_cast<int64_t>(stat[BURST_LOAD]);
    burstUsage = static_cast<int64_t>(stat[BURST_USAGE]);
    throttled = static_cast<int64_t>(stat[H_THROTTLED_TIME]);

    value = static_cast<uint64_t>(std::max(wait - std::max(burstLoad - burstUsage, static_cast<int64_t>(0)) - throttled,
                                           static_cast<int64_t>(0)));

    return OK;
}

bool TCpuSubsystem::SupportBurstUsage() const {
    return RootCgroup()->Has(CFS_BURST_USAGE);
}

TError TCpuSubsystem::GetBurstUsage(const TCgroup &cg, uint64_t &value) const {
    return cg.GetUint64(CFS_BURST_USAGE, value);
}

uint64_t TCpuSubsystem::PreparePeriod(uint64_t period) {
    period = period / 1000; /* ns -> us */

    if (period < 1000) /* 1ms */
        period = 1000;

    if (period > 1000000) /* 1s */
        period = 1000000;

    return period;
}

uint64_t TCpuSubsystem::PrepareQuota(uint64_t quota, uint64_t period) {
    quota = std::ceil((double)quota * period / CPU_POWER_PER_SEC);
    quota *= config().container().cpu_limit_scale();

    if (0 < quota && quota < 1000) /* 1ms */
        quota = 1000;

    return quota;
}

TError TCpuSubsystem::SetQuotaAndPeriodV1(const TCgroup &cg, uint64_t quota, uint64_t period) const {
    std::string quota_s = quota > 0 ? std::to_string(quota) : "-1";

    // Setting quota/period in cgroup-v1 can be tricky because
    // we cannot set it in one action as we can in cgroup-v2:
    // Changing one paramter can violate parent constraint,
    // while changing another paramter can violate child constraint.
    // In this case we temporarily disable quota (by writing -1)
    // and set it again after setting period.
    bool need_quota_change = false;
    auto error = cg.Set(CFS_QUOTA_US, quota_s);
    if (error) {
        if (error.Errno != EINVAL || !quota)
            return error;

        // disable quota
        error = cg.Set(CFS_QUOTA_US, "-1");
        if (error)
            return error;
        need_quota_change = true;
    }

    error = cg.SetUint64(CFS_PERIOD_US, period);
    if (error)
        return error;

    if (need_quota_change) {
        error = cg.Set(CFS_QUOTA_US, quota_s);
        if (error)
            return error;
    }

    return OK;
}

TError TCpuSubsystem::SetQuotaAndPeriodV2(const TCgroup &cg, uint64_t quota, uint64_t period) const {
    if (!quota)
        return cg.Set(MAX, "max");

    return cg.Set(MAX, fmt::format("{} {}", quota, period));
}

/* quota == 0 -> -1 / max */
TError TCpuSubsystem::SetQuotaAndPeriod(const TCgroup &cg, uint64_t quota, uint64_t period) const {
    if (IsCgroup2())
        return SetQuotaAndPeriodV2(cg, quota, period);
    return SetQuotaAndPeriodV1(cg, quota, period);
}

TError TCpuSubsystem::SetLimit(const TCgroup &cg, uint64_t quota, uint64_t period) {
    if (!HasQuota)
        return OK;

    period = PreparePeriod(period);
    quota = PrepareQuota(quota, period);

    return SetQuotaAndPeriod(cg, quota, period);
}

TError TCpuSubsystem::SetGuarantee(const TCgroup &cg, uint64_t guarantee) {
    if (HasReserve) {
        auto error = cg.SetUint64(CFS_RESERVE_US, 0);
        if (error)
            return error;
    }

    if (HasReserveRqs) {
        auto quot = guarantee / CPU_POWER_PER_SEC;
        auto rem = guarantee % CPU_POWER_PER_SEC;
        auto error = cg.SetUint64(CFS_RESERVE_RQS, quot + !!rem);
        if (error)
            return error;
    }

    return OK;
}

TError TCpuSubsystem::SetShares(const TCgroup &cg, const std::string &policy, uint64_t weight, uint64_t guarantee) {
    uint64_t shares = BaseShares * guarantee * weight / (100 * CPU_POWER_PER_SEC);

    if (policy == "rt" || policy == "high" || policy == "iso")
        shares *= 16;
    else if (policy == "idle")
        shares = BaseShares / 16;

    shares = std::min(std::max(shares, MinShares), MaxShares);
    return cg.SetUint64(IsCgroup2() ? WEIGHT : SHARES, shares);
}

// SetCpuIdle is invoked inside HasIdle condition
TError TCpuSubsystem::SetCpuIdle(const TCgroup &cg, bool value) {
    if (!HasIdle)
        return OK;
    return cg.SetBool(IDLE, value);
}

TError TCpuSubsystem::SetRtLimit(const TCgroup &cg, uint64_t quota, uint64_t period) {
    if (!HasRtGroup || IsCgroup2())
        return OK;

    period = PreparePeriod(period);

    TError error;
    uint64_t max = GetNumCores() * CPU_POWER_PER_SEC;
    int64_t root_runtime, root_period, runtime;

    if (RootCgroup()->GetInt64(RT_PERIOD_US, root_period))
        root_period = 1000000;

    if (RootCgroup()->GetInt64(RT_RUNTIME_US, root_runtime))
        root_runtime = 950000;
    else if (root_runtime < 0)
        root_runtime = root_period;

    if (quota <= 0 || quota >= max || (double)quota / max * root_period > root_runtime) {
        runtime = -1;
    } else {
        runtime = (double)quota * period / max;
        if (runtime < 1000) /* 1ms */
            runtime = 1000;
    }

    error = cg.SetInt64(RT_PERIOD_US, period);
    if (error) {
        (void)cg.SetInt64(RT_RUNTIME_US, runtime);
        error = cg.SetInt64(RT_PERIOD_US, period);
    }
    if (!error)
        error = cg.SetInt64(RT_RUNTIME_US, runtime);
    if (error) {
        (void)cg.SetInt64(RT_RUNTIME_US, 0);
        return error;
    }

    return OK;
}

// Cpuacct

TError TCpuacctSubsystem::Usage(const TCgroup &cg, uint64_t &value) const {
    std::string s;
    TError error = cg.Get(USAGE, s);
    if (error)
        return error;
    return StringToUint64(s, value);
}

TError TCpuacctSubsystem::SystemUsage(const TCgroup &cg, uint64_t &value) const {
    TUintMap stat;
    TError error = cg.GetUintMap(STAT, stat);
    if (error)
        return error;
    value = stat["system"] * (1000000000 / sysconf(_SC_CLK_TCK));
    return OK;
}

TError TCpuacctSubsystem::GetWait(const TCgroup &cg, uint64_t &value) const {
    return cg.GetUint64(WAIT, value);
}

// Cpuset

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

TError TCpusetSubsystem::GetCpus(const TCgroup &cg, std::string &cpus) const {
    return cg.Get(IsCgroup2() ? CPUS_EFFECTIVE : CPUS, cpus);
}

TError TCpusetSubsystem::SetCpus(const TCgroup &cg, const std::string &cpus) const {
    TError error;
    TPath copy;
    std::string value;

    if (cpus.empty()) {
        auto parent = cg.Path().DirName() / (IsCgroup2() ? CPUS_EFFECTIVE : CPUS);
        error = parent.ReadAll(value);
        if (error)
            return error;

        value = StringTrim(value);
    } else
        value = cpus;

    return cg.Set(CPUS, value);
}

TError TCpusetSubsystem::SetMems(const TCgroup &cg, const std::string &mems) const {
    TError error;
    TPath copy;
    std::string value;

    if (mems == "") {
        if (IsCgroup2())
            copy = TPath("/sys/devices/system/node/online");
        else
            // cgroup v1 does not support overcommit, so dirty hack is used
            copy = cg.Path().DirName() / MEMS;
    }

    if (mems == "all")
        copy = TPath("/sys/devices/system/node/online");

    if (!copy.IsEmpty()) {
        error = copy.ReadAll(value);
        if (error)
            return error;

        value = StringTrim(value);

    } else
        value = mems;

    return cg.Set(MEMS, value);
}

TError TCpusetSubsystem::GetCpus(const TCgroup &cg, TBitMap &cpus) const {
    TError error;
    std::string value;

    error = GetCpus(cg, value);
    if (error)
        return error;

    return cpus.Parse(value);
}

TError TCpusetSubsystem::SetCpus(const TCgroup &cg, const TBitMap &cpus) const {
    return SetCpus(cg, cpus.Format());
}

// Netcls

TError TNetclsSubsystem::InitializeSubsystem() {
    HasPriority = config().network().enable_netcls_priority() && RootCgroup()->Has(PRIORITY);
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
        error = cg.GetUint64(PRIORITY, cur);
        if (error || cur != classid) {
            error = cg.SetUint64(PRIORITY, classid);
            if (error)
                return error;
        }
    }

    error = cg.GetUint64(CLASSID, cur);
    if (error || cur != classid) {
        error = cg.SetUint64(CLASSID, classid);
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

TError TBlkioSubsystem::InitializeSubsystem() {
    if (IsCgroup2())
        return OK;

    HasThrottler = RootCgroup()->Has("blkio.throttle.io_serviced");
    auto sched = HasThrottler ? "throttle." : "";
    auto recOpsKnob = fmt::format("blkio.{}io_serviced_recursive", sched);
    auto recur = RootCgroup()->Has(recOpsKnob) ? "_recursive" : "";

    BytesKnob = fmt::format("blkio.{}io_service_bytes{}", sched, recur);
    OpsKnob = fmt::format("blkio.{}io_serviced{}", sched, recur);
    TimeKnob = fmt::format("blkio.{}io_service_time{}", sched, recur);

    return OK;
}

void TBlkioSubsystem::ParseIoStatV1(const std::vector<std::string> &lines, enum IoStat stat, TUintMap &map) const {
    std::string prev, name;
    bool sum = false, hide = false;
    uint64_t total = 0;

    for (auto &line: lines) {
        auto words = SplitString(line, ' ');
        if (words.size() != 3)
            continue;

        if (words[1] == "Read") {
            if (!(stat & IoStat::Read))
                continue;
        } else if (words[1] == "Write") {
            if (!(stat & IoStat::Write))
                continue;
        } else
            continue;

        if (words[0] != prev) {
            if (DiskName(words[0], name))
                continue;

            prev = words[0];
            sum = StringStartsWith(name, "sd") || StringStartsWith(name, "nvme") || StringStartsWith(name, "vd");
            hide = StringStartsWith(name, "ram");
        }

        if (hide)
            continue;

        uint64_t value;
        if (!StringToUint64(words[2], value) && value) {
            map[name] += value;
            if (sum)
                total += value;
        }
    }
    map["hw"] = total;
}

void TBlkioSubsystem::ParseIoStatV2(const std::vector<std::string> &lines, enum IoStat stat, TUintMap &map) const {
    bool sum = false;
    uint64_t total = 0;

    for (auto &line: lines) {
        // separates device numbers and stats
        auto sep = line.find(' ');

        TUintMap statMap;
        if (StringToUintMap(line.substr(sep + 1), statMap, ' ', '=') || statMap.size() < 6)
            continue;

        std::string name;
        if (DiskName(line.substr(0, sep), name))
            continue;

        // hide
        if (StringStartsWith(name, "ram"))
            continue;

        sum = StringStartsWith(name, "sd") || StringStartsWith(name, "nvme") || StringStartsWith(name, "vd");

        std::string kind;
        if (stat & IoStat::Bytes)
            kind = "bytes";
        else if (stat & IoStat::Iops)
            kind = "ios";
        else
            return;

        std::string prefixes = fmt::format("{}{}", stat & IoStat::Read ? "r" : "", stat & IoStat::Write ? "w" : "");
        for (char prefix: prefixes) {
            auto value = statMap[prefix + kind];
            map[name] += value;
            if (sum)
                total += value;
        }
    }
    map["hw"] = total;
}

TError TBlkioSubsystem::GetIoStat(const TCgroup &cg, enum IoStat stat, TUintMap &map) const {
    TError error;
    std::string knob;
    std::vector<std::string> lines;

    if (IsCgroup2() && (stat & IoStat::Time))
        return OK;

    if (IsCgroup2())
        knob = STAT;
    else if (stat & IoStat::Bytes)
        knob = BytesKnob;
    else if (stat & IoStat::Iops)
        knob = OpsKnob;
    else if (stat & IoStat::Time)
        knob = TimeKnob;
    else
        return TError(EError::InvalidValue, "Cannot get io stat of {}, invalid stat: {}", cg, static_cast<int>(stat));

    error = cg.Knob(knob).ReadLines(lines);
    if (error)
        return error;

    if (IsCgroup2())
        ParseIoStatV2(lines, stat, map);
    else
        ParseIoStatV1(lines, stat, map);

    return OK;
}

TError TBlkioSubsystem::SetIoLimitV1(const TCgroup &cg, const TPath &root, const TUintMap &map, bool iops) {
    std::string knob[2] = {
        iops ? READ_IOPS_DEVICE : READ_BPS_DEVICE,
        iops ? WRITE_IOPS_DEVICE : WRITE_BPS_DEVICE,
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
            if (sep != key.size() - 2 || (key[sep + 1] != 'r' && key[sep + 1] != 'w'))
                return TError(EError::InvalidValue, "Invalid io limit key: " + key);
            dir = key[sep + 1] == 'r' ? 0 : 1;
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

TError TBlkioSubsystem::SetIoLimitV2(const TCgroup &cg, const TPath &root, const TUintMap &map, bool iops) {
    TError error, result;
    TUintPairMap plan;  // <dev>: (<read limit>, <write limit>)
    std::string disk, kind;
    std::vector<std::string> lines;
    int dir;

    error = cg.Knob(MAX).ReadLines(lines);
    if (error)
        return error;

    for (auto &line: lines) {
        auto sep = line.find(' ');
        if (sep != std::string::npos)
            plan[line.substr(0, sep)] = {0, 0};  // 0 => max
    }

    for (auto &it: map) {
        auto key = it.first;
        auto sep = key.rfind(' ');

        dir = 2;
        if (sep != std::string::npos) {
            if (sep != key.size() - 2 || (key[sep + 1] != 'r' && key[sep + 1] != 'w'))
                return TError(EError::InvalidValue, "Invalid io limit key: " + key);
            dir = key[sep + 1] == 'r' ? 0 : 1;
            key = key.substr(0, sep);
        }

        if (key == "fs")
            continue;

        error = ResolveDisk(root, key, disk);
        if (error)
            return error;

        if (dir == 0 || dir == 2)
            plan[disk].first = it.second;
        if (dir == 1 || dir == 2)
            plan[disk].second = it.second;
    }

    if (iops)
        kind = "iops";
    else
        kind = "bps";

    for (auto &it: plan) {
        auto rlimit = it.second.first ? std::to_string(it.second.first) : "max";
        auto wlimit = it.second.second ? std::to_string(it.second.second) : "max";

        error = cg.Set(MAX, fmt::format("{} r{}={} w{}={}", it.first, kind, rlimit, kind, wlimit));
        if (error && !result)
            result = error;
    }

    return result;
}

TError TBlkioSubsystem::SetIoLimit(const TCgroup &cg, const TPath &root, const TUintMap &map, bool iops) {
    if (IsCgroup2())
        return SetIoLimitV2(cg, root, map, iops);
    else
        return SetIoLimitV1(cg, root, map, iops);
}

TError TBlkioSubsystem::SetIoWeight(const TCgroup &cg, const std::string &policy, double weight) const {
    TError error;
    double bfqWeight = weight;
    const std::string weightKnob = IsCgroup2() ? WEIGHT : LEGACY_WEIGHT;
    const std::string bfqKnob = IsCgroup2() ? BFQ_WEIGHT : LEGACY_BFQ_WEIGHT;

    /*
     * Cgroup v1 mess:
     * CFQ: 10..1000 default 500
     * BFQ: 1..1000 default 100
     */

    if (policy == "rt" || policy == "high") {
        weight *= 1000;
        bfqWeight *= 1000;
    } else if (policy == "" || policy == "none" || policy == "normal") {
        weight *= 500;
        bfqWeight *= 100;
    } else if (policy == "batch" || policy == "idle") {
        weight *= 10;
        bfqWeight *= 1;
    } else
        return TError(EError::InvalidValue, "unknown policy: " + policy);

    if (cg.Has(weightKnob)) {
        error = cg.SetUint64(weightKnob, std::min(std::max(weight, 10.), 1000.));
        if (error)
            return error;
    }

    if (cg.Has(bfqKnob)) {
        error = cg.SetUint64(bfqKnob, std::min(std::max(bfqWeight, 1.), 1000.));
        if (error)
            return error;
    }

    return OK;
}

// Devices

TError TDevicesSubsystem::SetAllow(const TCgroup &cg, const std::string &value) const {
    return cg.Set(ALLOW, value);
}

TError TDevicesSubsystem::SetDeny(const TCgroup &cg, const std::string &value) const {
    return cg.Set(DENY, value);
}

TError TDevicesSubsystem::GetList(const TCgroup &cg, std::vector<std::string> &lines) const {
    return cg.Knob(LIST).ReadLines(lines);
}
bool TDevicesSubsystem::Unbound(const TCgroup &cg) const {
    std::string s;
    return !cg.Knob(LIST).ReadAll(s) && StringStartsWith(s, "a");
}

// Pids

TError TPidsSubsystem::GetUsage(const TCgroup &cg, uint64_t &usage) const {
    return cg.GetUint64(CURRENT, usage);
}

TError TPidsSubsystem::SetLimit(const TCgroup &cg, uint64_t limit) const {
    if (!limit)
        return cg.Set(MAX, "max");
    return cg.SetUint64(MAX, limit);
}

// Systemd

TError TSystemdSubsystem::InitializeSubsystem() {
    TError error = TaskCgroup(getpid(), PortoService, false);
    if (!error)
        L_CG("porto service: {}", *PortoService);
    return error;
}

// Cgroup Driver

TError TCgroupDriver::InitializeCgroups() {
    std::string cgroup_root = "/sys/fs/cgroup";
    if (config().daemon().has_cgroupfs_root_path()) {
        cgroup_root = config().daemon().cgroupfs_root_path();
    }
    L_DBG("Initializing cgroup driver with cgroupfs root: {}", cgroup_root);
    TPath root(cgroup_root);
    std::vector<TMount> mounts;
    TMount mount;
    TError error;

    error = root.FindMount(mount);
    if (error) {
        L_ERR("Cannot find cgroups root mount: {}", error);
        return error;
    }

    if (mount.Type == "cgroup2") {
        Cgroup2Hierarchy = true;
        L_CG("Using cgroup v2 hierarchy due to {}", mount);
    }

    if (mount.Target != root) {
        if (Cgroup2Hierarchy)
            error = root.Mount("cgroup2", "cgroup2", MS_NOEXEC | MS_NOSUID | MS_NODEV | MS_RELATIME,
                               {"nsdelegate", "memory_recursiveprot"});
        else
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
            if (mnt.Target.IsInside(root) && mnt.Type == subsys->MountType && mnt.HasOption(subsys->TestOption())) {
                subsys->Root = mnt.Target;
                L_CG("Found cgroup subsystem {} mounted at {}", subsys->Type, subsys->Root);
                break;
            }
        }
    }

    if (!Cgroup2Hierarchy && config().daemon().merge_memory_blkio_controllers() && !MemorySubsystem->IsDisabled() &&
        !BlkioSubsystem->IsDisabled() && MemorySubsystem->Root.IsEmpty() && BlkioSubsystem->Root.IsEmpty()) {
        TPath path = root / "memory,blkio";

        if (!path.Exists())
            (void)path.Mkdir(0755);
        error = path.Mount("cgroup", "cgroup", 0, {"memory", "blkio"});
        if (!error) {
            (root / "memory").Symlink("memory,blkio");
            (root / "blkio").Symlink("memory,blkio");
            MemorySubsystem->Root = path;
            BlkioSubsystem->Root = path;
        } else {
            L_CG("Cannot merge memory and blkio {}", error);
            (void)path.Rmdir();
        }
    }

    if (Cgroup2Hierarchy) {
        for (auto subsys: Cgroup2Subsystems) {
            if (subsys->IsDisabled() || subsys->Root)
                continue;

            subsys->MountType = "cgroup2";
            subsys->Root = root;
            subsys->Supported = true;
            Cgroup2Subsystem->Controllers |= subsys->Kind;
            L_CG("Mount cgroup2 subsysem {} at {}", subsys->Type, root);
        }
    }

    for (auto subsys: AllSubsystems) {
        if (subsys->IsDisabled() || subsys->Root)
            continue;

        TPath path = root / subsys->Type;

        L_CG("Mount cgroup subsystem {} at {}", subsys->Type, path);
        if (!path.Exists()) {
            error = path.Mkdir(0755);
            if (error) {
                L_ERR("Cannot create cgroup mountpoint: {}", error);
                continue;
            }
        }

        error = path.Mount(subsys->MountType, subsys->MountType, 0, subsys->MountOptions());
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

    if (Cgroup2Hierarchy)
        Hierarchies.push_back(Cgroup2Subsystem.get());

    for (auto subsys: AllSubsystems) {
        if (subsys->IsDisabled()) {
            L_CG("Cgroup subsystem {} is disabled", subsys->Type);
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

    {
        auto cg = FreezerSubsystem->Cgroup(PORTO_CGROUP_PREFIX, false);
        if (!cg->Exists()) {
            error = CreateCgroup(*cg);
            if (error)
                return error;
        }
    }

    if (Cgroup2Subsystem->Supported) {
        auto cg = Cgroup2Subsystem->Cgroup(PORTO_CGROUP_PREFIX, false);
        if (!cg->Exists()) {
            error = CreateCgroup(*cg);
            if (error)
                return error;
        }
    }

    return error;
}

TError TCgroupDriver::InitializeDaemonCgroups() {
    std::vector<const TSubsystem *> DaemonSubsystems = {FreezerSubsystem.get(), MemorySubsystem.get(),
                                                        CpuacctSubsystem.get(), PerfSubsystem.get()};

    if (Cgroup2Subsystem->Supported)
        DaemonSubsystems.push_back(Cgroup2Subsystem.get());

    for (auto subsys: DaemonSubsystems) {
        auto hy = subsys->Hierarchy;
        TError error;

        if (!hy)
            continue;

        auto cg = hy->Cgroup(PORTO_DAEMON_CGROUP, false);
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

    auto cg = MemorySubsystem->Cgroup(PORTO_DAEMON_CGROUP, false);
    TError error = MemorySubsystem->SetLimit(*cg, config().daemon().memory_limit());
    if (error)
        return error;

    cg = MemorySubsystem->Cgroup(PORTO_HELPERS_CGROUP, false);
    error = RecreateCgroup(*cg);
    if (error)
        return error;

    error = MemorySubsystem->SetLimit(*cg, config().daemon().helpers_memory_limit());
    if (error)
        return error;

    error = MemorySubsystem->SetDirtyLimit(*cg, config().daemon().helpers_dirty_limit());
    if (error)
        L_ERR("Cannot set portod-helpers dirty limit: {}", error);

    return OK;
}

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

    Cgroup2Subsystems.push_back(MemorySubsystem.get());
    Cgroup2Subsystems.push_back(CpuSubsystem.get());
    Cgroup2Subsystems.push_back(CpusetSubsystem.get());
    Cgroup2Subsystems.push_back(BlkioSubsystem.get());
    Cgroup2Subsystems.push_back(PidsSubsystem.get());
}

bool TCgroupDriver::UseCgroup2() const {
    return Cgroup2Hierarchy;
}

bool TCgroupDriver::IsInitialized() const {
    return Initialized;
}

TError TCgroupDriver::Initialize() {
    TError error;

    error = CgroupDriver.InitializeCgroups();
    if (error)
        return TError("Cannot initialize cgroups: {}", error);

    error = CgroupDriver.InitializeDaemonCgroups();
    if (error)
        return TError("Cannot initialize daemon cgroups: {}", error);

    Initialized = true;

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

        error = CgroupSubtree(*(hy->RootCgroup()), cgroups);
        if (error)
            L_ERR("Cannot dump porto {} cgroups : {}", hy->Type, error);

        for (auto it = cgroups.rbegin(); it != cgroups.rend(); ++it) {
            auto &cg = *it;
            if (!StringStartsWith(cg->GetName(), PORTO_CGROUP_PREFIX))
                continue;

            if (cg->GetName() == PORTO_DAEMON_CGROUP &&
                (hy->Controllers & (CGROUP_FREEZER | CGROUP_MEMORY | CGROUP_CPUACCT | CGROUP2 | CGROUP_PERF)))
                continue;

            if (cg->GetName() == PORTO_HELPERS_CGROUP && (hy->Controllers & CGROUP_MEMORY))
                continue;

            if (cg->GetName() == PORTO_CGROUP_PREFIX && (hy->Controllers & (CGROUP_FREEZER | CGROUP2)))
                continue;

            bool found = false;
            for (auto &it: Containers) {
                auto ct = it.second;
                auto ctCg = GetContainerCgroup(*(it.second), hy);
                // child cgroups should not be removed in case of cgroupfs=rw
                if (ct->State != EContainerState::Stopped &&
                    (*ctCg == *cg ||
                     (ct->CgroupFs == ECgroupFs::Rw && StringStartsWith(cg->GetName(), ctCg->GetName() + "/"))))
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

std::unique_ptr<const TCgroup> TCgroupDriver::GetContainerCgroup(const TContainer &container,
                                                                 TSubsystem *subsystem) const {
    if (container.IsRoot())
        return subsystem->RootCgroup();

    if (subsystem->Controllers & (CGROUP_FREEZER | CGROUP2)) {
        if (container.JobMode)
            return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "/" + container.Parent->Name,
                                     container.ChildrenAllowed);
        return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "/" + container.Name, container.ChildrenAllowed);
    }

    if (subsystem->Controllers & CGROUP_SYSTEMD) {
        if (container.Controllers & CGROUP_SYSTEMD)
            return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "%" +
                                         StringReplaceAll(container.Name, "/", "%"), container.ChildrenAllowed);

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

    return subsystem->Cgroup(std::string(PORTO_CGROUP_PREFIX) + "%" + cg, container.ChildrenAllowed);
}

std::list<std::unique_ptr<const TCgroup>> TCgroupDriver::GetContainerCgroups(const TContainer &container) const {
    std::list<std::unique_ptr<const TCgroup>> cgroups;
    for (auto hy: Hierarchies)
        cgroups.push_back(GetContainerCgroup(container, hy));
    return cgroups;
}

std::unique_ptr<const TCgroup> TCgroupDriver::GetContainerCgroupByKnob(const TContainer &container,
                                                                       const std::string &knob) const {
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

TError TCgroupDriver::GetContainerCgroupsKnob(const TContainer &container, const std::string &knob,
                                              std::string &value) const {
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

    error = CgroupSubtree(cg, children, true);
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
    auto tmpcg = cg.GetSubsystem()->Cgroup(cg.GetName() + "_tmp", cg.HasLeaf);
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
            L_WRN("There is no {} cgroup for {}", TSubsystem::Format(hy->Controllers), container.Slug);
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

TError TCgroupDriver::CgroupSubtree(const TCgroup &cg, std::list<std::unique_ptr<const TCgroup>> &cgroups,
                                    bool all) const {
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

        /* Ingore leaf cgroups for cgroup2 */
        if (StringEndsWith(name, LEAF) && cg.IsCgroup2())
            continue;

        cgroups.push_back(cg.GetSubsystem()->Cgroup(name, cg.HasLeaf));
    }

    return OK;
}

TError TCgroupDriver::GetCgroupCount(const TCgroup &cgroup, uint64_t &count, bool thread) const {
    TError error;
    std::list<std::unique_ptr<const TCgroup>> cgroups;

    error = CgroupSubtree(cgroup, cgroups);
    if (error)
        return error;

    cgroups.push_back(cgroup.GetUniquePtr());

    count = 0;
    uint64_t buf;
    for (auto &cg: cgroups) {
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
