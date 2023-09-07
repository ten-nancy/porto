#include "util/nlohmann/json.hpp"
#include "util/http.hpp"
#include "util/log.hpp"
#include "helpers.hpp"
#include "common.hpp"
#include "storage.hpp"
#include "docker.hpp"
#include "util/string.hpp"

#include <fcntl.h>
#include <fstream>
#include <unordered_set>
#include <thread>

constexpr const char *DOCKER_IMAGES_FILE = "images.json";
constexpr const char *DOCKER_LAYERS_DIR = "layers";

using json = nlohmann::json;

TPath TDockerImage::TLayer::LayerPath(const TPath &place) const {
    return place / PORTO_DOCKER_LAYERS / "blobs" / Digest.substr(0, 2) / Digest;
}

TPath TDockerImage::TLayer::ArchivePath(const TPath &place) const {
    return LayerPath(place) / (Digest + ".tar.gz");
}

TError TDockerImage::TLayer::Remove(const TPath &place) const {
    TError error;
    TPath archivePath = ArchivePath(place);
    TStorage portoLayer;
    struct stat st;

    // refcount check
    if (!archivePath.Exists())
        return TError(EError::Docker, "Path {} doesn't exist", archivePath);

    error = archivePath.StatFollow(st);
    if (error)
        return error;

    if (st.st_nlink > 1)
        return OK;

    // layer removing
    error = archivePath.Unlink();
    if (error)
        return error;

    error = portoLayer.Resolve(EStorageType::DockerLayer, place, Digest);
    if (error)
        return error;

    if (!portoLayer.Exists())
        return TError(EError::Docker, "Path {} doesn't exist", portoLayer.Path);

    // we must not remove layers for image asynchronously
    error = portoLayer.Remove(false, false);
    if (error)
        return error;

    error = LayerPath(place).ClearEmptyDirectories(place / PORTO_DOCKER_LAYERS);
    if (error)
        return error;

    return OK;
}


TError TDockerImage::GetAuthToken() {
    TError error;
    std::string response;

    if (Registry != DOCKER_REGISTRY_HOST && AuthPath.empty() && AuthService.empty())
        return OK;

    std::string authService(DOCKER_AUTH_SERVICE);
    std::string authPath(DOCKER_AUTH_PATH);
    TUri uri;

    if (!AuthService.empty())
        authService = AuthService;

    if (!AuthPath.empty())
        authPath = AuthPath;

    if (authPath.find("://") == std::string::npos)
        authPath = "https://" + authPath;

    uri.Parse(authPath);

    if (!AuthPath.empty() && AuthService.empty())
        authService = uri.Host;

    THttpClient::THeaders headers;
    if (!uri.Credentials.empty())
        headers.emplace_back("Authorization", "Basic " + THttpClient::EncodeBase64(uri.Credentials));

    uri.Options.emplace_back("service", authService);
    uri.Options.emplace_back("scope", fmt::format("repository:{}:pull", RepositoryAndName()));

    error = THttpClient::SingleRequest(uri, response, headers);
    if (error) {
        L_WRN("Failed to get token from auth service: {}", error);
        return error;
    }

    auto responseJson = json::parse(response);
    AuthToken = "Bearer " + responseJson["token"].get<std::string>();

    return OK;
}

TPath TDockerImage::TagPath(const TPath &place) const {
    return place / PORTO_DOCKER_TAGS / fmt::format("v{}", SchemaVersion) / Registry / RepositoryAndName() / Tag;
}

TPath TDockerImage::DigestPath(const TPath &place) const {
    return !Digest.empty() ? place / PORTO_DOCKER_IMAGES / Digest.substr(0, 2) / Digest : TPath();
}

TError TDockerImage::DetectImage(const TPath &place) {
    TError error;

    if (Digest.empty()) {
        error = DetectTagPath(place);
        if (error)
            return error;

        // try to resolve tag symlink
        TPath tagPath = TagPath(place);
        TPath digestPath = tagPath.RealPath();
        if (digestPath == tagPath)
            return TError(EError::Docker, "Detected tag symlink is broken: {}", tagPath);

        Digest = digestPath.BaseName();

    } else {
        error = DetectDigestPath(place);
        if (error)
            return error;
    }

    return OK;
}

