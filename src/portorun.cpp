#include "device.hpp"
#include "env.hpp"
#include "filesystem.hpp"
#include "portod.hpp"
#include "seccomp.hpp"
#include "stream.hpp"
#include "taskwire.hpp"
#include "util/log.hpp"
#include "util/netlink.hpp"
#include "util/proc.hpp"
#include "util/signal.hpp"

extern "C" {
#include <fcntl.h>
#include <linux/openat2.h>
#include <linux/sched.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <wordexp.h>
}

namespace {

struct TTaskRun {
    runtime::TTaskSpec Spec;
    std::vector<TFile> Files;
    TUnixSocket Sock{TaskSocketFd};
    TMountNamespace Mnt;
    TEnv Env;
    TUlimit Ulimit;
    TCred Cred;
    TFile PortoInit;
    TSeccompProfile Seccomp;
    TSeccompContext SeccompContext;
    TSessionInfo SessionInfo;
    TStreams Streams;

    TError ReportPid(TaskMsg type) {
        TPidFd pidFd;
        auto error = pidFd.Open(GetPid());
        if (error)
            return error;
        error = Sock.SendInt(int(type));
        if (error)
            return error;
        return Sock.SendPidFd(pidFd);
    }

    TError ReportError(const TError &error) {
        auto sent = Sock.SendInt(int(TaskMsg::Error));
        if (sent)
            return sent;
        sent = Sock.SendError(error);
        if (sent)
            return sent;
        // Keep the process alive until portod has consumed any previously sent pidfds.
        return Sock.RecvZero();
    }

    [[noreturn]] void Abort(const TError &error) {
        L("portorun abort: {}", error);
        (void)ReportError(error);
        _exit(EXIT_FAILURE);
    }

    void AbortOnError(const TError &error) {
        if (error)
            Abort(error);
    }

    TError ValidateDescriptors() {
        std::set<int> seen;
        if (Spec.has_pid_fd() && Spec.pid_fd() < 0)
            return TError("Invalid task descriptor index {}", Spec.pid_fd());
        for (int fd: {Spec.pid_fd(), Spec.net_fd(), Spec.init_fd(), Spec.log_fd()}) {
            if (fd < -1 || (fd >= 0 && (size_t(fd) >= Files.size() || !Files[fd] || !seen.insert(fd).second)))
                return TError("Invalid task descriptor index {}", fd);
        }
        for (const auto &stream: Spec.streams()) {
            if (stream.has_pin() && (stream.pin() >= Files.size() || !seen.insert(stream.pin()).second))
                return TError("Invalid stream descriptor index {}", stream.pin());
        }
        if ((Spec.meta() || Spec.quadro_fork()) && Spec.init_fd() < 0)
            return TError("Missing portoinit descriptor");
        if (Spec.streams_size() != 3)
            return TError("Expected three standard streams");
        return OK;
    }

    TError Load() {
        snprintf(ReqId, sizeof(ReqId), "%s", Spec.operation_id().c_str());
        auto error = ValidateDescriptors();
        if (error)
            return error;

        if (Spec.init_fd() >= 0)
            PortoInit.Swap(Files[Spec.init_fd()]);
        if (Spec.log_fd() >= 0)
            LogFile.Swap(Files[Spec.log_fd()]);
        Verbose = Spec.verbose();
        Debug = Spec.debug();
        StdLog = false;
        Mnt.Load(Spec.mount());
        Cred.SetUid(Spec.uid());
        Cred.SetGid(Spec.gid());
        Cred.Groups.assign(Spec.groups().begin(), Spec.groups().end());
        for (const auto &var: Spec.env()) {
            auto error = Env.SetEnv(var.name(), var.value());
            if (error)
                return error;
        }
        error = Ulimit.Parse(Spec.ulimit());
        if (error)
            return error;
        if (Spec.has_seccomp()) {
            error = Seccomp.Parse(Spec.seccomp());
            if (error)
                return error;
        }
        return SessionInfo.Parse(Spec.session_info());
    }

    TError AttachCgroups() {
        for (const auto &path: Spec.cgroup_procs()) {
            TFile file;
            auto error = file.OpenWrite(path);
            if (error)
                return error;

            error = file.WriteAll(std::to_string(GetPid()));
            if (error)
                return error;
        }
        if (!Spec.resctrl_group().empty()) {
            TFile file;
            auto error = file.OpenWrite(TPath("/sys/fs/resctrl") / Spec.resctrl_group() / "tasks");
            if (!error)
                error = file.WriteAll(std::to_string(GetPid()));
            if (error)
                L_ERR("Failed attach to resctrl: {}", error);
        }
        return OK;
    }

