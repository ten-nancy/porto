#pragma once

#include <vector>

#include "util/cred.hpp"
#include "util/file.hpp"

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

// A source is used only during acquisition. Its directory FD is borrowed.
class TStreamSource {
    const int DirFd;
    const TPath Path;
    const uint64_t Resolve;

    TError Open(int flags, TFile &file) const;

public:
    explicit TStreamSource(const TPath &path);
    TStreamSource(const TFile &dir, const TPath &path);
    explicit TStreamSource(const TFile &pin);

    TError OpenInput(TFile &file) const;
    TError OpenOutput(TFile &file) const;
    TError OpenOrCreateOutput(TFile &file) const;
    TError OpenOwnedOutput(const TCred &cred, TFile &file) const;
};

class TStream {
    int Target;
    TPath Path;
    TFile File;

public:
    TStream(int target, const TPath &path)
        : Target(target),
          Path(path)
    {}
    TStream(int target, TFile &&file)
        : Target(target),
          File(std::move(file))
    {}

    TError Install();
};

using TStreams = std::vector<TStream>;
