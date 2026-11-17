#pragma once

#include <vector>

#include "util/cred.hpp"
#include "util/file.hpp"

namespace runtime {
class TStreamSpec;
}

struct TStreamSpec {
    enum class ESource { Null, Host, Container, Redirect };

    ESource Source = ESource::Null;
    std::string Value;
    uint64_t Inode = 0;
    uint64_t Device = 0;

    TStreamSpec() = default;
    TStreamSpec(ESource source, const std::string &value)
        : Source(source),
          Value(value)
    {}

    TStreamSpec(ESource source, const std::string &value, uint64_t ino, uint64_t dev)
        : Source(source),
          Value(value),
          Inode(ino),
          Device(dev)
    {}

    static TError Parse(const TFile &procFd, const std::string &value, TStreamSpec &spec);
    static TError Restore(const std::string &value, TStreamSpec &spec);
    std::string Save() const;
};

// Authorization and path pinning belong to portod; I/O opens run in the task's cgroups.
class TStream {
    int Target;
    TPath Path;
    TFile File;

    TError OpenFile(int dirFd, int flags, uint64_t resolve);
    TError OpenHost(const TFile &dir, uid_t uid, gid_t gid);
    TError OpenContainer();

public:
    explicit TStream(int target)
        : Target(target)
    {}

    TError Open(const runtime::TStreamSpec &spec, std::vector<TFile> &fds);
    TError Install();
};

using TStreams = std::vector<TStream>;
