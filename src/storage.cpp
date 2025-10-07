#include "storage.hpp"

#include <algorithm>
#include <condition_variable>

#include "client.hpp"
#include "config.hpp"
#include "docker.hpp"
#include "filesystem.hpp"
#include "helpers.hpp"
#include "util/log.hpp"
#include "util/md5.hpp"
#include "util/quota.hpp"
#include "util/string.hpp"
#include "util/unix.hpp"
#include "volume.hpp"

extern "C" {
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
}

static const char LAYER_TMP[] = "_tmp_";
static const char WEAK_PREFIX[] = "_weak_";
static const char IMPORT_PREFIX[] = "_import_";
static const char REMOVE_PREFIX[] = "_remove_";
static const char PRIVATE_PREFIX[] = "_private_";
static const char META_PREFIX[] = "_meta_";
static const char META_LAYER[] = "_layer_";

static uint64_t AsyncRemoveWatchDogPeriod;

extern std::map<std::string, const std::shared_ptr<TVolume>> VolumeById;

/* Protected with VolumesMutex */

static std::set<std::pair<dev_t, ino_t>> ActiveInodes;

static bool InodeIsActive(const TFile &f) {
    PORTO_LOCKED(VolumesMutex);

    struct stat st;
    auto error = f.Stat(st);
    if (error)
        return false;

    return ActiveInodes.find({st.st_dev, st.st_ino}) != ActiveInodes.end();
}

static TError AddActiveInode(const TFile &f) {
    PORTO_LOCKED(VolumesMutex);

    struct stat st;
    auto error = f.Stat(st);
    if (error)
        return error;
    ActiveInodes.emplace(st.st_dev, st.st_ino);
    return OK;
}

static TError RemoveActiveInode(const TFile &f) {
    PORTO_LOCKED(VolumesMutex);

    struct stat st;
    auto error = f.Stat(st);
    if (error)
        return error;
    ActiveInodes.erase({st.st_dev, st.st_ino});
    return OK;
}

static std::condition_variable StorageCv;

static TUintMap PlaceLoad;
static TUintMap PlaceLoadLimit;

extern std::atomic_bool NeedStopHelpers;

static std::thread AsyncRemoveThread;
static std::condition_variable AsyncRemoverCv;
static std::mutex AsyncRemoverMutex;

static std::set<TPath> Places;
static std::mutex PlacesMutex;

static inline std::unique_lock<std::mutex> LockAsyncRemover() {
    return std::unique_lock<std::mutex>(AsyncRemoverMutex);
}

static inline std::unique_lock<std::mutex> LockPlaces() {
    return std::unique_lock<std::mutex>(PlacesMutex);
}

static TPath getStorageName(EStorageType type) {
    switch (type) {
    case EStorageType::Volume:
        return PORTO_VOLUMES;
    case EStorageType::Layer:
        return PORTO_LAYERS;
    case EStorageType::Storage:
        return PORTO_STORAGE;
    case EStorageType::DockerLayer:
        return PORTO_DOCKER;
    case EStorageType::Meta:
        return "";
    default:
        return "";
    }
}

static TPath getStorageBase(const TPath &place, EStorageType type) {
    return place / getStorageName(type);
}

static unsigned getPermission(EStorageType type) {
    switch (type) {
    case EStorageType::Volume:
        return 0755;
    default:
        return 0700;
    }
}

void dropPlace(const TPath &place) {
    auto placesLock = LockPlaces();
    Places.erase(place);
}

void AsyncRemoveWatchDog() {
    TError error;

    SetProcessName("portod-AR");

    auto period = std::chrono::milliseconds(AsyncRemoveWatchDogPeriod);

    while (!NeedStopHelpers) {
        auto until = std::chrono::steady_clock::now() + period;
        auto placesLock = LockPlaces();
        const auto places = Places;
        placesLock.unlock();

        for (const auto &place: places) {
            TFile pin;
            auto error = pin.OpenDir(place);
            if (error) {
                if (error.Errno != ENOENT && error.Errno != ENOTDIR)
                    L_WRN("Open place: {}", error);
                dropPlace(place);
                continue;
            }

            bool drop = false;
            (void)TStorage::Cleanup(pin, false, drop);
            if (drop)
                dropPlace(place);
            if (NeedStopHelpers)
                return;
        }

        auto asyncRemoverLock = LockAsyncRemover();
        AsyncRemoverCv.wait_until(asyncRemoverLock, until);
    }
}

TError TStorage::Resolve(EStorageType type, const TPath &place, const std::string &name, bool strict) {
    TError error;

    Place = place;
    error = CL->ClientContainer->ResolvePlace(Place, strict);
    if (error)
        return error;

    Open(type, Place, name);
    return OK;
}

