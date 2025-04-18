#include "unix.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <string>

#include "util/cred.hpp"
#include "util/log.hpp"
#include "util/path.hpp"
#include "util/proc.hpp"
#include "util/string.hpp"

extern "C" {
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <malloc.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
}

#ifndef PR_SET_DUMPABLE_INIT_NS
#define PR_SET_DUMPABLE_INIT_NS 0x59410002
#endif

uint64_t TaskHandledSignals(pid_t pid) {
    std::string path = "/proc/" + std::to_string(pid) + "/stat";
    unsigned long mask;
    FILE *file;
    int res;

    file = fopen(path.c_str(), "r");
    if (!file)
        return 0;
    res = fscanf(file,
                 "%*d (%*[^)]) %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %*d "
                 "%*u %*u %*u %*u %*u %*u %*u %*u %*u %lu", &mask);
    fclose(file);
    return res == 1 ? mask : 0;
}

pid_t GetPid() {
    return syscall(SYS_getpid);
}

pid_t GetPPid() {
    return syscall(SYS_getppid);
}

pid_t GetTid() {
    return syscall(SYS_gettid);
}

TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens) {
    struct dirent *de;
    FILE *file;
    DIR *dir;
    int child_pid, parent_pid;

    childrens.clear();

    dir = opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (!dir)
        goto full_scan;

    while ((de = readdir(dir))) {
        file = fopen(("/proc/" + std::to_string(pid) + "/task/" + std::string(de->d_name) + "/children").c_str(), "r");
        if (!file) {
            if (atoi(de->d_name) != pid)
                continue;
            closedir(dir);
            goto full_scan;
        }

        while (fscanf(file, "%d", &child_pid) == 1)
            childrens.push_back(child_pid);
        fclose(file);
    }
    closedir(dir);

    return OK;

full_scan:
    dir = opendir("/proc");
    if (!dir)
        return TError::System("Cannot open /proc");

    while ((de = readdir(dir))) {
        file = fopen(("/proc/" + std::string(de->d_name) + "/stat").c_str(), "r");
        if (!file)
            continue;

        if (fscanf(file, "%d (%*[^)]) %*c %d", &child_pid, &parent_pid) == 2 && parent_pid == pid)
            childrens.push_back(child_pid);
        fclose(file);
    }
    closedir(dir);
    return OK;
}

void PrintProc(const std::string &knob, pid_t pid, bool debug) {
    std::string value;
    auto error = GetProc(pid, knob, value);
    if (!error)
        L("{}: {}", knob, value);
    else if (!debug)
        L_ERR("Can not get /proc/{}/{}: {}", pid, knob, error);
}

uint64_t GetCurrentTimeMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

bool WaitDeadline(uint64_t deadline, uint64_t wait) {
    uint64_t now = GetCurrentTimeMs();
    if (!deadline || int64_t(deadline - now) < 0)
        return true;
    if (deadline - now < wait)
        wait = deadline - now;
    if (wait)
        usleep(wait * 1000);
    return false;
}

uint64_t GetTotalMemory() {
    struct sysinfo si;
    if (sysinfo(&si) < 0)
        return 0;
    return (uint64_t)si.totalram * si.mem_unit;
}

/* total size in bytes, including surplus */
uint64_t GetHugetlbMemory() {
    int pages;
    if (TPath("/sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages").ReadInt(pages))
        return 0;
    return (uint64_t)pages << 21;
}

static __thread std::string *processName;

void SetProcessName(const std::string &name) {
    delete processName;
    processName = nullptr;
    prctl(PR_SET_NAME, (void *)name.c_str());
}

void SetDieOnParentExit(int sig) {
    (void)prctl(PR_SET_PDEATHSIG, sig, 0, 0, 0);
}

