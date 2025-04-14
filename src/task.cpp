#include "task.hpp"

#include <csignal>

#include "config.hpp"
#include "container.hpp"
#include "device.hpp"
#include "network.hpp"
#include "util/cred.hpp"
#include "util/log.hpp"
#include "util/netlink.hpp"
#include "util/proc.hpp"
#include "util/signal.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"

extern "C" {
#include <fcntl.h>
#include <grp.h>
#include <linux/sched.h>
#include <net/if.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>
}

std::string to_string(TTaskEnv::EMsgCode code) {
    switch (code) {
    case TTaskEnv::EMsgCode::Error:
        return "Error";
    case TTaskEnv::EMsgCode::WaitPid:
        return "WaitPid";
    case TTaskEnv::EMsgCode::TaskPid:
        return "TaskPid";
    case TTaskEnv::EMsgCode::SetupUserMapping:
        return "SetupUserMapping";
    default:
        return "Unknown";
    }
}

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

extern bool SupportCgroupNs;

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

unsigned ProcBaseDirs;

void InitProcBaseDirs() {
    std::vector<std::string> dirs;
    TPath("/proc").ListSubdirs(dirs);
    for (auto &dir: dirs)
        if (!StringOnlyDigits(dir))
            ProcBaseDirs++;
    ProcBaseDirs += 2;
}

void TTaskEnv::ReportPid(EMsgCode type) {
    TPidFd pidFd;

    AbortOnError(pidFd.Open(getpid()));
    L("Report {} pid={}", to_string(type), getpid());
    AbortOnError(Sock.SendInt(int(type)));
    AbortOnError(Sock.SendPidFd(pidFd));
}

void TTaskEnv::ReportError(const TError &error) {
    auto error2 = Sock.SendInt(int(EMsgCode::Error));
    if (!error2)
        error2 = Sock.SendError(error);

    if (error2) {
        L_ERR("Tried to send error: {}", error);
        L_ERR("Failed send error: {}", error2);
        _exit(EXIT_FAILURE);
    }
}

void TTaskEnv::Abort(const TError &error) {
    TError error2;

    L("abort due to {}", error);

    ReportError(error);
    _exit(EXIT_FAILURE);
}

void TTaskEnv::AbortOnError(const TError &error) {
    if (error)
        Abort(error);
}

TError TTaskEnv::OpenNamespaces(TContainer &ct) {
    TError error;

    auto target = &ct;
    while (target && !target->Task.Pid)
        target = target->Parent.get();

    if (!target)
        return OK;

    pid_t pid = target->Task.Pid;

    error = IpcFd.Open(pid, "ns/ipc");
    if (error)
        return error;

    error = UtsFd.Open(pid, "ns/uts");
    if (error)
        return error;

    if (NetFd.GetFd() < 0) {
        error = NetFd.Open(pid, "ns/net");
        if (error)
            return error;
    }

    error = PidFd.Open(pid, "ns/pid");
    if (error)
        return error;

    error = MntFd.Open(pid, "ns/mnt");
    if (error)
        return error;

    if (SupportCgroupNs) {
        error = CgFd.Open(pid, "ns/cgroup");
        if (error)
            return error;
    }

    error = UserFd.Open(pid, "ns/user");
    if (error)
        return error;

    /* https://github.com/torvalds/linux/blob/v4.19/kernel/user_namespace.c#L1263
     * Don't allow gaining capabilities by reentering
     * the same user namespace.
     */

    TNamespaceFd currentUserFd;
    error = currentUserFd.Open("proc/thread-self/ns/user");
    if (error)
        return error;

    if (UserFd.Inode() == currentUserFd.Inode())
        UserFd.Close();
    else
        PORTO_ASSERT(ct.InUserNs());

    currentUserFd.Close();

    error = RootFd.Open(pid, "root");
    if (error)
        return error;

    error = CwdFd.Open(pid, "cwd");
    if (error)
        return error;

    return OK;
}

