// Standalone private startup protocol tests. No daemon connection or reload.
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "common.hpp"
#include "rpc.pb.h"
#include "task.pb.h"
#include "util/log.hpp"

using std::string;
using std::vector;
static string Binary, Self, Init;
static bool CloseStdio;
#define CHECK(expr)                                                                 \
    do {                                                                            \
        if (!(expr)) {                                                              \
            std::cerr << __LINE__ << ": " << #expr << " (errno " << errno << ")\n"; \
            exit(1);                                                                \
        }                                                                           \
    } while (0)

static string Read(const string &path) {
    std::ifstream input(path);
    CHECK(input.good());
    return string(std::istreambuf_iterator<char>(input), {});
}

static string Link(const string &path) {
    char data[PATH_MAX];
    auto size = readlink(path.c_str(), data, sizeof(data));
    CHECK(size >= 0 && size < (ssize_t)sizeof(data));
    return string(data, size);
}

static string Proc(pid_t pid) {
    return "/proc/" + std::to_string(pid);
}

struct TTemp {
    string Path;
    TTemp() {
        char path[] = "/tmp/portorun-test-XXXXXX";
        CHECK(mkdtemp(path));
        Path = path;
    }
    ~TTemp() {
        // Only the host-side stream and work directory can exist outside private mounts.
        unlink((Path + "/stdout").c_str());
        rmdir((Path + "/work").c_str());
        rmdir(Path.c_str());
    }
};

struct TFd {
    int Fd;
    explicit TFd(int fd)
        : Fd(fd)
    {
        CHECK(fd >= 0);
    }
    ~TFd() {
        close(Fd);
    }
    TFd(const TFd &) = delete;
    TFd &operator=(const TFd &) = delete;
};

static int Temporary() {
    char path[] = "/tmp/portorun-output-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(!unlink(path));
    return fd;
}

static string Output(int fd) {
    CHECK(lseek(fd, 0, SEEK_SET) == 0);
    string data;
    char buffer[4096];
    ssize_t size;
    while ((size = read(fd, buffer, sizeof(buffer))) > 0)
        data.append(buffer, size);
    CHECK(size == 0);
    return data;
}

