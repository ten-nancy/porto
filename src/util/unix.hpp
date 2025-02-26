#pragma once

#include <functional>
#include <set>
#include <vector>

#include "common.hpp"
#include "util/path.hpp"
#include "util/signal.hpp"

extern "C" {
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
}

class TPath;

std::string FormatTime(time_t t, const char *fmt = "%F %T");
void LocalTime(const time_t *time, struct tm &tm);

void TaintPostFork(std::string message);

pid_t GetPid();
pid_t GetPPid();
pid_t GetTid();
pid_t Clone(unsigned long flags, void *child_stack = NULL, void *ptid = NULL, void *ctid = NULL);
pid_t Fork(bool ptrace = false);
inline pid_t PtracedVfork() __attribute__((always_inline));
pid_t PtracedVfork() {
    pid_t pid = -1;

#ifdef __x86_64__
    __asm__ __volatile__ (
        "mov $" STRINGIFY(SYS_clone) ", %%rax;"
        "mov $" STRINGIFY(CLONE_VM | CLONE_VFORK | CLONE_PTRACE | SIGCHLD) ", %%rdi;"
        "mov $0, %%rsi;"
        "mov $0, %%rdx;"
        "mov $0, %%r10;"
        "mov $0, %%r8;"
        "syscall;"
        "mov %%eax, %0"
        : "=r" (pid)
        :
        : "rax", "rdi", "rsi", "rdx", "r10", "r8"
    );
#endif

    return pid;
}
TError GetTaskChildrens(pid_t pid, std::vector<pid_t> &childrens);
void PrintProc(const std::string &knob, pid_t pid, bool debug = true);
inline void PrintStack(pid_t pid, bool debug = true) {
    PrintProc("stack", pid, debug);
}

uint64_t GetCurrentTimeMs();
bool WaitDeadline(uint64_t deadline, uint64_t sleep = 10);
uint64_t GetTotalMemory();
uint64_t GetHugetlbMemory();
void SetProcessName(const std::string &name);
void SetDieOnParentExit(int sig);
void SetPtraceProtection(bool enable);
std::string GetTaskName(pid_t pid = 0);
uint64_t TaskHandledSignals(pid_t pid);
TError GetTaskCgroups(const int pid, std::map<std::string, std::string> &cgmap);
std::string GetHostName();
TError SetHostName(const std::string &name);
TError GetSysctl(const std::string &name, std::string &value);
TError SetSysctl(const std::string &name, const std::string &value);
TError SetSysctlAt(const TFile &proc_sys, const std::string &name, const std::string &value);

TError SetOomScoreAdj(int value);
TError SetCoredumpFilter(uint32_t value);

std::string FormatExitStatus(int status);
int GetNumCores();
int GetPageSize();
void DumpMallocInfo();

int SetIoPrio(pid_t pid, int ioprio);

class TUnixSocket: public TNonCopyable {
    int SockFd;

public:
    static TError SocketPair(TUnixSocket &sock1, TUnixSocket &sock2);
    TUnixSocket(int sock = -1)
        : SockFd(sock)
    {};
    ~TUnixSocket() {
        Close();
    };
    void operator=(int sock);
    TUnixSocket &operator=(TUnixSocket &&Sock);
    void Close();
    int GetFd() const {
        return SockFd;
    }
    TError SendInt(int val) const;
    TError RecvInt(int &val) const;
    TError SendZero() const {
        return SendInt(0);
    }
    TError RecvZero() const {
        int zero;
        return RecvInt(zero);
    }
    TError SendPid(pid_t pid) const;
    TError RecvPid(pid_t &pid, pid_t &vpid) const;
    TError SendError(const TError &error) const;
    TError RecvError() const;
    TError SendFd(int fd) const;
    TError RecvFd(int &fd) const;
    TError SetRecvTimeout(int timeout_ms) const;
};

class TPidFile {
public:
    TPath Path;
    std::string Name;
    std::string AltName;
    pid_t Pid = 0;

    TPidFile(const std::string &path, const std::string &name, const std::string &altname)
        : Path(path),
          Name(name),
          AltName(altname)
    {}
    TError Read();
    bool Running();
    TError Save(pid_t pid);
    TError Remove();
};

#ifndef PR_SESSION_INFO
#define PR_SESSION_INFO 0x59410000
#endif

class TSessionInfo {
    static constexpr unsigned int USER_MAX_LEN = 256;
    struct session_info {
        __u32 kind;
        __u64 id;
        char *user;  // null-terminated
    } sessionInfo;

public:
    TSessionInfo() {
        sessionInfo.kind = 0;
        sessionInfo.id = 0;
        sessionInfo.user = new char[USER_MAX_LEN];
        sessionInfo.user = strcpy(sessionInfo.user, "");
    }
    ~TSessionInfo() {
        delete sessionInfo.user;
    }

    bool IsEmpty() const {
        return sessionInfo.kind == 0 && sessionInfo.id == 0;
    }

    void Set(uint32_t kind, uint64_t id, const std::string &user) {
        sessionInfo.kind = kind;
        sessionInfo.id = id;
        // substr to restrict out of memory range, USER_MAX_LEN - 1 because of '\0'
        sessionInfo.user = strcpy(sessionInfo.user, user.substr(0, USER_MAX_LEN - 1).c_str());
    }

    TError Parse(const std::string &str) {
        TError error;
        uint64_t kind, id;

        if (str.empty()) {
            Set(0, 0, "");
            return OK;
        }

        auto tokens = SplitString(str, ' ');
        if (tokens.size() != 3)
            return TError("Cannot parse {}: expected 3 arguments", str);

        error = StringToUint64(tokens[0], kind);
        if (error)
            return TError("Cannot convert to unsigned int {}: {}", tokens[0], error);

        error = StringToUint64(tokens[1], id);
        if (error)
            return TError("Cannot convert to unsigned int {}: {}", tokens[1], error);

        Set((uint32_t)kind, id, tokens[2]);

        return OK;
    }

    std::string ToString() const {
        return fmt::format("{} {} {}", sessionInfo.kind, sessionInfo.id, sessionInfo.user);
    }

    TError Apply() const {
        int ret = prctl(PR_SESSION_INFO, &sessionInfo);
        if (ret < 0)
            return TError::System("Cannot apply session info: {}", ToString());

        return OK;
    }
};
