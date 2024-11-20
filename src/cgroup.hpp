#pragma once

#include <memory>
#include <string>
#include <sys/types.h>

#include "config.hpp"
#include "util/path.hpp"

class TContainer;
struct TDevice;
class TSubsystem;

#define CGROUP_FREEZER  0x00001ull
#define CGROUP_MEMORY   0x00002ull
#define CGROUP_CPU      0x00004ull
#define CGROUP_CPUACCT  0x00008ull
#define CGROUP_NETCLS   0x00010ull
#define CGROUP_BLKIO    0x00020ull
#define CGROUP_DEVICES  0x00040ull
#define CGROUP_HUGETLB  0x00080ull
#define CGROUP_CPUSET   0x00100ull
#define CGROUP_PIDS     0x00200ull
#define CGROUP_PERF     0x00400ull
#define CGROUP_SYSTEMD  0x01000ull
#define CGROUP2         0x10000ull

extern const TFlagsNames ControllersName;

class TCgroup {
    const TSubsystem *Subsystem = nullptr;
    std::string Name;

public:
    // constructors and destructor
    TCgroup() = delete;
    TCgroup(const TSubsystem *subsystem, const std::string &name): Subsystem(subsystem), Name(name) {}
    TCgroup(const TSubsystem *subsystem, std::string &&name) : Subsystem(subsystem), Name(name) {}
    virtual ~TCgroup() = default;

    // operators
    friend std::ostream& operator<<(std::ostream& os, const TCgroup &cg) {
        return os << cg.Type() << ":" << cg.GetName();
    }

    friend bool operator==(const TCgroup &lhs, const TCgroup &rhs) {
        return lhs.Path() == rhs.Path();
    }

    friend bool operator!=(const TCgroup &lhs, const TCgroup &rhs) {
        return lhs.Path() != rhs.Path();
    }

    // status
    bool HasSubsystem() const;
    bool Exists() const;
    bool IsEmpty() const;
    bool IsRoot() const;
    bool IsCgroup2() const;
    bool IsSecondary() const;
    bool IsSubsystem(uint64_t kind) const;

    // getters
    std::string Type() const;
    TPath Path() const;
    std::string GetName() const;
    const TSubsystem *GetSubsystem() const;
    std::unique_ptr<const TCgroup> GetUniquePtr() const;

    // modifiers
    virtual TError Create() const = 0;
    virtual TError Remove() const = 0;

    // processes
    virtual TError Attach(pid_t pid, bool thread = false) const = 0;
    virtual TError AttachAll(const TCgroup &cg, bool thread = false) const = 0;
    virtual TError KillAll(int signal, bool abortFuse = false) const = 0;

    virtual TError SetPids(const std::string &knobPath, const std::vector<pid_t> &pids) const;
    virtual TError GetPids(const std::string &knobPath, std::vector<pid_t> &pids) const;
    virtual TError GetProcesses(std::vector<pid_t> &pids) const;
    virtual TError GetTasks(std::vector<pid_t> &tids) const = 0;
    virtual TError GetCount(uint64_t &count, bool thread = false) const = 0;

    // knobs
    TPath Knob(const std::string &knob) const;
    bool Has(const std::string &knob) const;

    TError Get(const std::string &knob, std::string &value) const;
    TError Set(const std::string &knob, const std::string &value) const;

    TError GetInt64(const std::string &knob, int64_t &value) const;
    TError SetInt64(const std::string &knob, int64_t value) const;

    TError GetUint64(const std::string &knob, uint64_t &value) const;
    TError SetUint64(const std::string &knob, uint64_t value) const;

    TError GetBool(const std::string &knob, bool &value) const;
    TError SetBool(const std::string &knob, bool value) const;

    TError GetUintMap(const std::string &knob, TUintMap &value) const;
};

class TSubsystem {
public:
    const uint64_t Kind = 0x0ull;
    uint64_t Controllers = 0x0ull;
    const std::string Type;
    std::string MountType;
    TSubsystem *Hierarchy = nullptr;
    TPath Root;
    TFile Base;
    bool Supported = false;

    TSubsystem(uint64_t kind, const std::string &type)
        : Kind(kind)
        , Type(type)
        , MountType(Kind == CGROUP2 ? "cgroup2" : "cgroup")
    { }

    friend bool operator==(const TSubsystem &lhs, const TSubsystem &rhs) {
        return lhs.Kind == rhs.Kind;
    }

    virtual ~TSubsystem() = default;