static void Write(int fd, const void *data, size_t size) {
    while (size) {
        auto count = send(fd, data, size, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        CHECK(count > 0);
        data = static_cast<const char *>(data) + count;
        size -= count;
    }
}

static bool Exact(int fd, void *data, size_t size) {
    size_t done = 0;
    while (done < size) {
        auto count = recv(fd, static_cast<char *>(data) + done, size - done, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (!count) {
            CHECK(!done);
            return false;
        }
        CHECK(count > 0);
        done += count;
    }
    return true;
}

static void SendFd(int socket, int fd) {
    char byte = 'F';
    iovec io{&byte, 1};
    union {
        cmsghdr Align;
        char Data[CMSG_SPACE(sizeof(int))];
    } control{};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = control.Data;
    msg.msg_controllen = sizeof(control.Data);
    auto cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(fd));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    CHECK(sendmsg(socket, &msg, MSG_NOSIGNAL) == 1);
}

static int ReceiveFd(int socket) {
    char byte;
    iovec io{&byte, 1};
    union {
        cmsghdr Align;
        char Data[CMSG_SPACE(sizeof(int))];
    } control{};
    msghdr msg{};
    msg.msg_iov = &io;
    msg.msg_iovlen = 1;
    msg.msg_control = control.Data;
    msg.msg_controllen = sizeof(control.Data);
    CHECK(recvmsg(socket, &msg, MSG_CMSG_CLOEXEC) == 1);
    CHECK(!(msg.msg_flags & MSG_CTRUNC));
    auto cmsg = CMSG_FIRSTHDR(&msg);
    CHECK(cmsg && !CMSG_NXTHDR(&msg, cmsg));
    CHECK(cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
    CHECK(cmsg->cmsg_len == CMSG_LEN(sizeof(int)));
    int fd;
    memcpy(&fd, CMSG_DATA(cmsg), sizeof(fd));
    return fd;
}

static pid_t ReceivePid(int socket) {
    TFd received(ReceiveFd(socket));
    std::istringstream info(Read("/proc/self/fdinfo/" + std::to_string(received.Fd)));
    string line;
    while (std::getline(info, line))
        if (line.compare(0, 4, "Pid:") == 0)
            return atoi(line.c_str() + 4);
    CHECK(false);
    return -1;
}

struct TResponse {
    vector<int> Errors;
    string Text;
    std::map<int, pid_t> Pids;
    std::map<pid_t, int> Statuses;
    pid_t Runner;
};

static void Receive(int socket, TResponse &result) {
    int code;
    while (Exact(socket, &code, sizeof(code))) {
        if (code == 8321) {
            int header[3];
            CHECK(Exact(socket, header, sizeof(header)));
            CHECK(header[2] >= 0 && header[2] <= 16 * 1024 * 1024);
            result.Text.resize(header[2]);
            if (header[2])
                CHECK(Exact(socket, &result.Text[0], header[2]));
            result.Errors.push_back(header[0]);
            code = 0;
            Write(socket, &code, sizeof(code));
            if (header[0])
                return;
            CHECK(header[1] == 0 && result.Text.empty());
        } else {
            CHECK(code == 8322 || code == 8323);
            CHECK(!result.Pids.count(code));
            result.Pids[code] = ReceivePid(socket);
            CHECK(result.Pids[code] > 0);
        }
    }
}

static pid_t Launch(int socket) {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid)
        return pid;
    CHECK(dup2(socket, 3) == 3);
    CHECK(!fcntl(3, F_SETFD, 0));
    rlimit limit;
    CHECK(!getrlimit(RLIMIT_NOFILE, &limit));
    for (rlim_t fd = 4; fd < limit.rlim_cur; ++fd)
        close(fd);
    if (CloseStdio)
        for (int fd = 0; fd < 3; ++fd)
            close(fd);
    char *args[] = {const_cast<char *>(Binary.c_str()), const_cast<char *>("run"), nullptr};
    char *env[] = {nullptr};
    execve(Binary.c_str(), args, env);
    _exit(127);
}

static TResponse Exchange(const string &data, const vector<int> &fds = {}) {
    int pair[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair));
    TFd parent(pair[0]), child(pair[1]);
    timeval timeout{10, 0};
    CHECK(!setsockopt(parent.Fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)));
    TResponse result;
    result.Runner = Launch(child.Fd);
    // Close the child's endpoint before waiting for EOF.
    close(child.Fd);
    child.Fd = -1;
    int size = data.size();
    Write(parent.Fd, &size, sizeof(size));
    Write(parent.Fd, data.data(), data.size());
    size = fds.size();
    Write(parent.Fd, &size, sizeof(size));
    for (int fd: fds)
        SendFd(parent.Fd, fd);
    Receive(parent.Fd, result);
    return result;
}

static TResponse Exchange(const runtime::TTaskSpec &spec, const vector<int> &fds = {}) {
    string data;
    CHECK(spec.SerializeToString(&data));
    return Exchange(data, fds);
}

static void Reap(TResponse &result) {
    int status;
    pid_t pid;
    do {
        pid = waitpid(-1, &status, 0);
        CHECK(pid > 0);
        result.Statuses[pid] = WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
    } while (!result.Statuses.count(result.Runner) ||
             (result.Pids.count(8322) && !result.Statuses.count(result.Pids.at(8322))));
}

static TResponse Run(const runtime::TTaskSpec &spec, const vector<int> &fds = {}) {
    auto result = Exchange(spec, fds);
    Reap(result);
    return result;
}

static void Success(const TResponse &result, int status = 0) {
    CHECK(result.Errors == vector<int>{0});
    CHECK(result.Pids.size() == 2);
    CHECK(result.Statuses.at(result.Pids.at(8322)) == status);
    for (auto entry: result.Statuses)
        CHECK(entry.second == 0 || entry.first == result.Pids.at(8322));
}