void TStorage::Open(EStorageType type, const TPath &place, const std::string &name) {
    PORTO_ASSERT(place.IsAbsolute());

    Type = type;
    Name = name;
    FirstName = name;
    Place = place;

    auto sep = name.find('/');
    if (sep != std::string::npos) {
        Meta = name.substr(0, sep);
        FirstName = name.substr(sep + 1);
    }

    switch (type) {
    case EStorageType::Place:
        Path = Place;
        break;
    case EStorageType::Layer:
        if (sep == std::string::npos)
            Path = Place / PORTO_LAYERS / Name;
        else
            Path =
                Place / PORTO_STORAGE / fmt::format("{}{}/{}{}", META_PREFIX, Meta, META_LAYER, name.substr(sep + 1));
        break;
    case EStorageType::DockerLayer:
        Path = TDockerImage::TLayer(Name).LayerPath(Place) / "content";
        break;
    case EStorageType::Storage:
        if (sep == std::string::npos)
            Path = Place / PORTO_STORAGE / Name;
        else
            Path = place / PORTO_STORAGE / fmt::format("{}{}/{}", META_PREFIX, Meta, name.substr(sep + 1));
        break;
    case EStorageType::Meta:
        Path = place / PORTO_STORAGE / fmt::format("{}{}", META_PREFIX, Name);
        break;
    case EStorageType::Volume:
        Path = place / PORTO_VOLUMES / name;
        break;
    }
}

void TStorage::Init() {
    if (StringToUintMap(config().volumes().place_load_limit(), PlaceLoadLimit))
        PlaceLoadLimit = {{"default", 1}};

    CheckPlace(PORTO_PLACE);
}

void TStorage::StartAsyncRemover() {
    AsyncRemoveWatchDogPeriod = config().volumes().async_remove_watchdog_ms();
    AsyncRemoveThread = std::thread(&AsyncRemoveWatchDog);
}

void TStorage::StopAsyncRemover() {
    AsyncRemoverCv.notify_all();
    AsyncRemoveThread.join();
}

void TStorage::IncPlaceLoad(const TPath &place) {
    auto lock = LockVolumes();
    auto id = place.ToString();

    if (!PlaceLoadLimit.count(id))
        id = "default";
    L_ACT("Start waiting for place load slot, id={} limit={}", id, PlaceLoadLimit[id]);
    StorageCv.wait(lock, [&] { return PlaceLoad[id] < PlaceLoadLimit[id]; });
    PlaceLoad[id]++;
    L_ACT("Finish waiting for place load slot, id={}", id);
}

void TStorage::DecPlaceLoad(const TPath &place) {
    auto lock = LockVolumes();
    auto id = place.ToString();

    if (!PlaceLoadLimit.count(id))
        id = "default";
    if (PlaceLoad[id]-- <= 1)
        PlaceLoad.erase(id);
    StorageCv.notify_all();
}

TError TStorage::CheckBaseDirectory(const TPath &place, EStorageType type, unsigned perms) {
    TError error;
    bool default_place = false;
    struct stat st;

    auto base = getStorageBase(place, type);

    if (place == PORTO_PLACE)
        default_place = true;
    else
        default_place = std::find(AuxPlacesPaths.begin(), AuxPlacesPaths.end(), place) != AuxPlacesPaths.end();

    error = base.StatStrict(st);
    if (error && error.Errno == ENOENT) {
        /* If base does not exist, we attempt to create base directory */
        error = base.MkdirAll(perms);
        if (!error)
            error = base.StatStrict(st);
    }
    if (error)
        return error;

    if (!S_ISDIR(st.st_mode))
        return TError(EError::InvalidValue, base.ToString() + " must be directory");

    if (default_place) {
        /* In default place root controls base structure */
        if (st.st_uid != RootUser || st.st_gid != PortoGroup) {
            error = base.Chown(RootUser, PortoGroup);
            if (error)
                return error;
        }
    } else {
        /* In non-default place user can control base structure */
        struct stat pst;

        error = base.DirName().StatStrict(pst);
        if (error)
            return error;

        error = base.Chown(pst.st_uid, pst.st_gid);
        if (error)
            return error;
    }

    if ((st.st_mode & 0777) != perms) {
        error = base.Chmod(perms);
        if (error)
            return error;
    }

    return OK;
}

static TError cleanupVolume(const TFile &dir, const TFile &pin, const std::string &name) {
    auto lock = LockVolumes();
    auto it = VolumeById.find(name);
    if (it != VolumeById.end()) {
        struct stat st1, st2;
        auto internal = it->second->GetInternal("");
        auto error2 = internal.StatStrict(st2);
        auto error = pin.Stat(st1);
        if (error)
            L_WRN("Failed stat {}: {}", pin.RealPath(), error);
        // handle race with volume create/destroy
        if (!st1.st_nlink)
            return OK;
        if (error2)
            return error2;
        if (st1.st_dev == st2.st_dev && st1.st_ino == st2.st_ino)
            return OK;
        L("Volume {} does not match via inodes with {}", pin.RealPath(), internal);
    } else
        L("Volume {} is junk", pin.RealPath());

    auto removeName = TPath(REMOVE_PREFIX) + name;
    auto error = dir.RenameAt(name, removeName);
    if (error)
        return error;
    lock.unlock();

    L_ACT("Remove junk: {}", pin.RealPath());
    return dir.RemoveAllAtInterruptible(removeName, NeedStopHelpers, true);
}

