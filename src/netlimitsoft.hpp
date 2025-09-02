#pragma once

#include <string>
#include <vector>

#include "common.hpp"

class TNetLimitSoftOfNet;

struct TNetLimitSoftStat {
    uint64_t marked = 0;
    uint64_t unmarked = 0;
    uint64_t fbed = 0;
    uint64_t dropping = 0;
    uint64_t pass = 0;

    uint64_t fbed_bytes = 0;
    uint64_t pass_bytes = 0;
};

class TNetLimitSoft: public TNonCopyable {
public:
    TNetLimitSoft();
    ~TNetLimitSoft();

    bool IsDisabled();
    TError Setup(const std::string &bpf_program_elf_path);
    TError SetupNetLimitSoftOfNet(TNetLimitSoftOfNet &netlimit);
    TError SetupNet(uint64_t key, uint32_t rate_in_bytes_per_s);

private:
    class TImpl;
    std::unique_ptr<TImpl> Impl;
};

class TNetLimitSoftOfNet: public TNonCopyable {
public:
    TNetLimitSoftOfNet();
    ~TNetLimitSoftOfNet();

    bool IsDisabled();
    TError Setup(const std::string &bpf_program_elf_code);

    int Prio() {
        return 61357;
    }  // hardcoded value so we can reliably delete/replace it

    TError BakeBpfProgCode(uint64_t key, std::vector<uint8_t> &prog_code);

private:
    class TImpl;
    std::unique_ptr<TImpl> Impl;
};
