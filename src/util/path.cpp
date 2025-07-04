#include "path.hpp"

#include <algorithm>
#include <sstream>

#include "util/log.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <linux/falloc.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/magic.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
}

#ifndef FALLOC_FL_COLLAPSE_RANGE
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#endif

constexpr off_t ROTATE_OFFSET_LIMIT = 16384;  // 16Kb

static TError umountAll(const char *path, int flags) {
    flags |= UMOUNT_NOFOLLOW;
    while (1) {
        if (umount2(path, flags)) {
            if (errno == EINVAL || errno == ENOENT)
                return OK; /* not a mountpoint */

            if (errno == EBUSY && !(flags & MNT_DETACH))
                flags |= MNT_DETACH;
            else
                return TError::System("umount2({})", path);
        }
    }
}

// TODO(ovov): maybe use version from gnulib with fts_cwd_fd:
// https://git.savannah.gnu.org/cgit/gnulib.git/tree/lib/fts_.h?id=d4ec02b3cc70cddaaa5183cc5a45814e0afb2292#n133
// With such approach we dont need chdir to handle long path names
static TError RemoveRecursive(const TPath &path, bool remove_root, std::atomic_bool *interrupt = nullptr,
                              bool doumount = false) {
    TPathWalk walk;

    auto error = walk.Open(path, FTS_PHYSICAL | FTS_XDEV | FTS_NOSTAT);
    if (error)
        return error;

    if (unshare(CLONE_FS) < 0)
        return TError::System("unshare(CLONE_FS)");

    while (1) {
        error = walk.Next();
        if (error || !walk.Path)
            return error;

        if (interrupt && *interrupt)
            return TError(EError::SocketError, "RemoveRecursive was interrupted");

        const char *pathname = !walk.Level() ? walk.Ent->fts_path : walk.Ent->fts_name;

        if (doumount && (!walk.Directory || !walk.Postorder)) {
            auto error = umountAll(pathname, MNT_DETACH);
            if (error)
                return error;
        }

        if (walk.Directory) {
            if (!walk.Postorder)
                continue;

            if (walk.Level() == 0 && !remove_root)
                continue;
        }

        int unlink_flags = walk.Directory ? AT_REMOVEDIR : 0;
        if (unlinkat(AT_FDCWD, pathname, unlink_flags) < 0) {
            if (errno != ENOENT)
                return TError::System("unlinkat");
        }
    }
}

void TStatFS::Init(const struct statfs &st) {
    SpaceUsage = (uint64_t)(st.f_blocks - st.f_bfree) * st.f_bsize;
    SpaceAvail = (uint64_t)st.f_bavail * st.f_bsize;
    InodeUsage = st.f_files - st.f_ffree;
    InodeAvail = st.f_ffree;
    MntFlags = ((st.f_flags & ST_NODEV) ? (uint64_t)MS_NODEV : MS_ALLOW_DEV) |
               ((st.f_flags & ST_NOEXEC) ? (uint64_t)MS_NOEXEC : MS_ALLOW_EXEC) |
               ((st.f_flags & ST_NOSUID) ? (uint64_t)MS_NOSUID : MS_ALLOW_SUID) |
               ((st.f_flags & ST_RDONLY) ? (uint64_t)MS_RDONLY : MS_ALLOW_WRITE);
    FsType = st.f_type;
}

void TStatFS::Reset() {
    SpaceUsage = SpaceAvail = InodeUsage = InodeAvail = 0;
    MntFlags = 0;
}

struct FileHandle {
    struct file_handle head;
    char data[MAX_HANDLE_SZ];
    FileHandle() {
        head.handle_bytes = MAX_HANDLE_SZ;
        head.handle_type = 0;
    }
};

TPath TPath::DirNameNormal() const {
    auto sep = Path.rfind('/');
    if (sep == std::string::npos)
        return Path.empty() ? "" : ".";
    if (sep == 0)
        return "/";
    return Path.substr(0, sep);
}

std::string TPath::BaseNameNormal() const {
    auto sep = Path.rfind('/');
    if (sep == std::string::npos || Path.size() == 1)
        return Path;
    return Path.substr(sep + 1);
}

TPath TPath::DirName() const {
    return NormalPath().DirNameNormal();
}

std::string TPath::BaseName() const {
    return NormalPath().BaseNameNormal();
}

TError TPath::StatStrict(struct stat &st) const {
    if (lstat(Path.c_str(), &st))
        return TError::System("lstat " + Path);
    return OK;
}

TError TPath::StatFollow(struct stat &st) const {
    if (stat(Path.c_str(), &st))
        return TError::System("stat " + Path);
    return OK;
}

bool TPath::IsRegularStrict() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISREG(st.st_mode);
}

bool TPath::IsRegularFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISREG(st.st_mode);
}

bool TPath::IsDirectoryStrict() const {
    struct stat st;
    return !lstat(c_str(), &st) && S_ISDIR(st.st_mode);
}

bool TPath::IsDirectoryFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISDIR(st.st_mode);
}

bool TPath::IsSocketFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISSOCK(st.st_mode);
}

bool TPath::IsCharFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISCHR(st.st_mode);
}

bool TPath::IsBlockFollow() const {
    struct stat st;
    return !stat(c_str(), &st) && S_ISBLK(st.st_mode);
}

