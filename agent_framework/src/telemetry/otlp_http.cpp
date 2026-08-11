#include "agent/telemetry/otlp_http.hpp"

#include <chrono>
#include <memory>
#include <stdexcept>

#include <httplib.hpp>
#include <nlohmann/json.hpp>

namespace agent_framework::telemetry {
namespace {
struct Endpoint { bool tls{false}; std::string host; int port{0}; std::string prefix; };
Endpoint parse(std::string value) {
    Endpoint out; std::string scheme;
    if (value.rfind("http://",0)==0) { scheme="http"; value.erase(0,7); out.port=80; }
    else if(value.rfind("https://",0)==0) { scheme="https"; value.erase(0,8); out.tls=true; out.port=443; }
    else throw std::invalid_argument("OTLP endpoint must use http or https");
    const auto slash=value.find('/'); const auto authority=value.substr(0,slash);
    out.prefix=slash==std::string::npos?"":value.substr(slash);
    const auto colon=authority.rfind(':');
    if(colon!=std::string::npos){out.host=authority.substr(0,colon);out.port=std::stoi(authority.substr(colon+1));}
    else out.host=authority;
    if(out.host.empty()||out.port<1||out.port>65535)throw std::invalid_argument("invalid OTLP endpoint");
    return out;
}
nlohmann::json attributes(const std::map<std::string,std::string>& values) {
    auto out=nlohmann::json::array();for(const auto&[key,value]:values)out.push_back({{"key",key},{"value",{{"stringValue",value}}}});return out;
}
bool post(const OtlpHttpOptions& options,const std::string& path,const nlohmann::json& body,std::string& error) {
    const auto endpoint=parse(options.endpoint);
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    if(endpoint.tls){error="HTTPS OTLP requires OpenSSL";return false;}
#endif
    const std::string origin = std::string(endpoint.tls ? "https://" : "http://") +
                               endpoint.host + ":" + std::to_string(endpoint.port);
    httplib::Client client(origin.c_str());
    client.set_connection_timeout(options.connect_timeout_ms / 1000,
                                  (options.connect_timeout_ms % 1000) * 1000);
    client.set_read_timeout(options.request_timeout_ms / 1000,
                            (options.request_timeout_ms % 1000) * 1000);
    const auto request_path = endpoint.prefix + path;
    const auto response=client.Post(request_path.c_str(),body.dump(),"application/json");
    if(!response){error="OTLP transport error";return false;}
    if(response->status<200||response->status>=300){error="OTLP HTTP status "+std::to_string(response->status);return false;}
    return true;
}
}

OtlpHttpTelemetrySink::OtlpHttpTelemetrySink(OtlpHttpOptions options):options_(std::move(options)) {
    (void)parse(options_.endpoint);if(options_.batch_size==0)throw std::invalid_argument("OTLP batch_size must be positive");
}
bool OtlpHttpTelemetrySink::export_span(const SpanRecord& span){std::lock_guard lock(mutex_);spans_.push_back(span);return spans_.size()+metrics_.size()<options_.batch_size||flush_locked();}
bool OtlpHttpTelemetrySink::export_metric(const MetricResult& metric){std::lock_guard lock(mutex_);metrics_.push_back(metric);return spans_.size()+metrics_.size()<options_.batch_size||flush_locked();}
bool OtlpHttpTelemetrySink::flush(){std::lock_guard lock(mutex_);return flush_locked();}
std::string OtlpHttpTelemetrySink::last_error()const{std::lock_guard lock(mutex_);return last_error_;}
bool OtlpHttpTelemetrySink::flush_locked(){
    if(spans_.empty()&&metrics_.empty()) return true;
    last_error_.clear();
    if(!spans_.empty()) {auto records=nlohmann::json::array();for(const auto&s:spans_)records.push_back({
        {"traceId",s.context.trace_id},{"spanId",s.context.span_id},{"parentSpanId",s.context.parent_span_id},
        {"name",s.name},{"attributes",attributes(s.attributes)},{"status",{{"message",s.status}}}});
        nlohmann::json body{{"resourceSpans",nlohmann::json::array({{{"resource",{{"attributes",nlohmann::json::array({{{"key","service.name"},{"value",{{"stringValue",options_.service_name}}}}})}}},{"scopeSpans",nlohmann::json::array({{{"scope",{{"name","taskflow-agent"}}},{"spans",records}}})}}})}};
        if(!post(options_,"/v1/traces",body,last_error_)) return false;
        spans_.clear();
    }
    if(!metrics_.empty()){auto records=nlohmann::json::array();for(const auto&m:metrics_)records.push_back({{"name",m.metric_name},{"unit",m.unit},{"gauge",{{"dataPoints",nlohmann::json::array({{{"asDouble",m.value},{"attributes",nlohmann::json::array()}}})}}}});
        nlohmann::json body{{"resourceMetrics",nlohmann::json::array({{{"resource",{{"attributes",nlohmann::json::array({{{"key","service.name"},{"value",{{"stringValue",options_.service_name}}}}})}}},{"scopeMetrics",nlohmann::json::array({{{"scope",{{"name","taskflow-agent"}}},{"metrics",records}}})}}})}};
        if(!post(options_,"/v1/metrics",body,last_error_)) return false;
        metrics_.clear();
    }
    return true;
}
}  // namespace agent_framework::telemetry
