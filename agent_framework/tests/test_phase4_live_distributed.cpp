#include <cassert>

#include "agent/distributed/durable_queue.hpp"
#include "agent/live/certification.hpp"

int main() {
    using namespace agent_framework;
    live::EnvironmentManifest environment;
    environment.metadata.identity.tenant_id = "certification";
    environment.metadata.identity.task_id = "nightly";
    environment.os = "linux";
    environment.build_digest = "sha256:build";
    environment.git_revision = "revision-a";
    environment.config_digest = "sha256:config";
    environment.provider_revision = "provider-v1";
    environment.endpoint_class = "staging";
    environment.memory_policy_revision = "memory-v1";
    environment.provider_generation = 7;
    environment.secret_refs = {"vault://live/key"};
    live::ExecutionAttestation skipped;
    skipped.cell_id = "real-mcp";
    skipped.required = true;
    skipped.reason = "credential unavailable";
    auto blocked = live::certify(environment, {skipped}, "2026-08-09", "2026-08-10");
    assert(!blocked.certified);
    live::ExecutionAttestation executed;
    executed.cell_id = "real-mcp";
    executed.required = true;
    executed.executed = true;
    executed.passed = true;
    executed.evidence_digests = {"sha256:evidence"};
    auto passed = live::certify(environment, {executed}, "2026-08-09", "2026-08-10");
    assert(passed.certified);
    assert(live::certification_valid_for(passed, environment, "2026-08-09"));
    environment.provider_revision = "provider-v2";
    assert(!live::certification_valid_for(passed, environment, "2026-08-09"));

    distributed::InMemoryDurableQueue queue;
    distributed::QueueTask task;
    task.task_id = "task-a";
    task.tenant_id = "tenant-a";
    task.idempotency_key = "idem-a";
    task.payload_digest = "sha256:payload";
    task.priority = 10;
    assert(queue.enqueue(task));
    assert(!queue.enqueue(task));
    auto first = queue.claim("worker-a", "tenant-a", 100, 50);
    assert(first && first->fencing_token == 1);
    assert(!queue.claim("worker-b", "tenant-a", 120, 50));
    auto reclaimed = queue.claim("worker-b", "tenant-a", 151, 50);
    assert(reclaimed && reclaimed->fencing_token == 2);
    assert(!queue.ack("task-a", "worker-a", first->fencing_token));
    assert(queue.renew("task-a", "worker-b", reclaimed->fencing_token, 160, 50));
    assert(queue.ack("task-a", "worker-b", reclaimed->fencing_token));
    assert(queue.inspect("task-a")->state == distributed::QueueState::Completed);

    distributed::QueueTask retry = task;
    retry.task_id = "task-b";
    retry.idempotency_key = "idem-b";
    retry.max_attempts = 1;
    assert(queue.enqueue(retry));
    auto lease = queue.claim("worker-a", "tenant-a", 200, 10);
    assert(lease);
    assert(queue.nack("task-b", "worker-a", lease->fencing_token, 201));
    assert(queue.inspect("task-b")->state == distributed::QueueState::DeadLetter);

    distributed::QueueTask abandoned = task;
    abandoned.task_id = "task-c";
    abandoned.idempotency_key = "idem-c";
    abandoned.max_attempts = 1;
    assert(queue.enqueue(abandoned));
    auto abandoned_lease = queue.claim("worker-a", "tenant-a", 300, 10);
    assert(abandoned_lease && abandoned_lease->task.task_id == "task-c");
    assert(!queue.claim("worker-b", "tenant-a", 311, 10));
    assert(queue.inspect("task-c")->state == distributed::QueueState::DeadLetter);
    return 0;
}
