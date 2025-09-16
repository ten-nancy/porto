#include "cpuset.hpp"

#include "cgroup.hpp"
#include "container.hpp"
#include "fmt/format.h"
#include "rpc.pb.h"
#include "util/log.hpp"
#include "util/path.hpp"

bool HyperThreadingEnabled = false;           /* hyperthreading is disabled in vms and sandbox tests */
static std::vector<unsigned> NeighborThreads; /* cpu -> neighbor hyperthread */

static TBitMap HostCpus;

static TBitMap NumaNodes;
/* numa node -> list of cpus */
static std::vector<TBitMap> NodeThreads;
/* cpu -> numa node */
static std::vector<unsigned> ThreadsNode;

static std::mutex JailStateLock;
/* 0,8,16,24,1,9,17,25,2,10,18,26,... */
static std::vector<unsigned> JailCpuPermutation;
/* cpu -> index in JailCpuPermutation */
static std::vector<unsigned> CpuToJailPermutationIndex;
/* how many containers are jailed at JailCpuPermutation[cpu] core */
static std::vector<unsigned> JailCpuPermutationUsage;

static inline std::unique_lock<std::mutex> LockJailState() {
    return std::unique_lock<std::mutex>(JailStateLock);
}

TError BuildCpuTopology() {
    PORTO_ASSERT(NumaNodes.Weight() == 0);

    auto error = HostCpus.Read(TPath("/sys/devices/system/cpu/online"));
    if (error)
        return error;

    error = NumaNodes.Read("/sys/devices/system/node/online");
    if (error)
        return error;

    NodeThreads.resize(NumaNodes.Size());
    ThreadsNode.resize(HostCpus.Size());

    for (unsigned node = 0; node < NumaNodes.Size(); node++) {
        if (!NumaNodes.Get(node))
            continue;

        auto &nodeThreads = NodeThreads[node];

        error = nodeThreads.Read(StringFormat("/sys/devices/system/node/node%u/cpulist", node));
        if (error)
            return error;
        for (unsigned cpu = 0; cpu < nodeThreads.Size(); cpu++) {
            if (!nodeThreads.Get(cpu))
                continue;

            ThreadsNode[cpu] = node;
        }
    }

    NeighborThreads.resize(HostCpus.Size());

    for (unsigned cpu = 0; cpu < HostCpus.Size(); cpu++) {
        if (!HostCpus.Get(cpu))
            continue;

        TBitMap neighbors;
        error = neighbors.Read(StringFormat("/sys/devices/system/cpu/cpu%u/topology/thread_siblings_list", cpu));
        if (error)
            return error;

        if (neighbors.Weight() > 1) {
            HyperThreadingEnabled = true;
            NeighborThreads[cpu] = neighbors.FirstBitIndex(cpu + 1);
        }
    }

    JailCpuPermutation.resize(HostCpus.Size());
    CpuToJailPermutationIndex.resize(HostCpus.Size());

    std::vector<unsigned> nodeThreadsIter(NumaNodes.Size());
    for (unsigned i = 0; i != HostCpus.Size() / NumaNodes.Size(); i += HyperThreadingEnabled ? 2 : 1) {
        unsigned j = 0;
        for (unsigned node = 0; node < NumaNodes.Size(); node++) {
            if (!NumaNodes.Get(node))
                continue;

            int cpu = NodeThreads[node].FirstBitIndex(nodeThreadsIter[node]);
            unsigned index = i * NumaNodes.Size() + j;
            JailCpuPermutation[index] = cpu;
            CpuToJailPermutationIndex[cpu] = index;
            if (HyperThreadingEnabled) {
                index = (i + 1) * NumaNodes.Size() + j;
                JailCpuPermutation[index] = NeighborThreads[cpu];
                CpuToJailPermutationIndex[NeighborThreads[cpu]] = index;
            }

            nodeThreadsIter[node] = cpu + 1;
            ++j;
        }
    }

    JailCpuPermutationUsage.resize(JailCpuPermutation.size());
    return OK;
}

class TInheritSpec: public TCpuSetSpec {
    std::weak_ptr<TContainer> Container;

public:
    TInheritSpec(const TContainer &container)
        : Container(container.Parent)
    {}

    TError Allocate(const TBitMap &current, TBitMap &affinity) override {
        auto ct = Container.lock();
        if (ct)
            return ct->TargetCpuSetSpec->Allocate(current, affinity);
        affinity = HostCpus;
        return OK;
    }

