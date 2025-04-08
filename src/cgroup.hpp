#pragma once

#include <sys/types.h>

#include <memory>
#include <string>

#include "config.hpp"
#include "util/path.hpp"

class TContainer;
struct TDevice;
class TSubsystem;

#define CGROUP_FREEZER 0x00001ull
#define CGROUP_MEMORY  0x00002ull
#define CGROUP_CPU     0x00004ull
#define CGROUP_CPUACCT 0x00008ull
#define CGROUP_NETCLS  0x00010ull
#define CGROUP_BLKIO   0x00020ull
#define CGROUP_DEVICES 0x00040ull
#define CGROUP_HUGETLB 0x00080ull
#define CGROUP_CPUSET  0x00100ull
#define CGROUP_PIDS    0x00200ull
#define CGROUP_PERF    0x00400ull
#define CGROUP_SYSTEMD 0x01000ull
#define CGROUP2        0x10000ull

extern const TFlagsNames ControllersName;

class TCgroup {
protected:
    const TSubsystem *Subsystem = nullptr;
    std::string Name;

public:
    // constructors and destructor
    TCgroup() = delete;
    TCgroup(const TSubsystem *subsystem, const std::string &name)
        : Subsystem(subsystem),
          Name(name)
    {}
    TCgroup(const TSubsystem *subsystem, std::string &&name)
        : Subsystem(subsystem),
          Name(name)
    {}
    virtual ~TCgroup() = default;