    virtual bool IsDisabled() const { return false; }
    virtual bool IsCgroup2() const { return MountType == "cgroup2"; }
    virtual bool IsOptional() const { return false; }
    virtual std::string TestOption() const { return Type; }
    virtual std::vector<std::string> MountOptions() const { return {Type}; }

    virtual TError InitializeSubsystem() {
        return OK;
    }

    virtual TError InitializeCgroup(const TCgroup &cg) const {
        (void)cg;
        return OK;
    }

    std::unique_ptr<const TCgroup> RootCgroup() const;
    std::unique_ptr<const TCgroup> Cgroup(const std::string &name) const;

    TError TaskCgroup(pid_t pid, std::unique_ptr<const TCgroup> &cg) const;
    bool IsBound(const TCgroup &cg) const;

    static std::string Format(uint64_t controllers) {
        return StringFormatFlags(controllers, ControllersName, ";");
    }
};

class TMemorySubsystem : public TSubsystem {
public:
    const std::string STAT = "memory.stat";
    const std::string OOM_CONTROL = "memory.oom_control";
    const std::string EVENT_CONTROL = "cgroup.event_control";
    const std::string USE_HIERARCHY = "memory.use_hierarchy";
    const std::string RECHARGE_ON_PAGE_FAULT = "memory.recharge_on_pgfault";
    const std::string USAGE = "memory.usage_in_bytes";
    const std::string LIMIT = "memory.limit_in_bytes";
    const std::string SOFT_LIMIT = "memory.soft_limit_in_bytes";
    const std::string LOW_LIMIT = "memory.low_limit_in_bytes";
    const std::string HIGH_LIMIT = "memory.high_limit_in_bytes";
    const std::string MEM_SWAP_LIMIT = "memory.memsw.limit_in_bytes";
    const std::string DIRTY_LIMIT = "memory.dirty_limit_in_bytes";
    const std::string DIRTY_RATIO = "memory.dirty_ratio";
    const std::string FS_BPS_LIMIT = "memory.fs_bps_limit";
    const std::string FS_IOPS_LIMIT = "memory.fs_iops_limit";
    const std::string ANON_USAGE = "memory.anon.usage";
    const std::string ANON_MAX_USAGE = "memory.anon.max_usage";
    const std::string ANON_LIMIT = "memory.anon.limit";
    const std::string ANON_ONLY = "memory.anon.only";
    const std::string NUMA_BALANCE_VMPROT = "memory.numa_balance_vmprot";
    const std::string WRITEBACK_BLKIO = "memory.writeback_blkio";
    const std::string MEMORY_LOCK_POLICY = "memory.mlock_policy";

    bool HasWritebackBlkio = false;
    bool HasMemoryLockPolicy = false;

    TMemorySubsystem() : TSubsystem(CGROUP_MEMORY, "memory") {}

    TError InitializeSubsystem() override;

    TError Statistics(const TCgroup &cg, TUintMap &stat) const {
        return cg.GetUintMap(STAT, stat);
    }

    TError Usage(const TCgroup &cg, uint64_t &value) const {
        return cg.GetUint64(USAGE, value);
    }

    TError GetSoftLimit(const TCgroup &cg, int64_t &limit) const {
        return cg.GetInt64(SOFT_LIMIT, limit);
    }

    TError SetSoftLimit(const TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(SOFT_LIMIT, limit);
    }

    bool SupportGuarantee() const {
        return RootCgroup()->Has(LOW_LIMIT);
    }

    TError SetGuarantee(const TCgroup &cg, uint64_t guarantee) const {
        if (!SupportGuarantee())
            return OK;
        return cg.SetUint64(LOW_LIMIT, guarantee);
    }

    bool SupportHighLimit() const {
        return RootCgroup()->Has(HIGH_LIMIT);
    }

    TError SetHighLimit(const TCgroup &cg, uint64_t limit) const {
        if (!SupportHighLimit())
            return OK;
        if (!limit)
            return cg.Set(HIGH_LIMIT, "-1");
        return cg.SetUint64(HIGH_LIMIT, limit);
    }

    bool SupportIoLimit() const {
        return RootCgroup()->Has(FS_BPS_LIMIT);
    }

    bool SupportDirtyLimit() const {
        return RootCgroup()->Has(DIRTY_LIMIT);
    }

    bool SupportSwap() const {
        return RootCgroup()->Has(MEM_SWAP_LIMIT);
    }

    bool SupportRechargeOnPgfault() const {
        return RootCgroup()->Has(RECHARGE_ON_PAGE_FAULT);
    }