bool TPath::IsSameInode(const TPath &other) const {
    struct stat a, b;
    if (stat(c_str(), &a) || stat(other.c_str(), &b))
        return false;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

bool TPath::IsMountPoint() const {
    TFile f;
    auto error = f.OpenPath(Path);
    if (error) {
        if (error.Errno != ENOENT)
            L_WRN("OpenPath('{}') failed during IsMountPoint check: {}", error);
        return false;
    }
    return f.IsMountPoint();
}

dev_t TPath::GetDev() const {
    struct stat st;

    if (stat(Path.c_str(), &st))
        return 0;

    return st.st_dev;
}

dev_t TPath::GetBlockDev() const {
    struct stat st;

    if (stat(Path.c_str(), &st) || !S_ISBLK(st.st_mode))
        return 0;

    return st.st_rdev;
}

TError TPath::GetDevName(dev_t dev, std::string &name) {
    TPath block = fmt::format("/sys/dev/block/{}:{}", major(dev), minor(dev));
    TError error;
    TPath link;

    error = block.ReadLink(link);
    if (error)
        return error;

    if ((block / "partition").Exists())
        name = link.DirNameNormal().BaseNameNormal();
    else
        name = link.BaseNameNormal();

    return OK;
}

bool TPath::Exists() const {
    return access(Path.c_str(), F_OK) == 0;
}

bool TPath::PathExists() const {
    struct stat st;
    return lstat(Path.c_str(), &st) == 0;
}

std::string TPath::ToString() const {
    return Path;
}

TPath TPath::AddComponent(const TPath &component) const {
    if (component.IsAbsolute()) {
        if (IsRoot())
            return TPath(component.Path);
        if (component.IsRoot())
            return TPath(Path);
        return TPath(Path + component.Path);
    }
    if (IsRoot())
        return TPath("/" + component.Path);
    if (component.IsEmpty())
        return TPath(Path);
    return TPath(Path + "/" + component.Path);
}

std::vector<std::string> TPath::Components() const {
    std::vector<std::string> result;
    std::stringstream ss(Path);
    std::string component;

    if (IsAbsolute())
        result.push_back("/");

    while (std::getline(ss, component, '/')) {
        if (component != "")
            result.push_back(component);
    }

    return result;
}

TError TPath::Chdir() const {
    if (unshare(CLONE_FS))
        return TError::System("unshare(CLONE_FS)");
    if (chdir(Path.c_str()) < 0)
        return TError(EError::InvalidPath, errno, "Cannot chdir {}", Path);
    return OK;
}

TError TPath::Chroot() const {
    L_ACT("chroot {}", Path);

    if (chroot(Path.c_str()) < 0)
        return TError::System("chroot(" + Path + ")");

    return OK;
}

TError TFile::Chroot() const {
    if (unshare(CLONE_FS))
        return TError::System("unshare(CLONE_FS)");
    if (fchdir(Fd))
        return TError::System("fchdir");
    if (chroot("."))
        return TError::System("chroot");
    return OK;
}

// https://github.com/lxc/lxc/commit/2d489f9e87fa0cccd8a1762680a43eeff2fe1b6e
TError TFile::PivotRoot() const {
    TFile oldroot;
    TError error;

    L_ACT("pivot root {}", RealPath());

    error = oldroot.OpenDir("/");
    if (error)
        return error;

    if (fchdir(Fd))
        return TError::System("fchdir(newroot)");

    if (syscall(__NR_pivot_root, ".", "."))
        return TError::System("pivot_root()");

    if (fchdir(oldroot.Fd) < 0)
        return TError::System("fchdir(oldroot)");

    if (umount2(".", MNT_DETACH) < 0)
        return TError::System("umount2(.)");

    if (fchdir(Fd) < 0)
        return TError::System("fchdir(newroot) reenter");

    return OK;
}

TError TPath::Chown(uid_t uid, gid_t gid) const {
    if (chown(Path.c_str(), uid, gid))
        return TError::System("chown(" + Path + ", " + std::to_string(uid) + ", " + std::to_string(gid) + ")");
    return OK;
}

TError TPath::Lchown(uid_t uid, gid_t gid) const {
    if (lchown(Path.c_str(), uid, gid))
        return TError::System("lchown(" + Path + ", " + std::to_string(uid) + ", " + std::to_string(gid) + ")");
    return OK;
}

TError TPath::ChownRecursive(uid_t uid, gid_t gid, TChownFilter filter) const {
    TPathWalk walk;
    TError error;

    error = walk.Open(*this);
    if (error)
        return error;

    while (1) {
        error = walk.Next();
        if (error || !walk.Path)
            return error;

        uid_t chownUid = uid;
        gid_t chownGid = gid;
        if (filter && walk.Stat)
            filter(*walk.Stat, chownUid, chownGid);

        if (chownUid != (uid_t)-1 || chownGid != (gid_t)-1) {
            if (walk.Stat && S_ISLNK(walk.Stat->st_mode))
                error = walk.Path.Lchown(chownUid, chownGid);
            else {
                int mode = walk.Stat ? walk.Stat->st_mode : 0;
                error = walk.Path.Chown(chownUid, chownGid);
                if (!error && mode)
                    error = walk.Path.Chmod(mode);
            }
            if (error && errno != ENOENT && errno != EROFS)
                return error;
        }
    }

    return OK;
}

TError TPath::Chmod(const int mode) const {
    int ret = chmod(Path.c_str(), mode);
    if (ret)
        return TError::System("chmod(" + Path + ", " + StringFormat("%#o", mode) + ")");

    return OK;
}

TError TPath::ReadLink(TPath &value) const {
    char buf[PATH_MAX];
    ssize_t len;

    len = readlink(Path.c_str(), buf, sizeof(buf) - 1);
    if (len < 0)
        return TError::System("readlink(" + Path + ")");

    buf[len] = '\0';

    value = TPath(buf);
    return OK;
}

TError TPath::Hardlink(const TPath &target) const {
    int ret = link(target.c_str(), Path.c_str());
    if (ret)
        return TError::System("link(" + target.ToString() + ", " + Path + ")");
    return OK;
}

TError TPath::Symlink(const TPath &target) const {
    int ret = symlink(target.c_str(), Path.c_str());
    if (ret)
        return TError::System("symlink(" + target.ToString() + ", " + Path + ")");
    return OK;
}

TError TPath::Mknod(unsigned int mode, unsigned int dev) const {
    int ret = mknod(Path.c_str(), mode, dev);
    if (ret)
        return TError::System("mknod(" + Path + ", " + StringFormat("%#o", mode) + ", " + StringFormat("%#x", dev) +
                              ")");
    return OK;
}

TError TPath::Mkfile(unsigned int mode) const {
    return Mknod(S_IFREG | (mode & 0777), 0);
}

TPath TPath::NormalPath() const {
    std::stringstream ss(Path);
    std::string component, path;

    if (IsEmpty())
        return TPath();

    if (IsAbsolute())
        path = "/";

    while (std::getline(ss, component, '/')) {
        if (component == "" || component == ".")
            continue;

        if (component == "..") {
            auto last = path.rfind('/');

            if (last == std::string::npos) {
                /* a/.. */
                if (!path.empty() && path != "..") {
                    path = "";
                    continue;
                }
            } else if (path.compare(last + 1, std::string::npos, "..") != 0) {
                if (last == 0)
                    path.erase(last + 1); /* /.. or /a/.. */
                else
                    path.erase(last); /* a/b/.. */
                continue;
            }
        }

        if (!path.empty() && path != "/")
            path += "/";
        path += component;
    }

    if (path.empty())
        path = ".";

    return TPath(path);
}

TPath TPath::AbsolutePath(const TPath &base) const {
    if (IsAbsolute() || IsEmpty())
        return TPath(Path);

    if (base)
        return base / Path;

    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd)))
        return TPath();

    return TPath(cwd) / Path;
}

TPath TPath::RelativePath(const TPath &base) const {
    if (!IsAbsolute() || !base.IsAbsolute())
        return TPath();

    std::string rel = NormalPath().Path;
    std::string pre = base.NormalPath().Path;

    while (pre.size()) {
        auto a = pre.find('/');
        auto b = rel.find('/');
        if (pre.substr(0, a) != rel.substr(0, b))
            break;
        pre = a != std::string::npos ? pre.substr(a + 1) : "";
        rel = b != std::string::npos ? rel.substr(b + 1) : "";
    }

    while (pre.size()) {
        auto a = pre.find('/');
        pre = a != std::string::npos ? pre.substr(a + 1) : "";
        rel = rel.size() ? "../" + rel : "..";
    }

    return rel.size() ? rel : ".";
}

TPath TPath::RealPath() const {
    char *p = realpath(Path.c_str(), NULL);
    if (!p)
        return Path;

    TPath path(p);

    free(p);
    return path;
}

/*
 * Returns relative or absolute path inside this or
 * empty path if argument path is not inside:
 *
 * "/root".InnerPath("/root/foo", true) -> "/foo"
 * "/root".InnerPath("/foo", true) -> ""
 */
TPath TPath::InnerPath(const TPath &path, bool absolute) const {
    unsigned len = Path.length();

    /* check prefix */
    if (!len || path.Path.compare(0, len, Path) != 0)
        return TPath();

    /* exact match */
    if (path.Path.length() == len) {
        if (absolute)
            return TPath("/");
        else
            return TPath(".");
    }

    /* prefix "/" act as "" */
    if (len == 1 && Path[0] == '/')
        len = 0;

    /* '/' must follow prefix */
    if (path.Path[len] != '/')
        return TPath();

    /* cut prefix */
    if (absolute)
        return TPath(path.Path.substr(len));
    else
        return TPath(path.Path.substr(len + 1));
}

bool TPath::IsInside(const TPath &base) const {
    return !base.InnerPath(*this).IsEmpty();
}

TError TPath::StatFS(TStatFS &result) const {
    struct statfs st;
    if (statfs(Path.c_str(), &st))
        return TError::System("statfs(" + Path + ")");
    result.Init(st);
    return OK;
}

TError TPath::Unlink() const {
    if (unlink(c_str()))
        return TError::System("unlink(" + Path + ")");
    return OK;
}

TError TPath::Rename(const TPath &dest) const {
    if (rename(c_str(), dest.c_str()))
        return TError::System("rename(" + Path + ", " + dest.Path + ")");
    return OK;
}