TError TStorage::Cleanup(const TFile &place, bool strict, bool &drop) {
    int not_exist = 0;
    for (auto type: {EStorageType::Volume, EStorageType::Layer, EStorageType::Storage}) {
        auto name = getStorageName(type);
        TFile dir;

        auto error = dir.OpenDirStrictAt(place, name);
        if (error) {
            if (error.Errno == ENOTDIR) {
                auto error2 = place.UnlinkAt(name);
                if (error2) {
                    L_WRN("Cannot unlink storage dir {} {}: {}", place.RealPath(), name, error2);
                    if (strict)
                        return error;
                }
            } else if (error.Errno != ENOENT) {
                L_WRN("Cannot open storage dir {} {}: {}", place.RealPath(), name, error);
                if (strict)
                    return error;
            }
            ++not_exist;
            continue;
        }

        error = TStorage::Cleanup(dir, type);
        if (error) {
            L_WRN("Cleanup place {} {} failed: {}", place.RealPath(), getStorageName(type), error);
            if (strict)
                return error;
        }
        if (NeedStopHelpers)
            return TError(EError::SocketError);
    }

    if (not_exist == 3)
        drop = true;
    return OK;
}

/* FIXME racy. rewrite with openat... etc */
TError TStorage::Cleanup(const TFile &dir, EStorageType type) {
    std::vector<std::string> list;
    auto error = dir.ReadDirectory(list);
    if (error)
        return error;

    for (auto &name: list) {
        if (NeedStopHelpers)
            return TError(EError::SocketError, "RemoveRecursive was interrupted");

        TFile pin;
        auto error = pin.OpenAt(dir, name, O_PATH | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY);
        if (error) {
            if (error.Errno != ENOENT)
                L_WRN("{}", error);
            continue;
        }

        struct stat st;
        error = pin.Stat(st);
        if (error) {
            L_WRN("{}", error);
            continue;
        }

        if (type == EStorageType::Volume) {
            error = cleanupVolume(dir, pin, name);
            if (error)
                L_WRN("Failed cleanup volume {}: {}", pin.RealPath(), error);
            continue;
        }

        if (type == EStorageType::Storage && S_ISDIR(st.st_mode) && StringStartsWith(name, META_PREFIX)) {
            error = Cleanup(pin, EStorageType::Meta);
            if (error)
                L_WRN("Cannot cleanup metastorage {} {}", pin.RealPath(), error);
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!CheckName(name))
                continue;
            if (type == EStorageType::Meta && StringStartsWith(name, META_LAYER))
                continue;
        }

        auto lock = LockVolumes();
        if (S_ISDIR(st.st_mode)) {
            if (InodeIsActive(pin))
                continue;
        } else if (S_ISREG(st.st_mode)) {
            if (StringStartsWith(name, PRIVATE_PREFIX)) {
                auto tail = name.substr(std::string(PRIVATE_PREFIX).size());
                struct stat st1;
                if (!dir.StatAt(tail, false, st1) && S_ISDIR(st1.st_mode))
                    continue;

                if (!dir.StatAt(std::string(IMPORT_PREFIX) + tail, false, st1) && S_ISDIR(st1.st_mode))
                    continue;
            }

            /* Remove random files if any */
            error = dir.UnlinkAt(name);
            continue;
        }
        lock.unlock();

        L_ACT("Remove junk: {} {}", dir.RealPath(), name);
        error = dir.RemoveAllAtInterruptible(name, NeedStopHelpers, false);
        if (error)
            L_WRN("Cannot remove junk {} {}: {}", dir.RealPath(), name, error);
    }

    return OK;
}

TError TStorage::CheckPlace(const TPath &place) {
    TError error;

    if (!place.IsAbsolute())
        return TError(EError::InvalidPath, "Place path {} must be absolute", place);

    if (!place.IsNormal())
        return TError(EError::InvalidPath, "Place path {} must be normalized", place);

    if (IsSystemPath(place))
        return TError(EError::InvalidPath, "Place path {} in system directory", place);

    if (place.IsDirectoryFollow()) {
        auto lockPlaces = LockPlaces();
        auto found = Places.find(place) != Places.end();
        lockPlaces.unlock();

        if (found)
            return OK;
    }

    for (auto type: {EStorageType::Volume, EStorageType::Layer, EStorageType::Storage}) {
        error = CheckBaseDirectory(place, type, getPermission(type));
        if (error)
            return error;
    }

    if (config().daemon().docker_images_support()) {
        error = CheckBaseDirectory(place, EStorageType::DockerLayer, 0700);
        if (error)
            return error;

        error = TDockerImage::InitStorage(place, 0700);
        if (error)
            return error;
    }

    auto lockPlaces = LockPlaces();
    Places.insert(place);
    lockPlaces.unlock();

    return OK;
}

