#pragma once

#include <functional>
#include <set>
#include <vector>

#include "common.hpp"
#include "util/path.hpp"
#include "util/signal.hpp"
#include "util/socket.hpp"

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
int PidFDOpen(pid_t pid, unsigned int flags);
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

class TPidFd {
    TPidFd(const TPidFd &) = delete;
    TPidFd &operator=(const TPidFd &) = delete;

public:
    TFile PidFd;

    TPidFd() = default;

    TPidFd(int fd)
        : PidFd(fd)
    {}
    TError Open(pid_t pid);
    // Check if process exists and is not zombie
    bool Running() const;
    // Wait process to terminate (but not to be reaped!, i.e. it can be zombie).
    TError Wait(int timeoutMs) const;
    TError Kill(int signo) const;
    TError KillWait(int signo, int timeoutMs) const;
    TError OpenProcFd(TFile &procFd) const;
    pid_t GetPid() const;
    pid_t GetNsPid() const;
    operator bool() const {
        return bool(PidFd);
    }
};

class TUnixSocket: public TSocket {
    TUnixSocket &operator=(int sock);

public:
    static TError SocketPair(TUnixSocket &sock1, TUnixSocket &sock2);
    TUnixSocket(int sock = -1)
        : TSocket(sock)
    {};
    TError SendInt(int val) const;
    TError RecvInt(int &val) const;
    TError SendZero() const {
        return SendInt(0);
    }
    TError RecvZero() const {
        int zero;
        auto error = RecvInt(zero);
        if (error)
            return error;
        if (zero)
            return TError("Expected 0 got {}", zero);
        return OK;
    }
    TError SendPid(pid_t pid) const;
    TError RecvPid(pid_t &pid, pid_t &vpid) const;
    TError SendPidFd(const TPidFd &pidfd) const;
    TError RecvPidFd(TPidFd &pidfd) const;
    TError SendError(const TError &error) const;
    TError RecvError() const;
    TError SendFd(int fd) const;
    TError RecvFd(int &fd) const;
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
    TError ReadPidFd(TPidFd &pidFd);
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
    char User[USER_MAX_LEN];

public:
    TSessionInfo() {
        memset(User, 0, sizeof(User));
        sessionInfo.kind = 0;
        sessionInfo.id = 0;
        sessionInfo.user = User;
    }

    bool IsEmpty() const {
        return sessionInfo.kind == 0 && sessionInfo.id == 0;
    }

    void Set(uint32_t kind, uint64_t id, const std::string &user) {
        sessionInfo.kind = kind;
        sessionInfo.id = id;
        // USER_MAX_LEN - 1 because of trailing '\0'
        strncpy(sessionInfo.user, user.c_str(), USER_MAX_LEN - 1);
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
