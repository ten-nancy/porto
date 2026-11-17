#pragma once

#include "task.pb.h"
#include "util/unix.hpp"

constexpr int TaskSocketFd = 3;
enum class TaskMsg { Error = 8321, WaitPid = 8322, TaskPid = 8323 };

TError SendTask(const TUnixSocket &socket, const runtime::TTaskSpec &spec, const std::vector<int> &fds);
TError ReceiveTask(const TUnixSocket &socket, runtime::TTaskSpec &spec, std::vector<TFile> &fds);