TError TStorage::CheckName(const std::string &name, bool meta) {
    auto sep = name.find('/');
    if (!meta && sep != std::string::npos) {
        TError error = CheckName(name.substr(0, sep), true);
        if (error)
            return error;
        return CheckName(name.substr(sep + 1), true);
    }
    auto pos = name.find_first_not_of(PORTO_NAME_CHARS);
    if (pos != std::string::npos)
        return TError(EError::InvalidValue, "forbidden character " + StringFormat("%#x", (unsigned char)name[pos]));
    if (name == "" || name == "." || name == ".." || StringStartsWith(name, LAYER_TMP) ||
        StringStartsWith(name, IMPORT_PREFIX) || StringStartsWith(name, REMOVE_PREFIX) ||
        StringStartsWith(name, PRIVATE_PREFIX) || StringStartsWith(name, META_PREFIX) ||
        StringStartsWith(name, META_LAYER))
        return TError(EError::InvalidValue, "invalid layer name '" + name + "'");
    return OK;
}

TError TStorage::List(EStorageType type, std::list<TStorage> &list) {
    std::vector<std::string> names;
    TPath path = Path;

    if (Type == EStorageType::Place) {
        if (type == EStorageType::Layer)
            path = Place / PORTO_LAYERS;
        else if (type == EStorageType::DockerLayer) {
            path = Place / PORTO_DOCKER_LAYERS / "blobs";
            if (!path.Exists())
                return OK;
        } else
            path = Place / PORTO_STORAGE;
    }

    TError error = path.ListSubdirs(names);
    if (error)
        return error;

    if (type == EStorageType::DockerLayer) {
        std::vector<std::string> prefixes = std::move(names);
        names.clear();
        for (auto &prefix: prefixes) {
            std::vector<std::string> prefixNames;
            error = TPath(path / prefix).ListSubdirs(prefixNames);
            if (error) {
                L_WRN("Cannot list subdirs {}: {}", path / prefix, error);
                continue;
            }
            names.insert(names.end(), prefixNames.begin(), prefixNames.end());
        }
    }

    for (auto &name: names) {
        if (Type == EStorageType::Place && StringStartsWith(name, META_PREFIX)) {
            TStorage meta;
            meta.Open(EStorageType::Meta, Place, name.substr(std::string(META_PREFIX).size()));
            list.push_back(meta);
            if (type == EStorageType::Storage) {
                error = meta.List(type, list);
                if (error)
                    return error;
            }
        } else if (Type == EStorageType::Meta) {
            if (StringStartsWith(name, META_LAYER)) {
                if (type == EStorageType::Layer) {
                    TStorage layer;
                    layer.Open(EStorageType::Layer, Place, Name + "/" + name.substr(std::string(META_LAYER).size()));
                    list.push_back(layer);
                }
            } else if (type == EStorageType::Storage && !CheckName(name)) {
                TStorage storage;
                storage.Open(EStorageType::Storage, Place, Name + "/" + name);
                list.push_back(storage);
            }
        } else if (!CheckName(name)) {
            TStorage storage;
            storage.Open(type, Place, name);
            list.push_back(storage);
        }
    }

    if (Type == EStorageType::Place && type == EStorageType::Layer) {
        names.clear();
        error = TPath(Place / PORTO_STORAGE).ListSubdirs(names);
        if (error) {
            if (!TPath(Place / PORTO_STORAGE).Exists())
                return OK;
            return error;
        }
        for (auto &name: names) {
            if (StringStartsWith(name, META_PREFIX)) {
                TStorage meta;
                meta.Open(EStorageType::Meta, Place, name.substr(std::string(META_PREFIX).size()));
                error = meta.List(EStorageType::Layer, list);
                if (error)
                    return error;
            }
        }
    }

    return error;
}

bool TStorage::Exists() const {
    return Path.Exists();
}

bool TStorage::Weak() const {
    return StringStartsWith(FirstName, WEAK_PREFIX);
}

uint64_t TStorage::LastUsage() const {
    return LastChange ? (time(nullptr) - LastChange) : 0;
}