TError TDockerImage::DetectTagPath(const TPath &place) {
    TPath tagPath = TagPath(place);
    if (!tagPath.PathExists()) {
        SchemaVersion = 1;
        tagPath = TagPath(place);
        if (!tagPath.PathExists()) {
            if (Repository == "library") {
                // try to load empty repository
                Repository = "";
                SchemaVersion = 2;
                tagPath = TagPath(place);
                if (!tagPath.PathExists()) {
                    SchemaVersion = 1;
                    tagPath = TagPath(place);
                    if (!tagPath.PathExists()) {
                        return TError(EError::DockerImageNotFound, FullName());
                    }
                }
            } else
                return TError(EError::DockerImageNotFound, FullName());
        }
    }

    return OK;
}

TError TDockerImage::DetectDigestPath(const TPath &place) {
    if (Digest.length() < 2)
        return TError(EError::Docker, "Too short digest prefix {}", Digest);

    TPath path = place / PORTO_DOCKER_IMAGES / Digest.substr(0, 2);
    if (!path.Exists())
        return TError(EError::DockerImageNotFound, Digest);

    if ((path / Digest).Exists())
        return OK;

    std::vector<std::string> digests;
    TError error = path.ListSubdirs(digests);
    if (error)
        return error;

    unsigned int matchCount = 0;
    std::string prefix = Digest;
    for (const auto& digest: digests)
        if (StringStartsWith(digest, prefix)) {
            Digest = digest;
            matchCount++;
        }

    if (matchCount > 1)
        return TError(EError::Docker, "Too many digests matched {}", prefix);
    else if (matchCount < 1)
        return TError(EError::DockerImageNotFound, Digest);

    return OK;
}

std::string TDockerImage::ManifestsUrl(const std::string &digest) const {
    return fmt::format("/v2/{}/manifests/{}", RepositoryAndName(), digest);
}

std::string TDockerImage::BlobsUrl(const std::string &digest) const {
    return fmt::format("/v2/{}/blobs/sha256:{}", RepositoryAndName(), digest);
}

TError TDockerImage::DownloadManifest(const THttpClient &client) {
    TError error;
    bool tokenSpecified = !AuthToken.empty();

    if (!tokenSpecified) {
        error = GetAuthToken();
        if (error)
            return error;
    }

    THttpClient::THeaders headers = {
        { "Accept", "application/vnd.docker.distribution.manifest.v2+json" },
        { "Accept", "application/vnd.docker.distribution.manifest.list.v2+json" },
        { "Accept", "application/vnd.docker.distribution.manifest.v1+json" },
    };

    if (!AuthToken.empty())
        headers.emplace_back("Authorization", AuthToken);

    std::string manifests;
    error = client.MakeRequest(ManifestsUrl(Tag), manifests, headers);
    if (error) {
        L_WRN("Failed to download manifests (1): {}", error);

        if (Repository == "library" && error.Errno == 404) {
            // retry if repository is default and we received code 404
            Repository = "";
            if (!tokenSpecified) {
                error = GetAuthToken();
                if (error)
                    return error;

                if (!AuthToken.empty()) {
                    auto it = find_if(headers.begin(),
                                      headers.end(),
                                      [](const THttpClient::THeader &h) { return h.first == "Authorization"; });
                    if (it != headers.end())
                        it->second = AuthToken;
                    else
                        headers.emplace_back("Authorization", AuthToken);
                }
            }

            error = client.MakeRequest(ManifestsUrl(Tag), manifests, headers);
            if (error) {
                L_WRN("Failed to download manifests (2): {}", error);
                return error;
            }

        } else
            return error;
    }

    auto manifestJson = json::parse(manifests);
    if (!manifestJson.contains("schemaVersion"))
        return TError(EError::Docker, "schemaVersion is not found in manifest");

    SchemaVersion = manifestJson["schemaVersion"].get<int>();
    if (SchemaVersion == 1) {
        Manifest = manifests;
    } else if (SchemaVersion == 2) {
        auto mediaType = manifestJson["mediaType"].get<std::string>();
        if (mediaType == "application/vnd.docker.distribution.manifest.v2+json") {
            Manifest = manifests;
        } else if (mediaType == "application/vnd.docker.distribution.manifest.list.v2+json") {
            const std::string targetArch = "amd64";
            const std::string targetOs = "linux";
            bool found = false;
            for (const auto &m: manifestJson["manifests"]) {
                if (!m.contains("platform"))
                    continue;
                auto p = m["platform"];
                if (!p.contains("architecture") || !p.contains("os"))
                    continue;
                if (p["architecture"] == targetArch && p["os"] == targetOs) {
                    found = true;
                    auto digest = m["digest"].get<std::string>();
                    error = client.MakeRequest(ManifestsUrl(digest), Manifest, headers);
                    if (error) {
                        L_WRN("Failed to download manifest: {}", error);
                        return error;
                    }
                }
            }
            if (!found)
                return TError(EError::Docker, "Manifest for arch {} and os {} is not found", targetArch, targetOs);
        } else
            return TError(EError::Docker, "Unknown manifest mediaType: {}", mediaType);
    } else
        return TError(EError::Docker, "Unknown manifest schemaVersion: {}", SchemaVersion);

    return OK;
}

