#pragma once

#include <string>

#include "cgroup.hpp"
#include "util/cred.hpp"
#include "util/path.hpp"
#include "util/string.hpp"

class TCgroup;

struct TDevice {
    TPath Path;
    TPath PathInside;

    dev_t Node = 0;
    uid_t Uid = 0;
    gid_t Gid = 0;
    mode_t Mode = S_IFCHR | 0666;

    bool MayRead = true;
    bool MayWrite = true;
    bool MayMknod = true;

    bool Wildcard = false;
    bool Optional = false;

    static TError CheckPath(const TPath &path);

    TDevice() {}
    TDevice(const TPath &path, dev_t node)
        : Path(path),
          PathInside(path),
          Node(node)
    {}

    TError Parse(TTuple &opt, const TCred &cred);
    std::string FormatNode() const;
    std::string FormatAccess() const;
    std::string Format() const;
    TError Makedev(const TPath &root = "/") const;

    TError Load(const rpc::TContainerDevice &dev, const TCred &cred);
    void Dump(rpc::TContainerDevice &dev) const;

    bool Allowed() const {
        return MayRead || MayWrite || MayMknod;
    }
};

struct TDevices {
    std::vector<TDevice> Devices;
    bool NeedCgroup = false;
    bool AllOptional = false;

    TDevices() = default;
    TDevices(const TDevices &other) = default;
    TDevices(TDevices &&other) = default;

    TDevices &operator=(const TDevices &other) = default;

    TError Parse(const std::string &str, const TCred &cred);
    std::string Format() const;

    void PrepareForUserNs(const TCred &userNsCred);
    TError Makedev(const TPath &root) const;
    TError Apply(const TCgroup &cg) const;

    TError InitDefault();
    TDevices &Merge(const TDevices &devices);
    bool Empty() const {
        return Devices.empty();
    }

    std::set<TPath> AllowedPaths() const {
        std::set<TPath> paths;
        for (auto &dev: Devices) {
            if (dev.Allowed())
                paths.insert(dev.PathInside);
        }
        return paths;
    }

    TDevices operator|(const TDevices &other) {
        return TDevices(*this).Merge(other);
    }

    bool operator<=(const TDevices &other) const;
};
