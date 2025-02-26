#include <cstdint>
#include <string>
#include <unordered_map>

extern "C" {
#include <seccomp.h>
}

#include "seccomp.hpp"
#include "util/error.hpp"

template <typename K, typename V, typename K1 = K>
static std::unordered_map<V, K1> reverseMap(const std::unordered_map<K, V> &m) {
    std::unordered_map<V, K1> r;
    for (const auto &kv: m)
        r[kv.second] = K1(kv.first);
    return r;
}

// xenail gcc cant handle enums as map keys
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=60970
static std::unordered_map<int, uint32_t> ActionTable{{seccomp::EAction::Allow, SCMP_ACT_ALLOW},
                                                     {seccomp::EAction::Notify, SCMP_ACT_NOTIFY},
                                                     {seccomp::EAction::Log, SCMP_ACT_LOG},
                                                     {seccomp::EAction::Trap, SCMP_ACT_TRAP},
                                                     {seccomp::EAction::KillProcess, SCMP_ACT_KILL_PROCESS},
                                                     {seccomp::EAction::Kill, SCMP_ACT_KILL}};

static std::unordered_map<uint32_t, seccomp::EAction> ActionRTable =
    reverseMap<int, uint32_t, seccomp::EAction>(ActionTable);

static std::unordered_map<int, int> CompareTable{
    {seccomp::ECompare::Ne, SCMP_CMP_NE}, {seccomp::ECompare::Lt, SCMP_CMP_LT}, {seccomp::ECompare::Le, SCMP_CMP_LE},
    {seccomp::ECompare::Eq, SCMP_CMP_EQ},
    {seccomp::ECompare::Ge, SCMP_CMP_GE},
    {seccomp::ECompare::Gt, SCMP_CMP_GT},
    {seccomp::ECompare::MaskedEq, SCMP_CMP_MASKED_EQ}};

// xenail gcc cant handle enums as map keys
// https://gcc.gnu.org/bugzilla/show_bug.cgi?id=60970
static std::unordered_map<int, seccomp::ECompare> CompareRTable = reverseMap<int, int, seccomp::ECompare>(CompareTable);

static TError resolveAction(const seccomp::TAction &input, uint32_t &action) {
    switch (input.action()) {
    case seccomp::EAction::Errno:
        action = SCMP_ACT_ERRNO(input.value());
        return OK;
    case seccomp::EAction::Trace:
        action = SCMP_ACT_TRACE(input.value());
        return OK;
    default:
        auto it = ActionTable.find(input.action());
        if (it == ActionTable.end())
            return TError(EError::InvalidValue, "unkonwn action {}", input.action());
        action = it->second;
        return OK;
    }
}

static void dumpAction(uint32_t action, seccomp::TAction &output) {
    auto it = ActionRTable.find(action);

    if (it != ActionRTable.end()) {
        output.set_action(it->second);
    } else if ((action & SCMP_ACT_ERRNO(0)) == SCMP_ACT_ERRNO(0)) {
        output.set_action(seccomp::EAction::Errno);
        output.set_value(action ^ SCMP_ACT_ERRNO(0));
    } else if ((action & SCMP_ACT_TRACE(0)) == SCMP_ACT_TRACE(0)) {
        output.set_action(seccomp::EAction::Trace);
        output.set_value(action ^ SCMP_ACT_TRACE(0));
    }
}

void TSeccompProfile::Dump(seccomp::TProfile &profile) const {
    dumpAction(DefaultAction, *profile.mutable_default_action());

    for (auto &rule: Rules) {
        auto &orule = *profile.add_rules();

        orule.set_syscall(rule.SyscallName);
        for (auto &arg: rule.Args) {
            auto &oarg = *orule.add_args();
            oarg.set_index(arg.arg);
            oarg.set_compare(CompareRTable[arg.op]);
            oarg.set_value(arg.datum_a);
        }
        dumpAction(rule.Action, *orule.mutable_action());
    }
}

TError TSeccompProfile::Parse(const seccomp::TProfile &profile) {
    TError error;
    error = resolveAction(profile.default_action(), DefaultAction);
    if (error)
        return error;

    for (const auto &irule: profile.rules()) {
        TSeccompRule rule;

        rule.Syscall = seccomp_syscall_resolve_name(irule.syscall().data());
        if (rule.Syscall < 0)
            return TError(EError::InvalidValue, "unkonwn syscall {}", irule.syscall());
        rule.SyscallName = irule.syscall();

        error = resolveAction(irule.action(), rule.Action);
        if (error)
            return error;

        for (const auto &iarg: irule.args()) {
            auto it = CompareTable.find(iarg.compare());
            if (it == CompareTable.end())
                return TError(EError::InvalidValue, "unkonwn op {}", iarg.compare());
            rule.Args.push_back(SCMP_CMP64(unsigned(iarg.index()), scmp_compare(it->second), iarg.value()));
        }
        Rules.push_back(rule);
    }

    return OK;
}

TSeccompContext::~TSeccompContext() {
    if (Ctx) {
        seccomp_release(Ctx);
        Ctx = nullptr;
    }
}

TError TSeccompContext::Apply(const TSeccompProfile &p) {
    TError error;
    int ret;

    if (Ctx)
        return TError::System("seccomp already applied");

    Ctx = seccomp_init(p.DefaultAction);
    if (!Ctx)
        return TError::System("seccomp_init failed");

    for (auto &rule: p.Rules) {
        ret = seccomp_rule_add_array(Ctx, rule.Action, rule.Syscall, rule.Args.size(), rule.Args.data());
        if (ret < 0) {
            error = TError::System("seccomp_rule_add_array");
            goto err;
        }
    }

    ret = seccomp_load(Ctx);
    if (ret < 0) {
        error = TError::System("seccomp_load failed");
        goto err;
    }

    return OK;

err:
    seccomp_release(Ctx);
    Ctx = nullptr;
    return error;
}
