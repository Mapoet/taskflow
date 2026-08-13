#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/distributed/object_store.hpp"
#include "agent/tool_runtime/types.hpp"

namespace agent_framework { class AuditSink; }

namespace agent_framework::tool_runtime {

enum class IncrementalStreamKind { Stdout, Stderr, Log, PartialResult, Checkpoint, Artifact };
enum class IncrementalStreamState { Open, Sealed, Aborted };
enum class IncrementalStatus { Committed, AlreadyApplied, NotFound, Conflict, Sealed, Invalid, LimitExceeded, IntegrityFailure, Error };

struct IncrementalChunkRef {
    std::uint64_t sequence{0}, offset{0}, size{0};
    std::string digest, media_type;
};

struct RedactionReceipt {
    std::string policy_revision;
    std::vector<std::string> rule_ids;
    std::uint64_t matches{0};
};

struct IncrementalManifest {
    std::string tenant_id, stream_id, run_id, invocation_id, attempt_id;
    IncrementalStreamKind kind{IncrementalStreamKind::PartialResult};
    IncrementalStreamState state{IncrementalStreamState::Open};
    std::uint64_t revision{0}, total_size{0};
    std::string parent_digest, manifest_digest, media_type, retention_class;
    RedactionReceipt redaction;
    std::vector<IncrementalChunkRef> chunks;
    bool truncated{false}, pinned{false};
};

struct IncrementalOpenRequest {
    std::string tenant_id, stream_id, run_id, invocation_id, attempt_id, media_type{"text/plain"};
    IncrementalStreamKind kind{IncrementalStreamKind::PartialResult};
    std::string retention_class{"standard"}, redaction_policy_revision{"default-v1"};
};

struct IncrementalAppendRequest {
    std::string tenant_id, stream_id, idempotency_key, bytes;
    std::uint64_t expected_revision{0};
};

struct IncrementalResult {
    IncrementalStatus status{IncrementalStatus::Error};
    IncrementalManifest manifest;
    bool deduplicated{false};
    std::string error;
    explicit operator bool() const noexcept { return status == IncrementalStatus::Committed || status == IncrementalStatus::AlreadyApplied; }
};

struct IncrementalPreview {
    std::string text, manifest_digest;
    std::uint64_t total_bytes{0}, included_bytes{0};
    bool truncated{false}, integrity_verified{false}, redacted{false};
    std::vector<IncrementalChunkRef> references;
};

PartialResultRef partial_result_ref(const IncrementalManifest&, bool information_gain = true);
class IncrementalResultStore;

class IncrementalResultViewAssembler {
public:
    IncrementalResultViewAssembler(IncrementalResultStore&, std::size_t maximum_streams = 8,
                                   std::size_t maximum_total_bytes = 32U * 1024U);
    nlohmann::json assemble(std::string_view tenant, const std::vector<PartialResultRef>&);
private:
    IncrementalResultStore& store_; std::size_t maximum_streams_, maximum_total_bytes_;
};

struct IncrementalLimits {
    std::size_t chunk_bytes{256U * 1024U}, maximum_stream_bytes{64U * 1024U * 1024U};
    std::size_t maximum_append_bytes{4U * 1024U * 1024U}, preview_bytes{16U * 1024U};
};

struct RedactionRule { std::string id, literal, replacement{"[REDACTED]"}; };
struct IncrementalMetrics {
    std::uint64_t streams_opened{0}, bytes_appended{0}, chunks_written{0}, chunks_deduplicated{0};
    std::uint64_t cas_conflicts{0}, redaction_matches{0}, preview_truncations{0};
    std::uint64_t integrity_failures{0}, orphan_objects{0};
    std::uint64_t objects_deleted{0}, bytes_reclaimed{0}, legal_hold_skips{0}, cross_append_matches{0};
};
struct IncrementalGcPolicy { std::int64_t grace_before_ms{0}; std::size_t maximum_objects{100}; bool dry_run{false}; };
struct IncrementalGcResult { std::uint64_t candidates{0}, deleted{0}, reclaimed_bytes{0}, quarantined{0}; std::string error; };

nlohmann::json encode(const IncrementalManifest&);
std::optional<IncrementalManifest> decode_incremental_manifest(const nlohmann::json&, std::string* error = nullptr);
std::string_view name(IncrementalStreamKind);
std::string_view name(IncrementalStreamState);

class IncrementalResultStore {
public:
    virtual ~IncrementalResultStore() = default;
    virtual IncrementalResult open(const IncrementalOpenRequest&) = 0;
    virtual IncrementalResult append(const IncrementalAppendRequest&) = 0;
    virtual IncrementalResult seal(std::string_view tenant, std::string_view stream, std::uint64_t expected_revision) = 0;
    virtual IncrementalResult abort(std::string_view tenant, std::string_view stream, std::uint64_t expected_revision) = 0;
    virtual std::optional<IncrementalManifest> load(std::string_view tenant, std::string_view stream) = 0;
    virtual IncrementalPreview preview(std::string_view tenant, std::string_view stream, std::size_t maximum_bytes = 0) = 0;
    virtual bool verify(std::string_view tenant, std::string_view stream, std::string* error = nullptr) = 0;
};

class SQLiteIncrementalResultStore final : public IncrementalResultStore {
public:
    SQLiteIncrementalResultStore(std::string database_path, distributed::ObjectStore&, IncrementalLimits = {},
                                 std::vector<RedactionRule> = {}, std::shared_ptr<AuditSink> = {});
    ~SQLiteIncrementalResultStore();
    IncrementalResult open(const IncrementalOpenRequest&) override;
    IncrementalResult append(const IncrementalAppendRequest&) override;
    IncrementalResult seal(std::string_view, std::string_view, std::uint64_t) override;
    IncrementalResult abort(std::string_view, std::string_view, std::uint64_t) override;
    std::optional<IncrementalManifest> load(std::string_view, std::string_view) override;
    IncrementalPreview preview(std::string_view, std::string_view, std::size_t = 0) override;
    bool verify(std::string_view, std::string_view, std::string* = nullptr) override;
    IncrementalMetrics metrics() const;
    std::uint64_t mark_orphans(std::int64_t older_than_ms);
    IncrementalGcResult collect(const IncrementalGcPolicy&);
    std::uint64_t reconcile_objects(std::string_view tenant, std::size_t maximum = 1000);
    bool set_pinned(std::string_view tenant, std::string_view stream, bool pinned);
private:
    void migrate();
    IncrementalResult transition(std::string_view, std::string_view, std::uint64_t, IncrementalStreamState);
    std::string redact(std::string_view, RedactionReceipt&) const;
    std::string redact_stream(std::string_view, std::string_view, std::string_view,
                              RedactionReceipt&);
    void audit(std::string_view event, const IncrementalManifest&, std::string_view outcome, std::string_view error = {}) const noexcept;
    std::string path_; void* db_{nullptr}; distributed::ObjectStore& objects_; IncrementalLimits limits_;
    std::vector<RedactionRule> rules_; std::shared_ptr<AuditSink> audit_; mutable std::mutex mutex_; IncrementalMetrics metrics_;
};

} // namespace agent_framework::tool_runtime
