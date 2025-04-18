#pragma once

#include <map>
#include <string>

#include "common.hpp"

constexpr const char *P_RAW_ROOT_PID = "_root_pid";
constexpr const char *P_SEIZE_PID = "seize_pid";
constexpr const char *P_RAW_ID = "_id";
constexpr const char *P_RAW_NAME = "_name";
constexpr const char *P_RAW_CREATION_TIME = "_creation_time";
constexpr const char *P_RAW_START_TIME = "_start_time";
constexpr const char *P_RAW_DEATH_TIME = "_death_time";

constexpr const char *P_TAINT = "taint";

constexpr const char *P_EXTRA_PROPS = "extra_properties";
constexpr const char *P_COMMAND = "command";
constexpr const char *P_COMMAND_ARGV = "command_argv";
constexpr const char *P_USER = "user";
constexpr const char *P_GROUP = "group";
constexpr const char *P_TASK_CRED = "task_cred";
constexpr const char *P_OWNER_USER = "owner_user";
constexpr const char *P_OWNER_GROUP = "owner_group";
constexpr const char *P_OWNER_CRED = "owner_cred";
constexpr const char *P_OWNER_CONTAINERS = "owner_containers";
constexpr const char *P_ENV = "env";
constexpr const char *P_ENV_SECRET = "env_secret";
constexpr const char *P_PORTO_NAMESPACE = "porto_namespace";
constexpr const char *P_PLACE = "place";
constexpr const char *P_PLACE_LIMIT = "place_limit";
constexpr const char *P_ROOT = "root";
constexpr const char *P_ROOT_PATH = "root_path";
constexpr const char *P_ROOT_RDONLY = "root_readonly";
constexpr const char *P_CWD = "cwd";
constexpr const char *P_STDIN_PATH = "stdin_path";
constexpr const char *P_STDOUT_PATH = "stdout_path";
constexpr const char *P_STDERR_PATH = "stderr_path";
constexpr const char *P_STDOUT_LIMIT = "stdout_limit";
constexpr const char *P_SECCOMP = "seccomp";
constexpr const char *P_SECCOMP_NAME = "seccomp_name";
constexpr const char *P_SESSION_INFO = "session_info";

constexpr const char *P_MEM_GUARANTEE = "memory_guarantee";
constexpr const char *P_MEM_LIMIT = "memory_limit";
constexpr const char *P_DIRTY_LIMIT = "dirty_limit";
constexpr const char *P_DIRTY_LIMIT_BOUND = "dirty_limit_bound";
constexpr const char *P_ANON_LIMIT = "anon_limit";
constexpr const char *P_HUGETLB_LIMIT = "hugetlb_limit";
constexpr const char *P_RECHARGE_ON_PGFAULT = "recharge_on_pgfault";
constexpr const char *P_PRESSURIZE_ON_DEATH = "pressurize_on_death";
constexpr const char *P_MEMORY_USAGE = "memory_usage";
constexpr const char *P_MEMORY_RECLAIMED = "memory_reclaimed";
constexpr const char *P_ANON_USAGE = "anon_usage";
constexpr const char *P_ANON_MAX_USAGE = "anon_max_usage";
constexpr const char *P_ANON_ONLY = "anon_only";
constexpr const char *P_MAX_RSS = "max_rss";
constexpr const char *P_CACHE_USAGE = "cache_usage";
constexpr const char *P_SHMEM_USAGE = "shmem_usage";
constexpr const char *P_MLOCK_USAGE = "mlock_usage";
constexpr const char *P_HUGETLB_USAGE = "hugetlb_usage";
constexpr const char *P_MINOR_FAULTS = "minor_faults";
constexpr const char *P_MAJOR_FAULTS = "major_faults";
constexpr const char *P_VIRTUAL_MEMORY = "virtual_memory";
constexpr const char *P_MEM_LIMIT_TOTAL = "memory_limit_total";
constexpr const char *P_MEM_GUARANTEE_TOTAL = "memory_guarantee_total";
constexpr const char *P_MEM_LOCK_POLICY = "memory_lock_policy";
constexpr const char *P_ANON_LIMIT_TOTAL = "anon_limit_total";
constexpr const char *P_MEM_PRESSURE = "memory_pressure";