static void Stop(TResponse &result) {
    CHECK(!kill(result.Pids.at(8322), SIGKILL));
    Reap(result);
    Success(result, -SIGKILL);
}

static runtime::TTaskSpec Spec(const vector<string> &argv = {"/bin/true"}) {
    runtime::TTaskSpec spec;
    spec.set_name("portorun-test");
    spec.set_id(1);
    spec.set_uid(0);
    spec.set_gid(0);
    spec.set_login_uid(UINT_MAX);
    spec.set_command_argv(true);
    for (auto &arg: argv)
        spec.add_argv(arg);
    // Explicit proto defaults, including required fields, without reflection.
    auto mount = spec.mutable_mount();
    mount->set_cwd("/");
    mount->set_root("/");
    mount->set_host_root("/");
    mount->set_root_ro(false);
    mount->set_bind_porto_sock(false);
    mount->set_isolate_run(false);
    mount->set_run_size(0);
    mount->set_systemd("");
    mount->set_bind_uid(0);
    mount->set_bind_gid(0);
    mount->set_mount_cgroup_fs(false);
    mount->set_rw_cgroup_fs(false);
    mount->set_cgroup2(false);
    mount->set_hugetlb(false);
    mount->set_enable_tracefs(false);
    mount->set_rw_net_cgroups(false);
    mount->set_dev_size(0);
    mount->set_devpts_max(0);
    for (int i = 0; i < 3; ++i)
        spec.add_streams()->set_source(runtime::TStreamSpec::Null);
    return spec;
}

static void Redirect(runtime::TTaskSpec &spec, int stream, int pin) {
    spec.mutable_streams(stream)->set_source(runtime::TStreamSpec::Redirect);
    spec.mutable_streams(stream)->set_pin(pin);
}

static string Capture(runtime::TTaskSpec spec, vector<int> fds = {}) {
    TFd output(Temporary());
    Redirect(spec, 1, fds.size());
    fds.push_back(output.Fd);
    Success(Run(spec, fds));
    return Output(output.Fd);
}

static runtime::TTaskSpec Probe(const string &mode) {
    return Spec({Self, "--probe", mode});
}

static string MountState() {
    std::istringstream info(Read("/proc/self/mountinfo"));
    string line, result = Link("/proc/self/ns/mnt") + "\n";
    while (std::getline(info, line)) {
        std::istringstream fields(line);
        string field;
        for (int i = 0; i < 5; ++i)
            fields >> field;
        if (field == "/" || field == "/proc" || field == "/proc/sys" || field == "/sys" || field == "/sys/fs/cgroup")
            result += line + "\n";
    }
    return result;
}

static bool ReadOnly(const string &path) {
    struct statvfs info;
    CHECK(!statvfs(path.c_str(), &info));
    return info.f_flag & ST_RDONLY;
}

static void Credentials() {
    gid_t groups[16];
    int size = getgroups(16, groups);
    CHECK(size == 2);
    std::sort(groups, groups + size);
    std::cout << getuid() << ' ' << getgid() << ' ' << groups[0] << ' ' << groups[1] << ' ' << umask(0) << '\n';
    std::istringstream status(Read("/proc/self/status"));
    string line;
    while (std::getline(status, line))
        if (line.compare(0, 7, "CapEff:") == 0 || line.compare(0, 7, "CapBnd:") == 0)
            std::cout << line << '\n';
}

static void Descriptors() {
    auto directory = opendir("/proc/self/fd");
    CHECK(directory);
    vector<int> fds;
    while (auto entry = readdir(directory)) {
        if (entry->d_name[0] == '.')
            continue;
        int fd = atoi(entry->d_name);
        if (fd != dirfd(directory))
            fds.push_back(fd);
    }
    CHECK(!closedir(directory));
    std::sort(fds.begin(), fds.end());
    for (int fd: fds)
        std::cout << fd << '\n';
}