    TError OpenStreams() {
        for (int target = 0; target < 3; ++target) {
            TStream stream(target);
            auto error = stream.Open(Spec.streams(target), Files);
            if (error)
                return error;
            Streams.emplace_back(std::move(stream));
        }
        return OK;
    }

    TError ApplyScheduling() {
        auto error = SetOomScoreAdj(Spec.oom_score_adj());
        if (error && Spec.oom_score_adj())
            return error;

        if (Spec.has_coredump_filter()) {
            error = SetCoredumpFilter(Spec.coredump_filter());
            if (error)
                return error;
        }
        if (setpriority(PRIO_PROCESS, 0, Spec.sched_nice()))
            return TError::System("setpriority");

        struct sched_param param = {};
        param.sched_priority = Spec.sched_priority();
        if (sched_setscheduler(0, Spec.sched_policy(), &param))
            return TError::System("sched_setscheduler");

        if (Spec.has_cpu_affinity()) {
            TBitMap affinity;
            error = affinity.Parse(Spec.cpu_affinity());
            if (error)
                return error;

            cpu_set_t mask;
            affinity.FillCpuSet(&mask);
            if (sched_setaffinity(0, sizeof(mask), &mask))
                return TError::System("sched_setaffinity({})", Spec.cpu_affinity());
        }
        if (SetIoPrio(0, Spec.io_priority()))
            return TError::System("ioprio");
        return OK;
    }

    TError DifferentPidNamespace(const TFile &proc, bool &different) {
        struct stat parent, self;
        auto error = proc.StatAt("ns/pid", true, parent);
        if (error)
            return error;

        error = TPath("/proc/thread-self/ns/pid").StatFollow(self);
        if (error)
            return error;

        different = !(parent.st_dev == self.st_dev && parent.st_ino == self.st_ino);
        return OK;
    }

    TError EnterParent(const TPidFd &parent, bool inheritNetwork, bool &differentPidNs) {
        TFile proc, cwd;
        auto error = parent.OpenProcFd(proc);
        if (error)
            return error;
        error = DifferentPidNamespace(proc, differentPidNs);
        if (error)
            return error;

        error = cwd.OpenDirAt(proc, "cwd");
        if (error)
            return error;

        int namespaces = CLONE_NEWIPC | CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWCGROUP;
        if (inheritNetwork)
            namespaces |= CLONE_NEWNET;
        if (setns(parent.PidFd.Fd, namespaces))
            return TError::System("Cannot enter parent namespaces");

        return cwd.Chdir();
    }

    TError EnterNamespaces(bool &differentPidNs) {
        if (Spec.net_fd() >= 0 && setns(Files[Spec.net_fd()].Fd, CLONE_NEWNET))
            return TError::System("Cannot enter network namespace");
        if (Spec.has_pid_fd()) {
            TPidFd parent;
            parent.PidFd.Swap(Files[Spec.pid_fd()]);
            auto error = EnterParent(parent, Spec.net_fd() < 0, differentPidNs);
            if (error)
                return error;
        }
        return OK;
    }

    TError MakeDevices() {
        for (const auto &spec: Spec.devices()) {
            TDevice device;
            device.PathInside = spec.path();
            device.Node = spec.node();
            device.Uid = spec.uid();
            device.Gid = spec.gid();
            device.Mode = spec.mode();
            device.Wildcard = spec.wildcard();
            device.MayMknod = spec.may_mknod();

            auto error = device.Makedev("/");
            if (error)
                return error;
        }
        return OK;
    }

    TError ConfigureFilesystem() {
        if (Spec.new_mount_ns()) {
            auto error = Mnt.Setup();
            if (error)
                return error;
        }
        if (!Mnt.Root.IsRoot()) {
            auto error = MakeDevices();
            if (error)
                return error;
        }
        for (const auto &sysctl: Spec.sysctl()) {
            auto error = SetSysctlAt(Mnt.ProcSysFd, sysctl.key(), sysctl.value());
            if (error)
                return error;
        }
        if (Spec.has_resolv_conf()) {
            auto error = TPath("/etc/resolv.conf").WritePrivate(Spec.resolv_conf());
            if (error)
                return error;
        }
        if (!Spec.etc_hosts().empty()) {
            auto error = TPath("/etc/hosts").WritePrivate(Spec.etc_hosts());
            if (error)
                return error;
        }
        if (!Spec.hostname().empty()) {
            auto error = TPath("/etc/hostname").WritePrivate(Spec.hostname() + "\n");
            if (error)
                return error;
            return SetHostName(Spec.hostname());
        }
        return OK;
    }

