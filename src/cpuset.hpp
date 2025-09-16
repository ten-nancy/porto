#pragma once
#include <mutex>
#include <vector>

#include "util/string.hpp"

class TContainer;
namespace rpc {
class TContainerCpuSet;
};

extern bool HyperThreadingEnabled;

class TCpuSetSpec {
public:
    static std::shared_ptr<TCpuSetSpec> Empty();
    static std::shared_ptr<TCpuSetSpec> Parse(const TContainer &container, const std::string &text);
    static std::shared_ptr<TCpuSetSpec> Load(const TContainer &container, const rpc::TContainerCpuSet &value);

    virtual bool HasNode() const {
        return false;
    }
    virtual TError Allocate(const TBitMap &current, TBitMap &target) = 0;
    virtual void Release() = 0;
    virtual TError Validate(TContainer &container) = 0;
    virtual std::string ToString() const = 0;
    virtual void Dump(rpc::TContainerCpuSet &value) const = 0;
};

struct TJailCpuState {
    std::vector<unsigned> Permutation;
    std::vector<unsigned> Usage;

    TJailCpuState(const std::vector<unsigned> &permutation, std::vector<unsigned> usage)
        : Permutation(permutation),
          Usage(usage)
    {}
};

TError BuildCpuTopology();
TJailCpuState GetJailCpuState();
TError ApplyCpuSet(TContainer &container);
TBitMap GetNoSmtCpus(const TBitMap &cpuAffinity);
