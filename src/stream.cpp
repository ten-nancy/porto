#include "stream.hpp"

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "stream.pb.h"

static TStreamSpec::ESource resolveSource(const std::string &value) {
    if (value.empty() || value == "/dev/null")
        return TStreamSpec::ESource::Null;
    if (StringStartsWith(value, "/dev/fd/"))
        return TStreamSpec::ESource::Redirect;
    return TStreamSpec::ESource::Container;
}

TError TStreamSpec::Parse(const TFile &procFd, const std::string &value, TStreamSpec &spec) {
    auto source = resolveSource(value);

    struct stat st = {};
    if (source == ESource::Redirect) {
        int fd;
        auto error = StringToInt(value.substr(8), fd);
        if (error || fd < 0)
            return TError(EError::InvalidValue, "Invalid stream redirect {}", value);
        error = procFd.StatAt(fmt::format("fd/{}", fd), true, st);
        if (error)
            return error;
    }
    spec = TStreamSpec(source, value, st.st_ino, st.st_dev);
    return OK;
}

TError TStreamSpec::Restore(const std::string &value, TStreamSpec &spec) {
    auto first = value.find(';');
    if (first == std::string::npos) {
        spec = TStreamSpec(resolveSource(value), value);
        return OK;
    }
    auto second = value.find(';', first + 1);
    if (second == std::string::npos)
        return TError(EError::InvalidValue, "Invalid saved stream {}", value);

    uint64_t inode, device;
    auto error = StringToUint64(value.substr(0, first), inode);
    if (error)
        return error;
    error = StringToUint64(value.substr(first + 1, second - first - 1), device);
    if (error)
        return error;

    auto path = value.substr(second + 1);
    spec = TStreamSpec(resolveSource(path), path, inode, device);
    return OK;
}

std::string TStreamSpec::Save() const {
    return fmt::format("{};{};{}", Inode, Device, Value);
}

TError TStream::OpenFile(int dirFd, int flags, uint64_t resolve) {
    struct open_how how = {};
    how.flags = flags | O_NOCTTY | O_NONBLOCK | O_CLOEXEC;
    how.mode = flags & O_CREAT ? 0660 : 0;
    how.resolve = resolve;
    auto error = File.OpenHow(dirFd, Path, how);
    if (!error && isatty(File.Fd)) {
        how.flags = O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC | (flags & O_NOFOLLOW);
        how.mode = 0;
        error = File.OpenHow(dirFd, Path, how);
    }
    if (error)
        return TError(error.Errno == EPERM ? EError::Permission : EError::InvalidValue, error.Errno, error.Text);
    return OK;
}

TError TStream::OpenHost(const TFile &dir, uid_t uid, gid_t gid) {
    for (;;) {
        auto error = OpenFile(dir.Fd, O_WRONLY | O_APPEND | O_NOFOLLOW, RESOLVE_NO_MAGICLINKS);
        if (error.Errno != ENOENT)
            return error;
        error = OpenFile(dir.Fd, O_WRONLY | O_APPEND | O_NOFOLLOW | O_CREAT | O_EXCL, RESOLVE_NO_MAGICLINKS);
        if (error.Errno == EEXIST)
            continue;
        if (error)
            return error;
        return File.Chown(uid, gid);
    }
}

TError TStream::OpenContainer() {
    int flags = Target ? O_WRONLY | O_APPEND : O_RDONLY;
    auto error = OpenFile(AT_FDCWD, flags, RESOLVE_NO_MAGICLINKS);
    if (!Target || error.Errno != ENOENT)
        return error;

    error = OpenFile(AT_FDCWD, flags | O_CREAT | O_EXCL, RESOLVE_NO_MAGICLINKS);
    // A concurrent creator may have supplied the file. Never chown it.
    if (error.Errno == EEXIST)
        return OpenFile(AT_FDCWD, flags, RESOLVE_NO_MAGICLINKS);
    if (error)
        return error;
    return File.Chown(geteuid(), getegid());
}

static TError TakeStreamPin(const runtime::TStreamSpec &spec, std::vector<TFile> &fds, TFile &pin) {
    if (!spec.IsInitialized())
        return TError(EError::InvalidValue, "Incomplete stream specification");
    if ((spec.source() == runtime::TStreamSpec::Host || spec.source() == runtime::TStreamSpec::Redirect) !=
        spec.has_pin())
        return TError(EError::InvalidValue, "Invalid stream descriptor specification");
    if (spec.has_pin()) {
        if (spec.pin() >= fds.size() || !fds[spec.pin()])
            return TError(EError::InvalidValue, "Invalid stream descriptor index");
        pin.Swap(fds[spec.pin()]);
    }
    return OK;
}

TError TStream::Open(const runtime::TStreamSpec &spec, std::vector<TFile> &fds) {
    TFile pin;
    auto error = TakeStreamPin(spec, fds, pin);
    if (error)
        return error;

    Path = spec.path();
    int flags = Target ? O_WRONLY | O_APPEND : O_RDONLY;
    switch (spec.source()) {
    case runtime::TStreamSpec::Container:
        return OK;
    case runtime::TStreamSpec::Host:
        return OpenHost(pin, spec.uid(), spec.gid());
    case runtime::TStreamSpec::Redirect:
        Path = pin.ProcPath();
        return OpenFile(AT_FDCWD, flags, 0);
    case runtime::TStreamSpec::Null:
        Path = "/dev/null";
        return OpenFile(AT_FDCWD, flags, RESOLVE_NO_MAGICLINKS);
    default:
        return TError(EError::InvalidValue, "Invalid stream source");
    }
}

TError TStream::Install() {
    if (!File) {
        auto error = OpenContainer();
        if (error)
            return error;
    }
    auto tty = isatty(File.Fd);
    int flags = tty ? O_RDWR : (Target ? O_APPEND : 0);

    if (dup2(File.Fd, Target) < 0 || fcntl(Target, F_SETFL, flags) < 0 || fcntl(Target, F_SETFD, 0) < 0)
        return TError::System("Cannot install {} to stream {}", File.Fd, Target);
    if (File.Fd == Target)
        File.SetFd = -1;
    File.Close();
    // A stream's terminal need not become the controlling terminal.
    if (tty)
        (void)ioctl(Target, TIOCSCTTY, 0);
    return OK;
}
