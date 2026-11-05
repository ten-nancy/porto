#pragma once

#include <atomic>
#include <functional>
#include <list>
#include <string>
#include <vector>

#include "util/cred.hpp"
#include "util/error.hpp"
#include "util/string.hpp"

extern "C" {
#include <fts.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vfs.h>
}

struct TStatFS {
    uint64_t SpaceUsage;
    uint64_t SpaceAvail;
    uint64_t InodeUsage;
    uint64_t InodeAvail;
    uint64_t MntFlags;
    uint32_t FsType;

    void Init(const struct statfs &st);
    void Reset();
};

struct TMount;

#ifndef MS_LAZYTIME
#define MS_LAZYTIME (1 << 25)
#endif

/* inverted flags with lower priority */
#define MS_ALLOW_WRITE (1ull << 60)
#define MS_ALLOW_EXEC  (1ull << 61)
#define MS_ALLOW_SUID  (1ull << 62)
#define MS_ALLOW_DEV   (1ull << 63)

class TPath {
private:
    std::string Path;
    friend class TFile;

    TPath AddComponent(const TPath &component) const;

    static void RootFilesChownFilter(const struct stat &st, uid_t &uid, gid_t &gid) {
        if (st.st_uid != 0)
            uid = (uid_t)-1;
        if (st.st_gid != 0)
            gid = (gid_t)-1;
    }

public:
    TPath(const std::string &path)
        : Path(path)
    {}
    TPath(const char *path)
        : Path(path)
    {}
    TPath()
        : Path("")
    {}

    bool IsAbsolute() const {
        return Path[0] == '/';
    }

    bool IsSimple() const {
        return Path.find('/') == std::string::npos;
    }

    bool IsRoot() const {
        return Path == "/";
    }

    bool IsEmpty() const {
        return Path.empty();
    }

    explicit operator bool() const {
        return !Path.empty();
    }

    bool IsNormal() const {
        return Path == NormalPath().Path;
    }

    bool IsInside(const TPath &base) const;

    bool StartsWithDotDot() const {
        return Path[0] == '.' && Path[1] == '.' && (Path[2] == '/' || Path[2] == '\0');
    }

    std::vector<std::string> Components() const;

    const char *c_str() const noexcept {
        return Path.c_str();
    }

    TPath operator+(const TPath &p) const {
        return TPath(Path + p.ToString());
    }

    friend bool operator==(const TPath &a, const TPath &b) {
        return a.ToString() == b.ToString();
    }

    friend bool operator!=(const TPath &a, const TPath &b) {
        return a.ToString() != b.ToString();
    }

    friend bool operator<(const TPath &a, const TPath &b) {
        return a.ToString() < b.ToString();
    }

    friend bool operator>(const TPath &a, const TPath &b) {
        return a.ToString() > b.ToString();
    }

    friend std::ostream &operator<<(std::ostream &os, const TPath &path) {
        return os << path.ToString();
    }

    friend TPath operator/(const TPath &a, const TPath &b) {
        return a.AddComponent(b);
    }

    TPath &operator/=(const TPath &b) {
        *this = AddComponent(b);
        return *this;
    }

    TPath NormalPath() const;

    TPath DirNameNormal() const;
    std::string BaseNameNormal() const;

    TPath DirName() const;
    std::string BaseName() const;

    TPath AbsolutePath(const TPath &base = "") const;
    TPath RelativePath(const TPath &base) const;

    TPath RealPath() const;
    TPath InnerPath(const TPath &path, bool absolute = true) const;

    TError StatStrict(struct stat &st) const;
    TError StatFollow(struct stat &st) const;

    bool IsRegularStrict() const;
    bool IsRegularFollow() const;

    bool IsDirectoryStrict() const;
    bool IsDirectoryFollow() const;

    bool IsSocketFollow() const;
    bool IsCharFollow() const;
    bool IsBlockFollow() const;

    bool IsSameInode(const TPath &other) const;
    bool IsMountPoint() const;

    dev_t GetDev() const;
    dev_t GetBlockDev() const;
    static TError GetDevName(dev_t dev, std::string &name);

    int64_t SinceModificationMs() const;
    std::string ToString() const;
    bool Exists() const;
    bool PathExists() const; /* or dangling symlink */

    TError Chdir() const;
    TError Chroot() const;

    TError Chown(uid_t uid, gid_t gid) const;
    TError Chown(const TCred &cred) const {
        return Chown(cred.GetUid(), cred.GetGid());
    }

    TError Lchown(uid_t uid, gid_t gid) const;
    TError Lchown(const TCred &cred) const {
        return Lchown(cred.GetUid(), cred.GetGid());
    }

    typedef void (*TChownFilter)(const struct stat &, uid_t &uid, gid_t &gid);
    TError ChownRecursive(uid_t uid, gid_t gid, TChownFilter filter = nullptr) const;
    TError ChownRecursive(const TCred &cred, TChownFilter filter = nullptr) const {
        return ChownRecursive(cred.GetUid(), cred.GetGid(), filter);
    }

