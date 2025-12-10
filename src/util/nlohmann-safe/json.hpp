#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>

#include "util/error.hpp"

namespace detail {
    struct TJsonImpl;
}

struct TJson {
    TJson();
    TJson(TJson&& other);
    ~TJson();

    TError Parse(const std::string& str);
    TError Parse(std::istream& is);

    TError Get(std::string& res) const;
    TError Get(int& res) const;
    TError Get(uint64_t& res) const;
    TError Get(bool& res) const;
    TError Get(std::vector<std::string>& res) const;
    TError Get(std::unordered_map<std::string, std::unordered_set<std::string>>& res) const;
    TError Get(std::vector<TJson>& res) const;

    TError Dump(std::string& res) const;
    TError Dump(std::ostream& os);

    TError From(const std::unordered_map<std::string, std::unordered_set<std::string>>& obj);

    bool Contains(const char* key) const;
    bool IsArray() const;
    bool IsNull() const;
    TJson operator[](const char* key) const;

private:
    std::unique_ptr<detail::TJsonImpl> impl;

    template <typename T>
    TError ParseImpl(T& input);

    template <typename T>
    TError GetImpl(T& res) const;

    template <typename T>
    TError FromImpl(const T& obj);
};