    void Release() override {};

    TError Validate(TContainer &) override {
        return OK;
    }

    std::string ToString() const override {
        return "";
    }

    void Dump(rpc::TContainerCpuSet &spec) const override {
        spec.set_policy("inherit");
    }
};

class TAbsoluteSpec: public TCpuSetSpec {
    const TBitMap Affinity;

public:
    TAbsoluteSpec(const TBitMap &affinity)
        : Affinity(affinity)
    {}

    TError Allocate(const TBitMap &, TBitMap &affinity) override {
        affinity = Affinity;
        return OK;
    }

    TError Validate(TContainer &) override {
        return OK;
    }

    void Release() override {}

    std::string ToString() const override {
        return Affinity.Format();
    }

    void Dump(rpc::TContainerCpuSet &cfg) const override {
        cfg.set_policy("set");
        for (unsigned cpu = 0; cpu < Affinity.Size(); cpu++) {
            if (Affinity.Get(cpu))
                cfg.add_cpu(cpu);
        }
        cfg.set_list(Affinity.Format());
        cfg.set_count(Affinity.Weight());
    }
};

class TNodeSpec: public TCpuSetSpec {
    const unsigned Node;

public:
    TNodeSpec(unsigned node)
        : Node(node)
    {}

    bool HasNode(void) const override {
        return true;
    }

    TError Allocate(const TBitMap &, TBitMap &affinity) override {
        affinity = NodeThreads[Node];
        return OK;
    }

    TError Validate(TContainer &) override {
        return OK;
    }

    void Release() override {};

    std::string ToString() const override {
        return fmt::format("node {}", Node);
    }

    void Dump(rpc::TContainerCpuSet &spec) const override {
        spec.set_policy("node");
        spec.set_arg(Node);
    }
};

class TJailSpec: public TCpuSetSpec {
    const unsigned Size;
    const int Node;
    TBitMap Allocated;

    static void UpdateJailCpuState(const TBitMap &curr, const TBitMap &next) {
        PORTO_LOCKED(JailStateLock);

        for (unsigned cpu = 0; cpu < std::max(curr.Size(), next.Size()); ++cpu) {
            if (curr.Get(cpu) == next.Get(cpu))
                continue;

            auto index = CpuToJailPermutationIndex[cpu];
            if (curr.Get(cpu)) {
                PORTO_ASSERT(JailCpuPermutationUsage[index] > 0);
                JailCpuPermutationUsage[index]--;
            } else
                JailCpuPermutationUsage[index]++;
        }
    }

    TError NextJailCpu(TBitMap &affinity) const {
        unsigned minUsage = UINT_MAX, minIndex;

        for (unsigned i = 0; i < JailCpuPermutationUsage.size(); ++i) {
            int cpu = JailCpuPermutation[i];

            if (affinity.Get(cpu))
                continue;

            if (Node >= 0 && ThreadsNode[cpu] != (unsigned)Node)
                continue;

            if (JailCpuPermutationUsage[i] < minUsage) {
                minIndex = i;
                minUsage = JailCpuPermutationUsage[i];
            }
        }

        if (minUsage == UINT_MAX)
            return TError("Failed allocate cpu to jail");

        JailCpuPermutationUsage[minIndex]++;
        affinity.Set(JailCpuPermutation[minIndex]);
        return OK;
    }

    bool Satisfies(const TBitMap &affinity) const {
        if (affinity.Weight() != Size)
            return false;

        if (Node < 0)
            return true;

        return affinity.IsSubsetOf(NodeThreads[Node]);
    }

public:
    TJailSpec(unsigned size, int node)
        : Size(size),
          Node(node)
    {}

    bool HasNode() const override {
        return Node >= 0;
    }

    TError Allocate(const TBitMap &current, TBitMap &target) override {
        if (Satisfies(current) && Allocated != current) {
            L("Jail restore allocation {}", current.Format());
            {
                auto lock = LockJailState();
                UpdateJailCpuState(Allocated, current);
            }
            Allocated = current;
        }

        if (Allocated.Size() > 0) {
            L("Jail cached allocation: {}", Allocated.Format());
            target = Allocated;
            return OK;
        }

        auto lock = LockJailState();

        for (unsigned i = 0; i < Size; i++) {
            auto error = NextJailCpu(target);
            if (error) {
                // rollback jail state
                UpdateJailCpuState(target, {});
                return error;
            }
        }

        lock.unlock();

        Allocated = target;
        L("Jail new allocation: {}", Allocated.Format());

        return OK;
    }

