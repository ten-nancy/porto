#include "device.hpp"
#include "cgroup.hpp"
#include "util/log.hpp"

extern "C" {
#include <sys/stat.h>
#include <linux/kdev_t.h>
#include <sys/sysmacros.h>
}

TError TDevice::CheckPath(const TPath &path) {
    if (!path.IsNormal())
        return TError(EError::InvalidValue, "Non-normalized device path: {}", path);

    if (!path.IsInside("/dev"))
        return TError(EError::InvalidValue, "Device path not in /dev: {}", path);

    return OK;
}

TError TDevice::Parse(TTuple &opt, const TCred &cred) {
    TError error;

    if (opt.size() < 2)
        return TError(EError::InvalidValue, "Invalid device config: " +
                      MergeEscapeStrings(opt, ' '));

    /* <device> [r][w][m][-][?] [path] [mode] [user] [group] */
    Path = opt[0];

    error = TDevice::CheckPath(Path);
    if (error)
        return error;

    MayRead = MayWrite = MayMknod = Wildcard = Optional = false;
    for (char c: opt[1]) {
        switch (c) {
            case 'r':
                MayRead = true;
                break;
            case 'w':
                MayWrite = true;
                break;
            case 'm':
                MayMknod = true;
                break;
            case '*':
                if (!cred.IsRootUser())
                    return TError(EError::Permission, "{} cannot setup wildcard {}", cred.ToString(), Path);
                Wildcard = true;
                break;
            case '-':
                Optional = true;
                break;
            case '?':
                Optional = true;
                break;
            default:
                return TError(EError::InvalidValue, "Invalid access: " + opt[1]);
        }
    }

    struct stat st;
    error = Path.StatFollow(st);
    if (error) {
        if (error.Errno == ENOENT)
            return TError(EError::DeviceNotFound, "Device {} does not exists", Path);
        return error;
    }

    if (!S_ISCHR(st.st_mode) && !S_ISBLK(st.st_mode))
        return TError(EError::DeviceNotFound, "Not a device node: {}", Path);

    Node = st.st_rdev;
    Uid = st.st_uid;
    Gid = st.st_gid;
    Mode = st.st_mode;

    /* Initial setup is done in container */
    if ((MayRead || MayWrite) && !Wildcard)
        MayMknod = true;

    // FIXME check acl
    if (MayRead && !TFile::Access(st, cred, TFile::R))
        return TError(EError::Permission, "{} cannot read device {}", cred.ToString(), Path);

    if (MayWrite && !TFile::Access(st, cred, TFile::W))
        return TError(EError::Permission, "{} cannot write device {}", cred.ToString(), Path);

    if (opt.size() > 2) {
        error = TDevice::CheckPath(opt[2]);
        if (error)
            return error;
        PathInside = opt[2];
    } else
        PathInside = opt[0];

    if (opt.size() > 3) {
        unsigned mode;
        error = StringToOct(opt[3], mode);
        if (error)
            return error;
        if (mode & ~0777)
            return TError(EError::InvalidValue, "invalid device mode: " + opt[3]);
        if ((mode & ~(Mode & 0777)) && cred.GetUid() != Uid && !cred.IsRootUser())
            return TError(EError::Permission, "{} cannot change device {} permissions {:#o} to {:#o}",
                          cred.ToString(), Path, Mode & 0777, mode);
        Mode = mode | (Mode & ~0777);
    }

    if (opt.size() > 4) {
        uid_t uid;
        error = UserId(opt[4], uid);
        if (error)
            return error;
        if (uid != Uid && cred.GetUid() != Uid && !cred.IsRootUser())
            return TError(EError::Permission, "{} cannot change device {} uid {} to {}",
                          cred.ToString(), Path, UserName(Uid), UserName(uid));
        Uid = uid;
    }

    if (opt.size() > 5) {
        gid_t gid;
        error = GroupId(opt[5], gid);
        if (error)
            return error;
        if (gid != Gid && cred.GetUid() != Uid && !cred.IsRootUser())
            return TError(EError::Permission, "{} cannot change device {} gid {} to {}",
                          cred.ToString(), Path, GroupName(Gid), GroupName(gid));
        Gid = gid;
    }

    return OK;
}

std::string TDevice::FormatNode() const {
    return fmt::format("{}:{}", major(Node), Wildcard ? "*" : std::to_string(minor(Node)));
}

std::string TDevice::FormatAccess() const {
    std::string perm;

    if (MayRead)
        perm += "r";
    if (MayWrite)
        perm += "w";
    if (MayMknod)
        perm += "m";
    if (Wildcard)
        perm += "*";
    if (perm == "")
        perm = "-";
    if (Optional)
        perm += "?";

    return perm;
}

std::string TDevice::Format() const {
    return fmt::format("{} {} {} {:#o} {} {}", Path, FormatAccess(), PathInside,
                       Mode & 0777, UserName(Uid), GroupName(Gid));
}