TError TTaskEnv::ChildExec() {
    /* set environment for wordexp */
    auto error = Env.Apply();

    auto envp = Env.Envp();

    if (CT->IsMeta()) {
        const char *args[] = {
            "portoinit",
            "--container",
            CT->Name.c_str(),
            NULL,
        };
        TFile::CloseAllExcept({PortoInit.Fd, LogFile.Fd, Sock.Fd});
        ReportError(OK);
        AbortOnError(Sock.RecvZero());
        L("Exec portoinit meta {}", CT->Slug);
        fexecve(PortoInit.Fd, (char *const *)args, envp);
        Abort(TError::System("fexec portoinit"));
    }

    std::vector<const char *> argv;
    if (CT->HasProp(EProperty::COMMAND_ARGV)) {
        argv.resize(CT->CommandArgv.size() + 1);
        for (unsigned i = 0; i < CT->CommandArgv.size(); i++)
            argv[i] = CT->CommandArgv[i].c_str();
        argv.back() = nullptr;
    } else {
        wordexp_t result;

        int ret = wordexp(CT->Command.c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
        switch (ret) {
        case WRDE_BADCHAR:
            return TError(EError::InvalidCommand,
                          "wordexp(): illegal occurrence of newline or one of |, &, ;, <, >, (, ), {{, }}");
        case WRDE_BADVAL:
            return TError(EError::InvalidCommand, "wordexp(): undefined shell variable was referenced");
        case WRDE_CMDSUB:
            return TError(EError::InvalidCommand, "wordexp(): command substitution is not supported");
        case WRDE_SYNTAX:
            return TError(EError::InvalidCommand, "wordexp(): syntax error");
        default:
        case WRDE_NOSPACE:
            return TError(EError::InvalidCommand, "wordexp(): error {}", ret);
        case 0:
            break;
        }

        argv.resize(result.we_wordc + 1);
        for (unsigned i = 0; i < result.we_wordc; i++)
            argv[i] = result.we_wordv[i];
        argv.back() = nullptr;
    }

    if (Verbose) {
        L("command={}", CT->Command);
        for (unsigned i = 0; argv[i]; i++)
            L("argv[{}]={}", i, argv[i]);
        for (unsigned i = 0; envp[i]; i++)
            L("environ[{}]={}", i, envp[i]);
    }

    PortoInit.Close();

    /* https://bugs.launchpad.net/upstart/+bug/1582199 */
    if (CT->Command == "/sbin/init" && CT->OsMode && !(CT->Controllers & CGROUP_SYSTEMD)) {
        L_VERBOSE("Reserve fd 9 for upstart JOB_PROCESS_SCRIPT_FD");
        dup2(open("/dev/null", O_RDWR | O_CLOEXEC), 9);
    }

    if (CT->UnshareOnExec) {
        if (unshare(CLONE_NEWNS))
            return TError::System("unshare(CLONE_NEWNS)");
    }

    TSeccompContext seccompContext;
    for (auto ct = CT.get(); ct; ct = ct->Parent.get()) {
        if (!ct->Seccomp.Empty()) {
            error = seccompContext.Apply(ct->Seccomp);
            if (error)
                return error;
            break;
        }
    }

    if (!CT->SessionInfo.IsEmpty()) {
        error = CT->SessionInfo.Apply();
        if (error)
            L_WRN("{}", error);
    }

    ReportError(OK);
    AbortOnError(Sock.RecvZero());

    L("Exec '{}'", argv[0]);
    execvpe(argv[0], (char *const *)argv.data(), envp);

    if (errno == EAGAIN)
        return TError(EError::ResourceNotAvailable, errno, "cannot exec {} not enough ulimit nproc", argv[0]);

    return TError(EError::InvalidCommand, errno, "cannot exec {}", argv[0]);
}

TError TTaskEnv::WriteResolvConf() {
    if (CT->HasProp(EProperty::RESOLV_CONF) ? !CT->ResolvConf.size() : CT->Root == "/")
        return OK;
    L_ACT("Write resolv.conf for {}", CT->Slug);
    return TPath("/etc/resolv.conf").WritePrivate(CT->ResolvConf.size() ? CT->ResolvConf : RootContainer->ResolvConf);
}

TError TTaskEnv::SetHostname() {
    TError error;

    if (CT->Hostname.size()) {
        error = TPath("/etc/hostname").WritePrivate(CT->Hostname + "\n");
        if (!error)
            error = SetHostName(CT->Hostname);
    }

    return error;
}

TError TTaskEnv::ApplySysctl() {
    TError error;

    if (CT->Isolate) {
        for (const auto &it: config().container().ipc_sysctl()) {
            error = SetSysctlAt(Mnt.ProcSysFd, it.key(), it.val());
            if (error)
                return error;
        }
    }

    for (const auto &it: CT->Sysctl) {
        auto &key = it.first;

        if (TNetwork::NetworkSysctl(key)) {
            if (!CT->NetIsolate)
                return TError(EError::Permission, "Sysctl " + key + " requires net isolation");
            continue; /* Set by TNetEnv */
        } else if (std::find(IpcSysctls.begin(), IpcSysctls.end(), key) != IpcSysctls.end()) {
            if (!CT->Isolate)
                return TError(EError::Permission, "Sysctl " + key + " requires ipc isolation");
        } else
            return TError(EError::Permission, "Sysctl " + key + " is not allowed");

        error = SetSysctlAt(Mnt.ProcSysFd, key, it.second);
        if (error)
            return error;
    }

    return OK;
}

TError TTaskEnv::ConfigureChild() {
    L("ConfigureChild");
    auto error = CT->GetUlimit().Apply();
    if (error)
        return error;

    if (setsid() < 0)
        return TError::System("setsid()");

    umask(0);

    TDevices devices = CT->EffectiveDevices();

    if (NewMountNs) {
        error = Mnt.Setup(*CT);
        if (error)
            return error;

        for (auto &device: devices.Devices) {
            for (auto &device_sysfs: config().container().device_sysfs()) {
                if (device.Path.ToString() == device_sysfs.device()) {
                    for (auto &sysfs: device_sysfs.sysfs()) {
                        TPath path(sysfs);
                        error = path.BindRemount(path, MS_ALLOW_WRITE);
                        if (error)
                            return error;
                    }
                }
            }
        }
    }

    if (!Mnt.Root.IsRoot()) {
        if (CT->InUserNs())
            devices.PrepareForUserNs(CT->UserNsCred);

        error = devices.Makedev("/");
        if (error)
            return error;
    }

    error = ApplySysctl();
    if (error)
        return error;

    error = WriteResolvConf();
    if (error)
        return error;

    if (CT->EtcHosts.size()) {
        error = TPath("/etc/hosts").WritePrivate(CT->EtcHosts);
        if (error)
            return error;
    }

    error = SetHostname();
    if (error)
        return error;

    /* Closing before directory changing for security.
     * More info: PORTO-925
     */
    TFile::CloseAllExcept(
        {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, Sock.Fd, LogFile.Fd, PortoInit.Fd, UserFd.GetFd()});
    error = Mnt.Cwd.Chdir();
    if (error)
        return error;

    if (QuadroFork) {
        pid_t pid = fork();
        if (pid < 0)
            return TError::System("fork()");

        if (pid)
            ExecPortoinit(pid);

        if (setsid() < 0)
            return TError::System("setsid()");
    }

    error = TPath("/proc/self/loginuid").WriteAll(std::to_string(LoginUid));
    if (error && error.Errno != ENOENT)
        L_WRN("Cannot set loginuid: {}", error);

    error = Cred.Apply();
    if (error)
        return error;

    if (CT->CapAmbient)
        L("Ambient capabilities: {}", CT->CapAmbient);

    error = CT->CapAmbient.ApplyAmbient();
    if (error)
        return error;

    L("Capabilities: {}", CT->CapBound);

    error = CT->CapBound.ApplyLimit();
    if (error)
        return error;

    if (!Cred.IsRootUser()) {
        error = CT->CapAmbient.ApplyEffective();
        if (error)
            return error;
    }

    L("open default streams in child");
    error = CT->Stdin.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stdout.OpenInside(*CT);
    if (error)
        return error;

    error = CT->Stderr.OpenInside(*CT);
    if (error)
        return error;

    umask(CT->Umask);

    error = UserFd.SetNs(CLONE_NEWUSER);
    if (error)
        return error;

    UserFd.Close();

    ReportPid(EMsgCode::TaskPid);
    if (CT->UserNs) {
        int unshareFlags = CLONE_NEWUSER | CLONE_NEWNET;

        // TODO: remove it later
        if (CT->Fuse && CT->NetInherit)
            unshareFlags &= ~CLONE_NEWNET;

        if (SupportCgroupNs)
            unshareFlags |= CLONE_NEWCGROUP;

        if (unshare(unshareFlags))
            return TError::System("unshare(CLONE_NEWUSER | {}{})",
                                  (unshareFlags & CLONE_NEWNET ? " | CLONE_NEWNET" : ""),
                                  (unshareFlags & CLONE_NEWCGROUP ? " | CLONE_NEWCGROUP" : ""));

        error = Sock.SendInt(int(EMsgCode::SetupUserMapping));
        if (error)
            return error;

        error = Sock.RecvZero();
        if (error)
            return error;
    }

    return OK;
}

TError TTaskEnv::WaitAutoconf() {
    if (Autoconf.empty())
        return OK;

    SetProcessName("portod-autoconf");

    auto sock = std::make_shared<TNl>();
    TError error = sock->Connect();
    if (error)
        return error;

    for (auto &name: Autoconf) {
        TNlLink link(sock, name);

        error = link.Load();
        if (error)
            return error;

        error = link.WaitAddress(config().network().autoconf_timeout_s());
        if (error)
            return error;
    }

    return OK;
}

void TTaskEnv::StartChild() {
    L("StartChild");

    ReportPid(EMsgCode::WaitPid);

    /* Apply configuration */
    AbortOnError(ConfigureChild());

    /* Reset signals before exec, signal block already lifted */
    ResetIgnoredSignals();

    AbortOnError(WaitAutoconf());

    Abort(ChildExec());
}

TError TTaskEnv::DoFork1() {
    TError error;

    /* Switch from signafd back to normal signal delivery */
    ResetBlockedSignals();

    SetDieOnParentExit(SIGKILL);

    SetProcessName(fmt::format("portod-CT{}", CT->Id));

    (void)setsid();

    L("Attach to cgroups");
    // move to target cgroups
    for (auto &cg: Cgroups) {
        error = cg->Attach(GetPid());
        if (error)
            return error;
    }

    error = SetOomScoreAdj(CT->OomScoreAdj);
    if (error && CT->OomScoreAdj)
        return error;

    if (CT->HasProp(EProperty::COREDUMP_FILTER)) {
        error = SetCoredumpFilter(CT->CoredumpFilter);
        if (error)
            return error;
    }

    L("setpriority");
    if (setpriority(PRIO_PROCESS, 0, CT->SchedNice))
        return TError::System("setpriority");

    struct sched_param param;
    param.sched_priority = CT->SchedPrio;
    if (sched_setscheduler(0, CT->SchedPolicy, &param))
        return TError::System("sched_setscheduler");

    if (CT->SchedNoSmt) {
        cpu_set_t taskMask;
        CT->GetNoSmtCpus().FillCpuSet(&taskMask);

        if (sched_setaffinity(0, sizeof(taskMask), &taskMask))
            return TError::System("sched_setaffinity");
    }

    if (SetIoPrio(0, CT->IoPrio))
        return TError::System("ioprio");

    L("open default streams");
    /* Default streams and redirections are outside */
    error = CT->Stdin.OpenOutside(*CT, *Client);
    if (error)
        return error;

    error = CT->Stdout.OpenOutside(*CT, *Client);
    if (error)
        return error;

    error = CT->Stderr.OpenOutside(*CT, *Client);
    if (error)
        return error;

    L("Enter namespaces");

    error = IpcFd.SetNs(CLONE_NEWIPC);
    if (error)
        return error;

    error = UtsFd.SetNs(CLONE_NEWUTS);
    if (error)
        return error;

    error = NetFd.SetNs(CLONE_NEWNET);
    if (error)
        return error;

    error = PidFd.SetNs(CLONE_NEWPID);
    if (error)
        return error;

    error = MntFd.SetNs(CLONE_NEWNS);
    if (error)
        return error;

    if (SupportCgroupNs) {
        error = CgFd.SetNs(CLONE_NEWCGROUP);
        if (error)
            return error;
    }

    error = RootFd.Chroot();
    if (error)
        return error;

    error = CwdFd.Chdir();
    if (error)
        return error;

    struct clone_args clargs = {};
    if (TripleFork) {
        /*
         * Enter into pid-namespace. fork() hangs in libc if child pid
         * collide with parent pid outside. vfork() has no such problem.
         */
        L("vfork");
        pid_t forkPid = vfork();
        if (forkPid < 0)
            Abort(TError::System("vfork()"));

        if (forkPid)
            _exit(EXIT_SUCCESS);
        clargs.flags |= CLONE_PARENT;
    } else
        clargs.exit_signal = SIGCHLD;

    if (CT->Isolate)
        clargs.flags |= CLONE_NEWIPC | CLONE_NEWPID;

    if (SupportCgroupNs && CT->CgroupFs != ECgroupFs::None)
        clargs.flags |= CLONE_NEWCGROUP;

    if (NewMountNs)
        clargs.flags |= CLONE_NEWNS;

    /* Create UTS namspace if hostname is changed or isolate=true */
    if (CT->Isolate || CT->Hostname != "")
        clargs.flags |= CLONE_NEWUTS;

    pid_t clonePid = syscall(__NR_clone3, &clargs, sizeof(clargs));

    if (clonePid < 0)
        return TError(errno == ENOMEM ? EError::ResourceNotAvailable : EError::Unknown, errno, "clone()");

    if (!clonePid)
        StartChild();

    return OK;
}

TError TTaskEnv::CommunicateChild(TPidFd &waitPidFd, TPidFd &taskPidFd) {
    TFile taskProcFd;
    TError childError = TError("Child did not send OK");

    while (1) {
        int val;
        auto error = MasterSock.RecvInt(val);
        if (error) {
            if (error.Error == EError::NoValue)  // EOF
                break;
            if (error.Errno == EWOULDBLOCK)
                Statistics->StartTimeouts++;
            return error;
        }

        auto code = EMsgCode(val);
        L("CommunicateChild {}", to_string(code));

        switch (code) {
        case EMsgCode::Error: {
            childError = MasterSock.RecvError();  // child sends ok before final exec
            if (childError)
                return childError;
            auto error = MasterSock.SendZero();
            if (error)
                return error;
            break;
        }
        case EMsgCode::WaitPid: {
            if (waitPidFd)
                return TError("already received wait task pidfd");
            auto error = MasterSock.RecvPidFd(waitPidFd);
            if (error)
                return error;
            CT->WaitTask.Pid = waitPidFd.GetPid();
            if (CT->WaitTask.Pid < 0)
                return TError("No pid for wait task");
            break;
        }
        case EMsgCode::TaskPid: {
            if (taskPidFd)
                return TError("already received task pidfd");
            auto error = MasterSock.RecvPidFd(taskPidFd);
            if (error)
                return error;
            CT->Task.Pid = taskPidFd.GetPid();
            if (CT->Task.Pid < 0)
                return TError("No pid for task");

            error = taskPidFd.OpenProcFd(taskProcFd);
            if (error)
                return error;
            CT->TaskVPid = GetProcVPid(taskProcFd);
            if (CT->TaskVPid < 0)
                return TError("No vpid for task");

            break;
        }
        case EMsgCode::SetupUserMapping: {
            error = CT->TaskCred.SetupMapping(taskProcFd);
            if (error)
                return error;

            error = TNetwork::StartNetwork(*CT, *this);
            if (error)
                return error;

            error = MasterSock.SendZero();
            if (error)
                return error;
            break;
        }
        default:
            return TError("Got unknown message code from child: {}", val);
        }
    }

    if (!waitPidFd || !taskProcFd)
        return TError("Child did not send pidfd");

    return childError;
}

TError TTaskEnv::Start() {
    /* Use third fork between entering into parent pid-namespace and
    cloning isolated child pid-namespace: porto keeps waiter task inside
    which waits sub-container main task and dies in the same way. */
    L("Start with TripleFork={} QuadroFork={}", TripleFork, QuadroFork);

    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;
    CT->SeizeTask.Pid = 0;

    auto error = TUnixSocket::SocketPair(MasterSock, Sock);
    if (error)
        return error;

    MasterSock.SetDeadline(GetCurrentTimeMs() + config().container().start_timeout_ms());

    // we want our child to have portod master as parent, so we
    // are doing double fork here (fork + clone);
    // we also need to know child pid so we are using pipe to send it back

    TTask task;

    error = task.Fork();
    if (error) {
        Sock.Close();
        MasterSock.Close();
        L("Fork failed: {}", error);
        return error;
    }

    if (!task.Pid) {
        MasterSock.Close();

        AbortOnError(DoFork1());
        _exit(EXIT_SUCCESS);
    }
    Sock.Close();

    TPidFd waitPidFd, taskPidFd;
    error = CommunicateChild(waitPidFd, taskPidFd);
    MasterSock.Close();
    if (error)
        goto kill_all;

    if (CT->Task.Pid < 0 || CT->TaskVPid < 0 || CT->WaitTask.Pid < 0) {
        error = TError("Child task is dead");
        goto kill_all;
    }

    error = task.Wait();
    if (error)
        goto kill_all;

    return OK;

kill_all:
    L("Task start failed: {}", error);
    if (waitPidFd)
        waitPidFd.Kill(SIGKILL);
    if (taskPidFd)
        taskPidFd.Kill(SIGKILL);
    if (task.Pid) {
        task.Kill(SIGKILL);
        task.Wait();
    }
    CT->Task.Pid = 0;
    CT->TaskVPid = 0;
    CT->WaitTask.Pid = 0;
    CT->SeizeTask.Pid = 0;
    return error;
}

void TTaskEnv::ExecPortoinit(pid_t pid) {
    auto pid_ = std::to_string(pid);
    const char *argv[] = {
        "portoinit", "--container", CT->Name.c_str(), "--wait", pid_.c_str(), NULL,
    };
    auto envp = Env.Envp();

    AbortOnError(PortoInitCapabilities.ApplyLimit());

    TFile::CloseAllExcept({PortoInit.Fd, LogFile.Fd, Sock.Fd});
    L("Exec portoinit {} wait {}", CT->Slug, pid);
    fexecve(PortoInit.Fd, (char *const *)argv, envp);
    kill(pid, SIGKILL);
    Abort(TError::System("fexec portoinit"));
}
