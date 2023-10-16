#include "json.hpp"
#include "util/nlohmann/json.hpp"

using json = nlohmann::json;

template <typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

namespace detail {
    struct TJsonImpl {
        TJsonImpl() = default;
        TJsonImpl(json json): Json(std::move(json)) {}

        json Json;
    };
}

TJson::TJson(): impl(make_unique<detail::TJsonImpl>()) {}
TJson::TJson(TJson&& other): impl(std::move(other.impl)) {}
TJson::~TJson() {}

template <typename T>
TError TJson::ParseImpl(T& src) {
    try {
        impl->Json = json::parse(src);
        return OK;
    } catch (const std::exception& e) {
        return TError("Failed to parse: {}", e.what());
    }
}

TError TJson::Parse(const std::string& str) {
    return ParseImpl(str);
}

TError TJson::Parse(std::istream& is) {
    return ParseImpl(is);
}

template <typename T>
TError TJson::GetImpl(T& res) const {
    try {
        res = impl->Json.get<T>();
        return OK;
    } catch (const std::exception& e) {
        return TError("Error while getting value: {}", e.what());
    }
}

TError TJson::Get(std::string& res) const {
    return GetImpl(res);
}

TError TJson::Get(int& res) const {
    return GetImpl(res);
}

TError TJson::Get(uint64_t& res) const {
    return GetImpl(res);
}

TError TJson::Get(std::vector<std::string>& res) const {
    return GetImpl(res);
}

TError TJson::Get(std::unordered_map<std::string, std::unordered_set<std::string>>& res) const {
    return GetImpl(res);
}

TError TJson::Get(std::vector<TJson>& res) const {
    try {
        res.clear();
        for (const auto& e: impl->Json) {
            res.emplace_back();
            res.back().impl = make_unique<detail::TJsonImpl>(e);
        }
        return OK;
    } catch (const std::exception& e) {
        return TError("Failed to get array: {}", e.what());
    }
}

TError TJson::Dump(std::string& res) const {
    try {
        res = impl->Json.dump();
        return OK;
    } catch (const std::exception& e) {
        return TError("Failed to dump into string: {}", e.what());
    }
}

TError TJson::Dump(std::ostream& os) {
    try {
        os << impl->Json;
        return OK;
    } catch (const std::exception& e) {
        return TError("Failed to dump into file: {}", e.what());
    }
}

template <typename T>
TError TJson::FromImpl(const T& obj) {
    try {
        impl = make_unique<detail::TJsonImpl>(obj);
        return OK;
    } catch (std::exception& e) {
        return TError("Failed to construct from object: {}", e.what());
    }
}

TError TJson::From(const std::unordered_map<std::string, std::unordered_set<std::string>>& obj) {
    return FromImpl(obj);
}

bool TJson::Contains(const char* key) const {
    return impl->Json.contains(key);
}

bool TJson::IsArray() const {
    return impl->Json.is_array();
}

bool TJson::IsNull() const {
    return impl->Json.is_null();
}

TJson TJson::operator[](const char* key) const {
    TJson res;
    res.impl = make_unique<detail::TJsonImpl>(impl->Json[key]);
    return res;
}