    void Release() override {
        if (Allocated.Size() == 0)
            return;

        L("Jail release allocation: {}", Allocated.Format());
        {
            auto lock = LockJailState();
            UpdateJailCpuState(Allocated, {});
        }
        Allocated.Clear();
    }

    TError Validate(TContainer &container) override {
        for (auto ct = container.Parent; ct; ct = ct->Parent) {
            auto spec = ct->TargetCpuSetSpec;
            if (dynamic_cast<TJailSpec *>(spec.get()) != nullptr)
                return TError(EError::ResourceNotAvailable, "Nested cpu jails are not supported for {}",
                              container.Slug);
        }

        for (auto &ct: container.Subtree()) {
            if (ct.get() == &container)
                continue;

            auto spec = ct->TargetCpuSetSpec;
            if (dynamic_cast<TJailSpec *>(spec.get()) != nullptr)
                return TError(EError::ResourceNotAvailable, "Nested cpu jails are not supported for {}",
                              container.Slug);
        }

        return OK;
    }

    std::string ToString() const override {
        return fmt::format("jail {}{}", Size, Node < 0 ? "" : fmt::format("; node {}", Node));
    }

    static std::shared_ptr<TCpuSetSpec> Parse(const TTuple &jailCfg, const TTuple &nodeCfg) {
        int node = -1;
        if (nodeCfg.size()) {
            if (nodeCfg.size() != 2)
                return nullptr;
            if (nodeCfg[0] != "node")
                return nullptr;

            unsigned val;
            auto error = StringToUint(nodeCfg[1], val);
            if (error)
                return nullptr;

            if (!NumaNodes.Get(val))
                return nullptr;

            node = (int)val;
        }

        if (jailCfg.size() != 2 || jailCfg[0] != "jail")
            return nullptr;

        unsigned size;
        auto error = StringToUint(jailCfg[1], size);
        if (error)
            return nullptr;

        if (size == 0 || size >= HostCpus.Weight())
            return nullptr;

        if (node >= 0 && size >= NodeThreads[node].Weight())
            return nullptr;

        return std::make_shared<TJailSpec>(size, node);
    }

    void Dump(rpc::TContainerCpuSet &spec) const override {
        spec.set_policy("jail");
        spec.set_arg(Node);
        spec.set_jail(Size);
    }
};

class TNopSpec: public TCpuSetSpec {
    TError Allocate(const TBitMap &, TBitMap &affinity) override {
        affinity.Clear();
        return OK;
    };
    void Release() override {};
    TError Validate(TContainer &) override {
        return OK;
    }
    std::string ToString() const override {
        return "";
    };
    void Dump(rpc::TContainerCpuSet &) const override {};
};

std::shared_ptr<TCpuSetSpec> TCpuSetSpec::Empty() {
    return std::make_shared<TNopSpec>();
}

std::shared_ptr<TCpuSetSpec> TCpuSetSpec::Parse(const TContainer &container, const std::string &text) {
    if (text == "" || text == "all" || text == "inherit")
        return std::make_shared<TInheritSpec>(container);

    auto cfgs = SplitEscapedString(text, ' ', ';');

    switch (cfgs.size()) {
    case 1: {
        auto &cfg = cfgs[0];

        switch (cfg.size()) {
        case 1: {
            TBitMap affinity;
            auto error = affinity.Parse(cfg[0]);
            if (error)
                return nullptr;

            if (!affinity.IsSubsetOf(HostCpus))
                return nullptr;

            return std::make_shared<TAbsoluteSpec>(affinity);
        }
        case 2: {
            auto &type = cfg[0];
            if (type == "node") {
                unsigned node;
                auto error = StringToUint(cfg[1], node);
                if (error)
                    return nullptr;

                if (!NumaNodes.Get(node))
                    return nullptr;

                return std::make_shared<TNodeSpec>(node);
            } else if (type == "jail")
                return TJailSpec::Parse(cfg, {});
            return nullptr;
        }
        default:
            return nullptr;
        }
    }
    case 2: {
        if (cfgs[0].size() != 2)
            return nullptr;

        // canonical form: "jail J; node N"
        auto cannonical = cfgs[0][0] == "jail";
        auto &jailCfg = cannonical ? cfgs[0] : cfgs[1];
        auto &nodeCfg = cannonical ? cfgs[1] : cfgs[0];

        return TJailSpec::Parse(jailCfg, nodeCfg);
    }
    default:
        return nullptr;
    }
}

