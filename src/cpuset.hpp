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

    virtual bool NodeBound() const {
        return false;
    }
    virtual TError Allocate(const TBitMap &current, TBitMap &target) = 0;
    virtual void Release() = 0;
    virtual TError Validate(TContainer &container) = 0;
    virtual std::string ToString() const = 0;
    virtual void Dump(rpc::TContainerCpuSet &value) const = 0;

    virtual std::shared_ptr<const TBitMap> GetAllocation() const {
        return nullptr;
    }
};

TError BuildCpuTopology();
std::vector<unsigned> GetJailCpuUsage();
TError ApplyCpuSet(TContainer &container, bool restore);
TBitMap GetNoSmtCpus(const TBitMap &cpuAffinity);

std::vector<std::pair<unsigned, unsigned>> FindUnbalancedJailCpus();
std::shared_ptr<TContainer> FindUnbalancedJailContainer(
    const std::vector<std::pair<unsigned, unsigned>> unbalancedCpus);
