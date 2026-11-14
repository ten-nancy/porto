#include "filesystem.hpp"

#include "cgroup.hpp"
#include "config.hpp"
#include "container.hpp"
#include "util/log.hpp"
#include "util/mount.hpp"

extern "C" {
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/magic.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
}

static std::vector<TPath> SystemPaths = {
    "/bin",    "/boot", "/dev",  "/etc",  "/lib", "/lib32", "/lib64",
    "/libx32", "/proc", "/root", "/sbin", "/sys", "/usr",   "/var",
};

static std::vector<TPath> ExcludedSystemPaths = {
    "/var/lib",
};

typedef struct {
    std::string Type;
    std::string Path;
    std::vector<std::string> Options;
    bool Cgroup1;
    bool Cgroup2;
} cgroupInfo;

const std::vector<cgroupInfo> &getCgroupInfos() {
    static const std::vector<cgroupInfo> cgroup = {{"cgroup2", "", {}, false, true},
                                                   {"cgroup", "freezer", {"freezer"}, true, true},
                                                   {"cgroup", "pids", {"pids"}, true, false},
                                                   {"cgroup", "cpuset", {"cpuset"}, true, false},
                                                   {"cgroup", "memory", {"memory"}, true, false},
                                                   {"cgroup", "blkio", {"blkio"}, true, false},
                                                   {"cgroup", "cpu,cpuacct", {"cpu", "cpuacct"}, true, false},
                                                   {"cgroup", "cpuacct", {"cpuacct"}, false, true},
                                                   {"cgroup", "devices", {"devices"}, true, true},
                                                   {"cgroup", "hugetlb", {"hugetlb"}, true, true},
                                                   {"cgroup", "net_cls,net_prio", {"net_cls", "net_prio"}, true, false},
                                                   {"cgroup", "perf_event", {"perf_event"}, true, true},
                                                   {"cgroup", "systemd", {"name=systemd"}, true, false},
                                                   {"cgroup2", "unified", {}, true, false}};
    return cgroup;
}

bool IsSystemPath(const TPath &path) {
    TPath normal = path.NormalPath();

    if (normal.IsRoot())
        return true;

    if (normal == "/home")
        return true;

    for (auto &excluded: ExcludedSystemPaths)
        if (normal.IsInside(excluded))
            return false;

    for (auto &sys: SystemPaths)
        if (normal.IsInside(sys))
            return true;

    return false;
}

TError TBindMount::ParseOptions(const std::string &options) {
    auto error = TMount::ParseMountAttr(options, Attr, Recursive);
    if (error)
        return error;

    if (Attr.Set & MOUNT_ATTR_NOSYMFOLLOW)
        return TError(EError::InvalidValue, "Bind mount option nosymfollow is not supported");

    // by default disable backward propagation
    Propagation = Attr.Propagation ? Attr.Propagation : MS_SLAVE | MS_SHARED;
    Attr.Propagation = 0;

    return OK;
}

uint64_t TBindMount::GetFlags() const {
    uint64_t mnt_flags = Attr.ToFlags();

    if (Recursive)
        mnt_flags |= MS_REC;

    /* default MS_SLAVE | MS_SHARED is not shown */
    return mnt_flags | (Propagation & (MS_PRIVATE | MS_UNBINDABLE));
}

TError TBindMount::Parse(const std::string &str, std::vector<TBindMount> &binds) {
    auto lines = SplitEscapedString(str, ' ', ';');
    TError error;

    for (auto &line: lines) {
        if (line.size() < 2)
            return TError(EError::InvalidValue, "Invalid bind mount {}", str);

        TBindMount bind;

        bind.Source = line[0];
        bind.Target = line[1];

        error = bind.ParseOptions(line.size() > 2 ? line[2] : "");
        if (error)
            return error;

        binds.push_back(bind);
    }

    return OK;
}

std::string TBindMount::Format(const std::vector<TBindMount> &binds) {
    TMultiTuple lines;
    for (auto &bind: binds)
        lines.push_back({bind.Source.ToString(), bind.Target.ToString(), TMount::FormatFlags(bind.GetFlags())});
    return MergeEscapeStrings(lines, ' ', ';');
}

