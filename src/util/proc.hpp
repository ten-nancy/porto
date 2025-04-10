#pragma once

#include "string.hpp"

class TFile;

class TVmStat {
public:
    TUintMap Stat;

    TVmStat();
    void Reset();
    TError Parse(pid_t pid);
    void Add(const TVmStat &a);
    void Dump(rpc::TVmStat &s);
};

TError GetFdSize(pid_t pid, uint64_t &fdSize);

TError GetProcNetStats(pid_t pid, TUintMap &stats, const std::string &basename);

TError GetProc(pid_t pid, const std::string &knob, std::string &value);

TError GetProcStartTime(pid_t pid, uint64_t &time);

TError GetProcField(const TFile &procFd, const TPath &path, const std::string &field, std::string &value);
TError GetProcField(const TPath &path, const std::string &field, std::string &value);
TError GetProcField(const TFile &knob, const std::string &field, std::string &value);
pid_t GetProcVPid(const TFile &procFd);
