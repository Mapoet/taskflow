#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

#include <httplib.hpp>
#include <nlohmann/json.hpp>

#include "agent/telemetry/otlp_http.hpp"

#if !defined(_WIN32)
#include <sys/wait.h>
#include <unistd.h>
#endif

int main() {
#if defined(_WIN32)
    return 0;
#else
    using namespace agent_framework;
    httplib::Server collector;
    std::atomic<int> traces{0}, metrics{0};
    collector.Post("/v1/traces", [&](const httplib::Request& request, httplib::Response& response) {
        const auto body = nlohmann::json::parse(request.body);
        assert(body.dump().find("4bf92f3577b34da6a3ce929d0e0e4736") != std::string::npos);
        ++traces; response.status = 200; response.set_content("{}", "application/json");
    });
    collector.Post("/v1/metrics", [&](const httplib::Request& request, httplib::Response& response) {
        const auto body = nlohmann::json::parse(request.body);
        assert(body.dump().find("sandbox.wall_time") != std::string::npos);
        ++metrics; response.status = 200; response.set_content("{}", "application/json");
    });
    const int port = collector.bind_to_any_port("127.0.0.1"); assert(port > 0);
    std::thread server([&] { collector.listen_after_bind(); });
    const pid_t child = ::fork(); assert(child >= 0);
    if (child == 0) {
        telemetry::OtlpHttpTelemetrySink sink({"http://127.0.0.1:" + std::to_string(port), 8,
                                               "phase4-loopback", 2000, 5000});
        telemetry::SpanRecord span; span.context.trace_id = "4bf92f3577b34da6a3ce929d0e0e4736";
        span.context.span_id = "00f067aa0ba902b7"; span.name = "sandbox.exec";
        span.status = "ok"; span.attributes = {{"run.state", "complete"}};
        telemetry::MetricResult metric; metric.metadata.identity.tenant_id = "tenant-a";
        metric.metric_name = "sandbox.wall_time"; metric.value = 10; metric.unit = "ms";
        metric.sample_count = 1;
        if (!sink.export_span(span) || !sink.export_metric(metric) || !sink.flush()) _exit(2);
        _exit(0);
    }
    int status = 0; assert(::waitpid(child, &status, 0) == child);
    for (int i = 0; i < 50 && (traces.load() != 1 || metrics.load() != 1); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    collector.stop(); server.join();
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(traces.load() == 1 && metrics.load() == 1);
    return 0;
#endif
}