constexpr const char *P_CPU_POLICY = "cpu_policy";
constexpr const char *P_CPU_GUARANTEE = "cpu_guarantee";
constexpr const char *P_CPU_GUARANTEE_TOTAL = "cpu_guarantee_total";
constexpr const char *P_CPU_GUARANTEE_BOUND = "cpu_guarantee_bound";
constexpr const char *P_CPU_LIMIT = "cpu_limit";
constexpr const char *P_CPU_LIMIT_TOTAL = "cpu_limit_total";
constexpr const char *P_CPU_LIMIT_BOUND = "cpu_limit_bound";
constexpr const char *P_CPU_PERIOD = "cpu_period";
constexpr const char *P_CPU_WEIGHT = "cpu_weight";
constexpr const char *P_CPU_SET = "cpu_set";
constexpr const char *P_CPU_SET_AFFINITY = "cpu_set_affinity";
constexpr const char *P_CPU_USAGE = "cpu_usage";
constexpr const char *P_CPU_SYSTEM = "cpu_usage_system";
constexpr const char *P_CPU_WAIT = "cpu_wait";
constexpr const char *P_CPU_THROTTLED = "cpu_throttled";
constexpr const char *P_CPU_BURST_USAGE = "cpu_burst_usage";
constexpr const char *P_CPU_UNCONSTRAINED_WAIT = "cpu_unconstrained_wait";
constexpr const char *P_CPU_PRESSURE = "cpu_pressure";

constexpr const char *P_IO_POLICY = "io_policy";
constexpr const char *P_IO_WEIGHT = "io_weight";
constexpr const char *P_IO_LIMIT = "io_limit";
constexpr const char *P_IO_GUARANTEE = "io_guarantee";
constexpr const char *P_IO_OPS_LIMIT = "io_ops_limit";
constexpr const char *P_IO_OPS_GUARANTEE = "io_ops_guarantee";
constexpr const char *P_IO_READ = "io_read";
constexpr const char *P_IO_WRITE = "io_write";
constexpr const char *P_IO_OPS = "io_ops";
constexpr const char *P_IO_READ_OPS = "io_read_ops";
constexpr const char *P_IO_WRITE_OPS = "io_write_ops";
constexpr const char *P_IO_TIME = "io_time";
constexpr const char *P_IO_PRESSURE = "io_pressure";

constexpr const char *P_NET_GUARANTEE = "net_guarantee";
constexpr const char *P_NET_LIMIT_SOFT = "net_limit_soft";
constexpr const char *P_NET_LIMIT = "net_limit";
constexpr const char *P_NET_RX_LIMIT = "net_rx_limit";
constexpr const char *P_NET_LIMIT_BOUND = "net_limit_bound";
constexpr const char *P_NET_RX_LIMIT_BOUND = "net_rx_limit_bound";
constexpr const char *P_RESPAWN = "respawn";
constexpr const char *P_RESPAWN_COUNT = "respawn_count";
constexpr const char *P_RESPAWN_LIMIT = "max_respawns";
constexpr const char *P_RESPAWN_DELAY = "respawn_delay";
constexpr const char *P_ISOLATE = "isolate";
constexpr const char *P_PRIVATE = "private";
constexpr const char *P_LABELS = "labels";
constexpr const char *P_ULIMIT = "ulimit";
constexpr const char *P_HOSTNAME = "hostname";
constexpr const char *P_BIND_DNS = "bind_dns";
constexpr const char *P_BIND = "bind";
constexpr const char *P_SYMLINK = "symlink";
constexpr const char *P_NET = "net";
constexpr const char *P_NET_TOS = "net_tos";
constexpr const char *P_DEVICES = "devices";
constexpr const char *P_DEVICES_EXPLICIT = "devices_explicit";
constexpr const char *P_CAPABILITIES = "capabilities";
constexpr const char *P_CAPABILITIES_AMBIENT = "capabilities_ambient";
constexpr const char *P_CAPABILITIES_ALLOWED = "capabilities_allowed";
constexpr const char *P_CAPABILITIES_AMBIENT_ALLOWED = "capabilities_ambient_allowed";
constexpr const char *P_IP = "ip";
constexpr const char *P_IP_LIMIT = "ip_limit";
constexpr const char *P_DEFAULT_GW = "default_gw";
constexpr const char *P_VIRT_MODE = "virt_mode";
constexpr const char *P_USERNS = "userns";
constexpr const char *P_UNSHARE_ON_EXEC = "unshare_on_exec";
constexpr const char *P_CGROUPFS = "cgroupfs";
constexpr const char *P_AGING_TIME = "aging_time";
constexpr const char *P_ENABLE_PORTO = "enable_porto";
constexpr const char *P_ENABLE_FUSE = "enable_fuse";
constexpr const char *P_RESOLV_CONF = "resolv_conf";
constexpr const char *P_ETC_HOSTS = "etc_hosts";
constexpr const char *P_WEAK = "weak";
constexpr const char *P_UMASK = "umask";
constexpr const char *P_CONTROLLERS = "controllers";
constexpr const char *P_LINK_MEMORY_WRITEBACK_BLKIO = "link_memory_writeback_blkio";
constexpr const char *P_OOM_IS_FATAL = "oom_is_fatal";
constexpr const char *P_OOM_SCORE_ADJ = "oom_score_adj";
constexpr const char *P_THREAD_LIMIT = "thread_limit";
constexpr const char *P_SYSCTL = "sysctl";
constexpr const char *P_CORE_COMMAND = "core_command";