void SetPtraceProtection(bool enable) {
    if (!enable)
        L_SYS("PTrace protection: disabled");
    else if (prctl(PR_SET_DUMPABLE_INIT_NS, 0, 0, 0, 0))
        L_SYS("PTrace protection: unsupported");
    else if (prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 3)
        L_SYS("PTrace protection: enabled");
    else
        L_SYS("PTrace protection: broken");
}

std::string GetTaskName(pid_t pid) {
    if (pid) {
        std::string name;
        if (TPath("/proc/" + std::to_string(pid) + "/comm").ReadAll(name, 32))
            return "???";
        return name.substr(0, name.length() - 1);
    }

    if (!processName) {
        char name[17];

        memset(name, 0, sizeof(name));

        /* prctl returns 16 bytes string */

        if (prctl(PR_GET_NAME, (void *)name) < 0)
            strncpy(name, program_invocation_short_name, sizeof(name) - 1);

        processName = new std::string(name);
    }

    return *processName;
}

TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap) {
    std::vector<std::string> lines;
    TError error = TPath("/proc/" + std::to_string(pid) + "/cgroup").ReadLines(lines);
    if (error)
        return error;

    std::vector<std::string> tokens;
    for (auto l: lines) {
        tokens = SplitString(l, ':', 3);
        if (tokens.size() > 2)
            cgmap[tokens[1]] = tokens[2];
    }

    return OK;
}

std::string GetHostName() {
    char buf[HOST_NAME_MAX + 1];
    int ret = gethostname(buf, sizeof(buf));
    if (ret < 0)
        return "";
    buf[sizeof(buf) - 1] = '\0';
    return buf;
}

TError SetHostName(const std::string &name) {
    int ret = sethostname(name.c_str(), name.length());
    if (ret < 0)
        return TError::System("sethostname(" + name + ")");

    return OK;
}

TError SetOomScoreAdj(int value) {
    return TPath("/proc/self/oom_score_adj").WriteAll(std::to_string(value));
}

TError SetCoredumpFilter(uint32_t value) {
    return TPath("/proc/self/coredump_filter").WriteAll(std::to_string(value));
}

std::string FormatExitStatus(int status) {
    if (WIFSIGNALED(status))
        return StringFormat("exit signal: %d (%s)%s", WTERMSIG(status), strsignal(WTERMSIG(status)),
                            WCOREDUMP(status) ? " (Core dumped)" : "");
    return StringFormat("exit code: %d", WEXITSTATUS(status));
}

int GetNumCores() {
    int ncores = sysconf(_SC_NPROCESSORS_CONF);
    if (ncores <= 0) {
        L_ERR("Cannot get number of CPU cores");
        return 1;
    }

    return ncores;
}

int GetPageSize() {
    int pagesize = sysconf(_SC_PAGESIZE);
    if (pagesize <= 0) {
        L_ERR("Cannot get size of page");
        return 4096;
    }

    return pagesize;
}

void DumpMallocInfo() {
    struct mallinfo mi = mallinfo();
    L("Total non-mapped bytes (arena):\t{}", mi.arena);
    L("# of free chunks (ordblks):\t{}", mi.ordblks);
    L("# of free fastbin blocks (smblks):\t{}", mi.smblks);
    L("# of mapped regions (hblks):\t{}", mi.hblks);
    L("Bytes in mapped regions (hblkhd):\t{}", mi.hblkhd);
    L("Max. total allocated space (usmblks):\t{}", mi.usmblks);
    L("Free bytes held in fastbins (fsmblks):\t{}", mi.fsmblks);
    L("Total allocated space (uordblks):\t{}", mi.uordblks);
    L("Total free space (fordblks):\t{}", mi.fordblks);
    L("Topmost releasable block (keepcost):\t{}", mi.keepcost);
}

TUnixSocket &TUnixSocket::operator=(int sock) {
    Close();
    SetFd = sock;
    return *this;
}