    TError ChownRecursiveRootFiles(uid_t uid, gid_t gid) const {
        return ChownRecursive(uid, gid, RootFilesChownFilter);
    }
    TError ChownRecursiveRootFiles(const TCred &cred) const {
        return ChownRecursiveRootFiles(cred.GetUid(), cred.GetGid());
    }

    TError Chmod(const int mode) const;
    TError ReadLink(TPath &value) const;
    TError Hardlink(const TPath &target) const;
    TError Symlink(const TPath &target) const;
    TError Mknod(unsigned int mode, unsigned int dev) const;
    TError Mkfile(unsigned int mode) const;
    TError Mkdir(unsigned int mode) const;
    TError MkdirAll(unsigned int mode, bool ignore = false) const;
    TError MkdirTmp(const TPath &parent, const std::string &prefix, unsigned int mode);
    TError Rmdir() const;
    TError Unlink() const;
    TError RemoveAll() const;
    TError ClearEmptyDirectories(const TPath &root) const;
    TError Rename(const TPath &dest) const;
    TError ReadDirectory(std::vector<std::string> &result) const;
    TError ListSubdirs(std::vector<std::string> &result) const;
    TError ClearDirectory() const;
    TError StatFS(TStatFS &result) const;
    TError GetXAttr(const std::string &name, std::string &value) const;
    TError SetXAttr(const std::string &name, const std::string &value) const;
    TError Truncate(off_t size) const;
    TError RotateLog(off_t max_disk_usage, off_t &loss) const;
    TError Chattr(unsigned add_flags, unsigned del_flags) const;
    TError Touch() const;

    static std::string UmountFlagsToString(uint64_t flags);

    TError FindMount(TMount &mount, bool exact = false) const;
    static TError ListMountsByFilter(std::vector<TMount> &list, pid_t pid,
                                     std::function<bool(const TMount &)> filter = [](const TMount &) { return true; });
    static TError ListFuseMounts(std::vector<TMount> &list, pid_t pid = 0);
    static TError ListAllMounts(std::vector<TMount> &list, pid_t pid = 0);

    TError Mount(const TPath &source, const std::string &type, uint64_t flags,
                 const std::vector<std::string> &options) const;
    TError SecureTmpfsMount(const TCred &cred, size_t size) const;
    TError Bind(const TPath &source, uint64_t flags = 0) const;
    TError MoveMount(const TPath &target) const;
    TError Remount(uint64_t flags) const;
    TError BindRemount(const TPath &source, uint64_t flags) const;
    TError Umount(uint64_t flags) const;
    TError UmountAll(int flags = 0) const;
    TError UmountNested() const;

    TError ReadAll(std::string &text, size_t max = 1048576) const;
    TError ReadLines(std::vector<std::string> &lines, size_t max = 1048576) const;
    TError ReadInt(int &value) const;
    TError ReadUint64(uint64_t &value) const;

    TError WriteAll(const std::string &text) const;
    TError WriteLines(const std::vector<std::string> &lines) const;
    TError WriteAtomic(const std::string &text) const;
    TError WritePrivate(const std::string &text) const;

    TError CreateRegular() const;
    TError CreateAndWriteAll(const std::string &text) const;
};

struct TMount {
    TPath Source;
    TPath Target;
    std::string Type;
    std::string Options;
    std::unordered_map<std::string, int> OptionalFields;

    int MountId;
    int ParentId;
    dev_t Device;
    TPath BindPath; /* path within filesystem */
    uint64_t MntFlags;

    static std::string Demangle(const std::string &s);
    TError ParseMountinfo(const std::string &line);
    bool HasOption(const std::string &option) const;

    friend std::ostream &operator<<(std::ostream &stream, const TMount &mount) {
        stream << mount.Source << " " << mount.Target << " -t " << mount.Type << " -o " << mount.Options;
        return stream;
    }

    static TError ParseFlags(const std::string &str, uint64_t &flags, uint64_t allowed = -1);
    static std::string FormatFlags(uint64_t flags);
};

class TPathWalk {
private:
    FTS *Fts = nullptr;

    TPathWalk(const TPathWalk &) = delete;
    TPathWalk &operator=(const TPathWalk &) = delete;
    static int CompareNames(const FTSENT **a, const FTSENT **b);
    static int CompareInodes(const FTSENT **a, const FTSENT **b);

public:
    FTSENT *Ent = nullptr;

    bool Postorder = false;
    bool Directory = false;
    TPath Path;
    struct stat *Stat;

    TPathWalk() {}
    ~TPathWalk() {
        Close();
    }
    TError Open(const TPath &path, int fts_flags = FTS_COMFOLLOW | FTS_NOCHDIR | FTS_PHYSICAL | FTS_XDEV,
                int (*compar)(const FTSENT **, const FTSENT **) = nullptr);
    TError OpenScan(const TPath &path);
    TError OpenList(const TPath &path);
    TError OpenNoStat(const TPath &path);
    TError Next();
    std::string Name() {
        return Ent ? Ent->fts_name : "";
    }
    int Level() {
        return Ent ? Ent->fts_level : -2;
    }
    void Close();
};
