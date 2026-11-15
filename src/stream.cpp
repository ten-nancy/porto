#include "stream.hpp"

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/ioctl.h>
#include <unistd.h>

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

TStreamSource::TStreamSource(const TPath &path)
    : DirFd(AT_FDCWD),
      Path(path),
      Resolve(RESOLVE_NO_MAGICLINKS)
{}

TStreamSource::TStreamSource(const TFile &dir, const TPath &path)
    : DirFd(dir.Fd),
      Path(path),
      Resolve(RESOLVE_NO_MAGICLINKS)
{}

TStreamSource::TStreamSource(const TFile &pin)
    : DirFd(AT_FDCWD),
      Path(pin.ProcPath()),
      Resolve(0)
{}

TError TStreamSource::Open(int flags, TFile &file) const {
    struct open_how how = {};
    how.flags = flags | O_NOCTTY | O_NONBLOCK | O_CLOEXEC;
    how.mode = flags & O_CREAT ? 0660 : 0;
    how.resolve = Resolve;
    auto error = file.OpenHow(DirFd, Path, how);
    if (!error && isatty(file.Fd)) {
        how.flags = O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC;
        how.mode = 0;
        error = file.OpenHow(DirFd, Path, how);
    }
    if (error)
        return TError(error.Errno == EPERM ? EError::Permission : EError::InvalidValue, error.Errno, error.Text);
    return OK;
}

TError TStreamSource::OpenInput(TFile &file) const {
    return Open(O_RDONLY, file);
}

TError TStreamSource::OpenOutput(TFile &file) const {
    return Open(O_WRONLY | O_APPEND, file);
}

TError TStreamSource::OpenOrCreateOutput(TFile &file) const {
    auto error = OpenOutput(file);
    if (error.Errno != ENOENT)
        return error;

    error = Open(O_WRONLY | O_APPEND | O_CREAT | O_EXCL, file);
    // A concurrent creator may have supplied the file. Never chown it.
    if (error.Errno == EEXIST)
        return OpenOutput(file);
    if (error)
        return error;
    return file.Chown(geteuid(), getegid());
}

TError TStreamSource::OpenOwnedOutput(const TCred &cred, TFile &file) const {
    for (;;) {
        auto error = Open(O_WRONLY | O_APPEND | O_NOFOLLOW, file);
        if (error.Errno != ENOENT)
            return error;
        error = Open(O_WRONLY | O_APPEND | O_NOFOLLOW | O_CREAT | O_EXCL, file);
        if (error.Errno == EEXIST)
            continue;
        if (error)
            return error;
        return file.Chown(cred);
    }
}

static TError InstallStream(TFile &file, int target) {
    auto tty = isatty(file.Fd);
    int flags = tty ? O_RDWR : (target ? O_APPEND : 0);

    if (dup2(file.Fd, target) < 0 || fcntl(target, F_SETFL, flags) < 0 || fcntl(target, F_SETFD, 0) < 0)
        return TError::System("Cannot install {} to stream {}", file.Fd, target);
    if (file.Fd == target)
        file.SetFd = -1;
    file.Close();
    // A stream's terminal need not become the controlling terminal.
    if (tty)
        (void)ioctl(target, TIOCSCTTY, 0);
    return OK;
}

TError TStream::Install() {
    if (File)
        return InstallStream(File, Target);
    TFile file;
    const TStreamSource source(Path);
    auto error = Target ? source.OpenOrCreateOutput(file) : source.OpenInput(file);
    if (error)
        return error;
    return InstallStream(file, Target);
}