TError TUnixSocket::SocketPair(TUnixSocket &sock1, TUnixSocket &sock2) {
    int sockfds[2];
    int ret, one = 1;

    ret = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockfds);
    if (ret)
        return TError::System("socketpair(AF_UNIX)");

    if (setsockopt(sockfds[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(int)) < 0 ||
        setsockopt(sockfds[1], SOL_SOCKET, SO_PASSCRED, &one, sizeof(int)) < 0) {
        close(sockfds[0]);
        close(sockfds[1]);
        return TError::System("setsockopt(SO_PASSCRED)");
    }

    sock1 = sockfds[0];
    sock2 = sockfds[1];
    return OK;
}

TError TUnixSocket::SendInt(int val) const {
    return Write(&val, sizeof(val));
}

TError TUnixSocket::RecvInt(int &val) const {
    auto error = Read(&val, sizeof(val));
    if (error && error.Errno == ECONNRESET)
        return TError(EError::NoValue);
    return error;
}

TError TUnixSocket::SendPid(pid_t pid) const {
    L("SendPid");
    struct iovec iovec = {
        .iov_base = &pid,
        .iov_len = sizeof(pid),
    };
    char buffer[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    struct ucred *ucred = (struct ucred *)CMSG_DATA(cmsg);
    ssize_t ret;

    auto error = ApplyWriteDeadline();
    if (error)
        return error;

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));
    ucred->pid = pid;
    ucred->uid = getuid();
    ucred->gid = getgid();

    ret = sendmsg(Fd, &msghdr, 0);
    if (ret < 0)
        return TError::System("cannot report real pid");
    if (ret != sizeof(pid))
        return TError("partial sendmsg: " + std::to_string(ret));
    return OK;
}

TError TUnixSocket::RecvPid(pid_t &pid, pid_t &vpid) const {
    L("RecvPid");
    struct iovec iovec = {
        .iov_base = &vpid,
        .iov_len = sizeof(vpid),
    };
    char buffer[CMSG_SPACE(sizeof(struct ucred))];
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    struct ucred *ucred = (struct ucred *)CMSG_DATA(cmsg);
    ssize_t ret;

    auto error = ApplyReadDeadline();
    if (error)
        return error;

    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_CREDENTIALS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(struct ucred));

    ret = recvmsg(Fd, &msghdr, 0);
    if (ret < 0)
        return TError::System("cannot receive real pid");
    if (ret != sizeof(pid))
        return TError("partial recvmsg: {}", ret);
    cmsg = CMSG_FIRSTHDR(&msghdr);
    if (!cmsg || cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_CREDENTIALS)
        return TError("no credentials after recvmsg");
    pid = ucred->pid;
    return OK;
}

TError TUnixSocket::SendError(const TError &error) const {
    auto error2 = ApplyWriteDeadline();
    if (error2)
        return error2;

    return error.Serialize(Fd);
}

TError TUnixSocket::RecvError() const {
    auto error = ApplyReadDeadline();
    if (error)
        return error;

    if (!TError::Deserialize(Fd, error))
        return TError("Unexpected end of stream during error receive");
    return error;
}

TError TUnixSocket::SendFd(int fd) const {
    char data[1];
    struct iovec iovec = {
        .iov_base = data,
        .iov_len = sizeof(data),
    };
    char buffer[CMSG_SPACE(sizeof(int))] = {0};
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    *((int *)CMSG_DATA(cmsg)) = fd;

    auto error = ApplyWriteDeadline();
    if (error)
        return error;

    ssize_t ret = sendmsg(Fd, &msghdr, 0);

    if (ret <= 0)
        return TError::System("cannot send fd");
    if (ret != sizeof(data))
        return TError("partial sendmsg: {}", ret);

    return OK;
}

