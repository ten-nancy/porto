#include "log.hpp"
#include "mutex.hpp"
#include "unix.hpp"

#include <sys/file.h>

__thread uint64_t LockTimer::LockTime  = 0;
__thread uint64_t LockTimer::LockLevel = 0;
__thread uint64_t LockTimer::LockStart = 0;

LockTimer::LockTimer(const std::string &name) : Name(name) {
    auto level = LockLevel++;

    StartTime = GetCurrentTimeMs();
    if (!level)
        LockStart = StartTime;
    Statistics->LockOperationsCount++;
}

LockTimer::~LockTimer() {
    auto level = --LockLevel;
    auto endTime = GetCurrentTimeMs();

    uint64_t requestTime = endTime - StartTime;

    if (requestTime > 1000) {
        L("Long lock {} operation time={} ms", Name, requestTime);
        Statistics->LockOperationsLonger1s++;
    }
    if (requestTime > 3000)
        Statistics->LockOperationsLonger3s++;
    if (requestTime > 30000)
        Statistics->LockOperationsLonger30s++;
    if (requestTime > 300000)
        Statistics->LockOperationsLonger5m++;

    if (!level)
        LockTime += endTime - LockStart;
}

MeasuredMutex::MeasuredMutex(const std::string &name) : Name(name) {}

void MeasuredMutex::lock() {
    LockTimer timer(Name);
    std::mutex::lock();
}

std::unique_lock<std::mutex> MeasuredMutex::UniqueLock() {
    LockTimer timer(Name);
    return std::unique_lock<std::mutex>(*this);
}

TFileMutex::TFileMutex(const TPath &path, int flags) {
    TError error = File.Open(path, flags);
    if (error)
        L_WRN("cannot open {} {}", path, error);

    lock();
}

TFileMutex::~TFileMutex() {
    if (!File)
        return;

    unlock();
}

void TFileMutex::lock() const {
    int ret = flock(File.Fd, LOCK_EX);
    if (ret)
        L_WRN("cannot flock lock {} {}", File.RealPath(), ret);
}

void TFileMutex::unlock() const {
    int ret = flock(File.Fd, LOCK_UN);
    if (ret)
        L_WRN("cannot flock unlock {} {}", File.RealPath(), ret);
}

std::unique_ptr<TFileMutex> TFileMutex::MakePathLock(const TPath &path, int flags) {
    return std::unique_ptr<TFileMutex>(new TFileMutex(path, flags));
}

std::unique_ptr<TFileMutex> TFileMutex::MakeDirLock(const TPath &path) {
    return std::unique_ptr<TFileMutex>(new TFileMutex(path, O_CLOEXEC | O_NOCTTY | O_DIRECTORY));
}

std::unique_ptr<TFileMutex> TFileMutex::MakeRegLock(const TPath &path) {
    return std::unique_ptr<TFileMutex>(new TFileMutex(path, O_CLOEXEC | O_NOCTTY));
}

std::unique_ptr<TFileMutex> TFileMutex::MakeSymLock(const TPath &path) {
    return std::unique_ptr<TFileMutex>(new TFileMutex(path, O_CLOEXEC | O_NOCTTY | O_NOFOLLOW));
}
