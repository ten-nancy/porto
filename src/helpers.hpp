#pragma once

#include <cgroup.hpp>
#include <string>
#include <vector>

#include "common.hpp"
#include "util/path.hpp"

struct TCgroupContext {
    std::string Memory;
    std::string Cpu;
    std::string Cpuset;
    pid_t Pid;

    TCgroupContext(const std::string &memory = PORTO_HELPERS_CGROUP, const std::string &cpu = PORTO_HELPERS_CGROUP,
                   const std::string &cpuset = PORTO_HELPERS_CGROUP)
        : Memory(memory),
          Cpu(cpu),
          Cpuset(cpuset),
          Pid(0)
    {}

    static TCgroupContext FromContainerByPid(pid_t tpid);
    static TCgroupContext FromContainer(const TContainer *container);
};

TError RunCommand(const std::vector<std::string> &command, const TFile &dir = TFile(), const TFile &input = TFile(),
                  const TFile &output = TFile(), const TCapabilities &caps = HelperCapabilities,
                  bool verboseError = false, bool interruptible = false);
TError RunCommand(const std::vector<std::string> &command, const std::vector<std::string> &env,
                  const TFile &dir = TFile(), const TFile &input = TFile(), const TFile &output = TFile(),
                  const TCapabilities &caps = HelperCapabilities,
                  const TCgroupContext &cgroupContext = TCgroupContext(), bool verboseError = false,
                  bool interruptible = false);

TError CopyRecursive(const TPath &src, const TPath &dst);

TError DownloadFile(const std::string &url, const TPath &path, const TCgroupContext &cgrpCtx,
                    const std::vector<std::string> &headers = {});
