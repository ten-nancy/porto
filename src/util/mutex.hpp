#pragma once

#include "util/path.hpp"

#include <mutex>

class MeasuredMutex: public std::mutex {
    const std::string Name;

public:
    MeasuredMutex(const std::string &name);

    void lock();
    std::unique_lock<std::mutex> UniqueLock();
};


class TFileMutex {
    TFile File;

    void lock() const;
    void unlock() const;

public:
    TFileMutex() = delete;
    TFileMutex(const TPath &path, int flags = 0);
    ~TFileMutex();

    static std::unique_ptr<TFileMutex> MakePathLock(const TPath &path, int flags);
    static std::unique_ptr<TFileMutex> MakeDirLock(const TPath &path);
    static std::unique_ptr<TFileMutex> MakeRegLock(const TPath &path);
    static std::unique_ptr<TFileMutex> MakeSymLock(const TPath &path);
};
