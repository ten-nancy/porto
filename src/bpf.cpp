#include "bpf.hpp"

#include <unistd.h>
#include <fcntl.h>
#include <linux/version.h>


// FIXME: libbpf is broken when building with C++
// https://github.com/libbpf/libbpf/issues/820

extern "C" {
#include <libbpf.h>
}

// FIXME: required linux-headers from 5.8
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 8, 0)
enum bpf_stats_type {
        /* enabled run_time_ns and run_cnt */
        BPF_STATS_RUN_TIME = 0,
};
#endif

#include <bpf.h>

TError TBpfMap::Open(uint32_t id)
{
    if (File.Fd >= 0)
        return TError("map already opened");

    File.SetFd = bpf_map_get_fd_by_id(id);
    if (File.Fd < 0) {
        if (errno == ENOENT)
            return TError(EError::NotFound, "can't get map by id {}", id);
        return TError::System("can't get map by id {}", id);
    }

    return Prepare();
}

TError TBpfMap::Open(const TPath &path)
{
    if (File.Fd >= 0)
        return TError("map already opened");

    if (!path.Exists())
        return TError(EError::NotFound, "path {} does not exists", path.ToString());

    File.SetFd = bpf_obj_get(path.c_str());
    if (File.Fd < 0)
        return TError::System("failed to get map by path {}", path);

    return Prepare();
}

TError TBpfMap::GetInfo(struct bpf_map_info &info)
{
    uint32_t length = sizeof(struct bpf_map_info);

    if (bpf_obj_get_info_by_fd(File.Fd, &info, &length))
        return TError::System("failed to get map info");

    return OK;
}

TError TBpfMap::Prepare()
{
    struct bpf_map_info info = {};
    TError error;

    error = GetInfo(info);
    if (error) {
        File.Close();
        return error;
    }

    Id = info.id;
    Name = info.name;
    Type = info.type;
    KeySize = info.key_size;
    ValueSize = info.value_size;
    MaxEntries = info.max_entries;
    Flags = info.map_flags;

    return OK;
}

TError TBpfMap::Set(TBpfMap::TBufferView key, TBpfMap::TBufferView value)
{
    if (File.Fd < 0)
        return TError("trying to update map which was not opened yet");

    if (key.second != KeySize)
        return TError("key size mismatch: provided {}, expected {}", key.second, KeySize);

    if (value.second != ValueSize)
        return TError("value size mismatch: provided {} expected {}", value.second, ValueSize);

    if (bpf_map_update_elem(File.Fd, key.first, value.first, BPF_ANY))
        return TError::System("bpf_map_update_elem() failed");

    return OK;
}

TError TBpfMap::Get(TBpfMap::TBufferView key, TBpfMap::TMutableBufferView value)
{
    if (File.Fd < 0)
        return TError("trying to lookup in map which was not opened yet");

    if (key.second != KeySize)
        return TError("key size mismatch: provided {}, expected {}", key.second, KeySize);

    if (value.second != ValueSize)
        return TError("value size mismatch: provided {} expected {}", value.second, ValueSize);

    if (bpf_map_lookup_elem(File.Fd, key.first, value.first)) {
        if (errno == ENOENT)
            return TError(EError::NotFound, "bad lookup - element does not exists");
        return TError::System("failed to lookup element");
    }

    return OK;
}

TError TBpfMap::Del(TBpfMap::TBufferView key)
{
    if (File.Fd < 0)
        return TError("trying to delete element in map which was not opened yet");

    if (key.second != KeySize)
        return TError("key size mismatch: provided {}, expected {}", key.second, KeySize);

    if (bpf_map_delete_elem(File.Fd, key.first))
        return TError::System("failed to delete element");

    return OK;
}

