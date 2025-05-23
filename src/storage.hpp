#pragma once

#include <list>

#include "util/path.hpp"

enum EStorageType {
    Place,
    Layer,
    DockerLayer,
    Storage,
    Meta,
    Volume,
};

class TStorage {
public:
    EStorageType Type;
    TPath Place;
    std::string Name;

    TPath Path;
    std::string Meta;
    std::string FirstName;

    TCred Owner;
    std::string Private;
    time_t LastChange = 0;
    uint64_t Size = 0;

    TError Resolve(EStorageType type, const TPath &place, const std::string &name = "", bool strict = false);
    void Open(EStorageType type, const TPath &place, const std::string &name = "");

    static TError CheckName(const std::string &name, bool meta = false);
    static TError CheckPlace(const TPath &place);
    static TError CheckBaseDirectory(const TPath &place, EStorageType type, unsigned perms);
    static TError Cleanup(const TFile &place, bool strict, bool &drop);
    static TError Cleanup(const TFile &dir, EStorageType type);
    static TError SanitizeLayer(const TPath &layer);
    TError List(enum EStorageType type, std::list<TStorage> &list);
    TError ImportArchive(const TPath &archive, const std::string &cgroup, const std::string &compress = "",
                         bool merge = false, bool verboseError = false);
    TError ExportArchive(const TPath &archive, const std::string &compress = "");
    bool Exists() const;
    bool Weak() const;
    uint64_t LastUsage() const;
    TError Load();
    TError Remove(bool weak = false, bool async = false);
    TError Touch();
    TError SaveOwner(const TCred &owner);
    TError SetPrivate(const std::string &text);
    TError SavePrivate(const std::string &text);
    TError SaveChecksums();

    TError CreateMeta(uint64_t space_limit, uint64_t inode_limit);
    TError ResizeMeta(uint64_t space_limit, uint64_t inode_limit);
    TError StatMeta(TStatFS &stat);

    static void Init();
    static void StartAsyncRemover();
    static void StopAsyncRemover();
    static void IncPlaceLoad(const TPath &place);
    static void DecPlaceLoad(const TPath &place);

private:
    TPath TempPath(const std::string &kind);
    TError CheckUsage();
};
