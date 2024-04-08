#include <vector>
#include <cstdint>

extern "C" {
#include <seccomp.h>
}


struct TSeccompRule {
    int Syscall;
    std::string SyscallName;
    uint32_t Action;
    std::vector<scmp_arg_cmp> Args;
};

struct TSeccompProfile {
    uint32_t DefaultAction = SCMP_ACT_ALLOW;
    std::vector<TSeccompRule> Rules;

    TSeccompProfile() = default;

    TSeccompProfile& operator=(const TSeccompProfile &other) = default;

    TSeccompProfile& operator=(TSeccompProfile &&other) {
        DefaultAction = other.DefaultAction;
        Rules = std::move(other.Rules);
        return *this;
    }

    TError Parse(const seccomp::TProfile& rules);

    void Dump(seccomp::TProfile &profile) const;

    bool Empty() const {
        return DefaultAction == SCMP_ACT_ALLOW && !Rules.size();
    }
};

class TSeccompContext {
    scmp_filter_ctx Ctx = nullptr;

public:
    ~TSeccompContext();

    TError Apply(const TSeccompProfile &profile);
};
