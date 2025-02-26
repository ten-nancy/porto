#include <iostream>
#include <string>

#include "config.hpp"
#include "core.hpp"
#include "fmt/format.h"
#include "kvalue.hpp"
#include "libporto.hpp"
#include "portod.hpp"
#include "util/log.hpp"
#include "util/path.hpp"
#include "util/string.hpp"
#include "version.hpp"

extern "C" {
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
}

static int CmdTimeout = -1;

static void KvDump() {
    TKeyValue::DumpAll(PORTO_CONTAINERS_KV);
    TKeyValue::DumpAll(PORTO_VOLUMES_KV);
    if (config().daemon().enable_nbd())
        TKeyValue::DumpAll(PORTO_NBD_KV);
}

static void PrintVersion() {
    TPath thisBin, currBin;

    TPath("/proc/self/exe").ReadLink(thisBin);
    if (MasterPidFile.Read() || TPath("/proc/" + std::to_string(MasterPidFile.Pid) + "/exe").ReadLink(currBin))
        TPath(PORTO_BINARY_PATH).ReadLink(currBin);

    std::cout << "version: " << PORTO_VERSION << " " << PORTO_REVISION << " " << thisBin << std::endl;

    Porto::Connection conn;
    std::string ver, rev;
    if (!conn.GetVersion(ver, rev))
        std::cout << "running: " << ver + " " + rev << " " << currBin << std::endl;
}

static int Status() {
    Signal(SIGPIPE, SIG_IGN);

    if (!MasterPidFile.Path.Exists()) {
        std::cout << "stopped" << std::endl;
        return EXIT_FAILURE;
    } else if (CheckPortoAlive()) {
        std::cout << "running" << std::endl;
        return EXIT_SUCCESS;
    } else {
        std::cout << "unknown" << std::endl;
        return EXIT_FAILURE;
    }
}