TError TDevice::Load(const rpc::TContainerDevice &dev, const TCred &cred) {
    // for consistency with set(string)
    TTuple cfg = {dev.device(), dev.access()};
    if (dev.has_path()) {
        cfg.push_back(dev.path());
        if (dev.has_mode()) {
            cfg.push_back(dev.mode());
            if (dev.has_user()) {
                cfg.push_back(dev.user());
                if (dev.has_group()) {
                    cfg.push_back(dev.group());
                }
            }
        }
    }

    return Parse(cfg, cred);
}

void TDevice::Dump(rpc::TContainerDevice &dev) const {
    dev.set_device(Path.ToString());
    dev.set_access(FormatAccess());
    dev.set_path(PathInside.ToString());
    dev.set_mode(fmt::format("{:#o}",Mode & 0777));
    dev.set_user(UserName(Uid));
    dev.set_group(GroupName(Gid));
}

TError TDevice::Makedev(const TPath &root) const {
    TPath path = root / PathInside;
    struct stat st;
    TError error;

    error = path.DirName().MkdirAll(0755);
    if (error)
        return error;

    if (Wildcard || !MayMknod)
        return OK;

    if (path.StatFollow(st)) {
        L_ACT("Make {} device node {} {}:{} {:#o} {}:{}",
                S_ISBLK(Mode) ? "blk" : "chr", PathInside,
                major(Node), minor(Node), Mode & 0777,
                Uid, Gid);
        error = path.Mknod(Mode, Node);
        if (error)
            return error;
        error = path.Chown(Uid, Gid);
        if (error)
            return error;
    } else {
        if ((st.st_mode & S_IFMT) != (Mode & S_IFMT) || st.st_rdev != Node)
            return TError(EError::Busy, "Different device node {} {:#o} {}:{} in container",
                          PathInside, st.st_mode, major(st.st_rdev), minor(st.st_rdev));
        if (st.st_mode != Mode) {
            L_ACT("Update device node {} permissions {:#o}", PathInside, Mode & 0777);
            error = path.Chmod(Mode & 0777);
            if (error)
                return error;
        }
        if (st.st_uid != Uid || st.st_gid != Gid) {
            L_ACT("Update device node {} owner {}:{}", PathInside, Uid, Gid);
            error = path.Chown(Uid, Gid);
            if (error)
                return error;
        }
    }

    return OK;
}

TError TDevices::Parse(const std::string &str, const TCred &cred) {
    auto devices_cfg = SplitEscapedString(str, ' ', ';');
    TError error;

    for (auto &cfg: devices_cfg) {

        if (cfg.size() == 2 && cfg[0] == "preset") {
            bool found = false;

            for (auto &preset: config().container().device_preset()) {
                if (preset.preset() != cfg[1])
                    continue;

                for (auto &device_cfg: preset.device()) {
                    auto dev = SplitEscapedString(device_cfg, ' ');
                    TDevice device;
                    error = device.Parse(dev, cred);
                    if (error) {
                        if (error == EError::DeviceNotFound && (device.Optional || AllOptional)) {
                            L("Skip optional device: {}", error);
                            continue;
                        }
                        return error;
                    }
                    L("Add device {} from preset {}", device.Format(), cfg[1]);
                    Devices.push_back(device);
                }

                found = true;
                break;
            }

            if (!found)
                return TError(EError::InvalidValue, "Undefined device preset {}", cfg[1]);

            NeedCgroup = true;
            continue;
        }

        TDevice device;
        error = device.Parse(cfg, cred);
        if (error) {
            if (error == EError::DeviceNotFound && (device.Optional || AllOptional)) {
                L("Skip optional device: {}", error);
                continue;
            }
            return error;
        }
        if (device.MayRead || device.MayWrite || !device.MayMknod)
            NeedCgroup = true;
        Devices.push_back(device);
    }

    return OK;
}

std::string TDevices::Format() const {
    std::string str;
    for (auto &device: Devices)
        str += device.Format() + "; ";
    return str;
}

void TDevices::PrepareForUserNs(const TCred &userNsCred) {
    for (auto &device: Devices) {
        if (device.Uid == RootUser)
            device.Uid = userNsCred.GetUid();
    }
}

TError TDevices::Makedev(const TPath &root) const {
    for (auto &dev: Devices) {
        if (!dev.Allowed())
            continue;

        auto error = dev.Makedev(root);
        if (error)
            return error;
    }

    return OK;
}

struct TDeviceRule {
    char Mode;
    std::string Node;
    char Action;

    bool operator==(const TDeviceRule &o) {
        return Mode == o.Mode &&
            Node == o.Node &&
            Action == o.Action;
    }