TError TBindMount::Load(const rpc::TContainerBindMount &spec) {
    TError error;
    std::string options;

    Source = spec.source();
    Target = spec.target();

    for (const auto &flag_str: spec.flag())
        options += flag_str + ",";

    error = ParseOptions(options);
    if (error)
        return error;

    return OK;
}

void TBindMount::Dump(rpc::TContainerBindMount &spec) {
    spec.set_source(Source.ToString());
    spec.set_target(Target.ToString());
    uint64_t mnt_flags = GetFlags();
    for (uint64_t flag = 1; flag <= mnt_flags && flag; flag <<= 1)
        if (flag & mnt_flags)
            spec.add_flag(TMount::FormatFlags(flag));
}

TError TBindMount::Mount(const TCred &cred, const TPath &target_root) const {
    TFile src, dst;

    auto error = src.OpenPath(Source);
    if (error)
        return error;

    if (!(Attr.Set & MOUNT_ATTR_RDONLY)) {
        auto srcFsType = src.FsType();
        if (srcFsType == PROC_SUPER_MAGIC || srcFsType == SYSFS_MAGIC)
            return TError(EError::Permission, "filesystem type is not allowed for write access");
    }

    auto directory = src.IsDirectory();

    if (!error && !ControlSource) {
        /* not read-only means read-write, protect system directories from dac override */
        if (!(Attr.Set & MOUNT_ATTR_RDONLY) || (directory && IsSystemPath(src.RealPath())))
            error = src.WriteAccess(cred);
        else
            error = src.ReadAccess(cred);
    }

    if (error)
        return TError(error, "Bindmount source {}", Source);

    TFile old_root, new_root;
    if (!target_root.IsRoot()) {
        error = old_root.OpenDir("/");
        if (error)
            return error;
        error = new_root.OpenDir(target_root);
        if (error)
            return TError(error, "Bindmount root {}", target_root);
        error = new_root.Chroot();
        if (error)
            return error;
    }

    if (!Target.Exists()) {
        TPath base = Target.DirName();
        std::list<std::string> dirs;
        TFile dir;

        while (!base.Exists()) {
            dirs.push_front(base.BaseName());
            base = base.DirName();
        }

        if (!error)
            error = dir.OpenDir(base);

        if (!error && target_root.IsRoot() && !ControlTarget)
            error = dir.WriteAccess(cred);

        for (auto &name: dirs) {
            if (!error)
                error = dir.MkdirAt(name, 0775);
            if (!error)
                error = dir.OpenDirStrictAt(dir, name);
            if (!error)
                error = dir.Chown(cred);
        }

        auto base_name = Target.BaseName();
        if (directory) {
            if (!error)
                error = dir.MkdirAt(base_name, 0775);
            if (!error)
                error = dst.OpenAt(dir, base_name, O_DIRECTORY | O_RDONLY | O_NOCTTY | O_NOFOLLOW | O_CLOEXEC, 0);
        } else if (!error) {
            error = dst.OpenAt(dir, base_name, O_CREAT | O_WRONLY | O_CLOEXEC, 0664);
        }
        if (!error)
            error = dst.Chown(cred);
    } else {
        if (!error)
            error = directory ? dst.OpenDir(Target) : dst.OpenRead(Target);

        // do not override non-writable directies in host or system directories
        if (!error && !ControlTarget && (target_root.IsRoot() || IsSystemPath(Target)))
            error = dst.WriteAccess(cred);
    }

    if (old_root) {
        TError error2 = old_root.Chroot();
        PORTO_ASSERT(!error2);
    }

    if (error)
        return TError(error, "Bindmount target {}", Target);

    TPath real_target = dst.RealPath();
    if (new_root && !real_target.IsInside(new_root.RealPath()))
        return TError(EError::InvalidPath, "Bindmount real target {} out of root {}", real_target, target_root);

    TMountFd mnt;
    error = mnt.Clone(src, TPath(), Recursive);
    if (!error && (Attr.Set || Attr.Clr))
        error = mnt.SetAttr(Attr.Set, Attr.Clr, Recursive);
    if (!error)
        error = mnt.SetFlagsPropagation(Propagation | (Recursive ? MS_REC : 0));
    if (!error)
        error = mnt.Move(dst, TPath());
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::MountRun(const TContainer &ct) {
    TPath run = "run";
    std::vector<std::string> run_paths, subdirs;
    std::vector<struct stat> run_paths_stat;
    TError error;

    if (run.Exists()) {
        error = run.ListSubdirs(subdirs);
        if (error)
            return error;
    }

    /* We want to recreate /run dir tree with up to RUN_SUBDIR_LIMIT nodes */
    if (subdirs.size() >= RUN_SUBDIR_LIMIT)
        return TError("Too many subdirectories in /run!");

    run_paths.reserve(RUN_SUBDIR_LIMIT);
    for (const auto &i: subdirs) {
        /* Skip creating special directories, we'll do it later */
        if (i == "shm" || i == "lock")
            continue;

        run_paths.push_back(i);
    }

    for (auto i = run_paths.begin(); i != run_paths.end(); ++i) {
        auto current = *i;
        TPath current_path = run / current;

        error = current_path.ListSubdirs(subdirs);
        if (error)
            return error;

        if (subdirs.size() + run_paths.size() >= RUN_SUBDIR_LIMIT)
            return TError("Too many subdirectories in /run!");

        for (auto dir: subdirs)
            run_paths.push_back(current + "/" + dir);
    }

    for (const auto &i: run_paths) {
        struct stat current_stat;
        TPath current_path = run / i;

        error = current_path.StatStrict(current_stat);
        if (error)
            return error;

        run_paths_stat.push_back(current_stat);
    }

    error = run.MkdirAll(0755);
    if (error)
        return error;

    error = run.Mount("tmpfs", "tmpfs", MS_NOSUID | MS_NODEV | MS_STRICTATIME,
                      {"mode=755", "size=" + std::to_string(RunSize)});
    if (error)
        return error;

    // recreate directories
    for (unsigned int i = 0; i < run_paths.size(); i++) {
        TPath current = run / run_paths[i];
        auto &current_stat = run_paths_stat[i];
        mode_t mode = current_stat.st_mode;

        /* forbid other-writable directory without sticky bit */
        if ((mode & 01002) == 02) {
            L("Other writable without sticky: {}", current);
            mode &= ~02;
        }

        error = current.Mkdir(mode);
        if (error)
            return error;

        error = current.Chown(current_stat.st_uid, current_stat.st_gid);
        if (error)
            return error;
    }

    if (ct.InUserNs()) {
        error = run.ChownRecursiveRootFiles(ct.UserNsCred);
        if (error)
            return error;
    }

    return OK;
}

TError TMountNamespace::RemountRun(const TContainer &ct) {
    TPath run("/run"), tmp("/tmp");
    TError error;

    std::vector<TMount> mounts;
    error = TPath::ListAllMounts(mounts);
    if (error)
        return error;

    error = TPath(run).MoveMount(tmp);
    if (error)
        return error;

    error = MountRun(ct);
    if (error)
        return error;

    TPath run_lock("/run/lock");
    error = run_lock.MkdirAll(01777);
    if (error)
        return error;

    error = BindMount(run_lock, run_lock, {MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC, 0, 0}, false);
    if (error)
        return error;

    TPath run_shm("/run/shm"), dev_shm("/dev/shm");
    error = run_shm.MkdirAll(01777);
    if (error)
        return error;

    error = dev_shm.UmountAll();
    if (error)
        return error;

    error = BindMount(run_shm, dev_shm,
                      {MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_STRICTATIME, MOUNT_ATTR__ATIME, 0}, false);
    if (error)
        return error;

    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it) {
        if (!it->Target.IsInside(run) || it->Target == run || it->Target == run_lock)
            continue;

        TPath src = tmp / run.InnerPath(it->Target);
        TPath dst = it->Target;

        if (src.IsDirectoryStrict()) {
            error = dst.MkdirAll(0755);
            if (error)
                return error;
        } else {
            error = dst.DirName().MkdirAll(0755);
            if (error)
                return error;
            error = dst.Mkfile(0);
            if (error)
                return error;
        }

        error = src.MoveMount(dst);
        if (error)
            return error;
    }

    error = tmp.Umount(UMOUNT_NOFOLLOW | MNT_DETACH);
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::MountTraceFs() {
    TError error;

    TPath tracefs = "sys/kernel/tracing";
    TPath parent_tracefs = "/sys/kernel/tracing";
    if (!config().container().enable_tracefs() || !tracefs.Exists())
        return OK;

    struct statfs st;
    if (statfs(parent_tracefs.c_str(), &st) || st.f_type != TRACEFS_MAGIC)
        return TError(EError::Unknown, "Tracefs is not mounted");

    /* read-only bind instead for new mount to preserve read-write in host */
    error = BindMount(parent_tracefs, tracefs, {MOUNT_ATTR_RDONLY, 0, 0}, false);
    if (error)
        return error;

    TPath debugfs = "sys/kernel/debug";
    if (debugfs.Exists()) {
        /* stub with only tracing inside */
        TMountFd mnt;
        error = mnt.Create("tmpfs", 0, {"mode=755", "size=0"});
        if (!error)
            error = mnt.MkdirAt("tracing", 0700);
        /* bind into detached mount needs move_mount to detached tree: kernel >= 6.15, attach first */
        if (!error)
            error = mnt.Move(TFile(), debugfs);
        if (!error)
            error = BindMount(tracefs, debugfs / "tracing", {MOUNT_ATTR_RDONLY, 0, 0}, false);
        if (!error)
            error = mnt.SetAttr(MOUNT_ATTR_RDONLY, 0, false);
        if (error)
            return error;
    }

    return OK;
}

TError TMountNamespace::MountSystemd() {
    if (Systemd.empty())
        return OK;

    TPath tmpfs = "sys/fs/cgroup";
    TPath systemd = tmpfs / "systemd";
    TPath systemd_rw = systemd / Systemd;
    TError error;

    error = tmpfs.UmountAll();
    if (!error)
        error = tmpfs.Mount("tmpfs", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV | MS_STRICTATIME, {"mode=755"});
    if (!error)
        error = systemd.MkdirAll(0755);
    if (!error)
        error = SetMountAttr(TFile(), tmpfs, {MOUNT_ATTR_RDONLY, 0, 0}, false);
    if (!error)
        error = systemd.Mount("cgroup", "cgroup", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, {"name=systemd"});
    if (!error)
        error = BindMount(systemd_rw, systemd_rw,
                          {MOUNT_ATTR_NOSUID | MOUNT_ATTR_NOEXEC | MOUNT_ATTR_NODEV, MOUNT_ATTR_RDONLY, 0}, false);

    return error;
}

TError TMountNamespace::MountCgroups(const TContainer &ct) {
    bool rwCgroupFs = ct.CgroupFs == ECgroupFs::Rw;

    L_DBG("rwCgroupFs {}", rwCgroupFs);
    TError error;
    TPath tmpfs = "sys/fs/cgroup";

    error = tmpfs.UmountAll();
    if (error)
        return error;

    error = tmpfs.Mount("tmpfs", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV | MS_STRICTATIME, {"mode=755"});
    if (error)
        return error;

    for (const auto &cg: getCgroupInfos()) {
        if (CgroupDriver.UseCgroup2()) {
            if (!cg.Cgroup2)
                continue;
        } else {
            if (!cg.Cgroup1)
                continue;
        }

        TPath cgroup = tmpfs / cg.Path;
        error = cgroup.MkdirAll(0755);
        if (error)
            return error;

        bool rw = rwCgroupFs && !(cg.Path == "net_cls,net_prio" && !config().container().enable_rw_net_cgroups());
        uint64_t flags = MS_NOSUID | MS_NOEXEC | MS_NODEV;
        L_DBG("rw: {}", rw);
        // for cgroup2 event RO is required we mount it for RW and then remount for RO
        if (rw || (cg.Path == "" && cg.Cgroup2))
            flags |= MS_ALLOW_WRITE;
        else
            flags |= MS_RDONLY;

        error = cgroup.Mount(cg.Type, cg.Type, flags, cg.Options);
        if (error)
            return error;

        if (rw) {
            error = cgroup.Chmod(0750);
            if (error)
                return error;
        }

        if (ct.InUserNs()) {
            error = cgroup.ChownRecursiveRootFiles(ct.UserNsCred);
            if (error)
                return error;
        }
    }

    static const struct {
        TPath path;
        TPath target;
    } symlinks[] = {
        {"sys/fs/cgroup/cpu", "cpu,cpuacct"},
        {"sys/fs/cgroup/cpuacct", "cpu,cpuacct"},
        {"sys/fs/cgroup/net_cls", "net_cls,net_prio"},
        {"sys/fs/cgroup/net_prio", "net_cls,net_prio"},
    };

    if (!CgroupDriver.UseCgroup2()) {
        for (auto &s: symlinks) {
            error = s.path.Symlink(s.target);
            if (error)
                return error;
        }
    }

    if (ct.InUserNs()) {
        error = tmpfs.ChownRecursiveRootFiles(ct.UserNsCred);
        if (error)
            return error;

        if (!CgroupDriver.UseCgroup2()) {
            for (auto &s: symlinks) {
                error = s.path.Lchown(ct.UserNsCred);
                if (error)
                    return error;
            }
        }
    }

    /* cgroup2 is mounted at /sys/fs/cgroup itself, keep it writable for rw cgroupfs */
    if (CgroupDriver.UseCgroup2() && rwCgroupFs && config().container().enable_rw_net_cgroups())
        return OK;

    return SetMountAttr(TFile(), tmpfs, {MOUNT_ATTR_RDONLY, 0, 0}, false);
}

TError TMountNamespace::SetupRoot(const TContainer &ct) {
    TPath dot(".");
    TError error;

    L_ACT("Setup root in {}", RootFd.RealPath());

    // mount proc in root dir
    error = ProcFd.OpenDirStrictAt(RootFd, "proc");
    if (error) {
        (void)RootFd.MkdirAt("proc", 0775);
        error = ProcFd.OpenDirStrictAt(RootFd, "proc");
    }
    if (error)
        return error;

    error = ProcFd.Chdir();
    if (error)
        return error;

    error = dot.Mount("proc", "proc", MS_NOSUID | MS_NOEXEC | MS_NODEV, {});
    if (error)
        return error;

    error = ProcFd.OpenDirStrictAt(RootFd, "proc");
    if (error)
        return error;

    if (ProcFd.FsType() != PROC_SUPER_MAGIC)
        return TError("Cannot open procfs");

    // return back to root dir
    error = RootFd.Chdir();
    if (error)
        return error;

    struct {
        TPath target;
        std::string type;
        unsigned long flags;
        std::vector<std::string> opts;
    } mounts[] = {
        {"dev", "tmpfs", MS_NOSUID | MS_STRICTATIME,
         {"mode=755", "size=" + std::to_string(config().container().dev_size())}},
        {"dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, {"newinstance", "ptmxmode=0666", "mode=620", "gid=5",
                                                      "max=" + std::to_string(config().container().devpts_max())}},
        {"sys", "sysfs", MS_NOSUID | MS_NOEXEC | MS_NODEV | MS_RDONLY, {}},
    };

    for (auto &m: mounts) {
        error = m.target.MkdirAll(0755);
        if (!error)
            error = m.target.Mount(m.type, m.type, m.flags, m.opts);
        if (error)
            return error;
    }

    error = MountRun(ct);
    if (error)
        return error;

    if (BindPortoSock) {
        TPath sock(PORTO_SOCKET_PATH);
        // TPath portoctl(PORTO_PORTOCTL_PATH);
        TPath dest = dot / sock;

        error = dest.Mkfile(0);
        if (error)
            return error;

        error = dest.Bind(sock);
        if (error)
            return error;
    }

    struct {
        TPath path;
        mode_t mode;
    } dirs[] = {
        {"run/lock", 01777},
        {"run/shm", 01777},
        {"dev/shm", 01777},
    };

    for (auto &d: dirs) {
        error = d.path.Mkdir(d.mode);
        if (error)
            return error;
    }

    struct {
        TPath path;
        TPath target;
    } symlinks[] = {
        {"dev/ptmx", "pts/ptmx"},          {"dev/fd", "/proc/self/fd"},       {"dev/stdin", "/proc/self/fd/0"},
        {"dev/stdout", "/proc/self/fd/1"}, {"dev/stderr", "/proc/self/fd/2"},
    };

    for (auto &s: symlinks) {
        error = s.path.Symlink(s.target);
        if (error)
            return error;
    }

    if (CgroupDriver.HugetlbSubsystem->Supported) {
        TPath path("dev/hugepages");
        error = path.Mkdir(0755);
        if (error)
            return error;
        error = path.Mount("hugetlbfs", "hugetlbfs", MS_NOSUID | MS_NODEV, {"mode=01777"});
        if (error)
            return error;
    }

    struct {
        TPath dst;
        TPath src;
        TMountAttr attr;
    } binds[] = {
        {"run/lock", "run/lock", {MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_NOEXEC, 0, 0}},
        {"dev/shm", "run/shm", {MOUNT_ATTR_NOSUID | MOUNT_ATTR_NODEV | MOUNT_ATTR_STRICTATIME, MOUNT_ATTR__ATIME, 0}},
    };

    for (auto &b: binds) {
        error = BindMount(b.src, b.dst, b.attr, false);
        if (error)
            return error;
    }

    error = MountTraceFs();
    if (error)
        L_WRN("Cannot mount tracefs: {}", error);

    if (ct.InUserNs()) {
        error = dot.ChownRecursiveRootFiles(ct.UserNsCred);
        if (error)
            return error;
    }

    return OK;
}