static int ReopenLog() {
    TError error;

    error = MasterPidFile.Read();
    if (error) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (kill(MasterPidFile.Pid, SIGUSR1) && errno != ESRCH) {
        std::cerr << "cannot send signal to portod: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

static int GetSystemProperties() {
    Porto::Connection conn;
    std::string rsp;
    int ret = conn.Call("GetSystem {}", rsp);
    if (ret) {
        std::cerr << conn.GetLastError() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << rsp << std::endl;
    return EXIT_SUCCESS;
}

static int SetSystemProperties(TTuple arg) {
    Porto::Connection conn;
    std::string rsp;
    if (arg.size() != 2)
        return EXIT_FAILURE;
    int ret = conn.Call(fmt::format("SetSystem {{ {}: {} }}", arg[0], arg[1]), rsp);
    if (ret) {
        std::cerr << conn.GetLastError() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static int ClearStatistics(TTuple arg) {
    Porto::Connection conn;
    std::string rsp;
    std::string req;
    if (arg.size() == 1)
        req = fmt::format("ClearStatistics {{ stat: \"{}\" }}", arg[0]);
    else
        req = "ClearStatistics {}";

    int ret = conn.Call(req, rsp);
    if (ret) {
        std::cerr << conn.GetLastError() << std::endl;
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

int StartPortod() {
    if (SanityCheck())
        return EXIT_FAILURE;

    pid_t pid = fork();
    if (pid < 0)
        return EXIT_FAILURE;

    if (!pid)
        return PortodMaster();

    uint64_t timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_start_timeout();
    uint64_t deadline = GetCurrentTimeMs() + timeout * 1000;
    do {
        if (CheckPortoAlive())
            return EXIT_SUCCESS;
        int status;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            std::cerr << "portod exited: " << FormatExitStatus(status) << std::endl;
            return EXIT_FAILURE;
        }
    } while (!WaitDeadline(deadline));
    std::cerr << "start timeout exceeded" << std::endl;
    return EXIT_FAILURE;
}

int KillPortod() {
    TError error;

    if (MasterPidFile.Read()) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_SUCCESS;
    }

    error = MasterPidFile.Remove();
    if (error)
        std::cerr << "cannot remove pidfile: " << error << std::endl;

    error = ServerPidFile.Remove();
    if (error)
        std::cerr << "cannot remove pidfile: " << error << std::endl;

    if (kill(MasterPidFile.Pid, SIGKILL) && errno != ESRCH) {
        std::cerr << "cannot kill portod: " << strerror(errno) << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int StopPortod() {
    TError error;

    if (MasterPidFile.Read()) {
        std::cerr << "portod already stopped" << std::endl;
        return EXIT_SUCCESS;
    }

    uint64_t timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_stop_timeout();

    pid_t pid = MasterPidFile.Pid;
    if (timeout > 0 && CheckPortoAlive()) {
        if (!kill(pid, SIGINT)) {
            uint64_t deadline = GetCurrentTimeMs() + timeout * 1000;
            do {
                if (MasterPidFile.Read() || MasterPidFile.Pid != pid)
                    return EXIT_SUCCESS;
            } while (!WaitDeadline(deadline));
        } else if (errno != ESRCH) {
            std::cerr << "cannot stop portod: " << strerror(errno) << std::endl;
            return EXIT_FAILURE;
        }
    }

    std::cerr << "portod not responding. sending sigkill" << std::endl;
    return KillPortod();
}

int ReexecPortod() {
    if (MasterPidFile.Read() || ServerPidFile.Read()) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (kill(MasterPidFile.Pid, SIGHUP)) {
        std::cerr << "cannot send signal" << std::endl;
        return EXIT_FAILURE;
    }

    uint64_t timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_start_timeout();
    uint64_t deadline = GetCurrentTimeMs() + timeout * 1000;
    do {
        if (!MasterPidFile.Running())
            return EXIT_FAILURE;
        if (CheckPortoAlive())
            return EXIT_SUCCESS;
    } while (!WaitDeadline(deadline));

    std::cerr << "timeout exceeded" << std::endl;
    return EXIT_FAILURE;
}

int UpgradePortod() {
    TPath symlink(PORTO_BINARY_PATH), procexe("/proc/self/exe"), update, backup;
    uint64_t timeout, deadline;
    TError error;

    error = procexe.ReadLink(update);
    if (error) {
        std::cerr << "cannot read /proc/self/exe" << error << std::endl;
        return EXIT_FAILURE;
    }

    error = MasterPidFile.Read();
    if (error) {
        std::cerr << "portod not running" << std::endl;
        return EXIT_FAILURE;
    }

    if (!CheckPortoAlive()) {
        std::cerr << "portod running but not responding" << std::endl;
        return EXIT_FAILURE;
    }

    error = ServerPidFile.Read();
    if (error) {
        std::cerr << "cannot find portod: " << error << std::endl;
        return EXIT_FAILURE;
    }

    error = symlink.ReadLink(backup);
    if (error) {
        if (error.Errno == ENOENT) {
            if (update != "/usr/sbin/portod") {
                std::cerr << "old portod can upgrade only to /usr/sbin/portod" << std::endl;
                return EXIT_FAILURE;
            }
        } else {
            std::cerr << "cannot read symlink " << symlink << ": " << error << std::endl;
            return EXIT_FAILURE;
        }
    }

    if (backup != update) {
        error = symlink.Unlink();
        if (error && error.Errno != ENOENT) {
            std::cerr << "cannot remove old symlink: " << error << std::endl;
            return EXIT_FAILURE;
        }

        error = symlink.Symlink(update);
        if (error) {
            std::cerr << "cannot replace portod symlink: " << error << std::endl;
            goto undo;
        }
    }

    if (kill(MasterPidFile.Pid, SIGHUP)) {
        std::cerr << "online upgrade failed: " << strerror(errno) << std::endl;
        goto undo;
    }

    timeout = CmdTimeout >= 0 ? CmdTimeout : config().daemon().portod_start_timeout();
    deadline = GetCurrentTimeMs() + timeout * 1000;
    do {
        if (!MasterPidFile.Running() || !ServerPidFile.Running())
            break;
    } while (!WaitDeadline(deadline));

    error = MasterPidFile.Read();
    if (error) {
        std::cerr << "online upgrade failed: " << error << std::endl;
        goto undo;
    }

    do {
        if (!MasterPidFile.Running())
            return EXIT_FAILURE;
        if (CheckPortoAlive()) {
            PrintVersion();
            return EXIT_SUCCESS;
        }
    } while (!WaitDeadline(deadline));

    std::cerr << "timeout exceeded" << std::endl;

undo:
    error = symlink.Unlink();
    if (error)
        std::cerr << "cannot remove symlink: " << error << std::endl;
    error = symlink.Symlink(backup);
    if (error)
        std::cerr << "cannot restore symlink: " << error << std::endl;
    return EXIT_FAILURE;
}

static void Usage() {
    std::cout << std::endl
              << "Usage: portod [options...] <command> [argments...]" << std::endl
              << std::endl
              << "Option: " << std::endl
              << "  -h | --help      print this message" << std::endl
              << "  -v | --version   print version and revision" << std::endl
              << "  --stdlog         print log into stdout" << std::endl
              << "  --norespawn      exit after failure" << std::endl
              << "  --verbose        verbose logging" << std::endl
              << "  --debug          debug logging" << std::endl
              << "  --discard        discard state after start" << std::endl
              << std::endl
              << "Commands: " << std::endl
              << "  status           check current portod status" << std::endl
              << "  daemon           start portod, this is default" << std::endl
              << "  start            daemonize and start portod" << std::endl
              << "  stop             stop running portod" << std::endl
              << "  kill             kill running portod" << std::endl
              << "  restart          stop followed by start" << std::endl
              << "  reload           reexec portod" << std::endl
              << "  reopenlog        reopen portod.log" << std::endl
              << "  upgrade          upgrade running portod" << std::endl
              << "  dump             print internal key-value state" << std::endl
              << "  get              print system properties" << std::endl
              << "  set <key> <val>  change system properties" << std::endl
              << "  clearstat [stat] reset statistics" << std::endl
              << "  freeze           freeze changes" << std::endl
              << "  unfreeze         unfreeze changes" << std::endl
              << "  core             receive and forward core dump" << std::endl
              << "  help             print this message" << std::endl
              << "  version          print version and revision" << std::endl
              << std::endl;
}

int main(int argc, char **argv) {
    int opt = 0;

    while (++opt < argc && argv[opt][0] == '-') {
        std::string arg(argv[opt]);

        if (arg == "-v" || arg == "--version") {
            PrintVersion();
            return EXIT_SUCCESS;
        }

        if (arg == "-h" || arg == "--help") {
            Usage();
            return EXIT_SUCCESS;
        }

        if (arg == "--stdlog")
            StdLog = true;
        else if (arg == "--verbose")
            Verbose = true;
        else if (arg == "--debug")
            Verbose = Debug = true;
        else if (arg == "--norespawn")
            RespawnPortod = false;
        else if (arg == "--discard")
            DiscardState = true;
        else if (arg == "--timeout") {
            if (StringToInt(argv[++opt], CmdTimeout))
                return EXIT_FAILURE;
        } else {
            std::cerr << "Unknown option: " << arg << std::endl;
            Usage();
            return EXIT_FAILURE;
        }
    }

    std::string cmd(argv[opt] ?: "");

    if (cmd == "status")
        return Status();

    if (cmd == "help") {
        Usage();
        return EXIT_SUCCESS;
    }

    if (cmd == "version") {
        PrintVersion();
        return EXIT_SUCCESS;
    }

    ReadConfigs(true);

    if (cmd == "" || cmd == "daemon")
        return PortodMaster();

    if (cmd == "start")
        return StartPortod();

    Signal(SIGPIPE, SIG_IGN);

    if (cmd == "stop")
        return StopPortod();

    if (cmd == "kill")
        return KillPortod();

    if (cmd == "restart") {
        StopPortod();
        return StartPortod();
    }

    if (cmd == "reload")
        return ReexecPortod();

    if (cmd == "upgrade")
        return UpgradePortod();

    if (cmd == "reopenlog")
        return ReopenLog();

    if (cmd == "dump") {
        OpenLog();
        KvDump();
        return EXIT_SUCCESS;
    }

    if (cmd == "get")
        return GetSystemProperties();

    if (cmd == "set")
        return SetSystemProperties(TTuple(argv + opt + 1, argv + argc));

    if (cmd == "clearstat")
        return ClearStatistics(TTuple(argv + opt + 1, argv + argc));

    if (cmd == "freeze")
        return SetSystemProperties({"frozen", "true"});

    if (cmd == "unfreeze")
        return SetSystemProperties({"frozen", "false"});

    if (cmd == "core") {
        TCore core;
        TError error = core.Handle(TTuple(argv + opt + 1, argv + argc));
        if (error)
            return EXIT_FAILURE;
        return EXIT_SUCCESS;
    }

    std::cerr << "Unknown command: " << cmd << std::endl;
    Usage();
    return EXIT_FAILURE;
}