TError TPath::Mkdir(unsigned int mode) const {
    if (mkdir(Path.c_str(), mode) < 0)
        return TError(errno == ENOSPC ? EError::NoSpace : EError::Unknown, errno,
                      "mkdir(" + Path + ", " + StringFormat("%#o", mode) + ")");
    return OK;
}

TError TPath::MkdirAll(unsigned int mode, bool ignore) const {
    std::vector<TPath> paths;
    TPath path(Path);
    TError error;

    while (!path.Exists()) {
        paths.push_back(path);
        path = path.DirName();
    }

    if (!path.IsDirectoryFollow())
        return TError("Not a directory: {}", path);

    for (auto path = paths.rbegin(); path != paths.rend(); path++) {
        error = path->Mkdir(mode);
        if (error && (!ignore || error.Errno != EEXIST))
            return error;
    }

    return OK;
}

TError TPath::MkdirTmp(const TPath &parent, const std::string &prefix, unsigned int mode) {
    Path = (parent / (prefix + "XXXXXX")).Path;
    if (!mkdtemp(&Path[0]))
        return TError::System("mkdtemp(" + Path + ")");
    if (mode != 0700)
        return Chmod(mode);
    return OK;
}

TError TPath::CreateTmpDirInplace() {
    char *tmpDirPathAddr = mkdtemp(&Path[0]);
    if (tmpDirPathAddr == NULL)
        return TError::System("Can`t create tmp dir: {}", Path);

    return OK;
}

TError TPath::Rmdir() const {
    if (rmdir(Path.c_str()) < 0)
        return TError::System("rmdir(" + Path + ")");
    return OK;
}

/*
 * Removes everything in the directory but not directory itself.
 * Works only on one filesystem and aborts if sees mountpint.
 */
TError TPath::ClearDirectory() const {
    return RemoveRecursive(*this, false);
}

TError TFile::ClearDirectory() const {
    return RealPath().ClearDirectory();
}

TError TFile::RemoveAt(const TPath &path) const {
    TError error;
    TFile dir;

    error = dir.OpenAt(*this, path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOCTTY | O_NOFOLLOW, 0);
    if (error) {
        error = UnlinkAt(path);
    } else {
        error = RemoveRecursive(dir.RealPath(), true);
    }
    return error;
}

TError TFile::RemoveAllAtInterruptible(const TPath &path, std::atomic_bool &interrupt, bool umount) const {
    return RemoveRecursive(ProcPath() / path, true, &interrupt, umount);
}

TError TPath::RemoveAll() const {
    return RemoveRecursive(*this, true);
}

TError TPath::ClearEmptyDirectories(const TPath &root) const {
    TError error;

    for (TPath path = Path; path != root; path = path.DirName()) {
        error = path.Rmdir();
        if (error) {
            if (error.Errno != ENOTEMPTY)
                return error;
            break;
        }
    }

    return OK;
}

TError TPath::ReadDirectory(std::vector<std::string> &result) const {
    struct dirent *de;
    DIR *dir;

    result.clear();
    dir = opendir(c_str());
    if (!dir)
        return TError::System("Cannot open directory " + Path);

    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            result.push_back(std::string(de->d_name));
    }
    closedir(dir);
    return OK;
}

TError TPath::ListSubdirs(std::vector<std::string> &result) const {
    struct dirent *de;
    DIR *dir;

    result.clear();
    dir = opendir(c_str());
    if (!dir)
        return TError::System("Cannot open directory " + Path);

    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..") &&
            (de->d_type == DT_DIR ||
             (de->d_type == DT_UNKNOWN && (*this / std::string(de->d_name)).IsDirectoryStrict())))
            result.push_back(std::string(de->d_name));
    }
    closedir(dir);
    return OK;
}

int64_t TPath::SinceModificationMs() const {
    struct stat st;

    if (lstat(Path.c_str(), &st))
        return -1;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 - (int64_t)st.st_mtim.tv_sec * 1000 -
           st.st_mtim.tv_nsec / 1000000;
}

TError TPath::GetXAttr(const std::string &name, std::string &value) const {
    ssize_t size = syscall(SYS_lgetxattr, Path.c_str(), name.c_str(), nullptr, 0);
    if (size >= 0) {
        value.resize(size);
        if (syscall(SYS_lgetxattr, Path.c_str(), name.c_str(), value.c_str(), size) >= 0)
            return OK;
    }
    return TError::System("getxattr(" + Path + ", " + name + ")");
}

TError TPath::SetXAttr(const std::string &name, const std::string &value) const {
    if (syscall(SYS_setxattr, Path.c_str(), name.c_str(), value.c_str(), value.length(), 0))
        return TError::System("setxattr {} {}", Path, name);
    return OK;
}

TError TPath::Truncate(off_t size) const {
    if (truncate(c_str(), size))
        return TError::System("truncate(" + Path + ")");
    return OK;
}

TError TPath::RotateLog(off_t max_disk_usage, off_t &loss) const {
    struct stat st;
    off_t hole_len;
    size_t pos = 0;
    TError error;
    TFile file;
    std::string buf;

    error = file.Open(*this, O_RDWR | O_CLOEXEC | O_NOCTTY);
    if (error)
        return error;

    error = file.Stat(st);
    if (error)
        return error;

    if (!S_ISREG(st.st_mode) || (st.st_size <= max_disk_usage) || (st.st_size <= st.st_blksize)) {
        loss = 0;
        return OK;
    }

    /* Keep half of allowed size and block size alignment */
    hole_len = st.st_size - max_disk_usage / 2;
    hole_len -= hole_len % st.st_blksize;
    loss = hole_len;

    /* Try to find endline and extend hole */
    error = file.Lseek(hole_len, SEEK_SET);
    if (!error) {
        buf.reserve(std::min(max_disk_usage / 2, ROTATE_OFFSET_LIMIT));
        error = file.Read(buf);
        if (!error) {
            pos = buf.find_first_of('\n');
            if (pos != std::string::npos)
                pos++;
        }

        file.Lseek(0, SEEK_SET);
    }

    /* Hole collapse */
    if (fallocate(file.Fd, FALLOC_FL_COLLAPSE_RANGE, 0, hole_len)) {
        /* Truncate to zero */
        L_WRN("Cannot collapse hole: {} {}", Path, strerror(errno));
        loss = st.st_size;
        error = file.Truncate(0);
        if (error)
            return error;
    }

    /* Space is ignored by json parser, for example, "k8s.io/kubernetes/pkg/kubelet/kuberuntime/logs" */
    if (pos > 0 && pos != std::string::npos) {
        buf.resize(pos);
        buf.assign(pos, ' ');
        error = file.WriteAll(buf);
        if (error)
            return error;
    }

    return OK;
}

TError TPath::Chattr(unsigned add_flags, unsigned del_flags) const {
    TError error;
    TFile file;

    error = file.Open(*this, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NOCTTY | O_NONBLOCK);
    if (error)
        return error;
    error = TFile::Chattr(file.Fd, add_flags, del_flags);
    if (error)
        return TError(error, "Cannot chattr {}", Path);
    return OK;
}

TError TPath::Touch() const {
    if (utimes(c_str(), NULL))
        return TError::System("utimes " + Path);
    return OK;
}

