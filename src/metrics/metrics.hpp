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

    TCounter Reloads       = TCounter(*this, "reload_count");
    TCounter ShutdownTime  = TCounter(*this, "reload_shutdown_ms");
    TCounter RestoreTime  = TCounter(*this, "reload_restore_ms");
    TCounter ContainerLost = TCounter(*this, "container_lost_count");
    TCounter VolumeLost    = TCounter(*this, "volume_lost_count");

    TMetricFabric<TGauge> Containers = TMetricFabric<TGauge>(*this, "container_count");
    TCounter ContainersCreated     = TCounter(*this, "container_created_rate");
    TCounter ContainersStarted     = TCounter(*this, "container_started_rate");
    TCounter ContainersFailedStart = TCounter(*this, "container_start_failed_rate");

    TMetricFabric<TCounter> Requests        = TMetricFabric<TCounter>(*this, "request_rate");
    TMetricFabric<TCounter> RequestWaitTime = TMetricFabric<TCounter>(*this, "request_wait_time");
    TMetricFabric<TCounter> RequestExecTime = TMetricFabric<TCounter>(*this, "request_exec_time");
    TMetricFabric<TCounter> RequestLockTime = TMetricFabric<TCounter>(*this, "request_lock_time");
    TMetricFabric<TGauge> RequestTopRunning = TMetricFabric<TGauge>(*this, "request_top_running_time");
    TMetricFabric<TGauge> RequestQueued     = TMetricFabric<TGauge>(*this, "request_queued");

    TGauge EventsQueued = TGauge(*this, "events_queued");

    TGauge Clients = TGauge(*this, "client_count");

    TCounter Rebalanced = TCounter(*this, "cpuset_rebalances_total");

    TMetricFabric<TGauge> Volumes = TMetricFabric<TGauge>(*this, "volume_count");
    TCounter VolumesCreated = TCounter(*this, "volume_created_rate");

    TCounter Errors = TCounter(*this, "error_total_count");
    TCounter CgErrors = TCounter(*this, "error_cgroup_count");
    TCounter Warnings = TCounter(*this, "warning_total_count");
    TCounter Taints = TCounter(*this, "taint_total_count");
};


extern std::unique_ptr<TMetricsRegistry> MetricsRegistry;


TError StartMetricsServer(const std::string &path);
void StopMetricsServer();
