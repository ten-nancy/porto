#include "http.hpp"
#include "util/log.hpp"
#include "util/string.hpp"

#include <regex>

#define CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_CONNECTION_TIMEOUT_SECOND 5
#include "cpp-httplib/httplib.h"

TUri::TUri(const std::string &uri) {
    TError error;

    error = Parse(uri);
    if (error)
        L_ERR("Cannot create TUri object: {}", error);
}

TError TUri::Parse(const std::string &uri) {
    TError error;
    std::string u = uri;
    static std::regex scheme_reg("([a-z0-9\\+\\-\\.]+):.*");

    // [<scheme>:][//[<credentials>@]<host>[:<port>]][/<path>][?<option>[&<option>]][#fragment]
    if (std::regex_match(u, scheme_reg)) {
        auto schemePos = u.find(":");
        if (schemePos != std::string::npos) {
            Scheme = u.substr(0, schemePos);
            u = u.substr(schemePos + 1);
        }
    }

    // [//[<credentials>@]<host>[:<port>]][/<path>][?<option>[&<option>]][#fragment]
    if (StringStartsWith(u, "//")) {
        std::string authority;
        u = u.substr(2);

        // [<credentials>@]<host>[:<port>]][/<path>][?<option>[&<option>]][#fragment]
        auto endPos = u.find_first_of("/?#");
        if (endPos != std::string::npos) {
            authority = u.substr(0, endPos);
            u = u.substr(endPos);
        } else {
            authority = std::move(u);
            u.clear();
        }

        // [<credentials>@]<host>[:<port>]
        auto atPos = authority.find("@");
        if (atPos != std::string::npos) {
            Credentials = authority.substr(0, atPos);
            authority = authority.substr(atPos + 1);
        }

        // <host>[:<port>]
        auto colonPos = authority.rfind(":");
        if (colonPos != std::string::npos) {
            std::string portString = authority.substr(colonPos + 1);
            std::size_t pos;

            Port = std::stoi(portString, &pos);
            if (pos != portString.size())
                error = TError(EError::InvalidValue, "Cannot parse URI '{}': invalid port '{}'", uri, portString);

            authority = authority.substr(0, colonPos);
        }

        // <host>
        Host = authority;
    }

    // [/<path>][?<option>[&<option>]][#fragment]
    auto latticePos = u.rfind("#");
    if (latticePos != std::string::npos) {
        Fragment = u.substr(latticePos + 1);
        u = u.substr(0, latticePos);
    }

    // [/<path>][?<option>[&<option>]]
    auto questionPos = u.rfind("?");
    if (questionPos != std::string::npos) {
        ParseOptions(u.substr(questionPos + 1));
        u = u.substr(0, questionPos);
    }

    // [/<path>]
    Path = u;

    return error;
}

void TUri::ParseOptions(const std::string &options) {
    std::string opts = options;
    std::string opt;
    // <option>[&<option>]
    while (!opts.empty()) {
        auto ampersandPos = opts.find("&");
        if (ampersandPos != std::string::npos) {
            opt = opts.substr(0, ampersandPos);
            opts = opts.substr(ampersandPos + 1);
        } else {
            opt = opts;
            opts.clear();
        }
        // <key>=<value> or <key>
        auto equalPos = opt.find("=");
        if (equalPos != std::string::npos)
            Options.emplace_back(opt.substr(0, equalPos), opt.substr(equalPos + 1));
        else
            Options.emplace_back(opt, "");
    }
}

std::string TUri::FormatOptions() const {
    std::string options;

    for (auto &opt: Options)
        options += fmt::format("{}{}{}&", opt.first, !opt.second.empty() ? "=" : "", opt.second);
    if (Options.size() > 0)
        options.erase(options.length() - 1);

    return options;
}


struct THttpClient::TImpl {
    TImpl(const std::string &host)
        : Host(host)
        , Client(host)
    {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        Client.enable_server_certificate_verification(false);
#endif
    }

    TError HandleResult(const httplib::Result &res, const std::string &path, std::string &response, const THeaders &headers = {}, const TRequest *request = nullptr) {
        if (!res)
            return TError::System("HTTP request to {} failed: {}", Host + path, res.error());

        if (res->status != 200) {
            if (res->status >= 300 && res->status < 400) {
                auto location = res->get_header_value("Location");
                if (!location.empty()) {
                    if (location[0] == '/')
                        location = Host + location;
                    return SingleRequest(location, response, headers, request);
                }
            }

            auto error = TError::System("HTTP request to {} failed: status {}", Host + path, res->status);
            error.Errno = res->status;
            return error;
        }

        response = res->body;

        return OK;
    }

    std::string Host;
    httplib::Client Client;
};

THttpClient::THttpClient(const std::string &host): Impl(new TImpl(host)) {}
THttpClient::~THttpClient() = default;

TError THttpClient::MakeRequest(const std::string &path, std::string &response, const THeaders &headers, const TRequest *request) const {
    httplib::Headers hdrs(headers.cbegin(), headers.cend());

    L_ACT("Send {} request to {} {}", request ? "POST" : "GET", Impl->Host, path);

    if (request)
        return Impl->HandleResult(Impl->Client.Post(path.c_str(), hdrs, request->Body, request->ContentType), path, response);

    return Impl->HandleResult(Impl->Client.Get(path.c_str(), hdrs), path, response);
}

TError THttpClient::SingleRequest(const std::string &rawUri, std::string &response, const THeaders &headers, const TRequest *request) {
    TUri uri(rawUri);
    return THttpClient::SingleRequest(uri, response, headers, request);
}

TError THttpClient::SingleRequest(const TUri &uri, std::string &response, const THeaders &headers, const TRequest *request) {
    std::string host = uri.Scheme + "://" + uri.Host;
    if (uri.Port > 0)
        host = fmt::format("{}:{}", host, uri.Port);

    std::string path = uri.Path;
    if (uri.Options.size())
        path += "?" + uri.FormatOptions();

    return THttpClient(host).MakeRequest(path, response, headers, request);
}

std::string THttpClient::EncodeBase64(const std::string &text) {
    return httplib::detail::base64_encode(text);
}