static const TFlagsNames MountFlags = {
    {MS_ALLOW_WRITE, "rw"},
    {MS_RDONLY, "ro"},
    {MS_ALLOW_SUID, "suid"},
    {MS_NOSUID, "nosuid"},
    {MS_ALLOW_DEV, "dev"},
    {MS_NODEV, "nodev"},
    {MS_ALLOW_EXEC, "exec"},
    {MS_NOEXEC, "noexec"},
    {MS_SYNCHRONOUS, "sync"},
    {MS_REMOUNT, "remount"},
    {MS_MANDLOCK, "mand"},
    {MS_DIRSYNC, "dirsync"},
    {MS_NOATIME, "noatime"},
    {MS_NODIRATIME, "nodiratime"},
    {MS_BIND, "bind"},
    {MS_MOVE, "move"},
    {MS_REC, "rec"},
    {MS_SILENT, "silent"},
    {MS_POSIXACL, "acl"},
    {MS_UNBINDABLE, "unbindable"},
    {MS_PRIVATE, "private"},
    {MS_SLAVE, "slave"},
    {MS_SHARED, "shared"},
    {MS_RELATIME, "relatime"},
    {MS_I_VERSION, "iversion"},
    {MS_STRICTATIME, "strictatime"},
    {MS_LAZYTIME, "lazyatime"},
};

static const TFlagsNames UmountFlags = {
    {MNT_FORCE, "force"},
    {MNT_DETACH, "detach"},
    {MNT_EXPIRE, "expire"},
    {UMOUNT_NOFOLLOW, "nofollow"},
};

TError TMount::ParseFlags(const std::string &str, uint64_t &mnt_flags, uint64_t allowed)
{
    TError error = StringParseFlags(str, MountFlags, mnt_flags);
    if (error)
        return error;
    if (mnt_flags & ~allowed)
        return TError(EError::InvalidValue, "Not allowed flags {}", TMount::FormatFlags(mnt_flags & ~allowed));
    return OK;
}

std::string TMount::FormatFlags(uint64_t mnt_flags) {
    return StringFormatFlags(mnt_flags, MountFlags);
}

std::string TPath::UmountFlagsToString(uint64_t mnt_flags) {
    return StringFormatFlags(mnt_flags, UmountFlags);
}

TError TPath::Mount(const TPath &source, const std::string &type, uint64_t mnt_flags,
                    const std::vector<std::string> &options) const {
    std::string data = MergeEscapeStrings(options, ',');

    if (data.length() >= 4096)
        return TError(EError::Unknown, E2BIG, "mount option too big: " + std::to_string(data.length()));

    L_ACT("mount {} -t {} {} -o {} {}", Path, type, source, data, TMount::FormatFlags(mnt_flags));

    if (mount(source.c_str(), Path.c_str(), type.c_str(), (uint32_t)mnt_flags, data.c_str()))
        return TError::System("mount({}, {}, {}, {}, {}", source, Path, type, TMount::FormatFlags(mnt_flags), data);

    return OK;
}

TError TPath::SecureTmpfsMount(const TCred &cred, size_t size) const {
    TError error;
    TMount mount;

    if (!IsDirectoryStrict()) {
        (void)Unlink();
        error = MkdirAll(0755);
        if (error)
            return error;
    }

    error = FindMount(mount);
    if (error || mount.Target != *this) {
        return Mount("tmpfs", "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV,
                     {"size=" + std::to_string(size), "mode=0750", "uid=" + std::to_string(cred.GetUid()),
                      "gid=" + std::to_string(cred.GetGid())});
    } else if (mount.Type != "tmpfs")
        return TError("found non-tmpfs mount at {}", *this);

    return error;
}

TError TPath::MoveMount(const TPath &target) const {
    L_ACT("mount move {} to {}", Path, target);
    if (mount(c_str(), target.c_str(), NULL, MS_MOVE, NULL))
        return TError::System("mount({}, {}, MS_MOVE)", Path, target);
    return OK;
}

TError TPath::Bind(const TPath &source, uint64_t mnt_flags) const {
    L_ACT("mount bind {} {} {}", Path, source, TMount::FormatFlags(mnt_flags));
    if (mount(source.c_str(), Path.c_str(), NULL, MS_BIND | (uint32_t)mnt_flags, NULL))
        return TError::System("mount({}, {}, {})", source, Path, TMount::FormatFlags(MS_BIND | mnt_flags));
    return OK;
}

TError TPath::Remount(uint64_t mnt_flags) const {
    if (!(mnt_flags & MS_SILENT))
        L_ACT("remount {} {}", Path, TMount::FormatFlags(mnt_flags));

    uint32_t recursive = mnt_flags & MS_REC;

    if (mnt_flags & MS_PRIVATE) {
        if (mount(NULL, Path.c_str(), NULL, MS_PRIVATE | recursive, NULL))
            return TError::System("Remount {} MS_PRIVATE", Path);
    }

    if (mnt_flags & MS_SLAVE) {
        if (mount(NULL, Path.c_str(), NULL, MS_SLAVE | recursive, NULL))
            return TError::System("Remount {} MS_SLAVE", Path);
    }

    if (mnt_flags & MS_SHARED) {
        if (mount(NULL, Path.c_str(), NULL, MS_SHARED | recursive, NULL))
            return TError::System("Remount {} MS_SHARED", Path);
    }

    if (mnt_flags & MS_UNBINDABLE) {
        if (mount(NULL, Path.c_str(), NULL, MS_UNBINDABLE | recursive, NULL))
            return TError::System("Remount {} MS_UNBINDABLE", Path);
    }

    uint64_t remount_flags = mnt_flags & ~(uint64_t)(MS_UNBINDABLE | MS_PRIVATE | MS_SLAVE | MS_SHARED | MS_REC);

    /* vfsmount remount isn't recursive in kernel */
    if (recursive && (remount_flags & MS_BIND)) {
        TPath normal = NormalPath();
        std::vector<TMount> mounts;
        TError error = TPath::ListAllMounts(mounts);
        if (error)
            return error;
        for (auto &mnt: mounts) {
            if (mnt.Target.IsInside(normal) && mnt.Target != normal) {
                error = mnt.Target.Remount(remount_flags | MS_SILENT);
                if (error) {
                    TFile dst;
                    TError error2 = dst.OpenPath(mnt.Target);
                    if (error2)
                        L("cannot remount {} {} and open {}", mnt.Target, error, error2);
                    else if (dst.GetMountId() != mnt.MountId)
                        L("cannot remount {} {} different mount id", mnt.Target, error);
                    else
                        return error;
                }
            }
        }
    }

    if (remount_flags) {
        struct statfs st;
        if (statfs(Path.c_str(), &st))
            return TError::System("statfs {}", Path);

        /* preserve ro,nodev,noexec,nosuid */
        if ((st.f_flags & ST_RDONLY) && !(MS_ALLOW_WRITE & remount_flags))
            remount_flags |= MS_RDONLY;
        if ((st.f_flags & ST_NODEV) && !(MS_ALLOW_DEV & remount_flags))
            remount_flags |= MS_NODEV;
        if ((st.f_flags & ST_NOEXEC) && !(MS_ALLOW_EXEC & remount_flags))
            remount_flags |= MS_NOEXEC;
        if ((st.f_flags & ST_NOSUID) && !(MS_ALLOW_SUID & remount_flags))
            remount_flags |= MS_NOSUID;

        if (mount(NULL, Path.c_str(), NULL, MS_REMOUNT | (uint32_t)remount_flags, NULL))
            return TError::System("Remount {} {}", Path, TMount::FormatFlags(remount_flags));
    }

    return OK;
}

TError TPath::BindRemount(const TPath &source, uint64_t mnt_flags) const {
    TError error;

    error = Bind(source, mnt_flags & MS_REC);
    if (error)
        return error;

    error = Remount(MS_BIND | mnt_flags);
    if (error)
        return error;

    return OK;
}

TError TPath::Umount(uint64_t flags) const {
    L_ACT("umount {} {}", Path, UmountFlagsToString(flags));
    if (!umount2(Path.c_str(), flags))
        return OK;
    if (errno == EBUSY)
        return TError(EError::Busy, "Mount is busy: " + Path);
    if (errno == EINVAL || errno == ENOENT)
        return TError(EError::NotFound, "Mount not found: " + Path);
    return TError::System("umount2({}, {})", Path, UmountFlagsToString(flags));
}