static int Payload(const string &mode) {
    if (mode == "pid")
        std::cout << getpid() << '\n';
    else if (mode == "net")
        std::cout << Link("/proc/self/ns/net") << '\n';
    else if (mode == "seccomp") {
        errno = 0;
        long result = syscall(SYS_getppid);
        std::cout << result << ' ' << errno << '\n';
    } else if (mode == "mount") {
        std::istringstream stat(Read("/proc/self/stat"));
        int pid;
        stat >> pid;
        std::cout << getpid() << ' ' << pid << ' ' << ReadOnly("/proc/sys") << ' ' << ReadOnly("/sys") << '\n';
    } else if (mode == "creds")
        Credentials();
    else if (mode == "fds")
        Descriptors();
    else
        CHECK(false);
    return 0;
}

static void TrueTest() {
    Success(Run(Spec()));
}

static void StatisticsTest() {
    TFd file(open(PORTOD_STAT_FILE, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
    CHECK(!ftruncate(file.Fd, sizeof(TStatistics)));
    auto stats =
        static_cast<TStatistics *>(mmap(nullptr, sizeof(TStatistics), PROT_READ | PROT_WRITE, MAP_SHARED, file.Fd, 0));
    CHECK(stats != MAP_FAILED);
    stats->Errors = 7;
    stats->LogLines = 11;

    TFd log(Temporary());
    auto spec = Spec();
    spec.set_log_fd(0);
    spec.set_resctrl_group("portorun-test-missing-" + std::to_string(getpid()));
    CHECK(access(("/sys/fs/resctrl/" + spec.resctrl_group()).c_str(), F_OK) < 0 && errno == ENOENT);
    Success(Run(spec, {log.Fd}));

    auto text = Output(log.Fd);
    CHECK(text.find("Failed attach to resctrl") != string::npos);
    CHECK(stats->Errors == 8);
    CHECK(stats->LogLines == 11 + static_cast<uint64_t>(std::count(text.begin(), text.end(), '\n')));
    CHECK(stats->LogBytes == text.size());
    CHECK(!munmap(stats, sizeof(TStatistics)));
}

static void ExitTest() {
    for (auto command: {"exit 130", "kill -TERM $$"}) {
        auto result = Run(Spec({"/bin/sh", "-c", command}));
        Success(result, command[0] == 'e' ? 130 : -SIGTERM);
        CHECK(result.Pids.at(8322) == result.Pids.at(8323));
    }
}

static void SeccompTest() {
    auto spec = Probe("seccomp");
    spec.mutable_seccomp()->mutable_default_action()->set_action(seccomp::Allow);
    auto rule = spec.mutable_seccomp()->add_rules();
    rule->set_syscall("getppid");
    rule->mutable_action()->set_action(seccomp::Errno);
    rule->mutable_action()->set_value(EPERM);
    CHECK(Capture(spec) == "-1 1\n");
}

static void QuadroTest() {
    auto spec = Spec({"/bin/sh", "-c", "exit 23"});
    spec.set_isolate(true);
    spec.set_quadro_fork(true);
    spec.set_init_fd(0);
    spec.set_init_cap_bound(1 << 5);
    TFd init(open(Init.c_str(), O_RDONLY));
    auto result = Run(spec, {init.Fd});
    Success(result, 23);
    CHECK(result.Pids.at(8322) != result.Pids.at(8323));
}

static void LowTest() {
    CloseStdio = true;
    auto spec = Spec({"/bin/sh", "-c", "printf payload; printf error >&2; exit 29"});
    spec.set_isolate(true);
    spec.set_quadro_fork(true);
    spec.set_init_fd(0);
    spec.set_init_cap_bound(1 << 5);
    spec.set_log_fd(1);
    spec.set_operation_id("deadbeef");
    Redirect(spec, 1, 2);
    Redirect(spec, 2, 3);
    TFd init(open(Init.c_str(), O_RDONLY)), log(Temporary()), out(Temporary()), err(Temporary());
    Success(Run(spec, {init.Fd, log.Fd, out.Fd, err.Fd}), 29);
    CHECK(Output(out.Fd) == "payload");
    CHECK(Output(err.Fd) == "error");
    CHECK(Output(log.Fd).find("][deadbeef]:") != string::npos);
    CHECK(Output(log.Fd).find("Exec") != string::npos);
}

static void MetaTest() {
    auto spec = Spec();
    spec.set_meta(true);
    spec.set_init_fd(0);
    TFd init(open(Init.c_str(), O_RDONLY));
    auto result = Exchange(spec, {init.Fd});
    struct stat actual, expected;
    CHECK(!stat((Proc(result.Pids.at(8323)) + "/exe").c_str(), &actual));
    CHECK(!stat(Init.c_str(), &expected));
    CHECK(actual.st_ino == expected.st_ino && actual.st_dev == expected.st_dev);
    Stop(result);
}

static void IsolateTest() {
    auto spec = Probe("pid");
    spec.set_isolate(true);
    CHECK(Capture(spec) == "1\n");
}

static void MountTest() {
    auto before = MountState();
    auto spec = Probe("mount");
    spec.set_isolate(true);
    spec.set_new_mount_ns(true);
    CHECK(Capture(spec) == "1 1 1 1\n");
    CHECK(MountState() == before);
}

static void RootMounts(runtime::TTaskSpec &spec) {
    spec.mutable_mount()->set_run_size(1024 * 1024);
    spec.mutable_mount()->set_dev_size(1024 * 1024);
    spec.mutable_mount()->set_devpts_max(4);
    auto link = spec.mutable_mount()->add_symlink();
    link->set_path("/test-link");
    link->set_target("/dev/null");
    auto device = spec.add_devices();
    device->set_path("/dev/null");
    device->set_node(makedev(1, 3));
    device->set_uid(0);
    device->set_gid(0);
    device->set_mode(S_IFCHR | 0666);
    device->set_may_mknod(true);
}

static void RootTest() {
    auto before = MountState();
    auto spec = Spec();
    spec.set_meta(true);
    spec.set_isolate(true);
    spec.set_new_mount_ns(true);
    spec.set_init_fd(0);
    TTemp directory;
    spec.mutable_mount()->set_root(directory.Path);
    RootMounts(spec);
    TFd init(open(Init.c_str(), O_RDONLY));
    auto result = Exchange(spec, {init.Fd});
    CHECK(result.Errors == vector<int>{0});
    string root = Proc(result.Pids.at(8323)) + "/root";
    struct stat info;
    CHECK(!stat((root + "/dev/null").c_str(), &info));
    CHECK(S_ISCHR(info.st_mode) && info.st_rdev == makedev(1, 3));
    CHECK(Link(root + "/dev/ptmx") == "pts/ptmx");
    CHECK(Link(root + "/dev/fd") == "/proc/self/fd");
    CHECK(Link(root + "/test-link") == "dev/null");
    CHECK(Read(root + "/proc/1/status").find("Pid:\t1\n") != string::npos);
    CHECK(ReadOnly(root + "/proc/sys"));
    CHECK(access((directory.Path + "/dev/null").c_str(), F_OK) < 0 && errno == ENOENT);
    Stop(result);
    CHECK(MountState() == before);
}

static void NamespaceAnchor(const string &root, int ready) {
    CHECK(!unshare(CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWNS | CLONE_NEWPID | CLONE_NEWCGROUP | CLONE_NEWNET));
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(!chroot(root.c_str()));
        CHECK(!chdir(root == "/" ? "/" : "/work"));
        char byte = 'R';
        CHECK(write(ready, &byte, 1) == 1);
        close(ready);
        for (;;)
            pause();
    }
    close(ready);
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    _exit(0);
}

struct TParent {
    pid_t Anchor, Pid;
    explicit TParent(const string &root = "/") {
        int ready[2];
        CHECK(!pipe2(ready, O_CLOEXEC));
        Anchor = fork();
        CHECK(Anchor >= 0);
        if (!Anchor) {
            close(ready[0]);
            NamespaceAnchor(root, ready[1]);
        }
        close(ready[1]);
        TFd pipe(ready[0]);
        pollfd event{pipe.Fd, POLLIN, 0};
        CHECK(poll(&event, 1, 10000) == 1);
        char byte;
        CHECK(read(pipe.Fd, &byte, 1) == 1 && byte == 'R');
        std::istringstream children(Read(Proc(Anchor) + "/task/" + std::to_string(Anchor) + "/children"));
        CHECK(bool(children >> Pid));
        CHECK(!(children >> byte));
    }
    ~TParent() {
        kill(Pid, SIGKILL);
        int status;
        CHECK(waitpid(Anchor, &status, 0) == Anchor);
    }
};

static void SharedTest() {
    TParent parent;
    TFd pidfd(syscall(SYS_pidfd_open, parent.Pid, 0));
    for (auto command: {"exit 130", "kill -TERM $$"}) {
        auto spec = Spec({"/bin/sh", "-c", command});
        spec.set_pid_fd(0);
        auto result = Run(spec, {pidfd.Fd});
        Success(result, command[0] == 'e' ? 130 : -SIGTERM);
        CHECK(result.Pids.at(8322) == result.Pids.at(8323));
    }
}

static void TripleTest() {
    TParent parent;
    TFd pidfd(syscall(SYS_pidfd_open, parent.Pid, 0));
    auto spec = Probe("pid");
    spec.set_isolate(true);
    spec.set_pid_fd(0);
    CHECK(Capture(spec, {pidfd.Fd}) == "1\n");
}

static void ParentTest() {
    TTemp directory;
    CHECK(!mkdir((directory.Path + "/work").c_str(), 0755));
    TParent parent(directory.Path);
    TFd pidfd(syscall(SYS_pidfd_open, parent.Pid, 0)), init(open(Init.c_str(), O_RDONLY));
    auto spec = Spec();
    spec.set_meta(true);
    spec.set_pid_fd(0);
    spec.set_init_fd(1);
    spec.mutable_mount()->set_cwd(".");
    auto result = Exchange(spec, {pidfd.Fd, init.Fd});
    CHECK(result.Errors == vector<int>{0});
    for (auto kind: {"ipc", "uts", "pid", "mnt", "cgroup", "net"})
        CHECK(Link(Proc(result.Pids.at(8323)) + "/ns/" + kind) == Link(Proc(parent.Pid) + "/ns/" + kind));
    CHECK(Link(Proc(result.Pids.at(8322)) + "/root") == "/");
    CHECK(Link(Proc(result.Pids.at(8323)) + "/root") == "/");
    CHECK(Link(Proc(result.Pids.at(8323)) + "/cwd") == directory.Path + "/work");
    Stop(result);
}

static void NetworkTest() {
    TParent parent;
    TFd pidfd(syscall(SYS_pidfd_open, parent.Pid, 0)), net(open("/proc/self/ns/net", O_RDONLY));
    CHECK(Link(Proc(parent.Pid) + "/ns/net") != Link("/proc/self/ns/net"));
    auto spec = Probe("net");
    spec.set_pid_fd(0);
    spec.set_net_fd(1);
    CHECK(Capture(spec, {pidfd.Fd, net.Fd}) == Link("/proc/self/ns/net") + "\n");
}

static void Rejected(const TResponse &result) {
    CHECK(!result.Errors.empty() && result.Errors.back() != 0);
    CHECK(result.Pids.empty());
}

static void DeadTest() {
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        for (;;)
            pause();
    }
    TFd fd(syscall(SYS_pidfd_open, pid, 0));
    CHECK(!kill(pid, SIGKILL));
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    auto spec = Spec();
    spec.set_pid_fd(0);
    Rejected(Run(spec, {fd.Fd}));
}