std::shared_ptr<TCpuSetSpec> TCpuSetSpec::Load(const TContainer &container, const rpc::TContainerCpuSet &cfg) {
    if (cfg.policy() == "" || cfg.policy() == "inherit")
        return std::make_shared<TInheritSpec>(container);

    if (cfg.policy() == "set") {
        TBitMap affinity;

        if (cfg.has_list()) {
            auto error = affinity.Parse(cfg.list());
            if (error)
                return nullptr;
        } else {
            for (auto cpu: cfg.cpu())
                affinity.Set(cpu);
        }

        return std::make_shared<TAbsoluteSpec>(affinity);
    }

    if (cfg.policy() == "node")
        return std::make_shared<TNodeSpec>(cfg.arg());

    if (cfg.policy() == "jail")
        return std::make_shared<TJailSpec>(cfg.jail(), cfg.has_arg() ? cfg.arg() : -1);

    return nullptr;
}

TJailCpuState GetJailCpuState() {
    auto lock = LockJailState();
    return {JailCpuPermutation, JailCpuPermutationUsage};
}

static TError Widen(const TCgroup &cgroup, TBitMap &current, const TBitMap &target) {
    if (target.IsSubsetOf(current))
        return OK;

    current.Set(target);  // union
    return CgroupDriver.CpusetSubsystem->SetCpus(cgroup, current);
}

static TError ApplyCgSubtreeCpus(const TCgroup &cgroup, const TBitMap &affinity) {
    std::vector<std::unique_ptr<const TCgroup>> children;
    auto error = cgroup.Children(children);

    TBitMap current;
    error = CgroupDriver.CpusetSubsystem->GetCpus(cgroup, current);
    if (error)
        return error;

    error = Widen(cgroup, current, affinity);
    if (error)
        return error;

    for (auto &child: children) {
        auto error = ApplyCgSubtreeCpus(*child, affinity);
        if (error)
            return error;
    }

    if (current != affinity)
        return CgroupDriver.CpusetSubsystem->SetCpus(cgroup, affinity);
    return OK;
}

TError ApplySubtreeCpus(TContainer &container) {
    if (!container.HasResources())
        return OK;

    auto cg = CgroupDriver.GetContainerCgroup(container, CgroupDriver.CpusetSubsystem.get());
    TBitMap target, current;

    auto error = container.TargetCpuSetSpec->Validate(container);
    if (error)
        return error;

    if (container.HasControllers(CGROUP_CPUSET)) {
        auto error = CgroupDriver.CpusetSubsystem->GetCpus(*cg, current);
        if (error)
            return error;

        container.CpuSetSpec->Release();
        error = container.TargetCpuSetSpec->Allocate(current, target);
        if (error)
            return error;
        container.CpuSetSpec = container.TargetCpuSetSpec;

        if (current == target)
            return OK;

        if (container.Children.empty())
            return ApplyCgSubtreeCpus(*cg, target);

        // Widen modifies current
        error = Widen(*cg, current, target);
        if (error)
            return error;
    }

    for (auto &child: container.Children) {
        auto error = ApplySubtreeCpus(*child);
        if (error)
            return error;
    }

    if (target != current) {
        if (cg->HasLeaf()) {
            auto error = CgroupDriver.CpusetSubsystem->SetCpus(*cg->Leaf(), target);
            if (error)
                return error;
        }
        return CgroupDriver.CpusetSubsystem->SetCpus(*cg, target);
    }

    return OK;
}

TError ApplyCpuSet(TContainer &container) {
    auto cpuSetSpec = container.CpuSetSpec;
    auto error = ApplySubtreeCpus(container);
    if (error) {
        L_ERR("Failed apply cpu_set: {}", error);
        container.TargetCpuSetSpec = cpuSetSpec;
        auto error2 = ApplySubtreeCpus(container);
        if (error2)
            L_ERR("Failed rollback cpu_set for {}: {}", container.Slug, error2);
    }
    return error;
}

TBitMap GetNoSmtCpus(const TBitMap &cpuAffinity) {
    TBitMap noSmt;

    noSmt.Set(cpuAffinity);
    for (unsigned cpu = 0; cpu < cpuAffinity.Size(); cpu++) {
        if (noSmt.Get(cpu)) {
            noSmt.Set(NeighborThreads[cpu], false);
        }
    }

    return noSmt;
}
