#include "loop.hpp"

#include <functional>
#include <mutex>

#include "config.hpp"
#include "util/error.hpp"
#include "util/log.hpp"
#include "util/path.hpp"

extern "C" {
#include <linux/loop.h>
#include <sys/ioctl.h>
}

static TError ClaimLoop(const std::function<TError(int)> &claim, TFile &dev, int &nr) {
    static std::mutex BigLoopLock;

    TFile ctl;
    auto error = ctl.OpenReadWrite("/dev/loop-control");
    if (error)
        return error;

    constexpr int LOOP_CLAIM_RETRIES = 16;
    auto lock = std::unique_lock<std::mutex>(BigLoopLock);
    for (int i = 0; i < LOOP_CLAIM_RETRIES; ++i) {
        nr = ioctl(ctl.Fd, LOOP_CTL_GET_FREE);

        TFile f;
        error = f.OpenReadWrite("/dev/loop" + std::to_string(nr));
        if (error)
            return error;

        auto error = claim(f.Fd);
        if (error) {
            if (error.Error != EError::ResourceNotAvailable)
                return error;
            continue;
        }

        dev.Swap(f);
        return OK;
    }
    return TError(EError::ResourceNotAvailable, "cannot allocate loop device");
}

static TError ConfigureLoopDev(const struct loop_config &cfg, TFile &dev, int &nr) {
    return ClaimLoop([&cfg](int devFd) {
        if (ioctl(devFd, LOOP_CONFIGURE, &cfg) < 0) {
            if (errno != EBUSY)
                return TError::System("ioctl(LOOP_CONFIGURE)");
            return TError(EError::ResourceNotAvailable, "ioctl(LOOP_CONFIGURE)");
        }
        return OK;
    }, dev, nr);
}

TError SetupLoopDev(const TFile &file, const TPath &path, int &devnr) {
    struct stat st;
    auto error = file.Stat(st);
    if (error)
        return error;

    struct loop_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.fd = file.Fd;
    cfg.block_size = st.st_blksize;
    strncpy((char *)cfg.info.lo_file_name, path.c_str(), LO_NAME_SIZE - 1);
    cfg.info.lo_flags = LO_FLAGS_DIRECT_IO;

    TFile dev;
    int nr = -1;
    error = ConfigureLoopDev(cfg, dev, nr);
    if (error)
        return error;

    error = TPath(fmt::format("/sys/block/loop{}/queue/scheduler", nr)).WriteAll("none");
    if (error) {
        (void)ioctl(dev.Fd, LOOP_CLR_FD, 0);
        return error;
    }

    devnr = nr;

    return OK;
}

TError PutLoopDev(const int loopNr) {
    TFile loop;
    TError error = loop.OpenReadWrite("/dev/loop" + std::to_string(loopNr));
    if (!error && ioctl(loop.Fd, LOOP_CLR_FD, 0) < 0)
        return TError::System("ioctl(LOOP_CLR_FD)");
    return error;
}
