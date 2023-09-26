#pragma once

#include "util/path.hpp"

#include <mutex>

class LockTimer {
    static __thread uint64_t LockTime;
    static __thread uint64_t LockLevel;
    static __thread uint64_t LockStart;

    uint64_t StartTime = 0;
    uint64_t EndTime = 0;
    const std::string &Name;

public:
    LockTimer(const std::string &name);

    LockTimer(std::string &&name) = delete;

    ~LockTimer();

    static void Reset() {
        LockTime = 0;
    }

    static uint64_t Get() {
        return LockTime;
    }
};

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
