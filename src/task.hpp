#pragma once

#include <list>
#include <string>
#include <vector>

#include "taskwire.hpp"
#include "util/namespace.hpp"
#include "util/task.hpp"

struct TTaskEnv {
    TTask &WaitTask;
    TTask &Task;
    pid_t &TaskVPid;

    runtime::TTaskSpec Spec;
    std::vector<TFile> StreamPins;
    TFile PortoInit;
    TNamespaceFd PidFd;
    TNamespaceFd NetFd;
    TUnixSocket Sock;

    TTaskEnv(TTask &waitTask, TTask &task, pid_t &taskVPid)
        : WaitTask(waitTask),
          Task(task),
          TaskVPid(taskVPid)
    {}

    TError SpawnTask();
    TError CommunicateTask(TPidFd &waitPidFd, TPidFd &taskPidFd);
    TError ReceiveTaskPid(TaskMsg code, TPidFd &pidFd);
    TError Launch(TTask &task);
};

extern std::list<std::string> IpcSysctls;
void InitIpcSysctl();