static void NamespaceFdTest() {
    TFd fd(open("/proc/self/ns/pid", O_RDONLY));
    auto spec = Spec();
    spec.set_pid_fd(0);
    Rejected(Run(spec, {fd.Fd}));
}

static void CredsTest() {
    auto spec = Probe("creds");
    spec.set_uid(65534);
    spec.set_gid(65534);
    spec.add_groups(65534);
    spec.add_groups(1);
    spec.set_umask(0027);
    CHECK(Capture(spec) == "65534 65534 1 65534 23\nCapEff:\t0000000000000000\nCapBnd:\t0000000000000000\n");
}

static void WordexpTest() {
    auto spec = Spec();
    spec.set_command_argv(false);
    spec.set_command("/bin/echo \"$SPECIAL\"");
    auto env = spec.add_env();
    env->set_name("SPECIAL");
    env->set_value("space ; literal $HOME");
    CHECK(Capture(spec) == "space ; literal $HOME\n");
}

static void SubstitutionTest() {
    auto spec = Spec();
    spec.set_command_argv(false);
    spec.set_command("/bin/echo $(printf forbidden)");
    CHECK(Run(spec).Errors.back() == rpc::InvalidCommand);
}

static void CwdTest() {
    auto spec = Spec();
    spec.mutable_mount()->set_cwd("/nonexistent-portorun-cwd");
    CHECK(Run(spec).Errors.back() == rpc::InvalidPath);
}

