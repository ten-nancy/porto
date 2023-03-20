#pragma once

#include <array>
#include <vector>
#include <string>
#include <utility>
#include <type_traits>

#include "common.hpp"
#include "util/error.hpp"
#include "util/path.hpp"

struct bpf_prog_info;
struct bpf_map_info;

class TBpfObject : public TNonCopyable {
public:
    uint32_t Id;
    TFile File;

    TBpfObject() : Id(0) { }

    virtual ~TBpfObject() = default;

    TBpfObject(TBpfObject&& other) noexcept {
        Id = other.Id;
        File.Close();
        File.SetFd = other.File.Fd;
        other.Id = 0;
        other.File.SetFd = -1;
    }

    TBpfObject& operator=(TBpfObject&& other) noexcept {
        Id = other.Id;
        File.Close();
        File.SetFd = other.File.Fd;
        other.Id = 0;
        other.File.SetFd = -1;
        return *this;
    }
};

class TBpfMap : public TBpfObject {
public:
    using TBufferView = std::pair<const void *, const uint32_t>;
    using TMutableBufferView = std::pair<void *, const uint32_t>;

    std::string Name;
    uint32_t Type;
    uint32_t KeySize;
    uint32_t ValueSize;
    uint32_t MaxEntries;
    uint32_t Flags;

    TBpfMap() = default;

    TError Open(uint32_t id);
    TError Open(const TPath &path);

    TError GetInfo(struct bpf_map_info &info);

    TError Set(TBufferView key, TBufferView value);
    TError Get(TBufferView key, TMutableBufferView value);
    TError Del(TBufferView key);

    template<typename TKey, typename TValue>
    TError Set(const TKey &key, const TValue &value) {
        static_assert(std::is_standard_layout<TKey>::value, "Key must be a standard-layout type");
        static_assert(std::is_standard_layout<TValue>::value, "Value must be a standard-layout type");
        TBufferView k(reinterpret_cast<const void *>(&key), sizeof(TKey));
        TBufferView v(reinterpret_cast<const void *>(&value), sizeof(TValue));
        return Set(k, v);
    }

    template<typename TKey, typename TValue>
    TError Get(const TKey &key, TValue &value) {
        static_assert(std::is_standard_layout<TKey>::value, "Key must be a standard-layout type");
        static_assert(std::is_standard_layout<TValue>::value, "Value must be a standard-layout type");
        TBufferView k(reinterpret_cast<const void *>(&key), sizeof(TKey));
        TMutableBufferView v(reinterpret_cast<void *>(&value), sizeof(TValue));
        return Get(k, v);
    }

    template<typename TKey>
    TError Del(const TKey &key) {
        static_assert(std::is_standard_layout<TKey>::value, "Key must be a standard-layout type");
        TBufferView k(reinterpret_cast<const void *>(&key), sizeof(TKey));
        return Del(k);
    }

private:
    TError Prepare();
};

class TBpfProgram : public TBpfObject {
public:
    std::string Name;
    std::array<uint8_t, 8> Tag;
    uint32_t Type;

    TError Open(uint32_t id);
    TError Open(const TPath &path);

    TError GetInfo(struct bpf_prog_info &info);

    TError GetMap(const std::string &name, TBpfMap &map);
    TError GetMaps(std::vector<TBpfMap> &maps);

private:
    TError Prepare();
};

