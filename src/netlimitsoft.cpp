#include "netlimitsoft.hpp"
#include "util/log.hpp"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <libbpf.h>


// FIXME: required linux-headers from 5.8
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
enum bpf_stats_type {
        /* enabled run_time_ns and run_cnt */
        BPF_STATS_RUN_TIME = 0,
};
#endif

#define p_err(fmt, ...) fprintf(stderr, fmt "\n",  __VA_ARGS__)

#include <bpf.h>

class TNetLimitSoft::TImpl {
public:
    std::string ProgCode;
    struct bpf_object *BpfObject;
};

class TNetLimitSoftOfNet::TImpl {
public:
    std::string ProgCode;
};


TNetLimitSoft::TNetLimitSoft() :
    Impl(new TImpl())
{
    Impl->BpfObject = nullptr;
}


bool TNetLimitSoft::IsDisabled() {
    return (Impl->BpfObject == nullptr);
}

TError TNetLimitSoft::Setup(const std::string &bpf_program_elf_path) {
    PORTO_ASSERT(!Impl->BpfObject);

    TError error;

    if (bpf_program_elf_path.empty())
        return OK;

    error = TPath(bpf_program_elf_path).ReadAll(Impl->ProgCode);
    if (error)
        return TError(EError::Unknown, "Failed to read network soft limit bpf object file in TNetLimitSoft::Setup() -- {}", error);

    struct bpf_object *obj = bpf_object__open_mem(Impl->ProgCode.data(), Impl->ProgCode.size(), NULL);
    if (!obj)
        return TError(EError::Unknown, "Failed to load network soft limit bpf object file in TNetLimitSoft::Setup() -- {}", strerror(errno));

    struct bpf_program *prog = bpf_object__find_program_by_name(obj, "netlimit_soft");
    if (!prog)
        return TError(EError::Unknown, "Object file does not contain 'netlimit_soft' bpf program in TNetLimitSoft::Setup()");

    bpf_program__set_autoload(prog, false);

    if (bpf_object__load(obj)) // this will also create and pin maps
        return TError(EError::Unknown, "Failed to setup network soft limit bpf maps in TNetLimitSoft::Setup() -- {}", strerror(errno));

    Impl->BpfObject = obj;
    return OK;
}


TError TNetLimitSoft::SetupNetLimitSoftOfNet(TNetLimitSoftOfNet &netlimit) {
    return netlimit.Setup(Impl->ProgCode);
}

bool TNetLimitSoftOfNet::IsDisabled() {
    return Impl->ProgCode.empty();
}

TError TNetLimitSoftOfNet::Setup(const std::string &prog_code) {
    Impl->ProgCode.clear();
    Impl->ProgCode.append(prog_code);
    return OK;
}


TNetLimitSoft::~TNetLimitSoft() {
    if (Impl->BpfObject)
        bpf_object__close(Impl->BpfObject);
}


TNetLimitSoftOfNet::TNetLimitSoftOfNet()
    : Impl(new TImpl())
{
}


TNetLimitSoftOfNet::~TNetLimitSoftOfNet() {
}


static TError SetupNetMap(struct bpf_object *obj, uint64_t key, uint32_t rate_in_kb_s);

TError TNetLimitSoft::SetupNet(uint64_t key, uint32_t rate_in_kb_s) {
    struct bpf_object *obj = Impl->BpfObject;

    TError err = SetupNetMap(obj, key, rate_in_kb_s);
    if (err)
        return err;

    return OK;
}

static TError SetupNetMap(struct bpf_object *obj, uint64_t key, uint32_t rate_in_kb_s) {
    if (!obj)
        return TError(EError::Unknown, "Failed to set network soft limit in TNetLimitSoft::SetupNetMap({}, {}) -- failed to load bpf object previously", key, rate_in_kb_s);

    struct bpf_map *map = bpf_object__find_map_by_name(obj, "netlimit_soft_m");
    if (!map)
        return TError(EError::Unknown, "Failed to set network soft limit in TNetLimitSoft::SetupNetMap({}, {}) -- no bpf map 'netlimit_soft_m'", key, rate_in_kb_s);

    struct netlimit_param {
        uint64_t lasttime;     /* In ns */
        uint32_t rate;         /* In bytes per NS << 20 */
        uint32_t padding;
        uint64_t continuously_dropped; // packets
    } value = {0, rate_in_kb_s, 0, 0};

    int err = bpf_map_update_elem(bpf_map__fd(map), &key, &value, BPF_ANY);

    if (err)
        return TError(EError::Unknown, "Failed to set network soft limit in TNetLimitSoft::SetupNetMap({}, {}) -- can not write to bpf map 'netlimit_soft_m'", key, rate_in_kb_s);

    return OK;
}



TError TNetLimitSoftOfNet::BakeBpfProgCode(uint64_t key, std::vector<uint8_t> &prog_code) {
    const int MAGIC_SIZE = 12;
    const char *magic = "\xbe\xba\xfe\xca\x00\x00\x00\x00\xef\xbe\xad\xde";

    prog_code.assign(Impl->ProgCode.begin(), Impl->ProgCode.end());
    uint8_t *bytes = prog_code.data();

    int replaces = 0;
    for(;;) {
        uint32_t *p = (uint32_t *)memmem(bytes, prog_code.size(), magic, MAGIC_SIZE);
        if (!p)
            break;

        p[0] = key & 0xffffffff;
        p[2] = (key >> 32) & 0xffffffff;
        replaces++;
    }

    if (replaces != 2)
        return TError(EError::Unknown, "Failed to set network soft limit in TNetLimitSoftOfNet::BakeBpfProgCode({}) -- there should be exactly two places of 0xDEADBEEFCAFEBABE to be replaced with key but there are {}", key, replaces);

    return OK;
}

