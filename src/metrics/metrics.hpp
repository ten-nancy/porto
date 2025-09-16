#pragma once

#include <cstdint>
#include <memory>
#include <map>

#include "rpc.pb.h"
#include "util/error.hpp"


class TMetricsRegistry;

template<typename M>
class TMetricFabric {
    TMetricsRegistry &Reg;
    std::string Name;
    std::map<const std::string, const std::string> Labels;

public:
    TMetricFabric(TMetricsRegistry &reg, const char *name, const std::map<const std::string, const std::string> &labels = {})
        : Reg(reg), Name(name), Labels(labels) { }

    M WithLabels(const std::map<const std::string, const std::string> &labels) {
        auto lbl = Labels;
        lbl.insert(labels.begin(), labels.end());
        return M(Reg, Name.c_str(), lbl);
    }

    TMetricFabric<M> Add(const std::map<const std::string, const std::string> &labels) {
        auto lbl = Labels;
        lbl.insert(labels.begin(), labels.end());
        return TMetricFabric(Reg, Name.c_str(), lbl);
    }
};

class TGauge {
    friend TMetricsRegistry;
    friend TMetricFabric<TGauge>;
    struct TImpl;
    TImpl *Impl;

    TGauge() = delete;
    TGauge(TImpl *impl) : Impl(impl) { }
    TGauge(TMetricsRegistry &reg, const char *name, const std::map<const std::string, const std::string> &labels = {});
public:
    ~TGauge();
    TGauge(TGauge &&other) : Impl(other.Impl) {
        other.Impl = nullptr;
    }
    TGauge(const TGauge&) = delete;
    TGauge& operator=(uint64_t v);
    TGauge& operator+=(int);
    TGauge& operator++(int);
    TGauge& operator--(int);
    operator uint64_t() const;
};

class TCounter {
    friend TMetricsRegistry;
    friend TMetricFabric<TCounter>;
    struct TImpl;
    TImpl *Impl;

    TCounter() = delete;
    TCounter(TImpl *impl) : Impl(impl) { }
    TCounter(TMetricsRegistry &reg, const char *name, const std::map<const std::string, const std::string> &labels = {});
public:
    TCounter(TCounter &&other) : Impl(other.Impl) {
        other.Impl = nullptr;
    }
    TCounter &operator=(TCounter &&other) {
        Impl = other.Impl;
        other.Impl = nullptr;

        return *this;
    }
    TCounter(const TCounter &other) = delete;

    TCounter &operator=(const TCounter &other) = delete;

    ~TCounter();
    TCounter& operator+=(uint64_t v);
    TCounter& operator++(int);
    operator uint64_t() const;
};

class TMetricsRegistry {
public:
    struct TImpl;
    std::unique_ptr<TImpl> Impl;
    TMetricsRegistry(const std::map<const std::string, const std::string> &labels);
    ~TMetricsRegistry();

    TMetricFabric<TGauge> Version = TMetricFabric<TGauge>(*this, "version");

    TCounter Reloads       = TCounter(*this, "daemon_reloads_total");
    TCounter ShutdownTime  = TCounter(*this, "daemon_reload_shutdown_ms_total");
    TCounter RestoreTime  = TCounter(*this, "daemon_reload_restore_ms_total");
    TCounter ContainerLost = TCounter(*this, "containers_lost_total");
    TCounter VolumeLost    = TCounter(*this, "volumes_lost_total");

    TMetricFabric<TGauge> Containers = TMetricFabric<TGauge>(*this, "containers");
    TCounter ContainersCreated     = TCounter(*this, "containers_created_total");
    TCounter ContainersStarted     = TCounter(*this, "containers_started_total");
    TCounter ContainersFailedStart = TCounter(*this, "containers_start_failed_total");

    TMetricFabric<TCounter> Requests        = TMetricFabric<TCounter>(*this, "requests_total");
    TMetricFabric<TCounter> RequestWaitTime = TMetricFabric<TCounter>(*this, "requests_wait_ms_total");
    TMetricFabric<TCounter> RequestExecTime = TMetricFabric<TCounter>(*this, "requests_exec_ms_total");
    TMetricFabric<TCounter> RequestLockTime = TMetricFabric<TCounter>(*this, "requests_lock_ms_total");
    TMetricFabric<TGauge> RequestTopRunning = TMetricFabric<TGauge>(*this, "requests_top_running_ms");
    TMetricFabric<TGauge> RequestQueued     = TMetricFabric<TGauge>(*this, "requests_queued");

    TGauge EventsQueued = TGauge(*this, "events_queued");
    TGauge EventsTopWaiting = TGauge(*this, "events_top_waiting_ms");

    TGauge Clients = TGauge(*this, "clients");

    TCounter Rebalanced = TCounter(*this, "cpuset_rebalances_total");

    TMetricFabric<TGauge> Volumes = TMetricFabric<TGauge>(*this, "volumes");
    TCounter VolumesCreated = TCounter(*this, "volumes_created_total");

    TCounter Errors = TCounter(*this, "errors_total");
    TCounter CgErrors = TCounter(*this, "cgroup_errors_total");
    TCounter Warnings = TCounter(*this, "warnings_total");
    TCounter Taints = TCounter(*this, "taints_total");
};


extern std::unique_ptr<TMetricsRegistry> MetricsRegistry;


TError StartMetricsServer(const std::string &path);
void StopMetricsServer();
