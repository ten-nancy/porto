#pragma once

#include <string>
#include <vector>

#include "common.hpp"


class TNetLimitSoftOfNet;


class TNetLimitSoft : public TNonCopyable {
public:
    TNetLimitSoft();
    ~TNetLimitSoft();

    bool IsDisabled();
    TError Setup(const std::string &bpf_program_elf_path);
    TError SetupNetLimitSoftOfNet(TNetLimitSoftOfNet &netlimit);
    TError SetupNet(uint64_t key, uint32_t rate_in_kb_s);

private:
    class TImpl;
    std::unique_ptr<TImpl> Impl;
};


class TNetLimitSoftOfNet : public TNonCopyable {
public:
    TNetLimitSoftOfNet();
    ~TNetLimitSoftOfNet();

    bool IsDisabled();
    TError Setup(const std::string &bpf_program_elf_code);

    int Prio() { return 61357; } // hardcoded value so we can reliably delete/replace it

    TError BakeBpfProgCode(uint64_t key, std::vector<uint8_t> &prog_code);

private:
    class TImpl;
    std::unique_ptr<TImpl> Impl;
};

