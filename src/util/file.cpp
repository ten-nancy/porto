#include "file.hpp"

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
#include <sys/types.h>
#include <unistd.h>
}

struct FileHandle {
    struct file_handle head;
    char data[MAX_HANDLE_SZ];
    FileHandle() {
        head.handle_bytes = MAX_HANDLE_SZ;
        head.handle_type = 0;
    }
};

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
    SetFd = PidFDOpen(pid, 0);
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

bool TFile::IsMountPointFallback(const TError &error) const {
    L_WRN("IsMountPoint fallback to mountinfo scan due to: {}", error);
    TMount unused;
    return !RealPath().FindMount(unused, true);
}

bool TFile::IsMountPoint() const {
    struct statx st;
    if (!statx(Fd, "", AT_EMPTY_PATH, 0, &st)) {
        if (st.stx_attributes_mask & STATX_ATTR_MOUNT_ROOT)
            return st.stx_attributes & STATX_ATTR_MOUNT_ROOT;
    } else
        L_WRN("{}", TError::System("statx"));

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

TError TFile::OpenAtNoSymlink(const TFile &dir, const TPath &path, int flags) {
    // TODO(ovov): replace this with openat2 when we get rid of 5.4:
    // https://man7.org/linux/man-pages/man2/openat2.2.html
    auto error = Dup(dir);
    if (error)
        return error;

    auto components = path.Components();
    for (size_t i = 0; i < components.size(); ++i) {
        if (components[i] == "..")
            return TError(EError::InvalidPath, "Non-normal path {}", path);

        auto flags1 = i == components.size() - 1 ? flags : O_RDONLY | O_CLOEXEC | O_NOCTTY;
        error = OpenAt(*this, components[i], flags1 | O_NOFOLLOW);
        if (error)
            return error;
    }

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
