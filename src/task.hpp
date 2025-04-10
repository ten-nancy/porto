#pragma once

#include <string>
#include <vector>

#include "cgroup.hpp"
#include "env.hpp"
#include "filesystem.hpp"
#include "util/cred.hpp"
#include "util/namespace.hpp"
#include "util/path.hpp"
#include "util/unix.hpp"

class TContainer;
class TClient;
class TPidFd;

struct TTaskEnv {
    enum class EMsgCode {
        Error = 8321,
        WaitPid = 8322,
        TaskPid = 8323,
        SetupUserMapping = 8324,
    };

    std::shared_ptr<TContainer> CT;
    TClient *Client;
    TFile PortoInit;
    TMountNamespace Mnt;

    TNamespaceFd IpcFd;
    TNamespaceFd UtsFd;
    TNamespaceFd NetFd;
    TNamespaceFd PidFd;
    TNamespaceFd MntFd;
    TNamespaceFd RootFd;
    TNamespaceFd CwdFd;
    TNamespaceFd CgFd;
    TNamespaceFd UserFd;

    TEnv Env;
    bool TripleFork;
    bool QuadroFork;
    std::vector<std::string> Autoconf;
    bool NewMountNs;
    std::list<std::unique_ptr<const TCgroup>> Cgroups;
    TCred Cred;
    uid_t LoginUid;

    TUnixSocket Sock, MasterSock;

    TError OpenNamespaces(TContainer &ct);

    TError Start();
    TError CommunicateChild(TPidFd &waitPidFd, TPidFd &taskPidFd);
    TError DoFork1();
    void StartChild();

    TError ConfigureChild();
    TError WriteResolvConf();
    TError SetHostname();
    TError ApplySysctl();

    TError WaitAutoconf();
    TError ChildExec();

    void ReportPid(EMsgCode type);
    void ReportError(const TError &error);
    void Abort(const TError &error);
    void AbortOnError(const TError &error);

    void ExecPortoinit(pid_t pid);
};

extern std::list<std::string> IpcSysctls;
void InitIpcSysctl();

extern unsigned ProcBaseDirs;
void InitProcBaseDirs();