TError TUnixSocket::RecvFd(int &fd) const {
    char data[1];
    struct iovec iovec = {
        .iov_base = data,
        .iov_len = sizeof(data),
    };
    char buffer[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(sizeof(struct ucred))] = {0};
    struct msghdr msghdr = {
        .msg_name = NULL,
        .msg_namelen = 0,
        .msg_iov = &iovec,
        .msg_iovlen = 1,
        .msg_control = buffer,
        .msg_controllen = sizeof(buffer),
        .msg_flags = 0,
    };
    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));

    auto error = ApplyReadDeadline();
    if (error)
        return error;

    int ret = recvmsg(Fd, &msghdr, 0);
    if (ret <= 0)
        return TError::System("cannot receive fd");

    if (ret != sizeof(data))
        return TError("partial recvmsg: {}", ret);

    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msghdr); cmsg; cmsg = CMSG_NXTHDR(&msghdr, cmsg)) {
        if ((cmsg->cmsg_level == SOL_SOCKET) && (cmsg->cmsg_type == SCM_RIGHTS)) {
            fd = *((int *)CMSG_DATA(cmsg));
            return OK;
        }
    }
    return TError("no rights after recvmsg");
}

TError TUnixSocket::SendPidFd(const TPidFd &pidfd) const {
    return SendFd(pidfd.PidFd.Fd);
}

TError TUnixSocket::RecvPidFd(TPidFd &pidfd) const {
    int fd;
    auto error = RecvFd(fd);
    if (error)
        return error;
    TFile tmp(fd);
    pidfd.PidFd.Swap(tmp);
    return OK;
}

TError GetSysctl(const std::string &name, std::string &value) {
    std::string path = "/proc/sys/" + name;
    /* all . -> / so abusing /../ is impossible */
    std::replace(path.begin() + 10, path.end(), '.', '/');
    TError error = TPath(path).ReadAll(value);
    if (!error)
        value = StringTrim(value);
    return error;
}

TError SetSysctl(const std::string &name, const std::string &value) {
    std::string path = "/proc/sys/" + name;
    /* all . -> / so abusing /../ is impossible */
    std::replace(path.begin() + 10, path.end(), '.', '/');
    L_ACT("Set sysctl {} = {}", name, value);
    return TPath(path).WriteAll(value);
}

TError SetSysctlAt(const TFile &proc_sys, const std::string &name, const std::string &value) {
    L_ACT("Set sysctl {} = {}", name, value);

    /* all . -> / so abusing /../ is impossible */
    std::string path = name;
    std::replace(path.begin(), path.end(), '.', '/');

    TFile file;
    TError error = file.OpenAt(proc_sys, path, O_WRONLY | O_CLOEXEC | O_NOCTTY, 0);
    if (error)
        return error;

    return file.WriteAll(value);
}

bool PostFork = false;
time_t ForkTime;
struct tm ForkLocalTime;

extern TStatistics *Statistics;

/* Some activity should not be performed after fork-from-thread */
void TaintPostFork(std::string message) {
    static bool currentReported = false;
    if (PostFork) {
        if (!currentReported)
            L_TAINT(message);

        if (Statistics) {
            if (!Statistics->PostForkIssues++)
                Stacktrace();
        }

        currentReported = true;
    }
}

// localtime_r isn't safe after fork because of lock inside
static void LocalTime(time_t time, struct tm &tm) {
    if (!PostFork) {
        localtime_r(&time, &tm);
    } else {
        tm = ForkLocalTime;
        time_t diff = tm.tm_sec + time - ForkTime;
        tm.tm_sec = diff % 60;
        diff = tm.tm_min + diff / 60;
        tm.tm_min = diff % 60;
        diff = tm.tm_hour + diff / 60;
        tm.tm_hour = diff % 24;
        tm.tm_mday += diff / 24;
    }
}

void LocalTime(const time_t *time, struct tm &tm) {
    if (!PostFork) {
        localtime_r(time, &tm);
    } else {
        tm = ForkLocalTime;
        time_t diff = tm.tm_sec + *time - ForkTime;
        tm.tm_sec = diff % 60;
        diff = tm.tm_min + diff / 60;
        tm.tm_min = diff % 60;
        diff = tm.tm_hour + diff / 60;
        tm.tm_hour = diff % 24;
        tm.tm_mday += diff / 24;
    }
}