    TError RechargeOnPgfault(const TCgroup &cg, bool enable) const {
        if (!SupportRechargeOnPgfault())
            return OK;
        return cg.SetBool(RECHARGE_ON_PAGE_FAULT, enable);
    }

    bool SupportNumaBalance() const {
        return RootCgroup()->Has(NUMA_BALANCE_VMPROT);
    }
    TError SetNumaBalance(const TCgroup &cg, uint64_t flag, uint64_t mask) {
        return cg.Set(NUMA_BALANCE_VMPROT, fmt::format("{} {}", flag, mask));
    }

    TError GetCacheUsage(const TCgroup &cg, uint64_t &usage) const;
    TError GetShmemUsage(const TCgroup &cg, uint64_t &usage) const;
    TError GetMLockUsage(const TCgroup &cg, uint64_t &usage) const;
    TError GetAnonUsage(const TCgroup &cg, uint64_t &usage) const;

    TError GetAnonMaxUsage(const TCgroup &cg, uint64_t &usage) const {
        return cg.GetUint64(ANON_MAX_USAGE, usage);
    }
    TError ResetAnonMaxUsage(const TCgroup &cg) const {
        return cg.SetUint64(ANON_MAX_USAGE, 0);
    }

    bool SupportAnonLimit() const;
    TError SetAnonLimit(const TCgroup &cg, uint64_t limit) const;

    bool SupportAnonOnly() const;
    TError SetAnonOnly(const TCgroup &cg, bool val) const;

    TError LinkWritebackBlkio(const TCgroup &memcg, const TCgroup &blkcg) const;

    TError SetLimit(const TCgroup &cg, uint64_t limit);
    TError SetIoLimit(const TCgroup &cg, uint64_t limit);
    TError SetIopsLimit(const TCgroup &cg, uint64_t limit);
    TError SetDirtyLimit(const TCgroup &cg, uint64_t limit);
    TError SetupOOMEvent(const TCgroup &cg, TFile &event);
    uint64_t GetOomEvents(const TCgroup &cg);
    TError GetOomKills(const TCgroup &cg, uint64_t &count);
    TError GetReclaimed(const TCgroup &cg, uint64_t &count) const;
    TError SetUseHierarchy(const TCgroup &cg, bool value) const;
};

class TFreezerSubsystem : public TSubsystem {
public:
    TFreezerSubsystem() : TSubsystem(CGROUP_FREEZER, "freezer") {}

    TError WaitState(const TCgroup &cg, const std::string &state) const;
    TError Freeze(const TCgroup &cg, bool wait = true) const;
    TError Thaw(const TCgroup &cg, bool wait = true) const;
    bool IsFrozen(const TCgroup &cg) const;
    bool IsSelfFreezing(const TCgroup &cg) const;
    bool IsParentFreezing(const TCgroup &cg) const;
};

class TCpuSubsystem : public TSubsystem {
public:
    static constexpr const char *CPU_IDLE = "cpu.idle";
    static constexpr const char *CPU_SHARES = "cpu.shares";
    static constexpr const char *CPU_CFS_QUOTA_US = "cpu.cfs_quota_us";
    static constexpr const char *CPU_CFS_PERIOD_US = "cpu.cfs_period_us";
    static constexpr const char *CPU_CFS_RESERVE_US = "cpu.cfs_reserve_us";
    static constexpr const char *CPU_CFS_RESERVE_SHARES = "cpu.cfs_reserve_shares";
    static constexpr const char *CPU_RT_RUNTIME_US = "cpu.rt_runtime_us";
    static constexpr const char *CPU_RT_PERIOD_US = "cpu.rt_period_us";

    bool HasShares = false;
    bool HasQuota = false;
    bool HasReserve = false;
    bool HasRtGroup = false;
    bool HasIdle = false;
    bool EnableIdle = false;

    uint64_t BaseShares = 0ull;
    uint64_t MinShares = 0ull;
    uint64_t MaxShares = 0ull;

    TCpuSubsystem() : TSubsystem(CGROUP_CPU, "cpu") { }
    TError InitializeSubsystem() override;
    TError InitializeCgroup(const TCgroup &cg) const override;
    TError SetPeriod(const TCgroup &cg, uint64_t period);
    TError SetLimit(const TCgroup &cg, uint64_t period, uint64_t limit);
    TError SetRtLimit(const TCgroup &cg, uint64_t period, uint64_t limit);
    TError SetGuarantee(const TCgroup &cg, uint64_t period, uint64_t guarantee);
    TError SetShares(const TCgroup &cg, const std::string &policy, double weight, uint64_t guarantee);
    TError SetCpuIdle(const TCgroup &cg, bool value);
};