constexpr const char *P_ID = "id";
constexpr const char *P_LEVEL = "level";
constexpr const char *P_ABSOLUTE_NAME = "absolute_name";
constexpr const char *P_ABSOLUTE_NAMESPACE = "absolute_namespace";
constexpr const char *P_STATE = "state";
constexpr const char *P_OOM_KILLED = "oom_killed";
constexpr const char *P_OOM_KILLS = "oom_kills";
constexpr const char *P_OOM_KILLS_TOTAL = "oom_kills_total";
constexpr const char *P_CORE_DUMPED = "core_dumped";
constexpr const char *P_PARENT = "parent";
constexpr const char *P_ROOT_PID = "root_pid";
constexpr const char *P_EXIT_STATUS = "exit_status";
constexpr const char *P_EXIT_CODE = "exit_code";
constexpr const char *P_START_ERROR = "start_error";
constexpr const char *P_STDOUT = "stdout";
constexpr const char *P_STDOUT_OFFSET = "stdout_offset";
constexpr const char *P_STDERR = "stderr";
constexpr const char *P_STDERR_OFFSET = "stderr_offset";

constexpr const char *P_NET_TX_MAX_SPEED = "net_tx_max_speed";
constexpr const char *P_NET_RX_MAX_SPEED = "net_rx_max_speed";
constexpr const char *P_NET_TX_SPEED_HGRAM = "net_tx_speed_hgram";
constexpr const char *P_NET_RX_SPEED_HGRAM = "net_rx_speed_hgram";
constexpr const char *P_NET_LIMIT_SOFT_STAT = "net_limit_soft_stat";
constexpr const char *P_NET_CLASS_ID = "net_class_id";
constexpr const char *P_NET_BYTES = "net_bytes";
constexpr const char *P_NET_PACKETS = "net_packets";
constexpr const char *P_NET_DROPS = "net_drops";
constexpr const char *P_NET_OVERLIMITS = "net_overlimits";
constexpr const char *P_NET_RX_BYTES = "net_rx_bytes";
constexpr const char *P_NET_RX_PACKETS = "net_rx_packets";
constexpr const char *P_NET_RX_DROPS = "net_rx_drops";
constexpr const char *P_NET_RX_OVERLIMITS = "net_rx_overlimits";
constexpr const char *P_NET_TX_BYTES = "net_tx_bytes";
constexpr const char *P_NET_TX_PACKETS = "net_tx_packets";
constexpr const char *P_NET_TX_DROPS = "net_tx_drops";
constexpr const char *P_NET_NETSTAT = "net_netstat";
constexpr const char *P_NET_SNMP = "net_snmp";
constexpr const char *P_NET_SNMP6 = "net_snmp6";

constexpr const char *P_TIME = "time";
constexpr const char *P_CREATION_TIME = "creation_time";
constexpr const char *P_START_TIME = "start_time";
constexpr const char *P_DEATH_TIME = "death_time";
constexpr const char *P_CHANGE_TIME = "change_time";
constexpr const char *P_PORTO_STAT = "porto_stat";
constexpr const char *P_PORTO_CPU_JAIL_STATE = "porto_cpu_jail_state";
constexpr const char *P_CGROUPS = "cgroups";
constexpr const char *P_PROCESS_COUNT = "process_count";
constexpr const char *P_THREAD_COUNT = "thread_count";
constexpr const char *P_PLACE_USAGE = "place_usage";

constexpr const char *P_OWNED_VOLUMES = "volumes_owned";
constexpr const char *P_LINKED_VOLUMES = "volumes_linked";
constexpr const char *P_REQUIRED_VOLUMES = "volumes_required";

constexpr const char *P_COREDUMP_FILTER = "coredump_filter";

