#pragma once

#include "file.hpp"
#include "path.hpp"

/*
 * Mount object, see fsmount(2), open_tree(2), mount_setattr(2) and move_mount(2).
 * Paths are relative to dir, empty dir means current directory and empty path
 * means dir itself.
 */

struct TMountAttr {
    uint64_t Set;         /* MOUNT_ATTR_* */
    uint64_t Clr;         /* MOUNT_ATTR_* */
    uint64_t Propagation; /* one of MS_PRIVATE, MS_SLAVE, MS_SHARED, MS_UNBINDABLE or 0 */

    /* MS_* flags: set as MS_RDONLY..., cleared as MS_ALLOW_*, atime and propagation */
    uint64_t ToFlags() const;
    std::string ToString() const;
};

TError SetMountAttr(const TFile &dir, const TPath &path, const TMountAttr &attr, bool recursive);

class TMountFd: public TFile {
public:
    TMountFd() = default;
    TMountFd(TMountFd &&other)
        : TFile(std::move(other))
    {}

    /* New detached instance of filesystem, attr is MOUNT_ATTR_*,
       options are "key=value" or bare "flag" as for mount(8) */
    TError Create(const std::string &type, uint64_t attr, const std::vector<std::string> &opts);
    /* Detached copy of mount tree at path, like bind mount */
    TError Clone(const TFile &dir, const TPath &path, bool recursive);

    /* Change MOUNT_ATTR_* */
    TError SetAttr(uint64_t set, uint64_t clr, bool recursive) const {
        return SetMountAttr(*this, TPath(), {set, clr, 0}, recursive);
    }
    /* Change propagation: MS_PRIVATE, MS_SLAVE, MS_SHARED or MS_UNBINDABLE */
    TError SetPropagation(uint64_t propagation, bool recursive) const {
        return SetMountAttr(*this, TPath(), {0, 0, propagation}, recursive);
    }
    /* Reconfigure superblock of filesystem at path, options as in Create, see fspick(2) */
    static TError Reconfigure(const TPath &path, const std::vector<std::string> &opts);
    /* MS_PRIVATE, MS_SLAVE, MS_SHARED and MS_UNBINDABLE from flags, in this order */
    TError SetFlagsPropagation(uint64_t mnt_flags) const;
    /* Attach over path, after that fd refers to attached mount */
    TError Move(const TFile &dir, const TPath &path);
};

TError BindMount(const TFile &srcDir, const TPath &source, const TFile &dstDir, const TPath &target,
                 const TMountAttr &attr, bool recursive);

static inline TError BindMount(const TPath &source, const TPath &target, const TMountAttr &attr, bool recursive) {
    return BindMount(TFile(), source, TFile(), target, attr, recursive);
}
