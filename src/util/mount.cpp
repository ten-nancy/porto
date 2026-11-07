#include "mount.hpp"

#include <tuple>
#include <unordered_map>

#include "util/log.hpp"
#include "util/string.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
}

static int DirFd(const TFile &dir) {
    return dir ? dir.Fd : AT_FDCWD;
}

static int EmptyPath(const TPath &path, int flag) {
    return path.IsEmpty() ? flag : 0;
}

TError TMountFd::Create(const std::string &type, uint64_t attr, const std::vector<std::string> &opts) {
    TFile fs;

    Close();

    fs.SetFd = syscall(SYS_fsopen, type.c_str(), FSOPEN_CLOEXEC);
    if (fs.Fd < 0)
        return TError::System("fsopen({})", type);

    for (const auto &opt: opts) {
        auto sep = opt.find('=');
        int ret;

        if (sep == std::string::npos)
            ret = syscall(SYS_fsconfig, fs.Fd, FSCONFIG_SET_FLAG, opt.c_str(), NULL, 0);
        else
            ret = syscall(SYS_fsconfig, fs.Fd, FSCONFIG_SET_STRING, opt.substr(0, sep).c_str(),
                          opt.substr(sep + 1).c_str(), 0);

        if (ret < 0)
            return TError::System("fsconfig({}, {})", type, opt);
    }

    if (syscall(SYS_fsconfig, fs.Fd, FSCONFIG_CMD_CREATE, NULL, NULL, 0) < 0)
        return TError::System("fsconfig({})", type);

    L_ACT("fsmount {} {} attr={:#x}", type, MergeEscapeStrings(opts, ','), attr);

    SetFd = syscall(SYS_fsmount, fs.Fd, FSMOUNT_CLOEXEC, attr);
    if (Fd < 0)
        return TError::System("fsmount({})", type);

    return OK;
}

TError TMountFd::Clone(const TFile &dir, const TPath &path, bool recursive) {
    Close();

    L_ACT("open_tree clone {} {}{}", dir.RealPath(), path, recursive ? " recursive" : "");

    SetFd =
        syscall(SYS_open_tree, DirFd(dir), path.c_str(),
                OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC | EmptyPath(path, AT_EMPTY_PATH) | (recursive ? AT_RECURSIVE : 0));
    if (Fd < 0)
        return TError::System("open_tree {} {}", dir.RealPath(), path);

    return OK;
}

TError TMountFd::Open(const TFile &dir, const TPath &path) {
    Close();

    SetFd = syscall(SYS_open_tree, DirFd(dir), path.c_str(), OPEN_TREE_CLOEXEC | EmptyPath(path, AT_EMPTY_PATH));
    if (Fd < 0)
        return TError::System("open_tree {} {}", dir.RealPath(), path);

    return OK;
}

TError SetMountAttr(const TFile &dir, const TPath &path, const TMountAttr &attr, bool recursive) {
    struct mount_attr mnt_attr = {};
    mnt_attr.attr_set = attr.Set;
    mnt_attr.attr_clr = attr.Clr;
    mnt_attr.propagation = attr.Propagation;

    L_ACT("mount_setattr {} {} {}{}", dir ? dir.RealPath() : "", path, attr.ToString(), recursive ? " recursive" : "");

    unsigned int flags = EmptyPath(path, AT_EMPTY_PATH) | (recursive ? AT_RECURSIVE : 0);
    if (syscall(SYS_mount_setattr, DirFd(dir), path.c_str(), flags, &mnt_attr, sizeof(mnt_attr)) < 0)
        return TError::System("mount_setattr");

    return OK;
}

TError TMountFd::Move(const TFile &dir, const TPath &path) {
    L_ACT("move_mount {} {}", dir.RealPath(), path);

    if (syscall(SYS_move_mount, Fd, "", DirFd(dir), path.c_str(),
                MOVE_MOUNT_F_EMPTY_PATH | EmptyPath(path, MOVE_MOUNT_T_EMPTY_PATH)) < 0)
        return TError::System("move_mount {} {}", dir.RealPath(), path);

    return OK;
}

TError BindMount(const TFile &srcDir, const TPath &source, const TFile &dstDir, const TPath &target,
                 const TMountAttr &attr, bool recursive) {
    L_ACT("bind mount {} {} {}{}", srcDir ? srcDir.RealPath() / source : source,
          dstDir ? dstDir.RealPath() / target : target, attr.ToString(), recursive ? " recursive" : "");

    TMountFd mnt;
    auto error = mnt.Clone(srcDir, source, recursive);
    if (error)
        return error;

    if (attr.Set || attr.Clr || attr.Propagation) {
        error = SetMountAttr(mnt, TPath(), attr, recursive);
        if (error)
            return error;
    }

    return mnt.Move(dstDir, target);
}

TError TMountFd::SetFlagsPropagation(uint64_t mnt_flags) const {
    for (auto propagation: {MS_PRIVATE, MS_SLAVE, MS_SHARED, MS_UNBINDABLE}) {
        if (mnt_flags & propagation) {
            auto error = SetPropagation(propagation, mnt_flags & MS_REC);
            if (error)
                return error;
        }
    }
    return OK;
}

