#include "task.hpp"

#include <csignal>

#include "config.hpp"
#include "util/log.hpp"
#include "util/proc.hpp"

std::list<std::string> IpcSysctls = {
    "fs.mqueue.queues_max",
    "fs.mqueue.msg_max",
    "fs.mqueue.msgsize_max",
    "fs.mqueue.msg_default",
    "fs.mqueue.msgsize_default",

    "kernel.shmmax",
    "kernel.shmall",
    "kernel.shmmni",
    "kernel.shm_rmid_forced",

    "kernel.msgmax",
    "kernel.msgmni",
    "kernel.msgmnb",

    "kernel.sem",
};

void InitIpcSysctl() {
    for (const auto &key: IpcSysctls) {
        bool set = false;
        for (const auto &it: config().container().ipc_sysctl())
            set |= it.key() == key;
        std::string val;
        /* load default ipc sysctl from host config */
        if (!set && !GetSysctl(key, val)) {
            auto sysctl = config().mutable_container()->add_ipc_sysctl();
            sysctl->set_key(key);
            sysctl->set_val(val);
        }
    }
}

static int AddFd(std::vector<int> &fds, int fd) {
    if (fd < 0)
        return -1;
    fds.push_back(fd);
    return fds.size() - 1;
}

TError TTaskEnv::Launch(TTask &task) {
    TUnixSocket peer;
    auto error = TUnixSocket::SocketPair(Sock, peer);
    if (error)
        return error;
    std::vector<int> fds;
    for (const auto &pin: StreamPins)
        fds.push_back(pin.Fd);
    Spec.set_net_fd(AddFd(fds, NetFd.GetFd()));
    if (PidFd.GetFd() >= 0)
        Spec.set_pid_fd(AddFd(fds, PidFd.GetFd()));
    Spec.set_init_fd(AddFd(fds, PortoInit.Fd));
    Spec.set_log_fd(AddFd(fds, LogFile.Fd));
    const char *argv[] = {"/proc/self/exe", "run", nullptr};
    error = task.Spawn(argv, peer.Fd, TaskSocketFd);
    peer.Close();
    if (error)
        return error;
    return SendTask(Sock, Spec, fds);
}

TError TTaskEnv::ReceiveTaskPid(TaskMsg code, TPidFd &pidFd) {
    if (pidFd)
        return TError("Already received task pidfd");
    auto error = Sock.RecvPidFd(pidFd);
    if (error)
        return error;
    auto pid = pidFd.GetPid();
    if (pid <= 0)
        return TError("No pid for task");
    if (code == TaskMsg::WaitPid) {
        WaitTask.Pid = pid;
        return OK;
    }
    Task.Pid = pid;
    TFile proc;
    error = pidFd.OpenProcFd(proc);
    if (error)
        return error;
    TaskVPid = GetProcVPid(proc);
    return TaskVPid <= 0 ? TError("No vpid for task") : OK;
}

TError TTaskEnv::CommunicateTask(TPidFd &waitPidFd, TPidFd &taskPidFd) {
    TError childError("Child did not send OK");
    while (true) {
        int value;
        auto error = Sock.RecvInt(value);
        if (error == EError::NoValue)
            break;
        if (error) {
            if (error.Errno == EWOULDBLOCK)
                Statistics->StartTimeouts++;
            return error;
        }
        switch (TaskMsg(value)) {
        case TaskMsg::Error:
            childError = Sock.RecvError();
            error = Sock.SendZero();
            if (childError)
                return childError;
            break;
        case TaskMsg::WaitPid:
            error = ReceiveTaskPid(TaskMsg::WaitPid, waitPidFd);
            break;
        case TaskMsg::TaskPid:
            error = ReceiveTaskPid(TaskMsg::TaskPid, taskPidFd);
            break;
        default:
            return TError("Unknown message code from portorun: {}", value);
        }
        if (error)
            return error;
    }
    if (!waitPidFd || !taskPidFd)
        return TError("Child did not send pidfd");
    return childError;
}

TError TTaskEnv::SpawnTask() {
    TTask runner;
    TPidFd waitPidFd, taskPidFd;
    auto error = Launch(runner);
    if (!error)
        error = CommunicateTask(waitPidFd, taskPidFd);
    Sock.Close();
    if (error && runner.Pid)
        runner.Kill(SIGKILL);
    if (runner.Pid) {
        auto waitError = runner.Wait();
        if (!error)
            error = waitError;
    }
    if (error) {
        L("Task start failed: {}", error);
        if (waitPidFd)
            waitPidFd.Kill(SIGKILL);
        if (taskPidFd)
            taskPidFd.Kill(SIGKILL);
        Task.Pid = TaskVPid = WaitTask.Pid = 0;
    }
    return error;
}
