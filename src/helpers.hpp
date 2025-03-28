#include <cgroup.hpp>
#include <string>
#include <vector>

#include "util/path.hpp"

TError RunCommand(const std::vector<std::string> &command, const TFile &dir = TFile(), const TFile &input = TFile(),
                  const TFile &output = TFile(), const TCapabilities &caps = HelperCapabilities,
                  bool verboseError = false, bool interruptible = false);
TError RunCommand(const std::vector<std::string> &command, const std::vector<std::string> &env,
                  const TFile &dir = TFile(), const TFile &input = TFile(), const TFile &output = TFile(),
                  const TCapabilities &caps = HelperCapabilities, const std::string &memCgroup = PORTO_HELPERS_CGROUP,
                  bool verboseError = false, bool interruptible = false);

TError CopyRecursive(const TPath &src, const TPath &dst);

TError DownloadFile(const std::string &url, const TPath &path, const std::vector<std::string> &headers = {});