class TCpuacctSubsystem : public TSubsystem {
public:
    TCpuacctSubsystem() : TSubsystem(CGROUP_CPUACCT, "cpuacct") {}
    TError Usage(const TCgroup &cg, uint64_t &value) const;
    TError SystemUsage(const TCgroup &cg, uint64_t &value) const;
};

class TCpusetSubsystem : public TSubsystem {
private:
    TError GetCpus(const TCgroup &cg, std::string &cpus) const {
        return cg.Get("cpuset.cpus", cpus);
    }
    TError SetCpus(const TCgroup &cg, const std::string &cpus) const;
    TError SetMems(const TCgroup &cg, const std::string &mems) const;
public:
    TCpusetSubsystem() : TSubsystem(CGROUP_CPUSET, "cpuset") {}
    bool IsOptional() const override { return true; }
    TError InitializeCgroup(const TCgroup &cg) const override;

    TError GetCpus(const TCgroup &cg, TBitMap &cpus) const;
    TError SetCpus(const TCgroup &cg, const TBitMap &cpus) const;
};

class TNetclsSubsystem : public TSubsystem {
public:
    bool HasPriority;
    TNetclsSubsystem() : TSubsystem(CGROUP_NETCLS, "net_cls") {}
    bool IsOptional() const override { return !config().network().enable_netcls_classid(); }
    TError InitializeSubsystem() override;
    TError SetClass(const TCgroup &cg, uint32_t classid) const;
};

class TBlkioSubsystem : public TSubsystem {
public:
    const std::string CFQ_WEIGHT = "blkio.weight";
    const std::string BFQ_WEIGHT = "blkio.bfq.weight";

    bool HasThrottler = false;
    std::string TimeKnob;
    std::string OpsKnob;
    std::string BytesKnob;

    TBlkioSubsystem() : TSubsystem(CGROUP_BLKIO, "blkio") {}
    bool IsDisabled() const override { return !config().container().enable_blkio(); }
    bool IsOptional() const override { return true; }
    TError InitializeSubsystem() override;

    enum IoStat {
        Read = 1,
        Write = 2,
        Iops = 4,
        ReadIops = Read | Iops,
        WriteIops = Write | Iops,
        Time = 8,
    };
    TError GetIoStat(const TCgroup &cg, enum IoStat stat, TUintMap &map) const;
    TError SetIoWeight(const TCgroup &cg, const std::string &policy, double weight) const;
    TError SetIoLimit(const TCgroup &cg, const TPath &root, const TUintMap &map, bool iops = false);

    TError DiskName(const std::string &disk, std::string &name) const;
    TError ResolveDisk(const TPath &root, const std::string &key, std::string &disk) const;
};

class TDevicesSubsystem : public TSubsystem {
    const char *DEVICES_DENY = "devices.deny";
    const char *DEVICES_ALLOW = "devices.allow";
public:
    TDevicesSubsystem() : TSubsystem(CGROUP_DEVICES, "devices") {}

    TError SetAllow(const TCgroup &cg, const std::string &value) const;
    TError SetDeny(const TCgroup &cg, const std::string &value) const;
};

class THugetlbSubsystem : public TSubsystem {
public:
    const std::string HUGE_USAGE = "hugetlb.2MB.usage_in_bytes";
    const std::string HUGE_LIMIT = "hugetlb.2MB.limit_in_bytes";
    const std::string GIGA_USAGE = "hugetlb.1GB.usage_in_bytes";
    const std::string GIGA_LIMIT = "hugetlb.1GB.limit_in_bytes";
    THugetlbSubsystem() : TSubsystem(CGROUP_HUGETLB, "hugetlb") {}
    bool IsDisabled() const override { return !config().container().enable_hugetlb(); }
    bool IsOptional() const override { return true; }

    /* for now supports only 2MB pages */
    TError InitializeSubsystem() override {
        if (!RootCgroup()->Has(HUGE_LIMIT))
            return TError(EError::NotSupported, "No {}", HUGE_LIMIT);
        return OK;
    }

    TError GetHugeUsage(const TCgroup &cg, uint64_t &usage) const {
        return cg.GetUint64(HUGE_USAGE, usage);
    }

    TError SetHugeLimit(const TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(HUGE_LIMIT, limit);
    }

    bool SupportGigaPages() const {
        return RootCgroup()->Has(GIGA_LIMIT);
    }

