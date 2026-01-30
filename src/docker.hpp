#pragma once

/*

Tags path has the following structure:

    /<place>/porto_docker/<storage version>/images/<schema version>/<registry>/<repository>/<image name>/tags/<tag> ->
<digest>

Images path has the following structure:

    /<place>/porto_docker/<storage version>/images/<digest prefix>/<digest>/
        -> manifest.json
        -> config.json
        -> images
        -> layers/
            -> <layer hard link>

Layers path has the following structure:

    /<place>/porto_docker/<storage version>/layers/blobs/<digest prefix>/<digest>/
        -> <digest>.tar.gz
        -> content/
            -> *

*/

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "client.hpp"
#include "storage.hpp"
#include "util/mutex.hpp"
#include "util/path.hpp"

constexpr const char *DOCKER_REGISTRY_HOST = "mirror.gcr.io";
constexpr const char *DOCKER_AUTH_PATH = "https://auth.docker.io/token";
constexpr const char *DOCKER_AUTH_SERVICE = "registry.docker.io";

struct THttpClient;
std::string getDefaultTargetArch();

enum DigestType {
    UnknownDigest,
    SHA256,
};

struct TDockerImage {
#ifdef SELF_TEST
    friend class TDockerImageForTest;
#endif
    std::string Digest;
    std::unordered_map<std::string, std::unordered_set<std::string>> Images;  // image:tags

    std::string Registry;
    std::string Repository;
    std::string Name;
    std::string Tag;
    std::string Platform;

    struct TLayer {
        std::string Digest;
        size_t Size;
        DigestType DigestAlg;

        TLayer(std::string digest, size_t size = 0)
            : Digest(digest),
              Size(size)
        {}

        TLayer(std::string digest, DigestType digestAlg, size_t size = 0)
            : Digest(digest),
              Size(size),
              DigestAlg(digestAlg)
        {}

        TPath LayerPath(const TPath &place) const;
        TPath ArchivePath(const TPath &place) const;

        TError Remove(const TPath &place) const;
    };

    std::vector<TLayer> Layers;

    std::string AuthToken;
    std::string AuthPath;
    std::string AuthService;

    int SchemaVersion = 2;
    std::string Manifest;
    std::string Config;

    uint64_t Size;
    std::vector<std::string> Command;
    std::vector<std::string> Env;

    TDockerImage(const std::string &name, const std::string &platform = "")
        : Registry(DOCKER_REGISTRY_HOST),
          Repository("library"),
          Tag("latest"),
          Platform(platform)
    {
        ParseName(name);
        // in case registry is docker.io request will be redirected to docker.com
        if (Registry == "docker.io")
            Registry = DOCKER_REGISTRY_HOST;
        L_DBG("TDockerImage: Platform {}", Platform);
    }

    TError GetAuthToken();
    static TError InitStorage(const TPath &place, unsigned perms);

    static TError List(const TPath &place, std::vector<TDockerImage> &images, const std::string &mask = "");
    TError Status(const TPath &place);
    TError Pull(const TPath &place);
    TError Remove(const TPath &place);

private:
    inline std::string RepositoryAndName() const {
        if (Repository.empty())
            return Name;
        return Repository + "/" + Name;
    }

    inline std::string FullName(bool hideTag = false) const {
        return fmt::format("{}/{}{}", Registry, RepositoryAndName(), hideTag ? "" : ":" + Tag);
    }

    // this function expects already trimmed digest, i.e. w/o sha256 prefix
    static inline TError checkDigest(const std::string &digest) {
        if (!std::all_of(digest.begin(), digest.end(), ::isxdigit)) {
            return TError("{} not a hexadecimal", digest);
        }
        return OK;
    }

    static inline std::string TrimDigest(const std::string &digest) {
        if (StringStartsWith(digest, "sha256:"))
            return digest.substr(7);
        return digest;
    }

    static inline TError TrimAndCheckDigest(const std::string &digest, std::string &outDigest) {
        std::string trimmedDigest = TrimDigest(digest);
        TError error = checkDigest(trimmedDigest);
        if (error)
            return error;
        outDigest = std::move(trimmedDigest);
        return OK;
    }

    static inline DigestType GetDigestType(const std::string &digest) {
        if (StringStartsWith(digest, "sha256:"))
            return SHA256;
        return UnknownDigest;
    }

    void ParseName(const std::string &name) {
        if (std::all_of(name.begin(), name.end(), ::isxdigit)) {
            Digest = name;
            return;
        }

        std::string image = name;

        // <image> ::= [<registry>/][<repository>/]<name>[:<tag>][@<digest>]
        auto regiPos = image.find('/');
        std::string registry = image.substr(0, regiPos);
        if ((regiPos != std::string::npos) && (StringContainsAny(registry, ".:") || registry == "localhost")) {
            Registry = image.substr(0, regiPos);
            image = image.substr(regiPos + 1);
        }

        // <image> ::= [<repository>/]<name>[:<tag>][@<digest>]
        auto repoPos = image.rfind('/');
        if (repoPos != std::string::npos) {
            Repository = image.substr(0, repoPos);
            image = image.substr(repoPos + 1);
        } else
            Repository = "library";

        // <image> ::= <name>[:<tag>][@<digest>]
        auto digestPos = image.rfind('@');
        if (digestPos != std::string::npos) {
            Digest = TrimDigest(image.substr(digestPos + 1));
            image = image.substr(0, digestPos);
        }

        // <image> ::= <name>[:<tag>]
        auto tagPos = image.find(':');
        if (tagPos != std::string::npos) {
            Name = image.substr(0, tagPos);
            Tag = image.substr(tagPos + 1);
        } else
            Name = image;
    }

    TPath TagPath(const TPath &place) const;
    TPath DigestPath(const TPath &place) const;

    TError DetectImage(const TPath &place);
    TError DetectTagPath(const TPath &place);
    TError DetectDigestPath(const TPath &place);

    std::string ManifestsUrl(const std::string &digest) const;
    std::string BlobsUrl(const std::string &digest) const;

    TError DownloadManifest(const THttpClient &client);
    TError ParseManifest();
    TError DownloadConfig(const THttpClient &client);
    TError ParseConfig();

    // thread function
    static TError DownloadLayer(const TPath &place, const TLayer &layer, TClient *client, const std::string &url,
                                const std::string &token);
    TError DownloadLayers(const TPath &place) const;
    void RemoveLayers(const TPath &place) const;

    TError LinkTag(const TPath &place) const;
    void UnlinkTag(const TPath &place) const;

    TError SaveImages(const TPath &place) const;
    TError SaveImages(const TPath &imagesPath,
                      const std::unordered_map<std::string, std::unordered_set<std::string>> &images) const;
    TError LoadImages(const TPath &place);
    TError LoadImages(const TPath &imagesPath,
                      std::unordered_map<std::string, std::unordered_set<std::string>> &images) const;

    TError Save(const TPath &place) const;
    TError Load(const TPath &place);
    std::string GetPlatform();
};

#ifdef SELF_TEST
class TDockerImageForTest {
    TDockerImage &image;

public:
    TDockerImageForTest(TDockerImage &img)
        : image(img)
    {}
    TError ParseManifest() {
        return image.ParseManifest();
    }
};
#endif
