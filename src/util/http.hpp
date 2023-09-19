#pragma once

#include <memory>

#include "error.hpp"

struct TUri {
    std::string Scheme;
    std::string Host;
    int Port = -1;
    std::string Path;
    std::string Credentials;
    std::vector<std::pair<std::string, std::string>> Options;
    std::string Fragment;

    TUri() = default;
    TUri(const std::string &uri);
    ~TUri() = default;

    TError Parse(const std::string &uri);
    void ParseOptions(const std::string &options);
    std::string FormatOptions() const;
};

struct THttpClient {
    THttpClient(const std::string &host);
    ~THttpClient();

    using THeader = std::pair<std::string, std::string>;
    using THeaders = std::vector<THeader>;

    struct TRequest {
        std::string Body;
        const char *ContentType;
    };

    TError MakeRequest(const std::string &path, std::string &response, const THeaders &headers = {}, const TRequest *request = nullptr) const;
    static TError SingleRequest(const std::string &rawUri, std::string &response, const THeaders &headers = {}, const TRequest *request = nullptr);
    static TError SingleRequest(const TUri &uri, std::string &response, const THeaders &headers = {}, const TRequest *request = nullptr);
    static std::string EncodeBase64(const std::string &text);

private:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
};