    // operators
    friend std::ostream &operator<<(std::ostream &os, const TCgroup &cg) {
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

    virtual TError SetPids(const std::string &knob, const std::vector<pid_t> &pids) const;
    virtual TError GetPids(const std::string &knob, std::vector<pid_t> &pids) const;
    virtual TError GetProcesses(std::vector<pid_t> &pids) const;
    virtual TError GetTasks(std::vector<pid_t> &tids) const = 0;
    virtual TError GetCount(uint64_t &count, bool thread = false) const = 0;

    // knobs
    TPath Knob(const std::string &knob) const;
    bool Has(const std::string &knob) const;

    virtual std::unique_ptr<const TCgroup> Leaf() const = 0;

    TError Get(const std::string &knob, std::string &value) const;
    TError Set(const std::string &knob, const std::string &value) const;

    TError GetInt64(const std::string &knob, int64_t &value) const;
    TError SetInt64(const std::string &knob, int64_t value) const;

    TError GetUint64(const std::string &knob, uint64_t &value) const;
    TError SetUint64(const std::string &knob, uint64_t value) const;

    TError GetBool(const std::string &knob, bool &value) const;
    TError SetBool(const std::string &knob, bool value) const;

    TError GetUintMap(const std::string &knob, TUintMap &value) const;
    static TError GetUintMap(const TFile &file, TUintMap &value);
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
        : Kind(kind),
          Type(type),
          MountType(Kind == CGROUP2 ? "cgroup2" : "cgroup")
    {}

    friend bool operator==(const TSubsystem &lhs, const TSubsystem &rhs) {
        return lhs.Kind == rhs.Kind;
    }

    virtual ~TSubsystem() = default;

    virtual bool IsDisabled() const {
        return false;
    }
    virtual bool IsCgroup2() const {
        return MountType == "cgroup2";
    }
    virtual bool IsOptional() const {
        return false;
    }
    virtual std::string TestOption() const {
        return Type;
    }
    virtual std::vector<std::string> MountOptions() const {
        return {Type};
    }

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

class TMemorySubsystem: public TSubsystem {
public:
    // common knobs
    static constexpr const char *STAT = "memory.stat";

    // cgroup v1 mainstream knobs
    static constexpr const char *OOM_CONTROL = "memory.oom_control";
    static constexpr const char *EVENT_CONTROL = "cgroup.event_control";
    static constexpr const char *USE_HIERARCHY = "memory.use_hierarchy";
    static constexpr const char *USAGE = "memory.usage_in_bytes";
    static constexpr const char *LIMIT = "memory.limit_in_bytes";
    static constexpr const char *SOFT_LIMIT = "memory.soft_limit_in_bytes";
    static constexpr const char *MEM_SWAP_LIMIT = "memory.memsw.limit_in_bytes";

    // cgroup v1 private knobs
    static constexpr const char *RECHARGE_ON_PAGE_FAULT = "memory.recharge_on_pgfault";
    static constexpr const char *LOW_LIMIT = "memory.low_limit_in_bytes";
    static constexpr const char *HIGH_LIMIT = "memory.high_limit_in_bytes";
    static constexpr const char *DIRTY_LIMIT = "memory.dirty_limit_in_bytes";
    static constexpr const char *DIRTY_RATIO = "memory.dirty_ratio";
    static constexpr const char *FS_BPS_LIMIT = "memory.fs_bps_limit";    // deprecated (before kernel 5.4 only)
    static constexpr const char *FS_IOPS_LIMIT = "memory.fs_iops_limit";  // deprecated (before kernel 5.4 only)
    static constexpr const char *ANON_USAGE = "memory.anon.usage";
    static constexpr const char *ANON_MAX_USAGE = "memory.anon.max_usage";
    static constexpr const char *ANON_LIMIT = "memory.anon.limit";
    static constexpr const char *ANON_ONLY = "memory.anon.only";
    static constexpr const char *NUMA_BALANCE_VMPROT = "memory.numa_balance_vmprot";
    static constexpr const char *WRITEBACK_BLKIO = "memory.writeback_blkio";
    static constexpr const char *MLOCK_POLICY = "memory.mlock_policy";

    // cgroup v2 only
    static constexpr const char *CURRENT = "memory.current";
    static constexpr const char *EVENTS_LOCAL = "memory.events.local";
    static constexpr const char *MAX = "memory.max";
    static constexpr const char *LOW = "memory.low";
    static constexpr const char *HIGH = "memory.high";
    static constexpr const char *SWAP_MAX = "memory.swap.max";
    static constexpr const char *PRESSURE = "memory.pressure";

    bool HasWritebackBlkio = false;
    bool HasMemoryLockPolicy = false;

    TMemorySubsystem()
        : TSubsystem(CGROUP_MEMORY, "memory")
    {}

    TError InitializeSubsystem() override;

    // stat
    TError Statistics(const TCgroup &cg, TUintMap &stat) const;
    TError GetCacheUsage(const TCgroup &cg, uint64_t &usage) const;
    TError GetShmemUsage(const TCgroup &cg, uint64_t &usage) const;
    TError GetMLockUsage(const TCgroup &cg, uint64_t &usage) const;
    TError GetReclaimed(const TCgroup &cg, uint64_t &value) const;
    TError Usage(const TCgroup &cg, uint64_t &value) const;

    // soft limit
    bool SupportSoftLimit() const;
    TError SetSoftLimit(const TCgroup &cg, int64_t limit) const;
    TError GetSoftLimit(const TCgroup &cg, int64_t &limit) const;

    // guarantee via low limit
    bool SupportGuarantee() const;
    TError SetGuarantee(const TCgroup &cg, uint64_t guarantee) const;

    // high limit
    bool SupportHighLimit() const;
    TError SetHighLimit(const TCgroup &cg, uint64_t limit) const;

    // recharge on page fault
    bool SupportRechargeOnPgfault() const;
    TError RechargeOnPgfault(const TCgroup &cg, bool enable) const;

    // numa balancing
    bool SupportNumaBalance() const;
    TError SetNumaBalance(const TCgroup &cg, uint64_t flag, uint64_t mask);

    // dirty limit
    bool SupportDirtyLimit() const;
    TError SetDirtyLimit(const TCgroup &cg, uint64_t limit);

    // anon limit
    bool SupportAnonLimit() const;
    TError SetAnonLimit(const TCgroup &cg, uint64_t limit) const;

    // anon usage
    TError ResetAnonMaxUsage(const TCgroup &cg) const;
    TError GetAnonMaxUsage(const TCgroup &cg, uint64_t &usage) const;
    TError GetAnonUsage(const TCgroup &cg, uint64_t &usage) const;

    // anon only
    bool SupportAnonOnly() const;
    TError SetAnonOnly(const TCgroup &cg, bool val) const;

    // memory limit
    bool SupportSwap() const;
    TError SetLimit(const TCgroup &cg, uint64_t limit);

    // io limits
    bool SupportIoLimit() const;
    TError SetIoLimit(const TCgroup &cg, uint64_t limit);
    TError SetIopsLimit(const TCgroup &cg, uint64_t limit);
    TError LinkWritebackBlkio(const TCgroup &memcg, const TCgroup &blkcg) const;

    // oom events notification
    TError SetupOomEvent(const TCgroup &cg, TFile &event) const;
    TError SetupOomEventLegacy(const TCgroup &cg, TFile &event) const;
    uint64_t NotifyOomEvents(TFile &event) const;
    TError GetOomKills(const TCgroup &cg, uint64_t &value) const;

    // hierarchy
    TError SetUseHierarchy(const TCgroup &cg, bool value) const;
};

class TFreezerSubsystem: public TSubsystem {
public:
    // cgroup v1 mainstream knobs
    static constexpr const char *STATE = "freezer.state";
    static constexpr const char *SELF_FREEZING = "freezer.self_freezing";
    static constexpr const char *PARENT_FREEZING = "freezer.parent_freezing";

    TFreezerSubsystem()
        : TSubsystem(CGROUP_FREEZER, "freezer")
    {}

    TError WaitState(const TCgroup &cg, const std::string &state) const;
    TError Freeze(const TCgroup &cg, bool wait = true) const;
    TError Thaw(const TCgroup &cg, bool wait = true) const;
    bool IsFrozen(const TCgroup &cg) const;
    bool IsSelfFreezing(const TCgroup &cg) const;
    bool IsParentFreezing(const TCgroup &cg) const;
};

class TCpuSubsystem: public TSubsystem {
    std::vector<uint64_t> SharesMultipliers;

    inline std::string ChooseThrottledKnob() const;
    static inline uint64_t PreparePeriod(uint64_t period);
    static inline uint64_t PrepareQuota(uint64_t quota, uint64_t period);
    TError SetQuotaAndPeriod(const TCgroup &cg, uint64_t quota, uint64_t period = 0) const;

public:
    // common knobs
    static constexpr const char *IDLE = "cpu.idle";
    static constexpr const char *STAT = "cpu.stat";

    // cgroup v1 mainstream knobs
    static constexpr const char *SHARES = "cpu.shares";
    static constexpr const char *CFS_QUOTA_US = "cpu.cfs_quota_us";
    static constexpr const char *CFS_PERIOD_US = "cpu.cfs_period_us";
    static constexpr const char *RT_RUNTIME_US = "cpu.rt_runtime_us";
    static constexpr const char *RT_PERIOD_US = "cpu.rt_period_us";

    // cgroup v1 private knobs
    static constexpr const char *CFS_RESERVE_US = "cpu.cfs_reserve_us";
    static constexpr const char *CFS_RESERVE_SHARES = "cpu.cfs_reserve_shares";
    static constexpr const char *CFS_BURST_USAGE = "cpu.cfs_burst_usage";
    static constexpr const char *CFS_BURST_LOAD = "cpu.cfs_burst_load";
    static constexpr const char *CFS_THROTTLED = "cpu.cfs_throttled";

    // cgroup v2 only
    static constexpr const char *MAX = "cpu.max";
    static constexpr const char *WEIGHT = "cpu.weight";
    static constexpr const char *PRESSURE = "cpu.pressure";

    // stat knobs
    static constexpr const char *THROTTLED_TIME = "throttled_time";      // cg1 mainstream
    static constexpr const char *THROTTLED_USEC = "throttled_usec";      // cg2 mainstream
    static constexpr const char *BURST_LOAD = "burst_load";              // cg1 private
    static constexpr const char *BURST_USAGE = "burst_usage";            // cg1 private
    static constexpr const char *H_THROTTLED_TIME = "h_throttled_time";  // cg1 private

    bool HasShares = false;
    bool HasQuota = false;
    bool HasReserve = false;
    bool HasRtGroup = false;
    bool HasIdle = false;
    bool EnableIdle = false;

    uint64_t BaseShares = 0ull;
    uint64_t MinShares = 0ull;
    uint64_t MaxShares = 0ull;

    TCpuSubsystem()
        : TSubsystem(CGROUP_CPU, "cpu")
    {}
    TError InitializeSubsystem() override;
    TError InitializeCgroup(const TCgroup &cg) const override;
    TError Statistics(const TCgroup &cg, TUintMap &stat) const;

    // stats
    bool SupportThrottled() const;
    TError GetThrottled(const TCgroup &cg, uint64_t &value) const;
    bool SupportUnconstrainedWait() const;
    TError GetUnconstrainedWait(const TCgroup &cg, uint64_t &value) const;
    bool SupportBurstUsage() const;
    TError GetBurstUsage(const TCgroup &cg, uint64_t &value) const;

    // knob setters
    TError SetPeriod(const TCgroup &cg, uint64_t period);
    TError SetLimit(const TCgroup &cg, uint64_t quota, uint64_t period);
    TError SetRtLimit(const TCgroup &cg, uint64_t quota, uint64_t period);
    TError SetGuarantee(const TCgroup &cg, uint64_t period, uint64_t guarantee);
    TError SetShares(const TCgroup &cg, const std::string &policy, double weight, uint64_t guarantee);
    TError SetCpuIdle(const TCgroup &cg, bool value);
};

class TCpuacctSubsystem: public TSubsystem {
public:
    // cgroup v1 mainstream knobs
    static constexpr const char *STAT = "cpuacct.stat";
    static constexpr const char *USAGE = "cpuacct.usage";
    static constexpr const char *WAIT = "cpuacct.wait";

    TCpuacctSubsystem()
        : TSubsystem(CGROUP_CPUACCT, "cpuacct")
    {}
    TError Usage(const TCgroup &cg, uint64_t &value) const;
    TError SystemUsage(const TCgroup &cg, uint64_t &value) const;
    TError GetWait(const TCgroup &cg, uint64_t &value) const;
};

class TCpusetSubsystem: public TSubsystem {
    TError GetCpus(const TCgroup &cg, std::string &cpus) const;
    TError SetCpus(const TCgroup &cg, const std::string &cpus) const;
    TError SetMems(const TCgroup &cg, const std::string &mems) const;

public:
    // common knobs
    static constexpr const char *CPUS = "cpuset.cpus";
    static constexpr const char *MEMS = "cpuset.mems";

    // cgroup v2 only
    static constexpr const char *CPUS_EFFECTIVE = "cpuset.cpus.effective";

    TCpusetSubsystem()
        : TSubsystem(CGROUP_CPUSET, "cpuset")
    {}
    bool IsOptional() const override {
        return true;
    }
    TError InitializeCgroup(const TCgroup &cg) const override;

    TError GetCpus(const TCgroup &cg, TBitMap &cpus) const;
    TError SetCpus(const TCgroup &cg, const TBitMap &cpus) const;
};

class TNetclsSubsystem: public TSubsystem {
public:
    // cgroup v1 mainstream knobs
    static constexpr const char *PRIORITY = "net_cls.priority";
    static constexpr const char *CLASSID = "net_cls.classid";

    bool HasPriority;
    TNetclsSubsystem()
        : TSubsystem(CGROUP_NETCLS, "net_cls")
    {}
    bool IsOptional() const override {
        return !config().network().enable_netcls_classid();
    }
    TError InitializeSubsystem() override;
    TError SetClass(const TCgroup &cg, uint32_t classid) const;
};

class TBlkioSubsystem: public TSubsystem {
    std::string BytesKnob;
    std::string TimeKnob;
    std::string OpsKnob;

public:
    // common knobs
    static constexpr const char *WEIGHT = "io.weight";
    static constexpr const char *LEGACY_WEIGHT = "blkio.weight";
    static constexpr const char *BFQ_WEIGHT = "io.bfq.weight";
    static constexpr const char *LEGACY_BFQ_WEIGHT = "blkio.bfq.weight";

    // cgroup v1 mainstream knobs
    static constexpr const char *READ_IOPS_DEVICE = "blkio.throttle.read_iops_device";
    static constexpr const char *READ_BPS_DEVICE = "blkio.throttle.read_bps_device";
    static constexpr const char *WRITE_IOPS_DEVICE = "blkio.throttle.write_iops_device";
    static constexpr const char *WRITE_BPS_DEVICE = "blkio.throttle.write_bps_device";

    // cgroup v2 only
    static constexpr const char *STAT = "io.stat";
    static constexpr const char *MAX = "io.max";
    static constexpr const char *PRESSURE = "io.pressure";

    bool HasThrottler = false;

    enum IoStat {
        // 2 bits for r and w
        Read = 1,
        Write = 2,
        // 3 bits for class
        Bytes = 4,
        Iops = 8,
        Time = 16,
        // derivative ones
        BytesRead = Bytes | Read,
        BytesWrite = Bytes | Write,
        BytesReadWrite = Bytes | Read | Write,
        IopsRead = Iops | Read,
        IopsWrite = Iops | Write,
        IopsReadWrite = Iops | Read | Write,
    };

    TBlkioSubsystem()
        : TSubsystem(CGROUP_BLKIO, "blkio")
    {}
    bool IsDisabled() const override {
        return !config().container().enable_blkio();
    }
    bool IsOptional() const override {
        return true;
    }
    TError InitializeSubsystem() override;

    void ParseIoStatV1(const std::vector<std::string> &lines, enum IoStat stat, TUintMap &map) const;
    void ParseIoStatV2(const std::vector<std::string> &lines, enum IoStat stat, TUintMap &map) const;
    TError GetIoStat(const TCgroup &cg, enum IoStat stat, TUintMap &map) const;

    TError SetIoLimitV1(const TCgroup &cg, const TPath &root, const TUintMap &map, bool iops = false);
    TError SetIoLimitV2(const TCgroup &cg, const TPath &root, const TUintMap &map, bool iops = false);
    TError SetIoLimit(const TCgroup &cg, const TPath &root, const TUintMap &map, bool iops = false);
    TError SetIoWeight(const TCgroup &cg, const std::string &policy, double weight) const;

    TError DiskName(const std::string &disk, std::string &name) const;
    TError ResolveDisk(const TPath &root, const std::string &key, std::string &disk) const;
};

class TDevicesSubsystem: public TSubsystem {
    static constexpr const char *DENY = "devices.deny";
    static constexpr const char *ALLOW = "devices.allow";
    static constexpr const char *LIST = "devices.list";

public:
    TDevicesSubsystem()
        : TSubsystem(CGROUP_DEVICES, "devices")
    {}

    TError SetAllow(const TCgroup &cg, const std::string &value) const;
    TError SetDeny(const TCgroup &cg, const std::string &value) const;
    TError GetList(const TCgroup &cg, std::vector<std::string> &lines) const;
    bool Unbound(const TCgroup &cg) const;
};

class THugetlbSubsystem: public TSubsystem {
public:
    // cgroup v1 mainstream knobs
    static constexpr const char *HUGE_USAGE = "hugetlb.2MB.usage_in_bytes";
    static constexpr const char *HUGE_LIMIT = "hugetlb.2MB.limit_in_bytes";
    static constexpr const char *GIGA_USAGE = "hugetlb.1GB.usage_in_bytes";
    static constexpr const char *GIGA_LIMIT = "hugetlb.1GB.limit_in_bytes";

    THugetlbSubsystem()
        : TSubsystem(CGROUP_HUGETLB, "hugetlb")
    {}
    bool IsDisabled() const override {
        return !config().container().enable_hugetlb();
    }
    bool IsOptional() const override {
        return true;
    }

    /* for now supports only 2MB pages */
    TError InitializeSubsystem() override {
        if (!RootCgroup()->Has(HUGE_LIMIT))
            return TError(EError::NotSupported, "No {}", std::string(HUGE_LIMIT));
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

class TPidsSubsystem: public TSubsystem {
public:
    // common knobs
    static constexpr const char *MAX = "pids.max";
    static constexpr const char *CURRENT = "pids.current";

    TPidsSubsystem()
        : TSubsystem(CGROUP_PIDS, "pids")
    {}
    bool IsOptional() const override {
        return true;
    }
    TError GetUsage(const TCgroup &cg, uint64_t &usage) const;
    TError SetLimit(const TCgroup &cg, uint64_t limit) const;
};

class TPerfSubsystem: public TSubsystem {
public:
    TPerfSubsystem()
        : TSubsystem(CGROUP_PERF, "perf_event")
    {}
};

class TSystemdSubsystem: public TSubsystem {
public:
    std::unique_ptr<const TCgroup> PortoService;
    TSystemdSubsystem()
        : TSubsystem(CGROUP_SYSTEMD, "systemd")
    {}
    TError InitializeSubsystem() override;
    bool IsDisabled() const override {
        return !config().container().enable_systemd();
    }
    bool IsOptional() const override {
        return true;
    }
    std::string TestOption() const override {
        return "name=" + Type;
    }
    std::vector<std::string> MountOptions() const override {
        return {"none", "name=" + Type};
    }
};

class TCgroup2Subsystem: public TSubsystem {
public:
    TCgroup2Subsystem()
        : TSubsystem(CGROUP2, "unified")
    {}
    bool IsDisabled() const override {
        return !config().container().enable_cgroup2();
    }
    bool IsOptional() const override {
        return true;
    }
    std::string TestOption() const override {
        return "";
    }
    std::vector<std::string> MountOptions() const override {
        return {};
    }
};

class TCgroupDriver: public TNonCopyable {
    bool Cgroup2Hierarchy = false;
    bool Initialized = false;

    TError InitializeCgroups();
    TError InitializeDaemonCgroups();

public:
    // constructors and destructor
    TCgroupDriver();
    ~TCgroupDriver() = default;

    std::unique_ptr<TMemorySubsystem> MemorySubsystem;
    std::unique_ptr<TFreezerSubsystem> FreezerSubsystem;
    std::unique_ptr<TCpuSubsystem> CpuSubsystem;
    std::unique_ptr<TCpuacctSubsystem> CpuacctSubsystem;
    std::unique_ptr<TCpusetSubsystem> CpusetSubsystem;
    std::unique_ptr<TNetclsSubsystem> NetclsSubsystem;
    std::unique_ptr<TBlkioSubsystem> BlkioSubsystem;
    std::unique_ptr<TDevicesSubsystem> DevicesSubsystem;
    std::unique_ptr<THugetlbSubsystem> HugetlbSubsystem;
    std::unique_ptr<TPidsSubsystem> PidsSubsystem;
    std::unique_ptr<TPerfSubsystem> PerfSubsystem;
    std::unique_ptr<TSystemdSubsystem> SystemdSubsystem;
    std::unique_ptr<TCgroup2Subsystem> Cgroup2Subsystem;

    std::vector<TSubsystem *> AllSubsystems;
    std::vector<TSubsystem *> Subsystems;
    std::vector<TSubsystem *> Hierarchies;
    std::vector<TSubsystem *> Cgroup2Subsystems;

    uint64_t DefaultControllers;

    bool UseCgroup2() const;
    bool IsInitialized() const;

    TError Initialize();
    void CleanupCgroups();

    TError CgroupSubtree(const TCgroup &cg, std::list<std::unique_ptr<const TCgroup>> &cgroups, bool all = false) const;

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
