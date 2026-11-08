#include "resctrl.hpp"

#include <algorithm>
#include <map>
#include <vector>

#if defined(__i386__) || defined(__x86_64__)
#include <cpuid.h>
#endif

#include "cpuset.hpp"
#include "util/log.hpp"
#include "util/mount.hpp"

extern "C" {
#include <errno.h>
#include <linux/magic.h>
}

static const TPath ResctrlPath = "/sys/fs/resctrl";
static bool HasResctrl = false;

static const std::string RESCTRL_PREFIX = "porto%";
static const std::string RESCTRL_TASKS = "tasks";
static const std::string RESCTRL_SCHEMATA = "schemata";

bool IsResctrlAvailable() {
    return HasResctrl && !L3Domains.empty();
}

TError _MountResctrl() {
    TFile dir;
    auto error = dir.OpenDir(ResctrlPath);
    if (error)
        return error;

    TStatFS stat;
    error = dir.StatFS(stat);
    if (error)
        return error;
    if (stat.FsType == RDTGROUP_SUPER_MAGIC)
        return OK;

    TMountFd mount;
    error = mount.Create("resctrl", MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC, {});
    if (error)
        return error;
    error = mount.Move(dir, TPath());
    if (error)
        return error;

    error = ResctrlPath.StatFS(stat);
    if (error)
        return error;
    if (stat.FsType != RDTGROUP_SUPER_MAGIC)
        return TError("Unexpected filesystem {:#x} at {}, expected resctrl", stat.FsType, ResctrlPath);

    return OK;
}

TError MountResctrl() {
#if defined(__i386__) || defined(__x86_64__)
    unsigned int eax, ebx, ecx, edx;
    if (!__get_cpuid(0, &eax, &ebx, &ecx, &edx) || ebx != signature_AMD_ebx || ecx != signature_AMD_ecx ||
        edx != signature_AMD_edx)
        return TError(EError::NotSupported, "resctrl bandwidth control is supported only on AMD CPUs");
#else
    return TError(EError::NotSupported, "resctrl bandwidth control is supported only on AMD CPUs");
#endif

    auto error = _MountResctrl();
    if (error)
        return error;

    HasResctrl = true;
    return OK;
}

std::string ResctrlGroupName(const std::string &name) {
    auto group = RESCTRL_PREFIX + name;
    std::replace(group.begin(), group.end(), '/', '%');
    return group;
}

static TPath ResctrlGroupPath(const std::string &group) {
    return ResctrlPath / group;
}

TError CreateResctrlGroup(const std::string &group) {
    if (!IsResctrlAvailable())
        return TError(EError::NotSupported, "resctrl is unavailable");

    auto path = ResctrlGroupPath(group);

    L_ACT("Create resctrl group {}", path);

    auto error = path.Mkdir(0755);
    if (error && error.Errno == EEXIST) {
        /* left over after unclean shutdown */
        L_WRN("Reuse stale resctrl group {}", path);
        return OK;
    }

    return error;
}

TError RemoveResctrlGroup(const std::string &group) {
    if (!IsResctrlAvailable())
        return TError(EError::NotSupported, "resctrl is unavailable");

    auto path = ResctrlGroupPath(group);

    if (!path.Exists())
        return OK;

    L_ACT("Remove resctrl group {}", path);

    /* rmdir returns remaining tasks back into the default group */
    auto error = path.Rmdir();
    if (error && error.Errno == ENOENT)
        return OK;

    return error;
}

static TError GetMinBandwidth(uint64_t &min) {
    auto path = ResctrlPath / "info" / "MB" / "min_bandwidth";
    std::string text;

    auto error = path.ReadAll(text);
    if (error)
        return error;

    return StringToUint64(StringTrim(text), min);
}

static TError formatSchemata(uint64_t membw, const TBitMap &affinity, std::string &schemata) {
    uint64_t min;

    auto error = GetMinBandwidth(min);
    if (error)
        return error;

    /* l3 domain -> number of cpus on this domain */
    std::vector<std::pair<unsigned, unsigned>> cpus;
    cpus.reserve(L3Domains.size());
    unsigned cpusLeft = 0;

    for (auto &kv: L3Domains) {
        auto id = kv.first;
        auto &map = kv.second;

        auto count = map.IntersectionSize(affinity);
        cpus.emplace_back(id, count);
        cpusLeft += count;
    }

    if (!cpusLeft)
        return TError(EError::InvalidValue, "No cpus of cpu_set={} on L3 domains", affinity.Format());

    uint64_t left = membw;

    schemata = "MB:";

    const char *sep = "";
    for (auto &kv: cpus) {
        auto id = kv.first;
        auto count = kv.second;
        /* domains without our cpus are throttled down to the floor */
        uint64_t value = min;

        if (count) {
            /* ceil(left * cpus / cpusLeft), split to never overflow */
            value = left / cpusLeft * count;
            value += (left % cpusLeft * count + cpusLeft - 1) / cpusLeft;

            left -= value;
            cpusLeft -= count;
        }

        schemata += fmt::format("{}{}={}", sep, id, value);
        sep = ";";
    }

    schemata += "\n";

    return OK;
}

TError SetResctrlSchemata(const std::string &group, uint64_t membw, const TBitMap &affinity) {
    if (!IsResctrlAvailable())
        return TError(EError::NotSupported, "resctrl is unavailable");

    std::string schemata;

    auto error = formatSchemata(membw, affinity, schemata);
    if (error)
        return error;

    auto path = ResctrlGroupPath(group) / RESCTRL_SCHEMATA;
    TFile file;

    L_ACT("Set resctrl group {} schemata {}", group, StringTrim(schemata));

    error = file.OpenWrite(path);
    if (error)
        return error;

    error = file.WriteAll(schemata);
    if (error)
        return TError(error, "Cannot set schemata {}", StringTrim(schemata));

    return OK;
}

TError AttachResctrlGroup(const std::string &group, pid_t pid) {
    if (!IsResctrlAvailable())
        return TError(EError::NotSupported, "resctrl is unavailable");

    auto path = ResctrlGroupPath(group) / RESCTRL_TASKS;
    TFile file;

    L_ACT("Attach task {} to resctrl group {}", pid, group);

    auto error = file.OpenWrite(path);
    if (error)
        return error;

    return file.WriteAll(std::to_string(pid));
}