static void HostOutputTest() {
    TTemp directory;
    TFd fd(open(directory.Path.c_str(), O_PATH | O_DIRECTORY));
    auto spec = Spec({"/bin/echo", "host output"});
    auto stream = spec.mutable_streams(1);
    stream->set_source(runtime::TStreamSpec::Host);
    stream->set_pin(0);
    stream->set_path("stdout");
    stream->set_uid(65534);
    stream->set_gid(65534);
    Success(Run(spec, {fd.Fd}));
    CHECK(Read(directory.Path + "/stdout") == "host output\n");
    struct stat info;
    CHECK(!stat((directory.Path + "/stdout").c_str(), &info));
    CHECK(info.st_uid == 65534 && info.st_gid == 65534);
}

static void ContainerOutputTest() {
    TTemp directory;
    auto spec = Spec({"/bin/sh", "-c", "test \"$(wc -c)\" -eq 0 && printf empty"});
    spec.mutable_streams(1)->set_source(runtime::TStreamSpec::Container);
    spec.mutable_streams(1)->set_path(directory.Path + "/stdout");
    Success(Run(spec));
    CHECK(Read(directory.Path + "/stdout") == "empty");
}

static void ExecTest() {
    auto result = Run(Spec({"/nonexistent-portorun-command"}));
    CHECK(result.Errors.back() == rpc::InvalidCommand);
    CHECK(result.Text.find("cannot exec") != string::npos);
}