TError TDockerImage::ParseManifest() {
    auto manifestJson = json::parse(Manifest);
    if (!manifestJson.contains("schemaVersion"))
        return TError(EError::Docker, "schemaVersion is not found in manifest");

    if (SchemaVersion != manifestJson["schemaVersion"].get<int>())
        return TError(EError::Docker, "schemaVersions are not equal");

    if (SchemaVersion == 1) {
        // there is no size info
        Size = 1;
        auto history = manifestJson["history"];
        if (history.is_null())
            return TError(EError::Docker, "history is empty in manifest");

        for (const auto &h: history) {
            if (h.contains("v1Compatibility")) {
                auto c = json::parse(h["v1Compatibility"].get<std::string>());
                Digest = c["id"].get<std::string>();
                Config = c.dump();
                break;
            }
        }

        for (const auto &layer: manifestJson["fsLayers"]) {
            Layers.emplace_back(TrimDigest(layer["blobSum"]));
        }
    } else if (SchemaVersion == 2) {
        Digest = TrimDigest(manifestJson["config"]["digest"].get<std::string>());
        Size = manifestJson["config"]["size"].get<uint64_t>();

        for (const auto &layer: manifestJson["layers"]) {
            auto mediaType = layer["mediaType"].get<std::string>();
            if (mediaType != "application/vnd.docker.image.rootfs.diff.tar.gzip")
                return TError(EError::Docker, "Unknown layer mediaType: {}", mediaType);

            Layers.emplace_back(TrimDigest(layer["digest"]), layer["size"]);
            Size += layer["size"].get<uint64_t>();
        }
    } else
        return TError(EError::Docker, "Unknown manifest schemaVersion: {}", SchemaVersion);

    return OK;
}

TError TDockerImage::DownloadConfig(const THttpClient &client) {
    if (SchemaVersion == 1)
        return OK;

    TError error;
    THttpClient::THeaders headers;

    if (!AuthToken.empty())
        headers.emplace_back("Authorization", AuthToken);

    error = client.MakeRequest(BlobsUrl(Digest), Config, headers);
    if (error) {
        L_WRN("Failed to download config: {}", error);
        return error;
    }

    return OK;
}

TError TDockerImage::ParseConfig() {
    auto configJson = json::parse(Config);
    auto config = configJson["config"];
    auto entrypoint = config["Entrypoint"];
    auto cmd = config["Cmd"];

    if (!entrypoint.is_null()) {
        for (const auto &c: entrypoint)
            Command.emplace_back(c);
        if (!cmd.is_null()) {
            for (const auto &c: cmd)
                Command.emplace_back(c);
        }
    } else {
        Command.emplace_back("/bin/sh");
        if (!cmd.is_null()) {
            Command.emplace_back("-c");
            Command.emplace_back(MergeWithQuotes(cmd, ' ', '\''));
        }
    }

    auto env = config["Env"];
    if (!env.is_null()) {
        for (const auto &e: env)
            Env.emplace_back(e);
    }

    return OK;
}

