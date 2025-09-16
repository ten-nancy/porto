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
/* how many containers are jailed at cpu */
static std::vector<unsigned> JailCpuUsage;

static std::vector<unsigned> NodeRequest; /* numa node -> total jail weight */
static std::vector<unsigned> NodeUsage;   /* numa node -> total jail usage */
static unsigned UnboundRequest = 0;

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
    NodeRequest.resize(NumaNodes.Size());
    NodeUsage.resize(NumaNodes.Size());

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

    JailCpuUsage.resize(JailCpuPermutation.size());
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

    bool NodeBound() const override {
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
    unsigned &Request;
    std::shared_ptr<const TBitMap> Allocated;

    void UpdateJailCpuState(const TBitMap &curr, const TBitMap &next) const {
        PORTO_LOCKED(JailStateLock);

        for (unsigned cpu = 0; cpu < std::max(curr.Size(), next.Size()); ++cpu) {
            if (curr.Get(cpu) == next.Get(cpu))
                continue;

            auto node = ThreadsNode[cpu];
            auto d = next.Get(cpu) ? 1 : -1;

            PORTO_ASSERT(next.Get(cpu) || JailCpuUsage[cpu] > 0);

            JailCpuUsage[cpu] += d;
            Request += d;
            NodeUsage[node] += d;
        }
    }

    TError NextJailCpu(TBitMap &affinity) const {
        // (cpu_usage, node_usage)
        std::tuple<unsigned, unsigned> bestScore(UINT_MAX, UINT_MAX);
        unsigned bestCpu = UINT_MAX;

        for (unsigned cpu = 0; cpu < JailCpuUsage.size(); ++cpu) {
            if (affinity.Get(cpu))
                continue;

            if (Node >= 0 && ThreadsNode[cpu] != (unsigned)Node)
                continue;

            auto node = ThreadsNode[cpu];
            auto score = std::make_tuple(JailCpuUsage[cpu], NodeUsage[node]);

            if (score < bestScore) {
                bestCpu = cpu;
                bestScore = score;
            }
        }

        if (!HostCpus.Get(bestCpu))
            return TError("Failed allocate cpu to jail");

        JailCpuUsage[bestCpu]++;
        Request++;
        NodeUsage[ThreadsNode[bestCpu]]++;

        affinity.Set(bestCpu);

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
          Node(node),
          Request(Node < 0 ? UnboundRequest : NodeRequest[Node])
    {}

    bool NodeBound() const override {
        return Node >= 0;
    }

    TError Allocate(const TBitMap &current, TBitMap &target) override {
        // Allocated is not protected from concurrent updates. Thus
        // ct->TargetCpuSpec->Allocate must be protected by ActionLock
        // for now.
        if (Satisfies(current) && (!Allocated || *Allocated != current)) {
            L("Jail restore allocation {}", current.Format());
            {
                auto lock = LockJailState();
                UpdateJailCpuState(Allocated ? *Allocated : TBitMap(), current);
            }
            Allocated = std::make_shared<const TBitMap>(current);
        }

        if (Allocated) {
            L("Jail cached allocation: {}", Allocated->Format());
            target = *Allocated;
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

        Allocated = std::make_shared<const TBitMap>(target);
        L("Jail new allocation: {}", Allocated->Format());

        return OK;
    }

    void Release() override {
        if (!Allocated)
            return;

        L("Jail release allocation: {}", Allocated->Format());
        {
            auto lock = LockJailState();
            UpdateJailCpuState(*Allocated, {});
        }
        Allocated.reset();
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

    std::shared_ptr<const TBitMap> GetAllocation() const override {
        return Allocated;
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

std::vector<unsigned> GetJailCpuUsage() {
    auto lock = LockJailState();
    return JailCpuUsage;
}

static TError Widen(const TCgroup &cgroup, TBitMap &current, const TBitMap &target) {
    if (target.IsSubsetOf(current))
        return OK;

    current.Set(target);  // union
    return CgroupDriver.CpusetSubsystem->SetCpus(cgroup, current);
}

static TError ApplyCgSubtreeCpus(const TCgroup &cgroup, const TBitMap &target) {
    std::vector<std::unique_ptr<const TCgroup>> children;
    auto error = cgroup.Children(children);

    TBitMap current;
    error = CgroupDriver.CpusetSubsystem->GetCpus(cgroup, current);
    if (error)
        return error;

    if (target == current)
        return OK;

    if (children.empty())
        return CgroupDriver.CpusetSubsystem->SetCpus(cgroup, target);

    error = Widen(cgroup, current, target);
    if (error)
        return error;

    for (auto &child: children) {
        auto error = ApplyCgSubtreeCpus(*child, target);
        if (error)
            return error;
    }

    if (current != target)
        return CgroupDriver.CpusetSubsystem->SetCpus(cgroup, target);
    return OK;
}

TError ApplySubtreeCpus(TContainer &container, bool restore) {
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
        error = container.TargetCpuSetSpec->Allocate(restore ? current : TBitMap(), target);
        if (error)
            return error;
        container.CpuSetSpec = container.TargetCpuSetSpec;
        L("{} current={} target={}", container.Slug, current.Format(), target.Format());

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
        auto error = ApplySubtreeCpus(*child, restore);
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

TError ApplyCpuSet(TContainer &container, bool restore) {
    auto cpuSetSpec = container.CpuSetSpec;
    auto error = ApplySubtreeCpus(container, restore);
    if (error) {
        L_ERR("Failed apply cpu_set: {}", error);
        container.TargetCpuSetSpec = cpuSetSpec;
        auto error2 = ApplySubtreeCpus(container, restore);
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

// Get optimal per-node request
static std::vector<unsigned> getNodeRequest(const std::vector<unsigned> &nodeUsage) {
    PORTO_LOCKED(JailStateLock);

    auto nodeRequest = NodeRequest;

    auto cmp = [&](unsigned n1, unsigned n2) {
        if (nodeRequest[n1] == nodeRequest[n2])
            return nodeUsage[n1] < nodeUsage[n2];
        return nodeRequest[n1] > nodeRequest[n2];
    };

    std::vector<unsigned> nodes;
    for (unsigned n = 0; n < NumaNodes.Size(); ++n) {
        if (!NumaNodes.Get(n))
            continue;
        nodes.push_back(n);
        std::push_heap(nodes.begin(), nodes.end(), cmp);
    }

    for (unsigned i = 0; i < UnboundRequest; ++i) {
        pop_heap(nodes.begin(), nodes.end(), cmp);
        auto node = nodes.back();
        nodeRequest[node] += 1;

        push_heap(nodes.begin(), nodes.end(), cmp);
    }

    return nodeRequest;
}

static unsigned ceildiv(unsigned x, unsigned y) {
    return x / y + !!(x % y);
}

// Returns vector of pairs (cpu, overload)
std::vector<std::pair<unsigned, unsigned>> FindUnbalancedJailCpus() {
    auto lock = LockJailState();

    auto nodeRequest = getNodeRequest(NodeUsage);
    std::vector<std::pair<unsigned, unsigned>> unbalanced;

    for (unsigned n = 0; n < NumaNodes.Size(); ++n) {
        if (!NumaNodes.Get(n))
            continue;

        auto nodeUnbalanced = NodeUsage[n] > nodeRequest[n];
        // Whole node is overloaded, need move jails to another node
        if (nodeUnbalanced) {
            auto imbalance = ceildiv(NodeUsage[n] - nodeRequest[n], NodeThreads[n].Weight());
            L("Node {} is unbalanced: usage={} request={}", n, NodeUsage[n], nodeRequest[n]);
            for (unsigned cpu = 0; cpu < NodeThreads[n].Size(); ++cpu) {
                if (!NodeThreads[n].Get(cpu))
                    continue;

                unbalanced.push_back(std::make_pair(cpu, imbalance));
            }
            continue;
        }
        // Try find overload inside node
        unsigned minUsage = UINT_MAX, maxUsage = 0;
        std::vector<unsigned> maxUsageCpus;
        for (unsigned cpu = 0; cpu < NodeThreads[n].Size(); ++cpu) {
            if (!NodeThreads[n].Get(cpu))
                continue;

            unsigned usage = JailCpuUsage[cpu];
            if (usage >= maxUsage) {
                if (usage > maxUsage)
                    maxUsageCpus.clear();

                maxUsage = usage;
                maxUsageCpus.push_back(cpu);
            }
            if (minUsage)
                minUsage = usage;
        }
        auto delta = maxUsage - minUsage;
        if (delta < 2)
            continue;  // balanced

        for (auto cpu: maxUsageCpus)
            unbalanced.push_back(std::make_pair(cpu, delta - 1));
    }

    return unbalanced;
}

std::shared_ptr<TContainer> FindUnbalancedJailContainer(
    const std::vector<std::pair<unsigned, unsigned>> unbalancedCpus) {
    auto ctLock = LockContainers();
    auto containers = Containers;
    ctLock.unlock();

    auto score = [&unbalancedCpus](const TBitMap &allocation, bool nodeBound) {
        unsigned ctOverload = 0;
        for (auto &pair: unbalancedCpus) {
            auto cpu = pair.first;
            auto overload = pair.second;

            if (allocation.Get(cpu))
                ctOverload += overload;
        }

        if (!ctOverload)
            return std::make_tuple(0, 0U);

        return std::make_tuple(int(!nodeBound), ctOverload);
    };

    auto maxScore = std::make_tuple(0, 0);
    std::shared_ptr<TContainer> unbalancedCt;

    for (auto &it: containers) {
        auto &ct = it.second;
        auto spec = ct->CpuSetSpec;

        auto allocation = spec->GetAllocation();
        if (!allocation)
            continue;

        auto state = ct->State.load();
        if (state != EContainerState::Running && state != EContainerState::Meta)
            continue;

        auto s = score(*allocation, spec->NodeBound());
        if (s > maxScore) {
            unbalancedCt = ct;
            maxScore = s;
        }
    }

    return unbalancedCt;
}
