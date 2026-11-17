#include "taskwire.hpp"

#include <fcntl.h>

static constexpr int MaxTaskSize = 16 * 1024 * 1024;
static constexpr int MaxTaskFds = 4096;

TError SendTask(const TUnixSocket &socket, const runtime::TTaskSpec &spec, const std::vector<int> &fds) {
    std::string data;
    if (!spec.SerializeToString(&data) || data.size() > MaxTaskSize || fds.size() > MaxTaskFds)
        return TError("Invalid portorun specification size");
    auto error = socket.SendInt(data.size());
    if (error)
        return error;
    error = socket.Write(data.data(), data.size());
    if (error)
        return error;
    error = socket.SendInt(fds.size());
    if (error)
        return error;
    for (auto fd: fds) {
        error = socket.SendFd(fd);
        if (error)
            return error;
    }
    return OK;
}

TError ReceiveTask(const TUnixSocket &socket, runtime::TTaskSpec &spec, std::vector<TFile> &fds) {
    int size;
    auto error = socket.RecvInt(size);
    if (error)
        return error;
    if (size <= 0 || size > MaxTaskSize)
        return TError("Invalid portorun specification size");
    std::string data(size, '\0');
    error = socket.Read(&data[0], data.size());
    if (error)
        return error;
    if (!spec.ParseFromString(data))
        return TError("Invalid portorun specification contents");
    error = socket.RecvInt(size);
    if (error)
        return error;
    if (size < 0 || size > MaxTaskFds)
        return TError("Invalid portorun descriptor count");
    for (int i = 0; i < size; ++i) {
        int fd;
        error = socket.RecvFd(fd);
        if (error)
            return error;
        fds.emplace_back(fd);
    }
    return OK;
}