TError TStorage::CheckUsage() {
    PORTO_LOCKED(VolumesMutex);

    if (Type == EStorageType::Layer) {
        if (!Exists())
            return TError(EError::LayerNotFound, "Layer " + Name + " not found");
        for (auto &it: Volumes) {
            for (auto &layer: it.second->Layers)
                if (Place == it.second->Place && Name == layer)
                    return TError(EError::Busy, "Layer " + Name + " in use by volume " + it.second->Path.ToString());
        }
    }

    if (Type == EStorageType::Storage) {
        if (!Exists())
            return TError(EError::VolumeNotFound, "Storage " + Name + " not found");
        for (auto &it: Volumes) {
            if (Place == it.second->Place && Name == it.second->Storage)
                return TError(EError::Busy, "Storage " + Name + " in use by volume " + it.second->Path.ToString());
        }
    }

    if (Type == EStorageType::Meta) {
        struct stat st;
        TError error = Path.StatStrict(st);
        if (error)
            return error;
        if (st.st_nlink != 2)
            return TError(EError::Busy, "MetaStorage {} in use, {} not empty", Name, Path);
    }

    return OK;
}

TPath TStorage::TempPath(const std::string &kind) {
    return Path.DirNameNormal() / kind + Path.BaseNameNormal();
}

TError TStorage::Load() {
    struct stat st;
    TError error;
    TFile priv;

    error = CheckName(Name);
    if (error)
        return error;

    error = priv.Open(TempPath(PRIVATE_PREFIX), O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NOFOLLOW);
    if (error || priv.Stat(st)) {
        if (error.Errno != ENOENT)
            return error;
        error = Path.StatStrict(st);
        if (error) {
            if (error.Errno == ENOENT) {
                if (Type == EStorageType::Layer)
                    return TError(EError::LayerNotFound, "Layer " + Name + " not found");
                if (Type == EStorageType::Storage)
                    return TError(EError::VolumeNotFound, "Storage " + Name + " not found");
            }
            return error;
        }
        Owner = TCred(NoUser, NoGroup);
        LastChange = st.st_mtime;
        Private = "";
        return OK;
    }

    Owner = TCred(st.st_uid, st.st_gid);
    LastChange = st.st_mtime;
    error = priv.ReadAll(Private, 4096);
    if (error)
        Private = "";
    return error;
}

TError TStorage::SaveOwner(const TCred &owner) {
    TPath priv = TempPath(PRIVATE_PREFIX);
    if (!priv.Exists())
        (void)priv.Mkfile(0644);
    TError error = priv.Chown(owner);
    if (!error)
        Owner = owner;
    return error;
}

TError TStorage::SetPrivate(const std::string &text) {
    TError error;

    if (text.size() > PRIVATE_VALUE_MAX)
        return TError(EError::InvalidValue, "Private value too log, max {} bytes", PRIVATE_VALUE_MAX);

    error = Load();
    if (error)
        return error;
    error = CL->CanControl(Owner);
    if (error)
        return TError(error, "Cannot set private {}", Name);
    return SavePrivate(text);
}

TError TStorage::SavePrivate(const std::string &text) {
    TPath priv = TempPath(PRIVATE_PREFIX);
    if (!priv.Exists())
        (void)priv.Mkfile(0644);
    TError error = priv.WriteAll(text);
    if (!error)
        Private = text;
    return error;
}

TError TStorage::Touch() {
    TError error = TempPath(PRIVATE_PREFIX).Touch();
    if (error && error.Errno == ENOENT)
        error = Path.Touch();
    return error;
}

static bool TarSupportsCompressArgs() {
    static bool tested = false, result = false;
    if (!tested) {
        TFile null;
        result = !null.OpenReadWrite("/dev/null") &&
                 !RunCommand({"tar", "--create", "--use-compress-program=gzip --best", "--files-from", "/dev/null"},
                             TFile(), null, null);
        L_SYS("tar {}supports compress program arguments", result ? "" : "not ");
        tested = true;
    }
    return result;
}