void TDockerImage::DownloadLayer(const TPath &place, const TLayer &layer, TClient *client, const std::string &url, const std::string &token) {
    TError error;
    TPath archivePath = layer.ArchivePath(place);

    CL = client;

    error = archivePath.DirName().MkdirAll(0755);
    if (error && error.Errno != EEXIST)
        L_ERR("Cannot create directory {}: {}", archivePath.DirName(), error);

    auto layerLock = TFileMutex::MakeDirLock(archivePath.DirName());

    if (archivePath.Exists()) {
        struct stat st;
        error = archivePath.StatStrict(st);
        if (error) {
            L_ERR("Cannot stat archive path: {}", error);
            return;
        }

        if ((size_t)st.st_size == layer.Size)
            return;

        (void)layer.Remove(place);
    }

    error = DownloadFile(url, archivePath);
    if (error && !token.empty()) {
        // retry if registry api expects to receive token and we received code 401
        error = DownloadFile(url, archivePath, { "Authorization: " + token });
        if (error) {
            L_ERR("Cannot download layer: {}", error);
            return;
        }
    }

    TStorage portoLayer;

    error = portoLayer.Resolve(EStorageType::DockerLayer, place, layer.Digest);
    if (error) {
        L_ERR("Cannot resolve layer storage: {}", error);
        return;
    }


    error = portoLayer.ImportArchive(archivePath, PORTO_HELPERS_CGROUP);
    if (error) {
        L_ERR("Cannot import archive: {}", error);
        return;
    }


    error = TStorage::SanitizeLayer(portoLayer.Path);
    if (error) {
        L_ERR("Cannot sanitize layer: {}", error);
        return;
    }
}

TError TDockerImage::DownloadLayers(const TPath &place) const {
    std::vector<std::thread> threads;

    // download layers using threads
    for (const auto &layer: Layers)
        threads.emplace_back(TDockerImage::DownloadLayer, place, layer, std::ref(CL), fmt::format("https://{}{}", Registry, BlobsUrl(layer.Digest)), AuthToken);

    // waiting all downloads
    for (auto &t: threads)
        t.join();

    return OK;
}

void TDockerImage::RemoveLayers(const TPath &place) const {
    TError error;

    for (const auto &layer: Layers) {
        auto layerLock = TFileMutex::MakeDirLock(layer.LayerPath(place));
        error = layer.Remove(place);
        if (error)
            L_ERR("Cannot remove layer: {}", error);
    }
}

TError TDockerImage::LinkTag(const TPath &place) const {
    TError error;
    TPath digestPath = DigestPath(place);
    TPath tagPath = TagPath(place);

    error = tagPath.DirName().MkdirAll(0755);
    if (error && error.Errno != EEXIST)
        L_ERR("Cannot create directory {}: {}", tagPath.DirName(), error);

    auto tagLock = TFileMutex::MakeDirLock(tagPath.DirName());

    error = tagPath.Symlink(digestPath);
    if (error) {
        if (error.Errno != EEXIST)
            return error;

        // skip if symlink to the same digest path exists
        if (tagPath.RealPath() == digestPath) {
            L_WRN("Reuse tag path symlink: {} to {}", tagPath, digestPath);
            return OK;
        }

        // load tags of current digest
        std::unordered_map<std::string, std::unordered_set<std::string>> images;
        std::string name = FullName(true);
        auto digestLock = TFileMutex::MakeDirLock(tagPath.RealPath());
        error = LoadImages(tagPath.RealPath() / DOCKER_IMAGES_FILE, images);
        if (error)
            return error;

        if (images.find(name) != images.end() && images[name].find(Tag) != images[name].end()) {
            if (images.size() <= 1 && images[name].size() <= 1) {
                // remove current digest
                error = tagPath.RealPath().RemoveAll();
                if (error)
                    return error;

                error = tagPath.RealPath().DirName().ClearEmptyDirectories(place / PORTO_DOCKER_IMAGES);
                if (error)
                    return error;
            } else {
                // delete tag from current digest
                images[name].erase(Tag);
                error = SaveImages(tagPath.RealPath() / DOCKER_IMAGES_FILE, images);
                if (error)
                    return error;
            }
        } // else ignore symlink

        // clean current tag
        error = tagPath.Unlink();
        if (error)
            return error;

        // attempt to recreate
        error = tagPath.Symlink(digestPath);
        if (error)
            return error;
    }

    return OK;
}

void TDockerImage::UnlinkTag(const TPath &place) const {
    TError error;
    TPath tagPath = TagPath(place);

    auto tagLock = TFileMutex::MakeDirLock(tagPath.DirName());

    error = tagPath.Unlink();
    if (error)
        L_ERR("Cannot unlink tag: {}", error);

    tagLock.reset();

    error = tagPath.DirName().ClearEmptyDirectories(place / PORTO_DOCKER_TAGS);
    if (error)
        L_ERR("Cannot clear directories: {}", error);
}