static void MalformedTest() {
    auto result = Exchange(string(1, '\x80'));
    Reap(result);
    Rejected(result);
}

static void MissingInitTest() {
    auto spec = Spec();
    spec.set_meta(true);
    auto result = Run(spec);
    CHECK(result.Errors.back() != 0);
}

static void InvalidStreamTest() {
    auto spec = Spec();
    Redirect(spec, 1, 0);
    CHECK(Run(spec).Errors.back() != 0);
}

static void ArgvTest() {
    auto spec = Spec({"/bin/sh", "-c", "printf '%s|%s|' \"$1\" \"$SPECIAL\"; cat; printf error >&2", "sh",
                      "arg ; $HOME \"quotes\""});
    auto env = spec.add_env();
    env->set_name("SPECIAL");
    env->set_value("env ; $PATH = spaces");
    TFd in(Temporary()), out(Temporary()), err(Temporary());
    CHECK(write(in.Fd, "input data\n", 11) == 11);
    // Match the existing test: the helper must reopen the input from offset zero.
    for (int i = 0; i < 3; ++i)
        Redirect(spec, i, i);
    Success(Run(spec, {in.Fd, out.Fd, err.Fd}));
    CHECK(Output(out.Fd) == "arg ; $HOME \"quotes\"|env ; $PATH = spaces|input data\n");
    CHECK(Output(err.Fd) == "error");
}

static void FdsTest() {
    TFd out(Temporary()), unused(Temporary());
    auto spec = Probe("fds");
    Redirect(spec, 1, 0);
    Success(Run(spec, {out.Fd, unused.Fd}));
    CHECK(Output(out.Fd) == "0\n1\n2\n");
}