static TError Compression(const TPath &archive, const TFile &arc, const std::string &compress, std::string &format,
                          std::string &option) {
    std::string name = archive.BaseName();

    format = "tar";
    if (compress != "") {
        if (compress == "txz" || compress == "tar.xz")
            goto xz;
        if (compress == "tgz" || compress == "tar.gz")
            goto gz;
        if (compress == "tzst" || compress == "tar.zst")
            goto zst;
        if (compress == "tbz2" || compress == "tar.bz2")
            goto bz2;
        if (compress == "tar")
            goto tar;
        if (StringEndsWith(compress, "squashfs"))
            goto squash;
        return TError(EError::InvalidValue, "Unknown archive " + archive.ToString() + " compression " + compress);
    }

    /* tar cannot guess compression for std streams */
    if (arc.Fd >= 0) {
        char magic[8];

        if (pread(arc.Fd, magic, sizeof(magic), 0) == sizeof(magic)) {
            if (!strncmp(magic,
                         "\xFD"
                         "7zXZ\x00", 6))
                goto xz;
            if (!strncmp(magic, "\x1F\x8B\x08", 3))
                goto gz;
            if (!strncmp(magic, "\x28\xB5\x2F\xFD", 4))
                goto zst;
            if (!strncmp(magic, "BZh", 3))
                goto bz2;
            if (!strncmp(magic, "hsqs", 4))
                goto squash;
        }

        if (pread(arc.Fd, magic, sizeof(magic), 257) == sizeof(magic)) {
            /* "ustar\000" or "ustar  \0" */
            if (!strncmp(magic, "ustar", 5))
                goto tar;
        }

        return TError(EError::InvalidValue, "Cannot detect archive " + archive.ToString() + " compression by magic");
    }

    if (StringEndsWith(name, ".xz") || StringEndsWith(name, ".txz"))
        goto xz;

    if (StringEndsWith(name, ".gz") || StringEndsWith(name, ".tgz"))
        goto gz;

    if (StringEndsWith(name, ".zst") || StringEndsWith(name, ".tzst"))
        goto zst;

    if (StringEndsWith(name, ".bz2") || StringEndsWith(name, ".tbz2"))
        goto bz2;

    if (StringEndsWith(name, ".squash") || StringEndsWith(name, ".squashfs"))
        goto squash;

tar:
    option = "--no-auto-compress";
    return OK;
gz:
    if (!arc && config().volumes().parallel_compression()) {
        if (TPath("/usr/bin/pigz").Exists()) {
            option = "--use-compress-program=pigz";
            return OK;
        }
    }
    option = "--gzip";
    return OK;
xz:
    if (!arc && config().volumes().parallel_compression()) {
        if (TPath("/usr/bin/pixz").Exists()) {
            option = "--use-compress-program=pixz";
            return OK;
        }
    }
    option = "--xz";
    return OK;
zst:
    if (!arc && config().volumes().parallel_compression()) {
        if (TPath("/usr/bin/zstdmt").Exists()) {
            if (TarSupportsCompressArgs())
                option = "--use-compress-program=zstdmt -19";
            else
                option = "--use-compress-program=zstdmt ";
            return OK;
        }
    }
    if (TPath("/usr/bin/zstd").Exists()) {
        if (!arc && TarSupportsCompressArgs())
            option = "--use-compress-program=zstd -19";
        else
            option = "--use-compress-program=zstd";
        return OK;
    }
    return TError(EError::NotSupported, "Compression: Can not find /usr/bin/zstd binary");
bz2:
    option = "--bzip2";
    return OK;
squash:
    format = "squashfs";
    auto sep = compress.find('.');
    if (sep != std::string::npos)
        option = compress.substr(0, sep);
    else
        option = config().volumes().squashfs_compression();
    return OK;
}

static bool TarSupportsXattrs() {
    static bool tested = false, result = false;
    if (!tested) {
        TFile null;
        result = !null.OpenReadWrite("/dev/null") && !RunCommand({config().daemon().tar_path(), "--create", "--xattrs",
                                                                  "--files-from", "/dev/null"}, TFile(), null, null);
        L_SYS("tar {}supports extended attributes", result ? "" : "not ");
        tested = true;
    }
    return result;
}

TError TStorage::SaveChecksums() {
    TPathWalk walk;
    TError error;

    error = walk.OpenScan(Path);
    if (error)
        return error;

    Size = 0;

    while (1) {
        error = walk.Next();
        if (error)
            return error;
        if (!walk.Path)
            break;
        if (!walk.Postorder)
            Size += walk.Stat->st_blocks * 512ull;
        if (!S_ISREG(walk.Stat->st_mode))
            continue;
        TFile file;
        error = file.OpenRead(walk.Path);
        if (error)
            return error;
        std::string sum;
        error = Md5Sum(file, sum);
        if (error)
            return error;
        error = file.SetXAttr("user.porto.md5sum", sum);
        if (error)
            return error;
    }

    return OK;
}