enum class EProperty {
    NONE,
    ROOT_PID,
    SEIZE_PID,
    LOOP_DEV,
    CREATION_TIME,
    START_TIME,
    DEATH_TIME,
    EXTRA_PROPS,
    COMMAND,
    COMMAND_ARGV,
    USER,
    GROUP,
    OWNER_USER,
    OWNER_GROUP,
    OWNER_CONTAINERS,
    ENV,
    ENV_SECRET,
    PORTO_NAMESPACE,
    PLACE,
    PLACE_LIMIT,
    ROOT,
    ROOT_RDONLY,
    CWD,
    STDIN,
    STDOUT,
    STDERR,
    STDOUT_LIMIT,
    MEM_GUARANTEE,
    MEM_LIMIT,
    MEM_HIGH_LIMIT,
    MEM_LOCK_POLICY,
    DIRTY_LIMIT,
    ANON_LIMIT,
    ANON_ONLY,
    HUGETLB_LIMIT,
    RECHARGE_ON_PGFAULT,
    PRESSURIZE_ON_DEATH,
    CPU_POLICY,
    CPU_GUARANTEE,
    CPU_LIMIT,
    CPU_PERIOD,
    CPU_WEIGHT,
    CPU_SET,
    IO_POLICY,
    IO_WEIGHT,
    IO_LIMIT,
    IO_GUARANTEE,
    IO_OPS_LIMIT,
    IO_OPS_GUARANTEE,
    NET_GUARANTEE,
    NET_LIMIT_SOFT,
    NET_LIMIT,
    RESPAWN,
    RESPAWN_LIMIT,
    RESPAWN_DELAY,
    ISOLATE,
    PRIVATE,
    LABELS,
    ULIMIT,
    HOSTNAME,
    BIND_DNS,
    BIND,
    SYMLINK,
    NET,
    NET_TOS,
    DEVICE_CONF,
    DEVICE_CONF_EXPLICIT,
    CAPABILITIES,
    IP,
    IP_LIMIT,
    DEFAULT_GW,
    VIRT_MODE,
    USERNS,
    UNSHARE_ON_EXEC,
    CGROUPFS,
    AGING_TIME,
    ENABLE_PORTO,
    ENABLE_FUSE,
    RESOLV_CONF,
    ETC_HOSTS,
    WEAK,
    STATE,
    OOM_KILLED,
    OOM_KILLS,
    OOM_KILLS_TOTAL,
    OOM_IS_FATAL,
    OOM_SCORE_ADJ,
    RESPAWN_COUNT,
    EXIT_STATUS,
    CAPABILITIES_AMBIENT,
    UMASK,
    CONTROLLERS,
    LINK_MEMORY_WRITEBACK_BLKIO,
    THREAD_LIMIT,
    SYSCTL,
    NET_RX_LIMIT,
    CORE_COMMAND,
    REQUIRED_VOLUMES,
    COREDUMP_FILTER,
    SECCOMP,
    SECCOMP_NAME,
    SESSION_INFO,
    NR_PROPERTIES,
};

class TProperty {
public:
    std::string Name;
    EProperty Prop;
    std::string Desc;
    uint64_t RequireControllers = 0;
    bool IsSupported = true;
    bool IsReadOnly = false;
    bool IsHidden = false;
    bool IsDynamic = false;
    bool IsRuntimeOnly = false;
    bool IsDeadOnly = false;
    bool IsAnyState = false;

    std::string GetDesc() const;

    TError CanGet() const;
    virtual TError CanSet() const;
    TProperty(std::string name, EProperty prop, std::string desc);

    virtual void Init(void) {}

    virtual TError Has() const;
    virtual TError Get(std::string &value) const = 0;
    virtual TError Set(const std::string &value);
    virtual TError Reset();

    virtual TError GetIndexed(const std::string &index, std::string &value);
    virtual TError SetIndexed(const std::string &index, const std::string &value);

    virtual bool Has(const rpc::TContainerSpec &spec) const;
    virtual TError Load(const rpc::TContainerSpec &spec);
    virtual void Dump(rpc::TContainerSpec &spec) const;
    virtual void Dump(rpc::TContainerStatus &spec) const;
    virtual void DumpIndexed(const std::string &index, rpc::TContainerSpec &spec);
    virtual void DumpIndexed(const std::string &index, rpc::TContainerStatus &spec);

    virtual TError Save(std::string &value);
    virtual TError Load(const std::string &value);

    virtual TError Start(void);
};

void InitContainerProperties(void);

class TContainer;
extern thread_local std::shared_ptr<TContainer> CT;
extern std::map<std::string, TProperty *> ContainerProperties;