static void PeerTest() {
    int pair[2];
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
    TResponse result;
    result.Runner = Launch(pair[1]);
    close(pair[0]);
    close(pair[1]);
    Reap(result);
    CHECK(!result.Statuses.empty());
}

static void Cleanup() {
    // Each case has its own subreaper. Repeatedly kill newly adopted descendants.
    for (;;) {
        std::istringstream children(Read("/proc/self/task/" + std::to_string(getpid()) + "/children"));
        pid_t pid;
        bool found = false;
        while (children >> pid) {
            kill(pid, SIGKILL);
            found = true;
        }
        if (!found)
            return;
        int status;
        while (waitpid(-1, &status, WNOHANG) > 0) {
        }
        usleep(1000);
    }
}

struct TCase {
    const char *Name;
    void (*Run)();
};
static const TCase Cases[] = {{"true", TrueTest}, {"exit_status", ExitTest}, {"statistics", StatisticsTest},
                              {"shared_pid_namespace_without_waiter", SharedTest},
                              {"seccomp", SeccompTest},
                              {"low_descriptors", LowTest},
                              {"quadro_fork", QuadroTest},
                              {"meta_init", MetaTest},
                              {"isolated_pid_namespace", IsolateTest},
                              {"mount_namespace_proc_protection", MountTest},
                              {"temporary_root_mounts_devices_symlinks", RootTest},
                              {"triple_fork_nested_pid_namespace", TripleTest},
                              {"parent_pidfd_namespaces_root_cwd", ParentTest},
                              {"parent_pidfd_network_override", NetworkTest},
                              {"dead_parent_pidfd", DeadTest},
                              {"parent_pidfd_rejects_namespace_fd", NamespaceFdTest},
                              {"credentials_caps_umask", CredsTest},
                              {"wordexp", WordexpTest},
                              {"wordexp_rejects_substitution", SubstitutionTest},
                              {"invalid_cwd", CwdTest},
                              {"host_output_creation", HostOutputTest},
                              {"container_output_null_input", ContainerOutputTest},
                              {"exec_failure", ExecTest},
                              {"malformed_spec", MalformedTest},
                              {"missing_init", MissingInitTest},
                              {"invalid_stream_fd", InvalidStreamTest},
                              {"argv_env_stdio", ArgvTest},
                              {"no_inherited_fds", FdsTest},
                              {"peer_close", PeerTest}};

static bool RunCase(const TCase &test) {
    std::cout << test.Name << ": " << std::flush;
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (!pid) {
        CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
        CHECK(!unshare(CLONE_NEWNS));
        CHECK(!mount(nullptr, "/", nullptr, MS_REC | MS_PRIVATE, nullptr));
        CHECK(!mount("tmpfs", "/run", "tmpfs", 0, "mode=755"));
        alarm(30);
        test.Run();
        exit(0);
    }
    int status;
    CHECK(waitpid(pid, &status, 0) == pid);
    Cleanup();
    bool ok = WIFEXITED(status) && !WEXITSTATUS(status);
    std::cout << (ok ? "OK" : "FAILED") << '\n';
    return ok;
}

int main(int argc, char **argv) {
    Self = Link("/proc/self/exe");
    if (argc == 3 && string(argv[1]) == "--probe")
        return Payload(argv[2]);
    if (geteuid()) {
        std::cout << "SKIP: requires root\n";
        return 77;
    }
    const char *binary = getenv("PORTOD_BINARY");
    char resolved[PATH_MAX];
    CHECK(realpath(binary ? binary : "./portod", resolved));
    Binary = resolved;
    Init = Binary.substr(0, Binary.rfind('/') + 1) + "portoinit";
    CHECK(!access(Binary.c_str(), X_OK));
    CHECK(!prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0));
    int failures = 0, count = 0;
    for (auto &test: Cases) {
        if (argc > 1 && string(argv[1]) != test.Name)
            continue;
        ++count;
        if (!RunCase(test))
            ++failures;
    }
    CHECK(count);
    std::cout << count << " tests, " << failures << " failures\n";
    return failures ? 1 : 0;
}