TError TPath::UmountAll(int flags) const {
    L_ACT("umount all {}", Path);
    return umountAll(c_str(), flags);
}

TError TPath::UmountNested() const {
    std::vector<TMount> mounts;
    TError error = ListAllMounts(mounts);
    if (error)
        return error;

    for (auto it = mounts.rbegin(); it != mounts.rend(); ++it) {
        if (it->Target.IsInside(*this)) {
            error = it->Target.UmountAll();
            if (error)
                L_WRN("Cannot umount {} {}", it->Target, error);
        }
    }

    return OK;
}

TError TPath::ReadAll(std::string &text, size_t max) const {
    TError error;
    TFile file;

    error = file.OpenRead(*this);
    if (error)
        return error;

    error = file.ReadAll(text, max);
    if (error)
        return TError(error, "Cannot read {}", Path);

    return OK;
}

TError TPath::WriteAll(const std::string &text) const {
    TError error;
    TFile file;

    error = file.OpenTrunc(*this);
    if (error)
        return error;

    error = file.WriteAll(text);
    if (error)
        return TError(error, "Cannot write {}", Path);

    return OK;
}

TError TPath::WriteAtomic(const std::string &text) const {
    TError error;
    TFile file;

    TPath temp = Path + ".XXXXXX";
    error = file.CreateTemporary(temp);
    if (!error) {
        error = file.WriteAll(text);
        if (!error)
            error = file.Chmod(0644);
        if (!error)
            error = temp.Rename(*this);
        if (error)
            (void)temp.Unlink();
    }
    return error;
}

TError TPath::CreateRegular() const {
    TError error;

    if (!Exists()) {
        error = DirName().MkdirAll(0755);
        if (!error)
            error = Mkfile(0644);
    } else if (!IsRegularStrict())
        error = TError(EError::InvalidValue, "non-regular file " + Path);

    return error;
}

TError TPath::WritePrivate(const std::string &text) const {
    TError error;
    TFile file;

    error = CreateRegular();
    if (error)
        return error;

    TPath temp = "/run/" + BaseName() + ".XXXXXX";
    error = file.CreateTemporary(temp);
    if (error)
        return error;

    error = temp.WriteAll(text);
    if (!error)
        error = file.Chmod(0644);
    if (!error)
        error = UmountAll();
    if (!error)
        error = Bind(file.ProcPath());
    (void)temp.Unlink();
    return error;
}

TError TPath::CreateAndWriteAll(const std::string &text) const {
    TError error;

    error = CreateRegular();
    if (error)
        return error;

    return WriteAll(text);
}

TError TPath::ReadLines(std::vector<std::string> &lines, size_t max) const {
    TFile file;
    auto error = file.OpenRead(*this);
    if (error)
        return error;
    return file.ReadLines(lines, max);
}

TError TPath::WriteLines(const std::vector<std::string> &lines) const {
    std::stringstream ss;

    for (const std::string &line: lines)
        ss << line << '\n';

    TError error = WriteAll(ss.str());
    if (error)
        return error;

    return OK;
}

TError TPath::ReadInt(int &value) const {
    std::string text;
    TError error = ReadAll(text);
    if (!error)
        error = StringToInt(text, value);
    return error;
}

TError TPath::ReadUint64(uint64_t &value) const {
    std::string text;
    TError error = ReadAll(text);
    if (!error)
        error = StringToUint64(text, value);
    return error;
}

TError TPath::FindMount(TMount &mount, bool exact) const {
    std::vector<std::string> lines;

    TError error = TPath("/proc/self/mountinfo").ReadLines(lines, MOUNT_INFO_LIMIT);
    if (error)
        return error;

    auto device = GetDev();
    if (!device)
        return TError(EError::NotFound, "Device not found: {}", Path);

    TPath normal = NormalPath();
    bool found = false;

    for (auto &line: lines) {
        TMount mnt;

        error = mnt.ParseMountinfo(line);
        if (error)
            return error;

        if (exact && mnt.Target != normal)
            continue;

        if (normal.IsInside(mnt.Target) && (mnt.Target.GetDev() == device || mnt.Source.GetBlockDev() == device)) {
            mount = mnt;
            found = true;
            /* get last matching mountpoint */
        }
    }

    if (!found)
        return TError(EError::NotFound, "Mount not found: {}", Path);

    return OK;
}

TError TPath::ListMountsByFilter(std::vector<TMount> &list, pid_t pid, std::function<bool(const TMount &)> filter) {
    TError error;
    std::vector<std::string> lines;

    error = TPath("/proc/" + (pid ? std::to_string(pid) : "self") + "/mountinfo").ReadLines(lines, MOUNT_INFO_LIMIT);
    if (error)
        return error;

    for (auto &line: lines) {
        TMount mount;

        error = mount.ParseMountinfo(line);
        if (error)
            return error;

        if (filter(mount))
            list.push_back(mount);
    }

    return OK;
}

TError TPath::ListFuseMounts(std::vector<TMount> &list, pid_t pid) {
    return ListMountsByFilter(list, pid, [](const TMount &m) { return StringStartsWith(m.Type, "fuse."); });
}

TError TPath::ListAllMounts(std::vector<TMount> &list, pid_t pid) {
    return ListMountsByFilter(list, pid);
}

std::string TMount::Demangle(const std::string &s) {
    std::string demangled;

    for (unsigned int i = 0; i < s.size();) {
        if (s[i] == '\\' && (i + 3 < s.size()) && ((s[i + 1] & ~7) == '0') && ((s[i + 2] & ~7) == '0') &&
            ((s[i + 3] & ~7) == '0')) {
            demangled.push_back(64 * (s[i + 1] & 7) + 8 * (s[i + 2] & 7) + (s[i + 3] & 7));
            i += 4;

        } else {
            demangled.push_back(s[i]);
            i++;
        }
    }

    return demangled;
}

TError TMount::ParseMountinfo(const std::string &line) {
    auto tokens = SplitString(line, ' ', 7);
    TError error;

    if (tokens.size() < 7)
        return TError(error, "invalid mountinfo header");

    error = StringToInt(tokens[0], MountId);
    if (error)
        return TError(error, "invalid mount id");

    error = StringToInt(tokens[1], ParentId);
    if (error)
        return TError(error, "invalid parent id");

    unsigned int maj, min;
    if (sscanf(tokens[2].c_str(), "%u:%u", &maj, &min) != 2)
        return TError(error, "invalid devno format");
    Device = makedev(maj, min);

    BindPath = TMount::Demangle(tokens[3]);
    Target = TMount::Demangle(tokens[4]);

    error = TMount::ParseFlags(tokens[5], MntFlags);
    if (error)
        return TError(error, "while parsing mountinfo flags");

    std::string opt;
    std::stringstream ss(tokens[6]);

    while (std::getline(ss, opt, ' ')) {
        if (opt == "-")
            break;

        auto pos = opt.find(':');
        int value = 0;
        if (pos != std::string::npos)
            (void)StringToInt(opt.substr(pos + 1), value);

        OptionalFields[opt.substr(0, pos)] = value;
    }

    if (opt != "-")
        return TError("optional delimiter not found");

    if (!std::getline(ss, opt) || !opt.size())
        return TError("remainder missing");

    tokens = SplitString(opt, ' ', 3);
    if (tokens.size() < 3)
        return TError(error, "invalid remainder format");

    Type = TMount::Demangle(tokens[0]);
    Source = TMount::Demangle(tokens[1]);
    Options = TMount::Demangle(tokens[2]);

    return OK;
}

bool TMount::HasOption(const std::string &option) const {
    if (option.empty())
        return true;
    std::string options = "," + Options + ",";
    std::string mask = "," + option + ",";
    return options.find(mask) != std::string::npos;
}