TError TDockerImage::SaveImages(const TPath &place) const {
    return SaveImages(DigestPath(place) / DOCKER_IMAGES_FILE, Images);
}

TError TDockerImage::SaveImages(const TPath &imagesPath, const std::unordered_map<std::string, std::unordered_set<std::string>> &images) const {
    TError error;
    std::ofstream file(imagesPath.ToString());

    json imagesJson = images;
    file << imagesJson;

    return OK;
}

TError TDockerImage::LoadImages(const TPath &place) {
    return LoadImages(DigestPath(place) / DOCKER_IMAGES_FILE, Images);
}

TError TDockerImage::LoadImages(const TPath &imagesPath, std::unordered_map<std::string, std::unordered_set<std::string>> &images) const {
    TError error;
    std::ifstream file(imagesPath.ToString());

    if (!imagesPath.Exists())
        return OK;

    json imagesJson = json::parse(file);
    images = imagesJson.get<std::unordered_map<std::string, std::unordered_set<std::string>>>();

    return OK;
}

TError TDockerImage::Save(const TPath &place) const {
    TError error;
    TPath digestPath = DigestPath(place);
    TPath layersPath =  digestPath / DOCKER_LAYERS_DIR;

    error = TPath(digestPath / "manifest.json").CreateAndWriteAll(Manifest);
    if (error)
        return error;

    error = TPath(digestPath / "config.json").CreateAndWriteAll(Config);
    if (error)
        return error;

    error = SaveImages(place);
    if (error)
        return error;

    if (!layersPath.Exists()) {
        error = layersPath.Mkdir(0755);
        if (error)
            return error;
    }

    for (const auto &layer: Layers) {
        TPath layerPath = layersPath / layer.Digest;
        if (layerPath.Exists())
            continue;
        error = layerPath.Hardlink(layer.ArchivePath(place));
        if (error)
            return error;
    }

    return OK;
}

TError TDockerImage::Load(const TPath &place) {
    TError error;
    TPath digestPath = DigestPath(place);

    if (digestPath.IsEmpty())
        return TError(EError::Docker, "Cannot find digest path of image {}", FullName());

    error = LoadImages(place);
    if (error)
        return error;

    if (Manifest.empty()) {
        error = TPath(digestPath / "manifest.json").ReadAll(Manifest, 1 << 30);
        if (error)
            return error;
    }

    if (Config.empty()) {
        error = TPath(digestPath / "config.json").ReadAll(Config, 1 << 30);
        if (error)
            return error;
    }

    error = ParseManifest();
    if (error)
        return error;

    error = ParseConfig();
    if (error)
        return error;

    return OK;
}


TError TDockerImage::InitStorage(const TPath &place, unsigned perms) {
    TError error;
    TPath dockerPath = place / PORTO_DOCKER;
    struct stat st;

    // Here dockerPath has been created in CheckBaseDirectory() or by user
    error = dockerPath.StatStrict(st);
    if (error)
        return error;

    // Create internal directories
    for (const auto& path: {PORTO_DOCKER_TAGS, PORTO_DOCKER_IMAGES, PORTO_DOCKER_LAYERS}) {
        error = TPath(place / path).MkdirAll(perms);
        if (error)
            return error;
    }

    // dockerPath has uid and gid either from CheckBaseDirectory() or from user
    error = dockerPath.ChownRecursive(st.st_uid, st.st_gid);
    if (error)
        return error;

    return OK;
}

TError TDockerImage::List(const TPath &place, std::vector<TDockerImage> &images, const std::string &mask) {
    TError error;
    TPath imagesPath = place / PORTO_DOCKER_IMAGES;
    TPathWalk walk;

    error = walk.OpenList(imagesPath);
    if (error)
        return error;

    while (true) {
        error = walk.Next();
        if (error || !walk.Path)
            break;

        if (walk.Postorder || walk.Level() != 2)
            continue;

        TDockerImage image(walk.Name());
        error = image.Load(place);
        if (error) {
            L_ERR("{}", error);
            continue;
        }

        if (!mask.empty()) {
            bool found = false;
            for (const auto& it: image.Images) {
                for (const auto& tag: it.second) {
                    if (StringMatch(it.first + ":" + tag, mask, false, false)) {
                        found = true;
                        break;
                    }
                }
                if (found)
                    break;
            }

            if (!found)
                continue;
        }

        images.emplace_back(image);
    }

    return OK;
}

