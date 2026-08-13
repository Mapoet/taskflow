#include "agent/tool_runtime/incremental_result_store.hpp"
#include "agent/observability/audit.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

using namespace agent_framework;
using namespace agent_framework::tool_runtime;

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("af-ltw7-" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    distributed::FilesystemObjectStore objects(root / "objects", 2U * 1024U * 1024U);
    IncrementalLimits limits; limits.chunk_bytes = 4; limits.maximum_append_bytes = 1024;
    limits.maximum_stream_bytes = 4096; limits.preview_bytes = 7;
    auto audit = std::make_shared<TestAuditSink>();
    SQLiteIncrementalResultStore store((root / "streams.sqlite").string(), objects, limits,
        {{"secret", "TOKEN-123", "[MASKED]"}}, audit);

    IncrementalOpenRequest open;
    open.tenant_id="tenant-a"; open.stream_id="stream-1"; open.run_id="run-1";
    open.invocation_id="inv-1"; open.attempt_id="attempt-1";
    open.kind=IncrementalStreamKind::Stdout;
    auto created=store.open(open); assert(created); assert(created.manifest.revision==0);
    assert(store.open(open).status==IncrementalStatus::AlreadyApplied);

    IncrementalAppendRequest append{"tenant-a","stream-1","append-1","hello TOKEN-123 world",0};
    auto first=store.append(append); assert(first); assert(first.manifest.revision==1);
    assert(first.manifest.chunks.size()>1); assert(first.manifest.redaction.matches==1);
    assert(store.append(append).status==IncrementalStatus::AlreadyApplied);
    auto stale=append; stale.idempotency_key="append-stale";
    assert(store.append(stale).status==IncrementalStatus::Conflict);

    auto preview=store.preview("tenant-a","stream-1");
    assert(preview.integrity_verified); assert(preview.truncated); assert(preview.text=="hello [");
    assert(preview.text.find("TOKEN-123")==std::string::npos);
    assert(store.verify("tenant-a","stream-1"));
    assert(!store.load("tenant-b","stream-1"));
    IncrementalResultViewAssembler assembler(store,2,6);
    auto view=assembler.assemble("tenant-a",{partial_result_ref(first.manifest)});
    assert(view["included_bytes"]==6); assert(view["streams"][0]["preview"]=="hello ");
    assert(view.dump().find("TOKEN-123")==std::string::npos);

    auto sealed=store.seal("tenant-a","stream-1",1); assert(sealed);
    auto late=append; late.expected_revision=2; late.idempotency_key="late";
    assert(store.append(late).status==IncrementalStatus::Sealed);

    { // durable restart restores the sealed head and immutable chunks.
        SQLiteIncrementalResultStore restarted((root / "streams.sqlite").string(), objects, limits,
            {{"secret", "TOKEN-123", "[MASKED]"}});
        auto restored=restarted.load("tenant-a","stream-1"); assert(restored);
        assert(restored->state==IncrementalStreamState::Sealed);
        assert(restored->manifest_digest==sealed.manifest.manifest_digest);
        assert(restarted.verify("tenant-a","stream-1"));
    }

    auto metrics=store.metrics(); assert(metrics.bytes_appended>0);
    assert(metrics.cas_conflicts==1); assert(metrics.redaction_matches==1);
    assert(metrics.preview_truncations==2); assert(!audit->events().empty());
    std::filesystem::remove_all(root);
    std::cout << "phase4 incremental result store tests passed\n";
}