TError TFile::Open(const TPath &path, int flags) {
    if (Fd >= 0)
        close(Fd);
    SetFd = open(path.c_str(), flags);
    if (Fd < 0)
        return TError::System("Cannot open " + path.ToString());
    return OK;
}

TError TFile::OpenPid(pid_t pid) {
    if (Fd >= 0)
        close(Fd);
    SetFd = syscall(__NR_pidfd_open, pid, 0);
    if (Fd < 0)
        return TError::System("pid_fd_open({})", pid);
    return OK;
}

TError TFile::OpenRead(const TPath &path) {
    return Open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY);
}

TError TFile::OpenWrite(const TPath &path) {
    return Open(path, O_WRONLY | O_CLOEXEC | O_NOCTTY);
}

TError TFile::OpenReadWrite(const TPath &path) {
    return Open(path, O_RDWR | O_CLOEXEC | O_NOCTTY);
}

TError TFile::OpenAppend(const TPath &path) {
    return Open(path, O_WRONLY | O_CLOEXEC | O_APPEND | O_NOCTTY);
}

TError TFile::OpenTrunc(const TPath &path) {
    return Open(path, O_WRONLY | O_CLOEXEC | O_TRUNC | O_NOCTTY);
}

TError TFile::OpenDir(const TPath &path) {
    return Open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOCTTY);
}

TError TFile::OpenDirStrict(const TPath &path) {
    return Open(path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOCTTY | O_NOFOLLOW);
}

TError TFile::OpenPath(const TPath &path) {
    return Open(path, O_PATH | O_CLOEXEC);
}

#ifndef O_TMPFILE
#define O_TMPFILE (O_DIRECTORY | 020000000)
#endif

TError TFile::CreateTemporary(TPath &temp, int flags) {
    Close();
    SetFd = mkostemp(&temp.Path[0], O_CLOEXEC | flags);
    if (Fd < 0)
        return TError::System("Cannot create temporary " + temp.Path);
    return OK;
}

TError TFile::CreateUnnamed(const TPath &dir, int flags) {
    TError error = Create(dir, O_RDWR | O_TMPFILE | O_CLOEXEC | flags, 0600);
    if (error) {
        TPath temp = dir / "unnamed.XXXXXX";
        error = CreateTemporary(temp, flags);
        if (!error)
            error = temp.Unlink();
    }
    return error;
}

TError TFile::Create(const TPath &path, int flags, int mode) {
    if (Fd >= 0)
        close(Fd);
    SetFd = open(path.c_str(), flags, mode);
    if (Fd < 0)
        return TError::System("Cannot create " + path.ToString());
    return OK;
}

TError TFile::CreateNew(const TPath &path, int mode) {
    return Create(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, mode);
}

TError TFile::CreateTrunc(const TPath &path, int mode) {
    return Create(path, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
}

void TFile::Close(void) {
    if (Fd >= 0)
        close(Fd);
    SetFd = -1;
}

void TFile::Swap(TFile &other) {
    int tmp = Fd;
    SetFd = other.Fd;
    other.SetFd = tmp;
}

void TFile::Close(const std::vector<int> &fds) {
    for (int fd: fds)
        close(fd);
}

static void closeAllExceptSlow(const TError &reason, const std::vector<int> &except) {
    L_WRN("TFile::CloseAllExcept fallback due to {}", reason);
    int max = getdtablesize();
    for (int fd = 0; fd < max; fd++) {
        if (std::find(except.begin(), except.end(), fd) == except.end())
            close(fd);
    }
}

static TError openProc(TFile &proc) {
    auto error = proc.OpenDir("/proc");
    if (error)
        return error;

    struct stat st;
    error = proc.Stat(st);
    if (error)
        return error;

    if (st.st_ino != 1)
        return TError("/proc has unexpected ino: {}", st.st_ino);

    TStatFS statFS;
    error = proc.StatFS(statFS);
    if (error)
        return error;

    if (statFS.FsType != PROC_SUPER_MAGIC)
        return TError("/proc has unexpected fs_type: {}", statFS.FsType);

    return OK;
}

void TFile::CloseAllExcept(const std::vector<int> &except) {
    TFile proc, pin;
    auto error = openProc(proc);
    if (error)
        return closeAllExceptSlow(error, except);

    error = pin.OpenAt(proc, "thread-self/fd", O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (error)
        return closeAllExceptSlow(error, except);

    std::vector<std::string> vals;
    error = pin.ReadDirectory(vals);
    if (error)
        return closeAllExceptSlow(error, except);

    for (auto &v: vals) {
        int fd;
        error = StringToInt(v, fd);
        if (error)
            return closeAllExceptSlow(error, except);
        if (std::find(except.begin(), except.end(), fd) == except.end())
            close(fd);
    }
}

TError TFile::Lseek(off_t off, int whence) const {
    if (lseek(Fd, off, whence) < 0)
        return TError::System("lseek");
    return OK;
}

TPath TFile::RealPath(void) const {
    TPath path;
    if (Fd >= 0)
        (void)ProcPath().ReadLink(path);
    return path;
}

TPath TFile::ProcPath(void) const {
    if (Fd < 0)
        return TPath();
    return TPath("/proc/self/fd/" + std::to_string(Fd));
}

TError TFile::Read(std::string &text) const {
    if (!text.size())
        text.resize(16 << 10);
    ssize_t ret = read(Fd, &text[0], text.size());
    if (ret < 0)
        return TError::System("read");
    text.resize(ret);
    return OK;
}

TError TFile::ReadAll(std::string &text, size_t max) const {
    struct stat st;
    if (fstat(Fd, &st) < 0)
        return TError::System("fstat");

    if (st.st_size > (off_t)max)
        return TError("File too large: {}", st.st_size);

    size_t size = st.st_size;
    if (st.st_size < 4096)
        size = 4096;
    text.resize(size);

    size_t off = 0;
    ssize_t ret;
    do {
        if (size - off < 1024) {
            size += 16384;
            if (size > max)
                return TError("File too large: {}", size);
            text.resize(size);
        }
        ret = read(Fd, &text[off], size - off);
        if (ret < 0)
            return TError::System("read");
        off += ret;
    } while (ret > 0);

    text.resize(off);

    return OK;
}

TError TFile::ReadLines(std::vector<std::string> &lines, size_t max) const {
    std::string text, line;

    TError error = ReadAll(text, max);
    if (error)
        return error;

    std::stringstream ss(text);

    while (std::getline(ss, line))
        lines.push_back(line);

    return OK;
}

TError TFile::ReadEnds(std::string &text, size_t max) const {
    ssize_t head = 0, tail, size;
    struct stat st;

    if (Stat(st) || st.st_size <= (off_t)max) {
        size = st.st_size ?: max;
        text.resize(size);
        tail = pread(Fd, &text[0], size, 0);
    } else {
        std::string cut = fmt::format("\n--cut {}--\n", StringFormatSize(st.st_size));
        size = (max - cut.size()) / 2;
        text.resize(max);
        head = pread(Fd, &text[0], size, 0);
        if (head < 0)
            return TError::System("read");
        memcpy(&text[head], cut.c_str(), cut.size());
        head += cut.size();
        size = max - head;
        tail = pread(Fd, &text[head], size, st.st_size - size);
    }
    if (tail < 0)
        return TError::System("read");
    text.resize(head + tail);
    return OK;
}

TError TFile::Truncate(off_t size) const {
    if (ftruncate(Fd, size))
        return TError::System("ftruncate");
    return OK;
}

TError TFile::WriteAll(const std::string &text) const {
    size_t len = text.length(), off = 0;
    do {
        ssize_t ret = write(Fd, &text[off], len - off);
        if (ret < 0)
            return TError::System("write");
        off += ret;
    } while (off < len);

    return OK;
}

TError TFile::WriteAllAt(const TPath &path, const std::string &text) const {
    TFile other;
    auto error = other.OpenAt(*this, path, O_WRONLY | O_CLOEXEC | O_NOCTTY);
    if (error)
        return error;
    return other.WriteAll(text);
}

TError TFile::Chattr(int fd, unsigned add_flags, unsigned del_flags) {
    unsigned old_flags, new_flags;

    if (ioctl(fd, FS_IOC_GETFLAGS, &old_flags))
        return TError::System("ioctlFS_IOC_GETFLAGS)");

    new_flags = (old_flags & ~del_flags) | add_flags;
    if ((new_flags != old_flags) && ioctl(fd, FS_IOC_SETFLAGS, &new_flags))
        return TError::System("ioctl(FS_IOC_SETFLAGS)");

    return OK;
}

int TFile::GetMountId(const TPath &relative) const {
    FileHandle fh;
    int mnt;
    if (name_to_handle_at(Fd, relative.ToString().c_str(), &fh.head, &mnt, AT_EMPTY_PATH))
        return -1;
    return mnt;
}

bool TFile::IsMountPointFallback(const TError &error) const {
    L_WRN("IsMountPoint fallback to mountinfo scan due to: {}", error);
    TMount unused;
    return !RealPath().FindMount(unused, true);
}

bool TFile::IsMountPoint() const {
    TFile parent;
    auto error = parent.OpenDirAt(*this, "..");
    if (error)
        return IsMountPointFallback(error);

    int x, y;
    error = GetMountId(x);
    if (!error)
        error = parent.GetMountId(y);

    if (error)
        return IsMountPointFallback(error);

    return x != y;
}

TError TFile::GetMountId(int &mountId) const {
    std::vector<std::string> lines;
    TError error = TPath(fmt::format("/proc/thread-self/fdinfo/{}", Fd)).ReadLines(lines);
    for (const auto &line: lines) {
        if (!StringStartsWith(line, "mnt_id:"))
            continue;
        auto tokens = SplitString(line, '\t', 2);
        if (tokens.size() != 2)
            return TError("Invalid mnt_id entry: '{}'", line);
        auto error = StringToInt(tokens[1], mountId);
        if (error)
            return TError(error, "Failed parse mnt_id: {}", tokens[1]);
        return OK;
    }
    return TError(error, "Cannot find mnt_id in fdinfo");
}

// Open same inode at different mount
TError TFile::OpenAtMount(const TFile &mount, const TFile &file, int flags) {
    struct stat mount_st, file_st;
    FileHandle fh;
    int mount_id;
    TError error;

    error = mount.Stat(mount_st);
    if (error)
        return error;

    error = file.Stat(file_st);
    if (error)
        return error;

    if (mount_st.st_dev != file_st.st_dev)
        return TError(EError::InvalidPath, EXDEV, "Cannot open {} at {}", file.RealPath(), mount.RealPath());

    if (name_to_handle_at(file.Fd, "", &fh.head, &mount_id, AT_EMPTY_PATH))
        return TError::System("OpenAtMount name_to_handle_at {}", file.RealPath());

    int fd = open_by_handle_at(mount.Fd, &fh.head, flags);
    if (fd < 0)
        return TError::System("OpenAtMount open_by_handle_at {}", mount.RealPath());

    Close();
    SetFd = fd;
    return OK;
}

TError TFile::Dup(const TFile &other) {
    if (&other != this) {
        Close();
        SetFd = fcntl(other.Fd, F_DUPFD_CLOEXEC, 3);
        if (Fd < 0)
            return TError::System("Cannot dup fd {}", other.Fd);
    }
    return OK;
}

TError TFile::OpenAt(const TFile &dir, const TPath &path, int flags, int mode) {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    int fd = openat(dir.Fd, path.c_str(), flags, mode);
    if (fd < 0)
        return TError::System("Cannot openat {} {}", dir.RealPath(), path);
    Close();
    SetFd = fd;
    return OK;
}

TError TFile::OpenDirAt(const TFile &dir, const TPath &path) {
    return OpenAt(dir, path, O_RDONLY | O_CLOEXEC | O_DIRECTORY);
}

TError TFile::OpenDirStrictAt(const TFile &dir, const TPath &path) {
    return OpenAt(dir, path, O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW);
}

TError TFile::MknodAt(const TPath &path, int mode, int dev) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (mknodat(Fd, path.c_str(), mode, dev))
        return TError::System("Cannot mknodat {} {}", RealPath(), path);
    return OK;
}

TError TFile::MkfileAt(const TPath &path, int mode) const {
    return MknodAt(path, S_IFREG | (mode & 0777), 0);
}

TError TFile::MkdirAt(const TPath &path, int mode) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (mkdirat(Fd, path.c_str(), mode))
        return TError::System("Cannot mkdirat {} {}", RealPath(), path);
    return OK;
}