    TError SetCwd() {
        struct open_how how = {};
        how.flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
        how.resolve = RESOLVE_NO_MAGICLINKS;
        TFile dir;
        auto error = dir.OpenHow(AT_FDCWD, Mnt.Cwd, how);
        if (error)
            return TError(EError::InvalidPath, error.Errno, error.Text);
        return dir.Chdir();
    }

    TError ApplyCredentials() {
        auto error = TPath("/proc/self/loginuid").WriteAll(std::to_string(Spec.login_uid()));
        if (error && error.Errno != ENOENT)
            L_WRN("Cannot set loginuid: {}", error);
        error = Cred.Apply();
        if (error)
            return error;

        error = TCapabilities(Spec.cap_ambient()).ApplyAmbient();
        if (error)
            return error;

        error = TCapabilities(Spec.cap_bound()).ApplyLimit();
        if (error)
            return error;

        if (!Cred.IsRootUser())
            return TCapabilities(Spec.cap_ambient()).ApplyEffective();
        return OK;
    }

    TError WaitAutoconf() {
        if (Spec.autoconf().empty())
            return OK;
        auto sock = std::make_shared<TNl>();
        auto error = sock->Connect();
        if (error)
            return error;
        for (const auto &name: Spec.autoconf()) {
            TNlLink link(sock, name);
            error = link.Load();
            if (error)
                return error;
            error = link.WaitAddress(Spec.autoconf_timeout());
            if (error)
                return error;
        }
        return OK;
    }

    TError ExpandCommand(std::vector<std::string> &args) {
        if (Spec.command_argv()) {
            args.assign(Spec.argv().begin(), Spec.argv().end());
            if (args.empty() || args.front().empty())
                return TError(EError::InvalidCommand, "Empty command argv");
            return OK;
        }
        wordexp_t result = {};
        int ret = wordexp(Spec.command().c_str(), &result, WRDE_NOCMD | WRDE_UNDEF);
        if (ret == WRDE_NOSPACE)
            wordfree(&result);
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
        case 0:
            break;
        default:
            return TError(EError::InvalidCommand, "wordexp(): error {}", ret);
        }
        for (size_t i = 0; i < result.we_wordc; ++i)
            args.emplace_back(result.we_wordv[i]);
        wordfree(&result);
        if (args.empty() || args.front().empty())
            return TError(EError::InvalidCommand, "Empty command");
        return OK;
    }

    TError ApplyExecPolicy() {
        if (!Seccomp.Empty()) {
            auto error = SeccompContext.Apply(Seccomp);
            if (error)
                return error;
        }
        if (!SessionInfo.IsEmpty()) {
            auto error = SessionInfo.Apply();
            if (error)
                L_WRN("{}", error);
        }
        return OK;
    }

    [[noreturn]] void ExecCommand() {
        std::vector<std::string> args;
        AbortOnError(ExpandCommand(args));

        std::vector<char *> argv;
        for (auto &arg: args)
            argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        auto envp = Env.Envp();
        if (Verbose) {
            L("command={}", Spec.command());
            for (unsigned i = 0; i < args.size(); ++i)
                L("argv[{}]={}", i, args[i]);
            for (unsigned i = 0; envp[i]; ++i)
                L("environ[{}]={}", i, envp[i]);
        }

        AbortOnError(ApplyExecPolicy());
        AbortOnError(ReportError(OK));
        L("Exec '{}'", argv.front());
        execvpe(argv.front(), argv.data(), envp);
        if (errno == EAGAIN)
            Abort(TError(EError::ResourceNotAvailable, errno, "cannot exec {} not enough ulimit nproc", argv.front()));
        Abort(TError(EError::InvalidCommand, errno, "cannot exec {}", argv.front()));
    }

    [[noreturn]] void ExecMeta() {
        const char *argv[] = {"portoinit", "--container", Spec.name().c_str(), nullptr};
        TFile::CloseAllExcept({PortoInit.Fd, LogFile.Fd, Sock.Fd});
        AbortOnError(ReportError(OK));
        fexecve(PortoInit.Fd, const_cast<char *const *>(argv), Env.Envp());

        Abort(TError::System("fexec portoinit"));
    }