    TError SetGigaLimit(const TCgroup &cg, int64_t limit) const {
        return cg.SetInt64(GIGA_LIMIT, limit);
    }
};

class TPidsSubsystem : public TSubsystem {
public:
    TPidsSubsystem() : TSubsystem(CGROUP_PIDS, "pids") {}
    bool IsOptional() const override { return true; }
    TError GetUsage(const TCgroup &cg, uint64_t &usage) const;
    TError SetLimit(const TCgroup &cg, uint64_t limit) const;
};

class TPerfSubsystem : public TSubsystem {
public:
    TPerfSubsystem() : TSubsystem(CGROUP_PERF, "perf_event") {}
};

class TSystemdSubsystem : public TSubsystem {
public:
    std::unique_ptr<const TCgroup> PortoService;
    TSystemdSubsystem() : TSubsystem(CGROUP_SYSTEMD, "systemd") {}
    TError InitializeSubsystem() override;
    bool IsDisabled() const override { return !config().container().enable_systemd(); }
    bool IsOptional() const override { return true; }
    std::string TestOption() const override { return "name=" + Type; }
    std::vector<std::string> MountOptions() const override { return { "none", "name=" + Type }; }
};

class TCgroup2Subsystem : public TSubsystem {
public:
    TCgroup2Subsystem() : TSubsystem(CGROUP2, "unified") { }
    bool IsDisabled() const override { return !config().container().enable_cgroup2(); }
    bool IsOptional() const override { return true; }
    std::string TestOption() const override { return ""; }
    std::vector<std::string> MountOptions() const override { return {}; }
};

class TCgroupDriver: public TNonCopyable {
public:
    // constructors and destructor
    TCgroupDriver();
    ~TCgroupDriver() = default;

    std::unique_ptr<TMemorySubsystem>     MemorySubsystem;
    std::unique_ptr<TFreezerSubsystem>    FreezerSubsystem;
    std::unique_ptr<TCpuSubsystem>        CpuSubsystem;
    std::unique_ptr<TCpuacctSubsystem>    CpuacctSubsystem;
    std::unique_ptr<TCpusetSubsystem>     CpusetSubsystem;
    std::unique_ptr<TNetclsSubsystem>     NetclsSubsystem;
    std::unique_ptr<TBlkioSubsystem>      BlkioSubsystem;
    std::unique_ptr<TDevicesSubsystem>    DevicesSubsystem;
    std::unique_ptr<THugetlbSubsystem>    HugetlbSubsystem;
    std::unique_ptr<TPidsSubsystem>       PidsSubsystem;
    std::unique_ptr<TPerfSubsystem>       PerfSubsystem;
    std::unique_ptr<TSystemdSubsystem>    SystemdSubsystem;
    std::unique_ptr<TCgroup2Subsystem>    Cgroup2Subsystem;

    std::vector<TSubsystem *> AllSubsystems;
    std::vector<TSubsystem *> Subsystems;
    std::vector<TSubsystem *> Hierarchies;

    uint64_t DefaultControllers;

    TError InitializeCgroups();
    TError InitializeDaemonCgroups();
    void CleanupCgroups();

    TError CgroupChildren(const TCgroup &cg, std::list<std::unique_ptr<const TCgroup>> &cgroups, bool all = false) const;

    std::unique_ptr<const TCgroup> GetContainerCgroup(const TContainer &container, TSubsystem *subsystem) const;
    std::list<std::unique_ptr<const TCgroup>> GetContainerCgroups(const TContainer &container) const;

    std::unique_ptr<const TCgroup> GetContainerCgroupByKnob(const TContainer &container, const std::string &knob) const;
    bool HasContainerCgroupsKnob(const TContainer &container, const std::string &knob) const;
    TError GetContainerCgroupsKnob(const TContainer &container, const std::string &knob, std::string &value) const;

    TError CreateCgroup(const TCgroup &cg) const;
    TError RemoveCgroup(const TCgroup &cg) const;
    TError RecreateCgroup(const TCgroup &cg) const;

    TError CreateContainerCgroups(TContainer &container, bool onRestore = false) const;
    TError RemoveContainerCgroups(const TContainer &container, bool ignore) const;

    TError GetCgroupCount(const TCgroup &cg, uint64_t &count, bool thread = false) const;
    TError GetCgroupThreadCount(const TCgroup &cg, uint64_t &count) const;
    TError GetCgroupProcessCount(const TCgroup &cg, uint64_t &count) const;
};

extern TCgroupDriver CgroupDriver;