TError TBpfProgram::Open(const std::string &prog_name, const std::vector<uint8_t> &prog_code)
{
    if (File.Fd >= 0)
        return TError("program already opened");

    struct bpf_object *obj = bpf_object__open_mem(prog_code.data(), prog_code.size(), NULL);

    struct bpf_program *prog = bpf_object__find_program_by_name(obj, prog_name.c_str());
    if (!prog) {
        bpf_object__close(obj);
        return TError(EError::Unknown, "Failed to load program '{}' -- failed to find bpf program in the loaded object", prog_name);
    }

    bpf_program__set_type(prog, BPF_PROG_TYPE_SCHED_CLS);

    if (bpf_object__load(obj)) {
        bpf_object__close(obj);
        return TError(EError::Unknown, "Failed to load program '{}' -- failed to load bpf object", prog_name);
    }

    int fd = bpf_program__fd(prog);
    int nfd = dup(fd);
    fcntl(nfd, F_SETFD, FD_CLOEXEC);
    bpf_object__close(obj);

    if (nfd < 0) {
        return TError(EError::Unknown, "Failed to load program '{}' -- failed to obtain bpf program file descriptor", prog_name);
    }

    File.SetFd = nfd;
    return Prepare();
}

TError TBpfProgram::Open(uint32_t id)
{
    if (File.Fd >= 0)
        return TError("program already opened");

    File.SetFd = bpf_prog_get_fd_by_id(id);
    if (File.Fd < 0) {
        if (errno == ENOENT)
            return TError(EError::NotFound, "can't get prog by id {}", id);
        return TError::System("can't get prog by id {}", id);
    }

    return Prepare();
}

TError TBpfProgram::Open(const TPath &path)
{
    if (File.Fd >= 0)
        return TError("program already opened");

    if (!path.Exists())
        return TError(EError::NotFound, "path {} does not exists", path.ToString());

    File.SetFd = bpf_obj_get(path.c_str());
    if (File.Fd < 0)
        return TError::System("failed to get prog by path {}", path);

    return Prepare();
}

TError TBpfProgram::Prepare()
{
    struct bpf_prog_info info = {};
    TError error;

    error = GetInfo(info);
    if (error) {
        File.Close();
        return error;
    }

    Id = info.id;
    Type = info.type;
    Name = info.name;
    static_assert(std::tuple_size<decltype(TBpfProgram::Tag)>::value == BPF_TAG_SIZE, "bpf tag size mismatch");
    memcpy(Tag.data(), info.tag, Tag.size());

    return OK;
}

TError TBpfProgram::GetInfo(struct bpf_prog_info &info)
{
    uint32_t length = sizeof(struct bpf_prog_info);

    if (bpf_obj_get_info_by_fd(File.Fd, &info, &length))
        return TError::System("failed to get prog info");

    return OK;
}

TError TBpfProgram::GetMap(const std::string &name, TBpfMap &map)
{
    TError error;
    std::vector<TBpfMap> maps;

    error = GetMaps(maps);
    if (error)
        return error;

    for (auto& m : maps) {
        if (m.Name == name) {
            map = std::move(m);
            return OK;
        }
    }

    return TError("failed to get map with name {} in prog {}", name, Id);
}

TError TBpfProgram::GetMaps(std::vector<TBpfMap> &maps)
{
    TError error;

    if (File.Fd < 0)
        return TError("failed to get maps due to program is closed");

    struct bpf_prog_info info = {};
    error = GetInfo(info);
    if (error)
        return error;

    if (info.nr_map_ids) {
        std::vector<uint32_t> mapIds(info.nr_map_ids);
        struct bpf_prog_info tmp = {};

        tmp.nr_map_ids = info.nr_map_ids;
        tmp.map_ids = (uint64_t)mapIds.data();

        error = GetInfo(tmp);
        if (error)
            return error;

        for (uint32_t id : mapIds) {
            TBpfMap map;
            TError error = map.Open(id);
            if (error)
                return error;
            maps.push_back(std::move(map));
        }
    }

    return OK;
}