std::string FormatTime(time_t t, const char *fmt) {
    std::stringstream ss;
    struct tm tm;

    LocalTime(t, tm);

    // FIXME gcc 4.x don't have this
    // ss << std::put_time(&tm, fmt);

    char buf[256];
    strftime(buf, sizeof(buf), fmt, &tm);
    ss << buf;

    return ss.str();
}

TError TPidFd::Open(pid_t pid) {
    auto error = PidFd.OpenPid(pid);
    if (error)
        return error;
    return OK;
}

bool TPidFd::Running() const {
    struct pollfd pfd = {
        .fd = PidFd.Fd,
        .events = POLLIN,
        .revents = 0,
    };

    return !poll(&pfd, 1, 0);
}

TError TPidFd::Wait(int timeoutMs) const {
    struct pollfd pfd = {
        .fd = PidFd.Fd,
        .events = POLLIN,
        .revents = 0,
    };

    int ret = poll(&pfd, 1, timeoutMs);
    if (ret == 1)
        return OK;

    if (ret == 0)
        return TError("timeout={}ms", timeoutMs);

    return TError::System("poll");
}

TError TPidFd::OpenProcFd(TFile &procFd) const {
    auto pid = GetPid();
    auto error = procFd.OpenDir(fmt::format("/proc/{}", pid));
    if (error)
        return error;

    if (GetPid() < 0)
        return TError(EError::Unknown, ESRCH, "process with pid={} gone", pid);
    return OK;
}

TError TPidFd::Kill(int signo) const {
    if (syscall(__NR_pidfd_send_signal, PidFd.Fd, signo, nullptr, 0) < 0)
        return TError::System("pidfd_send_signal");
    return OK;
}

TError TPidFd::KillWait(int signo, int timeoutMs) const {
    auto error = Kill(signo);
    if (error && error.Errno != ESRCH)
        return error;
    return Wait(timeoutMs);
}

pid_t TPidFd::GetPid() const {
    pid_t pid = -1;
    std::string value;
    if (!GetProcField(fmt::format("/proc/thread-self/fdinfo/{}", PidFd.Fd), "Pid:", value))
        (void)StringToInt(value, pid);
    return pid;
}

TError TPidFile::Read() {
    std::string str;
    TError error;
    int pid;

    Pid = 0;
    error = Path.ReadAll(str, 32);
    if (error)
        return error;
    error = StringToInt(str, pid);
    if (error)
        return error;
    if (kill(pid, 0) && errno == ESRCH)
        return TError::System("Task not found");
    str = GetTaskName(pid);
    if (str != Name && str != AltName)
        return TError("Wrong task name: {} expected: {}", str, Name);
    Pid = pid;
    return OK;
}

TError TPidFile::ReadPidFd(TPidFd &pidFd) {
    auto error = Read();
    if (error)
        return error;
    return pidFd.Open(Pid);
}

bool TPidFile::Running() {
    if (Pid && (!kill(Pid, 0) || errno != ESRCH)) {
        std::string name = GetTaskName(Pid);
        if (name == Name || name == AltName)
            return true;
    }
    Pid = 0;
    return false;
}

TError TPidFile::Save(pid_t pid) {
    TFile file;
    TError error = file.CreateTrunc(Path, 0644);
    if (error)
        return error;
    error = file.WriteAll(std::to_string(pid));
    if (error)
        return error;
    Pid = pid;
    return OK;
}

TError TPidFile::Remove() {
    Pid = 0;
    return Path.Unlink();
}

int SetIoPrio(pid_t pid, int ioprio)
{
    return syscall(SYS_ioprio_set, 1, pid, ioprio);
}