TError TDockerImage::Status(const TPath &place) {
    TError error;

    error = DetectImage(place);
    if (error)
        return error;

    error = Load(place);
    if (error)
        return error;

    return OK;
}

TError TDockerImage::Pull(const TPath &place) {
    TError error;
    THttpClient client("https://" + Registry);

    error = DownloadManifest(client);
    if (error)
        return error;

    error = ParseManifest();
    if (error)
        return error;

    error = DownloadConfig(client);
    if (error)
        return error;

    error = ParseConfig();
    if (error)
        return error;

    std::string name = FullName(true);
    TPath digestPath = DigestPath(place);
    if (!digestPath.IsEmpty() && (digestPath / DOCKER_IMAGES_FILE).Exists()) {
        // digest already exists and we check its tags and images
        auto digestLock = TFileMutex::MakeDirLock(digestPath);

        error = LoadImages(place);
        if (error)
            return error;

        if (Images.find(name) == Images.end() || Images[name].find(Tag) == Images[name].end()) {
            // it's a new tag
            error = LinkTag(place);
            if (error)
                return error;

            Images[name].emplace(Tag);

            error = SaveImages(place);
            if (error)
                return error;
        }

        return OK;
    }

    error = DownloadLayers(place);
    if (error) {
        RemoveLayers(place);
        return error;
    }

    error = digestPath.MkdirAll(0755);
    if (error && error.Errno != EEXIST)
        L_ERR("Cannot create directory {}: {}", digestPath, error);

    error = LinkTag(place);
    if (error) {
        UnlinkTag(place);

        TError error2 = digestPath.DirName().ClearEmptyDirectories(place / PORTO_DOCKER_IMAGES);
        if (error2)
            L_ERR("Cannot clear directories: {}", error);

        RemoveLayers(place);

        return error;
    }

    auto digestLock = TFileMutex::MakeDirLock(digestPath);
    Images[name].emplace(Tag);
    error = Save(place);
    if (error) {
        TError error2 = digestPath.RemoveAll();
        if (error2)
            L_ERR("Cannot cleanup image: {}", error2);

        digestLock.reset();

        error2 = digestPath.DirName().ClearEmptyDirectories(place / PORTO_DOCKER_IMAGES);
        if (error2)
            L_ERR("Cannot clear directories: {}", error);

        UnlinkTag(place);

        RemoveLayers(place);

        return error;
    }

    return OK;
}

TError TDockerImage::Remove(const TPath &place) {
    TError error;
    bool tagSpecified = Digest.empty();

    error = DetectImage(place);
    if (error) {
        if (tagSpecified && StringStartsWith(error.ToString(), "Docker:(Detected tag symlink is broken"))
            UnlinkTag(place);
        return error;
    }

    error = Load(place);
    if (error) {
        if (tagSpecified && StringStartsWith(error.ToString(), "Docker:(Cannot find digest path of image"))
            UnlinkTag(place);
        return error;
    }

    TPath tagPath;
    if (tagSpecified) {
        std::string name = FullName(true);
        if (Images.size() > 1 || Images[name].size() > 1) {
            // delete only tag
            UnlinkTag(place);

            if (Images[name].size() <= 1)
                Images.erase(name);
            else
                Images[name].erase(Tag);

            auto digestLock = TFileMutex::MakeDirLock(DigestPath(place).DirName());
            error = SaveImages(place);
            if (error)
                return error;

            return OK;
        }
    }

    // delete tags
    for (const auto &image: Images) {
        ParseName(image.first);
        for (const auto &tag: image.second) {
            Tag = tag;

            error = DetectTagPath(place);
            if (error)
                return error;

            UnlinkTag(place);
        }
    }

    // delete digest
    TPath digestPath = DigestPath(place);
    auto digestLock = TFileMutex::MakeDirLock(digestPath.DirName());
    error = digestPath.RemoveAll();
    if (error)
        return error;

    error = digestPath.DirName().ClearEmptyDirectories(place / PORTO_DOCKER_IMAGES);
    if (error)
        L_ERR("Cannot clear directories: {}", error);

    digestLock.reset();

    RemoveLayers(place);

    return OK;
}