TError TStorage::ImportArchive(const TPath &archive, const std::string &memCgroup, const std::string &compress,
                               bool merge, bool verboseError) {
    TPath temp = TempPath(IMPORT_PREFIX);
    TError error;
    TFile arc;

    error = CheckName(Name);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    if (!archive.IsAbsolute())
        return TError(EError::InvalidValue, "archive path must be absolute");

    if (!archive.Exists())
        return TError(EError::InvalidValue, "archive not found");

    if (!archive.IsRegularFollow())
        return TError(EError::InvalidValue, "archive not a file");

    error = arc.OpenRead(archive);
    if (error)
        return error;

    error = CL->ReadAccess(arc);
    if (error)
        return TError(error, "Cannot import {} from {}", Name, archive);

    std::string compress_format, compress_option;
    error = Compression(archive, arc, compress, compress_format, compress_option);
    if (error)
        return error;

    if (TempPath(REMOVE_PREFIX).Exists())
        return TError(EError::Busy, "{} is being removed", Name);

    auto lock = LockVolumes();

    TFile import_dir;

    while (!import_dir.OpenDir(temp) && InodeIsActive(import_dir)) {
        if (merge)
            return TError(EError::Busy, Name + " is importing right now");
        StorageCv.wait(lock);
    }

    if (merge && Exists()) {
        TStorage layer;
        layer.Open(Type, Place, Name);
        error = layer.Load();
        if (error)
            return error;
        error = CL->CanControl(layer.Owner);
        if (error)
            return TError(error, "Cannot merge {}", Path);
    }

    if (Path.Exists()) {
        if (!merge)
            return TError(EError::LayerAlreadyExists, "Layer already exists");
        error = CheckUsage();
        if (error)
            return error;
        error = Path.Rename(temp);
        if (error)
            return error;
    } else {
        error = temp.Mkdir(0775);
        if (error)
            return error;
    }

    error = import_dir.OpenDir(temp);
    if (error)
        return error;
    error = AddActiveInode(import_dir);
    if (error)
        return error;
    temp = import_dir.RealPath();

    lock.unlock();

    IncPlaceLoad(Place);
    Statistics->LayerImport++;

    if (compress_format == "tar") {
        TTuple args = {config().daemon().tar_path(), "--numeric-owner", "--preserve-permissions", compress_option,
                       "--extract"};

        if (TarSupportsXattrs())
            args.insert(args.begin() + 3, {"--xattrs", "--xattrs-include=security.capability",
                                           "--xattrs-include=trusted.overlay.*", "--xattrs-include=user.*"});

        error = RunCommand(args, {}, import_dir, arc, TFile(), HelperCapabilities, memCgroup, verboseError, true);
    } else if (compress_format == "squashfs") {
        TTuple args = {"unsquashfs", "-force", "-no-progress",  "-processors",
                       "1",          "-dest",  temp.ToString(), archive.ToString()};

        TFile parent_dir;
        error = parent_dir.OpenDirStrictAt(import_dir, "..");
        if (error)
            return error;

        error = RunCommand(args, {}, parent_dir, TFile(), TFile(), HelperCapabilities, memCgroup, verboseError, true);
    } else
        error = TError(EError::NotSupported, "Unsuported format " + compress_format);

    if (error)
        goto err;

    if (!merge && Type == EStorageType::Layer) {
        error = SanitizeLayer(temp);
        if (error)
            goto err;
    }

    if (!Owner.IsUnknown()) {
        error = SaveOwner(Owner);
        if (error)
            goto err;
    }

    if (!Private.empty()) {
        error = SavePrivate(Private);
        if (error)
            goto err;
    }

    lock.lock();
    error = temp.Rename(Path);
    if (!error)
        error = RemoveActiveInode(import_dir);
    lock.unlock();
    if (error)
        goto err;

    DecPlaceLoad(Place);

    StorageCv.notify_all();

    return OK;

err:
    TError error2 = temp.RemoveAll();
    if (error2)
        L_WRN("Cannot cleanup layer: {}", error2);

    DecPlaceLoad(Place);

    lock.lock();
    (void)RemoveActiveInode(import_dir);
    lock.unlock();

    StorageCv.notify_all();

    return error;
}

TError TStorage::ExportArchive(const TPath &archive, const std::string &compress) {
    TFile dir, arc;
    TError error;

    error = CheckName(Name);
    if (error)
        return error;

    error = CL->CanControl(Owner);
    if (error)
        return TError(error, "Cannot export {}", Path);

    if (!archive.IsAbsolute())
        return TError(EError::InvalidValue, "archive path must be absolute");

    if (archive.Exists())
        return TError(EError::InvalidValue, "archive already exists");

    std::string compress_format, compress_option;
    error = Compression(archive, TFile(), compress, compress_format, compress_option);
    if (error)
        return error;

    error = dir.OpenDir(archive.DirName());
    if (error)
        return error;

    error = CL->WriteAccess(dir);
    if (error)
        return error;

    if (Type == EStorageType::Storage) {
        auto lock = LockVolumes();
        error = CheckUsage();
        if (error)
            return error;
    }

    error = arc.OpenAt(dir, archive.BaseName(), O_CREAT | O_WRONLY | O_EXCL | O_CLOEXEC, 0664);
    if (error)
        return error;

    IncPlaceLoad(Place);
    Statistics->LayerExport++;

    if (Type == EStorageType::Volume && compress_format == "tar") {
        L_ACT("Save checksums in {}", Path);
        error = SaveChecksums();
        if (error)
            return error;
        L("Unpacked size {} {}", Path, StringFormatSize(Size));
        error = arc.SetXAttr("user.porto.unpacked_size", std::to_string(Size));
        if (error)
            L_WRN("Cannot save unpacked size in xattr: {}", error);
    }

    if (compress_format == "tar") {
        TTuple args = {config().daemon().tar_path(), "--one-file-system", "--numeric-owner", "--preserve-permissions",
                       "--sparse",
                       "--transform",
                       "s:^./::",
                       compress_option,
                       "--create",
                       "-C",
                       Path.ToString(),
                       "."};

        if (TarSupportsXattrs())
            args.insert(args.begin() + 4, "--xattrs");

        error = RunCommand(args, dir, TFile(), arc);
    } else if (compress_format == "squashfs") {
        TTuple args = {"mksquashfs", Path.ToString(), archive.BaseName(), "-noappend", "-comp", compress_option};

        error = RunCommand(args, dir, TFile());
    } else
        error = TError(EError::NotSupported, "Unsupported format " + compress_format);

    if (!error)
        error = arc.Chown(CL->TaskCred);
    if (error)
        (void)dir.UnlinkAt(archive.BaseName());

    DecPlaceLoad(Place);

    return error;
}

