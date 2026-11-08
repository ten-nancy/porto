#pragma once

#include <string>

#include "util/path.hpp"
#include "util/string.hpp"

/*
 * Resource control groups (Intel RDT / AMD QoS) live in a flat directory
 * under the resctrl filesystem. Porto group is named after the container:
 * "porto%" + container name with '/' replaced by '%'.
 */

TError MountResctrl();
bool IsResctrlAvailable();

std::string ResctrlGroupName(const std::string &name);

TError CreateResctrlGroup(const std::string &group);
TError RemoveResctrlGroup(const std::string &group);
TError AttachResctrlGroup(const std::string &group, pid_t tid);

/*
 * Spread membw (in 256MB/s units) over the L3 domains holding cpus of the
 * affinity, proportionally to the amount of cpus on each domain.
 */
TError SetResctrlSchemata(const std::string &group, uint64_t membw, const TBitMap &affinity);