TError TFile::OpenDirAllAt(const TFile &dir, const TPath &path) {
    TError error;

    error = Dup(dir);
    if (error)
        return error;

    for (auto &name: path.Components()) {
        if (name == "..")
            return TError(EError::InvalidPath, "Non-normal path {}", path.Path);
        error = OpenDirStrictAt(*this, name);
        if (error)
            return error;
    }

    return OK;
}

TError TFile::CreateDirAllAt(const TFile &dir, const TPath &path, int mode, const TCred &cred) {
    TError error;

    error = Dup(dir);
    if (error)
        return error;

    for (auto name: path.Components()) {
        if (name == "..")
            return TError(EError::InvalidPath, "Non-normal path {}", path.Path);
        error = OpenDirStrictAt(*this, name);
        if (error && error.Errno == ENOENT) {
            error = MkdirAt(name, mode);
            if (!error) {
                error = ChownAt(name, cred);
                if (error)
                    return error;
            } else if (error.Errno != EEXIST)
                return error;
            error = OpenDirStrictAt(*this, name);
        }
        if (error)
            return error;
    }

    return OK;
}

TError TFile::HardlinkAt(const TPath &path, const TFile &target, const TPath &target_path) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (target_path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", target_path.Path);
    if (linkat(target.Fd, target_path.c_str(), Fd, path.c_str(), AT_EMPTY_PATH))
        return TError::System("Cannot create hardlink {} {} to {} {}", RealPath(), path, target.RealPath(),
                              target_path);
    return OK;
}

TError TFile::SymlinkAt(const TPath &path, const TPath &target) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (symlinkat(target.c_str(), Fd, path.c_str()))
        return TError::System("Cannot symlinkat {} {}", RealPath(), path);
    return OK;
}

TError TFile::ReadlinkAt(const TPath &path, TPath &target) const {
    target.Path.resize(PATH_MAX + 1);
    ssize_t len = readlinkat(Fd, path.c_str(), &target.Path[0], PATH_MAX);
    if (len < 0)
        return TError::System("readlinkat {} {}", RealPath(), path);
    target.Path.resize(len);
    return OK;
}

TError TFile::UnlinkAt(const TPath &path) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (unlinkat(Fd, path.c_str(), 0))
        return TError::System("Cannot unlinkat {} {}", RealPath(), path);
    return OK;
}

TError TFile::RmdirAt(const TPath &path) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (unlinkat(Fd, path.c_str(), AT_REMOVEDIR))
        return TError::System("Cannot rmdirat {} {}", RealPath(), path);
    return OK;
}

TError TFile::ReadDirectory(std::vector<std::string> &result) const {
    result.clear();

    int fd = fcntl(Fd, F_DUPFD_CLOEXEC, 3);
    if (fd < 0)
        return TError::System("fcntl(_, F_DUPFD_CLOEXEC)");

    auto dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        return TError::System("fdopendir");
    }

    struct dirent *de;
    while ((de = readdir(dir))) {
        if (strcmp(de->d_name, ".") && strcmp(de->d_name, ".."))
            result.push_back(std::string(de->d_name));
    }
    closedir(dir);
    return OK;
}