TError TStorage::Remove(bool weak, bool async) {
    TError error;

    error = CheckName(Name);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    error = Load();
    if (error == EError::LayerNotFound || error == EError::VolumeNotFound) {
        auto temp = TempPath(REMOVE_PREFIX);
        if (temp.Exists())
            return temp.RemoveAll();
    }
    if (error)
        return error;

    error = CL->CanControl(Owner);
    if (error && !weak)
        return TError(error, "Cannot remove {}", Path);

    auto lock = LockVolumes();

    error = CheckUsage();
    if (error)
        return error;

    auto priv = TempPath(PRIVATE_PREFIX);
    if (priv.Exists()) {
        error = priv.Unlink();
        if (error)
            L_WRN("Cannot remove private: {}", error);
    }

    TFile temp_dir;
    auto temp = TempPath(REMOVE_PREFIX);

    // We don't have to use active paths, because async temp files are skipped by Cleanup
    error = Path.Rename(temp);
    if (!error && !async) {
        error = temp_dir.OpenDir(temp);
        if (!error)
            error = AddActiveInode(temp_dir);
    }

    lock.unlock();

    if (error)
        return error;

    IncPlaceLoad(Place);
    Statistics->LayerRemove++;

    if (Type == EStorageType::Meta) {
        TProjectQuota quota(temp);
        error = quota.Destroy();
        if (error)
            L_WRN("Cannot destroy quota {}: {}", temp, error);
    }

    if (!async) {
        error = temp.RemoveAll();
        if (error)
            L_WRN("Cannot remove storage {}: {}", temp, error);
    }

    DecPlaceLoad(Place);

    if (!async) {
        lock.lock();
        RemoveActiveInode(temp_dir);
        lock.unlock();
    }

    return error;
}

TError TStorage::SanitizeLayer(const TPath &layer) {
    TPathWalk walk;
    TError error;

    error = walk.Open(layer);
    if (error)
        return error;

    while (true) {
        error = walk.Next();
        if (error || !walk.Path)
            return error;

        /* Handle aufs whiteouts and metadata */
        if (StringStartsWith(walk.Name(), ".wh.")) {
            /* Remove it completely */
            error = walk.Path.RemoveAll();
            if (error)
                return error;

            /* Opaque directory - hide entries in lower layers */
            if (walk.Name() == ".wh..wh..opq") {
                error = walk.Path.DirName().SetXAttr("trusted.overlay.opaque", "y");
                if (error)
                    return error;
            }

            /* Metadata is done */
            if (StringStartsWith(walk.Name(), ".wh..wh."))
                continue;

            /* Remove whiteouted entry */
            TPath real = walk.Path.DirName() / walk.Name().substr(4);
            if (real.PathExists()) {
                error = real.RemoveAll();
                if (error)
                    return error;
            }

            /* Convert into overlayfs whiteout */
            error = real.Mknod(S_IFCHR, 0);
            if (error)
                return error;
        }
    }
}

TError TStorage::CreateMeta(uint64_t space_limit, uint64_t inode_limit) {
    TError error;

    error = CheckName(Name, true);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    auto lock = LockVolumes();

    error = Path.Mkdir(0700);
    if (error)
        return error;

    lock.unlock();

    TProjectQuota quota(Path);
    quota.SpaceLimit = space_limit;
    quota.InodeLimit = inode_limit;

    error = quota.Create();
    if (error)
        goto err;

    if (!Owner.IsUnknown()) {
        error = SaveOwner(Owner);
        if (error)
            goto err;
    }

    if (!Private.empty()) {
        error = SavePrivate(Private);
        if (error)
            goto err;
    }

    return OK;

err:
    Path.Rmdir();
    return error;
}

TError TStorage::ResizeMeta(uint64_t space_limit, uint64_t inode_limit) {
    TError error;

    error = CheckName(Name);
    if (error)
        return error;

    error = CheckPlace(Place);
    if (error)
        return error;

    error = Load();
    if (error)
        return error;

    error = CL->CanControl(Owner);
    if (error)
        return TError(error, "Cannot resize {}", Path);

    auto lock = LockVolumes();
    TProjectQuota quota(Path);
    quota.SpaceLimit = space_limit;
    quota.InodeLimit = inode_limit;
    return quota.Resize();
}