TError TMountNamespace::ProtectProc() {
    TError error;

    std::vector<TPath> proc_ro = {
        "sysrq-trigger",
        "irq",
        "bus",
        "sys",
    };

    error = ProcFd.Chdir();
    if (error)
        return error;

    for (auto &path: proc_ro) {
        error = BindMount(path, path, {MOUNT_ATTR_RDONLY, 0, 0}, false);
        if (error)
            return error;
    }

    error = BindMount("/dev/null", "kcore", {MOUNT_ATTR_RDONLY, 0, 0}, false);
    if (error)
        return error;

    error = RootFd.Chdir();
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::Setup(const TContainer &ct) {
    TPath dot(".");
    TError error;

    // remount as slave to receive propagations from parent namespace
    error = SetMountAttr(TFile(), "/", {0, 0, MS_SLAVE}, true);
    if (error)
        return error;

    error = RootFd.OpenDir(Root);
    if (error)
        return error;

    error = RootFd.Chdir();
    if (error)
        return error;

    // new root must be a different mounts
    if (!Root.IsRoot() && !RootFd.IsMountPoint()) {
        error = dot.Bind(dot, MS_REC);
        if (error)
            return error;
        error = RootFd.OpenDir(Root);
        if (error)
            return error;
        error = RootFd.Chdir();
        if (error)
            return error;
    }

    // allow suid binaries at root volume
    if (!Root.IsRoot()) {
        error = SetMountAttr(TFile(), dot, {0, MOUNT_ATTR_NOSUID, 0}, false);
        if (error)
            return error;
    }

    if (RootRo) {
        error = SetMountAttr(TFile(), dot, {MOUNT_ATTR_RDONLY, 0, 0}, false);
        if (error)
            return error;
    }

    // remount proc in pid-ns
    TPath proc("/proc");

    error = proc.UmountAll();
    if (error)
        return error;

    error = proc.Mount("proc", "proc", MS_NOEXEC | MS_NOSUID | MS_NODEV, {});
    if (error)
        return error;

    error = ProcFd.OpenDirStrict(proc);
    if (error)
        return error;

    if (ProcFd.FsType() != PROC_SUPER_MAGIC)
        return TError("Cannot open procfs");

    if (HostRoot.IsRoot()) {
        error = TPath("/sys/fs/cgroup").UmountAll();
        if (error)
            return error;

        error = TPath("/sys/fs/pstore").UmountAll();
        if (error)
            return error;

        error = TPath("/sys/kernel/security").UmountAll();
        if (error)
            return error;

        error = TPath(PORTO_CONTAINERS_KV).UmountAll();
        if (error)
            return error;

        error = TPath(PORTO_VOLUMES_KV).UmountAll();
        if (error)
            return error;

        error = TPath(PORTO_NBD_KV).UmountAll();
        if (error)
            return error;
    }

    if (Root.IsRoot()) {
        // always mount new sysfs
        TPath sys("/sys");

        error = sys.UmountAll();
        if (error)
            return error;

        error = sys.Mount("sysfs", "sysfs", MS_RDONLY | MS_NOSUID | MS_NOEXEC | MS_NODEV, {});
        if (error)
            return error;

        TPath tracefs = "/sys/kernel/tracing";
        if (config().container().enable_tracefs() && tracefs.Exists()) {
            error = tracefs.Mount("none", "tracefs", MS_NOEXEC | MS_NOSUID | MS_NODEV, {"mode=755"});
            if (error && error.Errno != EBUSY)
                L_SYS("Cannot mount tracefs: {}", error);
        }

        if (IsolateRun) {
            error = RemountRun(ct);
            if (error)
                return error;
        }
    } else {
        error = SetupRoot(ct);
        if (error)
            return error;
    }

    /* Open writable sysctl for later setup */
    error = ProcSysFd.OpenDirStrictAt(ProcFd, "sys");
    if (error)
        return error;

    error = ProtectProc();
    if (error)
        return error;

    if (ct.CgroupFs != ECgroupFs::None)
        error = MountCgroups(ct);
    else
        error = MountSystemd();

    if (error)
        return error;

    for (const auto &bind: BindMounts) {
        error = bind.Mount(BindCred, Root);
        if (error)
            return error;
    }

    if (!Root.IsRoot()) {
        error = RootFd.PivotRoot();
        if (error)
            return error;
    }

    for (const auto &link: Symlink) {
        error = CreateSymlink(link.first, link.second);
        if (error)
            return error;
    }

    // remount as shared: subcontainers will get propgation from us
    return SetMountAttr(TFile(), dot, {0, 0, MS_SHARED}, true);
}

TError TMountNamespace::Enter(pid_t pid) {
    TError error;

    error = HostNs.Open("/proc/thread-self/ns/mnt");
    if (error)
        return error;

    error = ContainerNs.Open(pid, "ns/mnt");
    if (error)
        return error;

    if (unshare(CLONE_FS))
        return TError::System("unshare(CLONE_FS)");

    error = ContainerNs.SetNs(CLONE_NEWNS);
    if (error)
        return error;

    error = TPath("/").Chdir();
    if (error) {
        PORTO_ASSERT(!Leave());
        return error;
    }

    return OK;
}

TError TMountNamespace::Leave() {
    TError error;

    error = HostNs.SetNs(CLONE_NEWNS);
    if (error)
        return error;

    error = TPath("/").Chdir();
    if (error)
        return error;

    return OK;
}

TError TMountNamespace::CreateSymlink(const TPath &symlink, const TPath &target) {
    TPath sym = symlink.AbsolutePath(Cwd).NormalPath();
    TPath sym_dir = sym.DirNameNormal();
    TPath sym_name = sym.BaseNameNormal();
    TError error;
    TFile dir;

    for (auto &name: sym_dir.Components()) {
        error = dir ? dir.OpenDirAt(dir, name) : dir.OpenDir(name);
        if (!error)
            continue;
        if (error.Errno != ENOENT)
            return error;
        if (Root.IsRoot()) {
            error = dir.WriteAccess(BindCred);
            if (error)
                return error;
        }
        error = dir.MkdirAt(name, 0775);
        if (error)
            return error;
        error = dir.OpenDirStrictAt(dir, name);
        if (error)
            return error;
        error = dir.Chown(BindCred);
        if (error)
            return error;
    }

    if (Root.IsRoot()) {
        error = dir.WriteAccess(BindCred);
        if (error)
            return error;
    }

    TPath tgt = target.AbsolutePath(Cwd).NormalPath().RelativePath(sym_dir);
    TPath cur_tgt;

    if (!dir.ReadlinkAt(sym_name, cur_tgt)) {
        if (!target) {
            L_ACT("symlink {} remove {}", sym, cur_tgt);
            error = dir.UnlinkAt(sym_name);
        } else if (cur_tgt == tgt) {
            L_ACT("symlink {} already points to {}", sym, tgt);
            // note there is no error variable using,
            // so error from calls above could be propagated in return statement
        } else {
            L_ACT("symlink {} replace {} with {}", sym, cur_tgt, tgt);
            TPath sym_next = ".next_" + sym_name.ToString();
            (void)dir.UnlinkAt(sym_next);
            error = dir.SymlinkAt(sym_next, tgt);
            if (!error)
                error = dir.ChownAt(sym_next, BindCred);
            if (!error)
                error = dir.RenameAt(sym_next, sym_name);
        }
    } else {
        L_ACT("symlink {} to {}", sym, tgt);
        error = dir.SymlinkAt(sym_name, tgt);
        if (!error)
            error = dir.ChownAt(sym_name, BindCred);
    }

    return error;
}
