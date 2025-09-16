#include "metrics.hpp"

#include "util/log.hpp"

#include "rpc.hpp"
#include "util/cpp-httplib/httplib.h"
#include "util/nlohmann-safe/json.hpp"
#include "util/unix.hpp"
#include "util/prometheus-cpp-lite/core/include/prometheus/counter.h"
#include "util/prometheus-cpp-lite/core/include/prometheus/family.h"
#include "util/prometheus-cpp-lite/core/include/prometheus/gauge.h"
#include "util/prometheus-cpp-lite/core/include/prometheus/registry.h"
#include "util/prometheus-cpp-lite/core/include/prometheus/summary.h"
#include "util/prometheus-cpp-lite/core/include/prometheus/text_serializer.h"

#include <iostream>
#include <sstream>
#include <thread>
#include <string>

using TLabels = prometheus::Family::Labels;

struct TMetricsRegistry::TImpl {
    prometheus::Registry reg;
    TLabels Lbls;

    TImpl(const TLabels &lbls)
        : Lbls(lbls) { }
};

template<typename T>
struct TMetricImpl {
    using Family = typename T::Family;
    T &M;

    TMetricImpl(Family &base, const TLabels &labels)
        : M(base.Add(labels))
        {  }

    TMetricImpl(TMetricsRegistry &reg, const char *name, const TLabels &labels)
        : TMetricImpl(
            Family::Build(reg.Impl->reg, name, "", reg.Impl->Lbls),
            labels) {  }

    ~TMetricImpl() { }
};

// Gauge
struct TGauge::TImpl : public TMetricImpl< prometheus::Gauge<uint64_t> > {
    using TMetricImpl::TMetricImpl;
};

TGauge::TGauge(TMetricsRegistry &reg, const char *name, const TLabels &labels)
    : Impl(new TImpl(reg, name, labels)) { }

TGauge::~TGauge() {
    if (Impl)
        delete Impl;
}

TGauge& TGauge::operator++(int) {
    Impl->M.Increment();
    return *this;
}

TGauge& TGauge::operator--(int) {
    Impl->M.Decrement();
    return *this;
}

TGauge &TGauge::operator=(uint64_t v) {
    Impl->M.Set(v);
    return *this;
}

TGauge &TGauge::operator+=(int v) {
    Impl->M += v;
    return *this;
}

TGauge::operator uint64_t() const { return Impl->M.Get(); }

// Counter
struct TCounter::TImpl : public TMetricImpl< prometheus::Counter<uint64_t> > {
    using TMetricImpl::TMetricImpl;
    ~TImpl() { }
};

TCounter::TCounter(TMetricsRegistry &reg, const char *name, const TLabels &labels)
    : Impl(new TImpl(reg, name, labels)) { }
TCounter::~TCounter() {
    if (Impl)
        delete Impl;
}

TCounter &TCounter::operator+=(uint64_t v) {
    Impl->M += v;
    return *this;
}

TCounter& TCounter::operator++(int) {
    Impl->M.Increment();
    return *this;
}


TCounter::operator uint64_t() const { return Impl->M.Get(); }

TMetricsRegistry::TMetricsRegistry(const std::map<const std::string, const std::string> &labels)
    : Impl(new TImpl(labels)) {
}

TMetricsRegistry::~TMetricsRegistry() = default;

std::unique_ptr<TMetricsRegistry> MetricsRegistry;

static void Handle(const httplib::Request&, httplib::Response& res) {
    try {
        L("Metrics request");
        prometheus::TextSerializer textSerializer;
        std::stringstream ss;

        (void)RpcRequestsTopRunningTime();
        MetricsRegistry->EventsTopWaiting = EventQueueTopWaitingTime();
        MetricsRegistry->Reloads += Statistics->PortoStarts - MetricsRegistry->Reloads;
        MetricsRegistry->ShutdownTime += Statistics->ShutdownTime - MetricsRegistry->ShutdownTime;
        MetricsRegistry->RestoreTime += Statistics->RestoreTime - MetricsRegistry->RestoreTime;
        MetricsRegistry->Errors += Statistics->Errors - MetricsRegistry->Errors;
        MetricsRegistry->CgErrors += Statistics->CgErrors - MetricsRegistry->CgErrors;
        MetricsRegistry->Warnings += Statistics->Warns - MetricsRegistry->Warnings;
        MetricsRegistry->Taints += Statistics->Taints - MetricsRegistry->Taints;

        textSerializer.Serialize(ss, MetricsRegistry->Impl->reg.Collect());

        res.set_content(ss.str(), "text/plain");
    } catch (const std::exception& e) {
        L_ERR("Prometheus handle failed: {}", e.what());
    }  catch (...) {
        L_ERR("Prometheus handle failed with unknown error");
    }
}

static httplib::Server Server;
static std::thread ServerThread;

TError StartMetricsServer(const std::string &path) {
    (void)TPath(path).Unlink();

    Server.set_address_family(AF_UNIX);
    if (!Server.bind_to_port(path, 80, 0) && false)
        return TError("Failed to start metrics server");

    Server.new_task_queue = [] { return new httplib::ThreadPool(/* threads */ 1, /* max queued request */ 32); };

    ServerThread = std::thread([]() {
        SetProcessName("portod-MT");
        Server.Get("/metrics", Handle);
        Server.listen_after_bind();
    });
    L_SYS("Run metrics server on {}", path);
    return OK;
}

void StopMetricsServer() {
    Server.stop();
    if (ServerThread.joinable())
        ServerThread.join();

    MetricsRegistry = nullptr;
}