    [[noreturn]] void ExecPortoinit(pid_t pid) {
        auto child = std::to_string(pid);
        const char *argv[] = {"portoinit", "--container", Spec.name().c_str(), "--wait", child.c_str(), nullptr};
        auto error = TCapabilities(Spec.init_cap_bound()).ApplyLimit();
        if (!error) {
            TFile::CloseAllExcept({PortoInit.Fd, LogFile.Fd, Sock.Fd});
            fexecve(PortoInit.Fd, const_cast<char *const *>(argv), Env.Envp());
            error = TError::System("fexec portoinit");
        }
        kill(pid, SIGKILL);
        Abort(error);
    }

    [[noreturn]] void StartChild() {
        AbortOnError(ReportPid(TaskMsg::WaitPid));
        AbortOnError(Ulimit.Apply());
        if (setsid() < 0)
            Abort(TError::System("setsid()"));
        umask(0);
        AbortOnError(ConfigureFilesystem());
        AbortOnError(SetCwd());

        if (Spec.quadro_fork()) {
            // careful, vfork child must not return from
            // current function
            pid_t pid = vfork();
            if (pid < 0)
                Abort(TError::System("vfork()"));
            if (pid)
                ExecPortoinit(pid);
            if (setsid() < 0)
                Abort(TError::System("setsid()"));
        }
        AbortOnError(ApplyCredentials());

        for (auto &stream: Streams)
            AbortOnError(stream.Install());

        TFile::CloseAllExcept({STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, Sock.Fd, LogFile.Fd, PortoInit.Fd});
        umask(Spec.umask());
        AbortOnError(ReportPid(TaskMsg::TaskPid));
        ResetIgnoredSignals();
        AbortOnError(WaitAutoconf());
        AbortOnError(Env.Apply());

        if (Spec.meta())
            ExecMeta();
        else
            ExecCommand();
    }

    [[noreturn]] void DoClone(bool tripleFork) {
        struct clone_args args = {};
        args.exit_signal = SIGCHLD;
        if (tripleFork) {
            // careful, vfork child must not return from
            // current function
            pid_t pid = vfork();
            if (pid < 0)
                Abort(TError::System("clone3 entering pid namespace"));

            if (pid) {
                int status;
                while (waitpid(pid, &status, 0) < 0) {
                    if (errno != EINTR)
                        _exit(EXIT_FAILURE);
                }
                _exit(WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE);
            }
            args.flags |= CLONE_PARENT;
            args.exit_signal = 0;
        }
        if (Spec.isolate())
            args.flags |= CLONE_NEWIPC | CLONE_NEWPID;
        if (Mnt.MountCgroupFs)
            args.flags |= CLONE_NEWCGROUP;
        if (Spec.new_mount_ns())
            args.flags |= CLONE_NEWNS;
        if (Spec.isolate() || !Spec.hostname().empty())
            args.flags |= CLONE_NEWUTS;

        pid_t pid = syscall(__NR_clone3, &args, sizeof(args));
        if (pid < 0)
            Abort(TError(errno == ENOMEM ? EError::ResourceNotAvailable : EError::Unknown, errno, "clone3()"));
        if (!pid)
            StartChild();
        _exit(EXIT_SUCCESS);
    }

    [[noreturn]] void Run() {
        AbortOnError(Load());
        if (Spec.ptrace_protection()) {
            auto error = SetPtraceProtection();
            if (error)
                L_SYS("{}", error.Text);
        }

        InitCapabilities();
        ResetBlockedSignals();
        SetDieOnParentExit(SIGKILL);
        SetProcessName(fmt::format("portorun-CT{}", Spec.id()));
        (void)setsid();

        AbortOnError(AttachCgroups());
        AbortOnError(OpenStreams());
        AbortOnError(ApplyScheduling());

        bool differentPidNs = false;
        AbortOnError(EnterNamespaces(differentPidNs));
        Files.clear();
        DoClone(Spec.isolate() && differentPidNs);
    }

    int Start() {
        if (fcntl(TaskSocketFd, F_SETFD, FD_CLOEXEC) < 0)
            return EXIT_FAILURE;
        TFile::CloseAllExcept({TaskSocketFd});
        // Reserve standard descriptors before receiving or opening any files.
        if (open("/dev/null", O_RDWR) != STDIN_FILENO || dup2(STDIN_FILENO, STDOUT_FILENO) < 0 ||
            dup2(STDIN_FILENO, STDERR_FILENO) < 0)
            return EXIT_FAILURE;
        MapStatistics();
        auto error = ReceiveTask(Sock, Spec, Files);
        if (error)
            Abort(error);
        Run();
    }
};

}  // namespace

int PortodRun() {
    TTaskRun task{};
    return task.Start();
}
