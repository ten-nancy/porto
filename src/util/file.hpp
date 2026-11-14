#pragma once

#include "path.hpp"

class TFile {
private:
    TFile(const TFile &) = delete;
    TFile &operator=(const TFile &) = delete;

    TError Access(const TPath &root, const TCred &cred, std::function<TError(const TCred &)> check) const;
    bool IsMountPointFallback(const TError &error) const;

public:
    union {
        const int Fd;
        int SetFd;
    };
    TFile()
        : Fd(-1)
    {}
    TFile(int fd)
        : Fd(fd)
    {}
    TFile(TFile &&other)
        : Fd(other.Fd)
    {
        other.SetFd = -1;
    }
    ~TFile() {
        Close();
    }
    explicit operator bool() const {
        return Fd >= 0;
    }
    TError Open(const TPath &path, int flags);
    TError OpenPid(pid_t pid);
    TError OpenRead(const TPath &path);
    TError OpenWrite(const TPath &path);
    TError OpenReadWrite(const TPath &path);
    TError OpenTrunc(const TPath &path);
    TError OpenDir(const TPath &path);
    TError OpenDirStrict(const TPath &path);
    TError OpenPath(const TPath &path);
    TError CreateTemporary(TPath &temp, int flags = 0);
    TError CreateUnnamed(const TPath &dir, int flags = 0);
    TError Create(const TPath &path, int flags, int mode);
    TError CreateNew(const TPath &path, int mode);
    TError CreateTrunc(const TPath &path, int mode);
    void Close(void);
    void Swap(TFile &other);
    void Set(int fd) {
        Close();
        SetFd = fd;
    }
    static void CloseAllExcept(const std::vector<int> &except);
    TError Lseek(off_t off, int whence) const;
    TPath RealPath(void) const;
    TPath ProcPath(void) const;
    TError Read(std::string &text) const;
    TError ReadAll(std::string &text, size_t max) const;
    TError ReadLines(std::vector<std::string> &lines, size_t max = 1048576) const;
    TError ReadEnds(std::string &text, size_t max) const;
    TError Truncate(off_t size) const;
    TError WriteAll(const std::string &text) const;
    TError WriteAllAt(const TPath &path, const std::string &text) const;
    TError GetMountId(int &mountId) const;
    bool IsMountPoint() const;
    TError Dup(const TFile &other);
    TError OpenAt(const TFile &dir, const TPath &path, int flags, int mode = 0);
    TError OpenAtNoSymlink(const TFile &dir, const TPath &path, int flags);
    TError OpenDirAt(const TFile &dir, const TPath &path);
    TError OpenDirStrictAt(const TFile &dir, const TPath &path);
    TError OpenAtMount(const TFile &mount, const TFile &file, int flags);
    TError CreateDirAllAt(const TFile &dir, const TPath &path, int mode, const TCred &cred);
    TError MknodAt(const TPath &path, int mode, int dev) const;
    TError MkfileAt(const TPath &path, int mode) const;
    TError MkdirAt(const TPath &path, int mode) const;
    TError UnlinkAt(const TPath &path) const;
    TError RemoveAt(const TPath &path) const;
    TError RemoveAllAtInterruptible(const TPath &path, std::atomic_bool &interrupt, bool doumount) const;
    TError ReadDirectory(std::vector<std::string> &result) const;
    TError RenameAt(const TPath &oldpath, const TPath &newpath) const;
    TError HardlinkAt(const TPath &path, const TFile &target, const TPath &target_path = "") const;
    TError SymlinkAt(const TPath &path, const TPath &target) const;
    TError ReadlinkAt(const TPath &path, TPath &target) const;
    TError Chown(uid_t uid, gid_t gid) const;
    TError Chown(const TCred &cred) const {
        return Chown(cred.GetUid(), cred.GetGid());
    }
    TError Chmod(mode_t mode) const;
    TError ChownAt(const TPath &path, uid_t uid, gid_t gid) const;
    TError ChownAt(const TPath &path, const TCred &cred) const {
        return ChownAt(path, cred.GetUid(), cred.GetGid());
    }
    TError SetXAttr(const std::string &name, const std::string &value) const;
    TError Chdir() const;
    TError ClearDirectory() const;
    bool IsRegular() const;
    bool IsDirectory() const;
    TError Stat(struct stat &st) const;
    TError StatAt(const TPath &path, bool follow, struct stat &st) const;
    bool ExistsAt(const TPath &path) const;

    uint32_t FsType() const;
    TError StatFS(TStatFS &result) const;
    TError Chroot() const;
    TError PivotRoot() const;

    enum AccessMode {
        E = 000, /* Exists */
        X = 001,
        W = 002,
        R = 004,
    };

    static bool Access(const struct stat &st, const TCred &cred, enum AccessMode mode);

    TError WriteAccess(const TPath &root, const TCred &cred) const {
        return Access(root, cred, [this](const TCred &cred) { return WriteAccess(cred); });
    }

    TError ReadAccess(const TCred &cred) const;
    TError WriteAccess(const TCred &cred, const struct statfs &fs) const;
    TError WriteAccess(const TCred &cred) const {
        struct statfs fs;
        if (fstatfs(Fd, &fs))
            return TError::System("fstatfs");
        return WriteAccess(cred, fs);
    }
};