TError TMountFd::Reconfigure(const TPath &path, const std::vector<std::string> &opts) {
    TFile fs;

    L_ACT("fspick reconfigure {} {}", path, MergeEscapeStrings(opts, ','));

    fs.SetFd = syscall(SYS_fspick, AT_FDCWD, path.c_str(), FSPICK_CLOEXEC);
    if (fs.Fd < 0)
        return TError::System("fspick({})", path);

    for (const auto &opt: opts) {
        auto sep = opt.find('=');
        int ret;

        if (sep == std::string::npos)
            ret = syscall(SYS_fsconfig, fs.Fd, FSCONFIG_SET_FLAG, opt.c_str(), NULL, 0);
        else
            ret = syscall(SYS_fsconfig, fs.Fd, FSCONFIG_SET_STRING, opt.substr(0, sep).c_str(),
                          opt.substr(sep + 1).c_str(), 0);

        if (ret < 0)
            return TError::System("fsconfig({}, {})", path, opt);
    }

    if (syscall(SYS_fsconfig, fs.Fd, FSCONFIG_CMD_RECONFIGURE, NULL, NULL, 0) < 0)
        return TError::System("fsconfig({}) reconfigure", path);

    return OK;
}

TError TMount::ParseMountAttr(const std::string &options, TMountAttr &attr, bool &recursive, bool strict) {
    /* option: attributes to set, attributes to clear */
    static const std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> attrs = {
        {"ro", {MOUNT_ATTR_RDONLY, 0}},
        {"rw", {0, MOUNT_ATTR_RDONLY}},
        {"nosuid", {MOUNT_ATTR_NOSUID, 0}},
        {"suid", {0, MOUNT_ATTR_NOSUID}},
        {"nodev", {MOUNT_ATTR_NODEV, 0}},
        {"dev", {0, MOUNT_ATTR_NODEV}},
        {"noexec", {MOUNT_ATTR_NOEXEC, 0}},
        {"exec", {0, MOUNT_ATTR_NOEXEC}},
        {"noatime", {MOUNT_ATTR_NOATIME, MOUNT_ATTR__ATIME}},
        {"relatime", {MOUNT_ATTR_RELATIME, MOUNT_ATTR__ATIME}},
        {"strictatime", {MOUNT_ATTR_STRICTATIME, MOUNT_ATTR__ATIME}},
        {"nodiratime", {MOUNT_ATTR_NODIRATIME, 0}},
        {"nosymfollow", {MOUNT_ATTR_NOSYMFOLLOW, 0}},
    };

    attr = {0, 0, 0};
    recursive = false;

    /* attributes in both set and clear end up set, as in mount_setattr(2) */
    for (auto &opt: SplitString(options, ',')) {
        auto name = StringTrim(opt);
        if (name.empty())
            continue;

        if (name == "rec") {
            recursive = true;
        } else if (name == "private") {
            if (attr.Propagation != MS_UNBINDABLE)
                attr.Propagation = MS_PRIVATE;
        } else if (name == "unbindable") {
            attr.Propagation = MS_UNBINDABLE;
        } else {
            auto it = attrs.find(name);
            if (it != attrs.end()) {
                attr.Set |= it->second.first;
                attr.Clr |= it->second.second;
            } else if (strict)
                return TError(EError::InvalidValue, "Unknown mount option: {}", name);
        }
    }

    return OK;
}

uint64_t TMountAttr::ToFlags() const {
    /* flag to set, flag to clear, attribute */
    static const std::vector<std::tuple<uint64_t, uint64_t, uint64_t>> attrs = {
        {MS_RDONLY, MS_ALLOW_WRITE, MOUNT_ATTR_RDONLY}, {MS_NODEV, MS_ALLOW_DEV, MOUNT_ATTR_NODEV},
        {MS_NOSUID, MS_ALLOW_SUID, MOUNT_ATTR_NOSUID},  {MS_NOEXEC, MS_ALLOW_EXEC, MOUNT_ATTR_NOEXEC},
        {MS_NODIRATIME, 0, MOUNT_ATTR_NODIRATIME},
    };
    uint64_t mnt_flags = 0;

    for (auto &it: attrs) {
        if (Set & std::get<2>(it))
            mnt_flags |= std::get<0>(it);
        else if (Clr & std::get<2>(it))
            mnt_flags |= std::get<1>(it);
    }

    if (Clr & MOUNT_ATTR__ATIME) {
        if ((Set & MOUNT_ATTR__ATIME) == MOUNT_ATTR_NOATIME)
            mnt_flags |= MS_NOATIME;
        else if ((Set & MOUNT_ATTR__ATIME) == MOUNT_ATTR_STRICTATIME)
            mnt_flags |= MS_STRICTATIME;
        else
            mnt_flags |= MS_RELATIME;
    }

    return mnt_flags | Propagation;
}

std::string TMountAttr::ToString() const {
    return TMount::FormatFlags(ToFlags());
}