    static std::vector<TDeviceRule> Flatten(const TDevices &devices) {
        std::vector<TDeviceRule> rules;
        for (auto &dev: devices.Devices) {
            auto mode = S_ISBLK(dev.Mode) ? 'b' : 'c';
            auto node = dev.FormatNode();
            if (dev.MayRead)
                rules.push_back({mode, node, 'r'});
            if (dev.MayWrite)
                rules.push_back({mode, node, 'w'});
            if (dev.MayMknod)
                rules.push_back({mode, node, 'm'});
        }
        return rules;
    }

    static TError Parse(const TCgroup &cg, std::vector<TDeviceRule> &rules) {
        std::vector<std::string> lines;
        auto error = CgroupDriver.DevicesSubsystem->GetList(cg, lines);
        return error ?: Parse(lines, rules);
    }

    static TError Parse(const TTuple &lines, std::vector<TDeviceRule> &rules) {
        rules.clear();

        for (auto &line: lines) {
            auto invalidLine = TError(EError::Unknown, "Invalid device rule: '{}'", line);

            /* a|b|c <device> [r][w][m] */
            auto toks = SplitString(line, ' ');
            if (toks.size() != 3)
                return invalidLine;

            if (toks[0].size() != 1)
                return invalidLine;
            if (toks[0][0] != 'b' && toks[0][0] != 'c' && toks[0][0] != 'a')
                return invalidLine;

            auto mode = toks[0][0];
            auto node = toks[1];

            for (auto x: toks[2]) {
                if (x != 'r' && x != 'w' && x != 'm')
                    return invalidLine;
                rules.push_back({mode, node, x});
            }
        }

        return OK;
    }

    static TError Apply(const TCgroup &cg, const std::vector<TDeviceRule> &rules, bool allow) {
        std::map<std::pair<char, std::string>, std::string> grouped;

        for (auto &r: rules)
            grouped[std::make_pair(r.Mode, r.Node)].push_back(r.Action);

        for (auto &p: grouped) {
            auto s = fmt::format("{} {} {}", p.first.first, p.first.second, p.second);
            auto error = allow ?
                CgroupDriver.DevicesSubsystem->SetAllow(cg, s) :
                CgroupDriver.DevicesSubsystem->SetDeny(cg, s);
            if (error)
                return error;
        }
        return OK;
    }

    std::string Format() const {
        return fmt::format("{} {} {}", Mode, Node, Action);
    }
};

TError TDevices::Apply(const TCgroup &cg) const {
    std::vector<TDeviceRule> curr;
    auto error = TDeviceRule::Parse(cg, curr);
    if (error)
        return error;

    auto next = TDeviceRule::Flatten(*this);

    // symmetrical difference
    std::vector<TDeviceRule> allow, deny;
    for (const auto &r: curr) {
        if (std::find(next.begin(), next.end(), r) == next.end())
            deny.push_back(r);
    }
    for (const auto &r: next) {
        if (std::find(curr.begin(), curr.end(), r) == curr.end())
            allow.push_back(r);
    }

    error = TDeviceRule::Apply(cg, deny, false);
    if (error)
        return error;
    error = TDeviceRule::Apply(cg, allow, true);
    if (error)
        return error;

    return OK;
}

TError TDevices::InitDefault() {
    TError error;

    Devices = {
        {"/dev/null", MKDEV(1, 3)},
        {"/dev/zero", MKDEV(1, 5)},
        {"/dev/full", MKDEV(1, 7)},
        {"/dev/random", MKDEV(1, 8)},
        {"/dev/urandom", MKDEV(1, 9)},
        {"/dev/tty", MKDEV(5, 0)},
        {"/dev/console", MKDEV(1, 3)},
        {"/dev/ptmx", MKDEV(5, 2)},
        {"/dev/pts/*", MKDEV(136, 0)},
    };

    Devices[6].Path = "/dev/null";
    Devices[7].MayMknod = false;
    Devices[8].Wildcard = true;
    Devices[8].MayMknod = false;

    AllOptional = true;

    error = Parse(config().container().extra_devices(), TCred(RootUser, RootGroup));
    if (error)
        return error;

    return OK;
}

TDevices &TDevices::Merge(const TDevices &other) {
    for (auto &dev: other.Devices) {
        auto it = std::find_if(
            Devices.begin(), Devices.end(),
            [&dev](const TDevice &d) { return d.PathInside == dev.PathInside; }
        );
        if (it != Devices.end()) {
            *it = dev;
        } else {
            Devices.push_back(dev);
        }
    }
    return *this;
}

bool TDevices::operator<=(const TDevices &other) const {
        for (auto &dev : Devices) {
            if (!dev.Allowed())
                continue;

            auto it = std::find_if(
                other.Devices.begin(), other.Devices.end(),
                [&dev](const TDevice &d) { return d.Node == dev.Node; }
            );
            if (it == other.Devices.end())
                return false;

            if  (dev.MayRead > it->MayRead || dev.MayWrite > it->MayWrite || dev.MayMknod > it->MayMknod)
                return false;
        }
        return true;
    }