TError TFile::RenameAt(const TPath &oldpath, const TPath &newpath) const {
    if (oldpath.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", oldpath.Path);
    if (newpath.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", newpath.Path);
    if (renameat(Fd, oldpath.c_str(), Fd, newpath.c_str()))
        return TError::System("Cannot renameat {} {} {}", RealPath(), oldpath, newpath);
    return OK;
}

TError TFile::Chown(uid_t uid, gid_t gid) const {
    if (fchown(Fd, uid, gid))
        return TError::System("Cannot chown " + std::to_string(Fd));
    return OK;
}

TError TFile::Chmod(mode_t mode) const {
    if (fchmod(Fd, mode))
        return TError::System("Cannot chmod " + std::to_string(Fd));
    return OK;
}

TError TFile::ChownAt(const TPath &path, uid_t uid, gid_t gid) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (fchownat(Fd, path.c_str(), uid, gid, AT_SYMLINK_NOFOLLOW))
        return TError::System("Cannot chownat {} {}", RealPath(), path);
    return OK;
}

TError TFile::ChmodAt(const TPath &path, mode_t mode) const {
    if (path.IsAbsolute())
        return TError(EError::InvalidPath, "Absolute path {}", path.Path);
    if (fchmodat(Fd, path.c_str(), mode, AT_SYMLINK_NOFOLLOW))
        return TError::System("Cannot chmodat {} {}", RealPath(), path);
    return OK;
}

TError TFile::Touch() const {
    if (futimes(Fd, NULL))
        return TError::System("futimes");
    return OK;
}

TError TFile::GetXAttr(const std::string &name, std::string &value) const {
    ssize_t size = syscall(SYS_fgetxattr, Fd, name.c_str(), nullptr, 0);
    if (size >= 0) {
        value.resize(size);
        if (syscall(SYS_fgetxattr, Fd, name.c_str(), value.c_str(), size) >= 0)
            return OK;
    }
    return TError::System("getxattr {}", name);
}

TError TFile::SetXAttr(const std::string &name, const std::string &value) const {
    if (syscall(SYS_fsetxattr, Fd, name.c_str(), value.c_str(), value.length(), 0))
        return TError::System("setxattr {}", name);
    return OK;
}

TError TFile::Chdir() const {
    if (unshare(CLONE_FS))
        return TError::System("unshare(CLONE_FS)");
    if (fchdir(Fd))
        return TError::System("fchdir");
    return OK;
}

bool TFile::IsRegular() const {
    struct stat st;
    return !fstat(Fd, &st) && S_ISREG(st.st_mode);
}

bool TFile::IsDirectory() const {
    struct stat st;
    return !fstat(Fd, &st) && S_ISDIR(st.st_mode);
}

TError TFile::Stat(struct stat &st) const {
    if (fstat(Fd, &st))
        return TError::System("Cannot fstat: {}", Fd);
    return OK;
}

TError TFile::StatAt(const TPath &path, bool follow, struct stat &st) const {
    if (fstatat(Fd, path.c_str(), &st, AT_EMPTY_PATH | (follow ? 0 : AT_SYMLINK_NOFOLLOW)))
        return TError::System("Cannot fstatat {} {}", RealPath(), path);
    return OK;
}

bool TFile::ExistsAt(const TPath &path) const {
    struct stat st;
    return !StatAt(path, false, st);
}

bool TFile::FileIsDirectoryFollowAt(const TPath &filePathObject) const {
    const char *filePath = filePathObject.c_str();
    if (filePath == NULL)
        return false;
    if (filePath[0] == '/')
        filePath++;

    struct stat statbuf;
    int returnCode = fstatat(Fd, filePath, &statbuf, 0);
    if (returnCode == -1)
        return false;

    return S_ISDIR(statbuf.st_mode);
}

bool TFile::FileExistsAt(const TPath &filePathObject) const {
    const char *filePath = filePathObject.c_str();
    if (filePath == NULL)
        return false;
    if (filePath[0] == '/')
        filePath++;

    int returnCode = syscall(__NR_faccessat, Fd, filePath, F_OK, 0);
    if (returnCode == -1)
        return false;

    return true;
}

TError TFile::OpenTree(const TPath &filePathObject, int flags) {
    if (Fd >= 0)
        Close();

    SetFd = syscall(__NR_open_tree, 0, filePathObject.c_str(), flags);
    if (Fd < 0)
        return TError::System("Can`t open_tree {}", filePathObject.ToString());

    return OK;
}

TError TFile::MoveMount(const TPath &dirPathObject) {
    int returnCode = syscall(__NR_move_mount, Fd, "", 0, dirPathObject.c_str(), MOVE_MOUNT_F_EMPTY_PATH);
    if (returnCode == -1)
        return TError::System("Can`t move_mount {}", RealPath().ToString());

    return OK;
}

TError TFile::StatFS(TStatFS &result) const {
    struct statfs st;
    if (fstatfs(Fd, &st))
        return TError::System("statfs");
    result.Init(st);
    return OK;
}

uint32_t TFile::FsType() const {
    struct statfs st;
    if (fstatfs(Fd, &st))
        return 0;
    return st.f_type;
}

int TPathWalk::CompareNames(const FTSENT **a, const FTSENT **b) {
    return strcmp((**a).fts_name, (**b).fts_name);
}

int TPathWalk::CompareInodes(const FTSENT **a, const FTSENT **b) {
    ino_t a_ino = ((**a).fts_info == FTS_NS || (**a).fts_info == FTS_NSOK) ? 0 : (**a).fts_statp->st_ino;
    ino_t b_ino = ((**b).fts_info == FTS_NS || (**b).fts_info == FTS_NSOK) ? 0 : (**b).fts_statp->st_ino;
    if (a_ino < b_ino)
        return -1;
    if (a_ino > b_ino)
        return 1;
    return 0;
}

TError TPathWalk::Open(const TPath &path, int fts_flags, int (*compar)(const FTSENT **, const FTSENT **)) {
    Close();
    char *paths[] = {(char *)path.c_str(), nullptr};
    Fts = fts_open(paths, fts_flags, compar);
    if (!Fts)
        return TError::System("fts_open");
    return OK;
}

TError TPathWalk::OpenScan(const TPath &path)
{
    return Open(path, FTS_COMFOLLOW | FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, TPathWalk::CompareInodes);
}

TError TPathWalk::OpenList(const TPath &path)
{
    return Open(path, FTS_COMFOLLOW | FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV, TPathWalk::CompareNames);
}

TError TPathWalk::OpenNoStat(const TPath &path)
{
    return Open(path, FTS_COMFOLLOW | FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV | FTS_NOSTAT, nullptr);
}

TError TPathWalk::Next() {
next:
    Ent = fts_read(Fts);
    if (!Ent) {
        if (errno)
            return TError(EError::Unknown, errno, "fts_read");
        Path = "";
        return OK;
    }
    switch (Ent->fts_info) {
    case FTS_DNR:
        if (Ent->fts_errno == ENOTDIR)
            goto next;
        // fall through
    case FTS_ERR:
    case FTS_NS:
        if (Ent->fts_errno == ENOENT)
            goto next;
        return TError(EError::Unknown, Ent->fts_errno, "fts_read {}", Ent->fts_path);
    case FTS_D:
    case FTS_DC:
        Directory = true;
        Postorder = false;
        break;
    case FTS_DP:
        Directory = true;
        Postorder = true;
        break;
    default:
        Directory = false;
        Postorder = false;
        break;
    }
    Path = Ent->fts_path;
    Stat = Ent->fts_statp;
    return OK;
}

void TPathWalk::Close() {
    if (Fts)
        fts_close(Fts);
    Fts = nullptr;
}
