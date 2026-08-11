#include <cassert>

#include "agent/distributed/durable_queue.hpp"
#include "agent/distributed/object_store.hpp"
#include "agent/distributed/schema_migration.hpp"
#include "agent/distributed/leader_election.hpp"
#include <filesystem>
#include <fstream>
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
    assert(queue.enqueue(task));
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

    const auto path=(std::filesystem::temp_directory_path()/"phase4-durable-queue.sqlite").string();std::filesystem::remove(path);{
      distributed::SQLiteDurableQueue first_store(path),second_store(path);auto durable=task;durable.task_id="durable-a";durable.idempotency_key="durable-key";assert(first_store.enqueue(durable));std::string duplicate_error;assert(second_store.enqueue(durable,&duplicate_error));durable.payload_digest="sha256:conflict";assert(!second_store.enqueue(durable,&duplicate_error));assert(duplicate_error=="queue idempotency conflict");auto first_lease=first_store.claim("worker-a","tenant-a",1000,10);assert(first_lease&&first_lease->fencing_token==1);assert(!second_store.claim("worker-b","tenant-a",1005,10));auto takeover=second_store.claim("worker-b","tenant-a",1010,10);assert(takeover&&takeover->fencing_token==2);assert(!first_store.ack("durable-a","worker-a",1));assert(second_store.ack("durable-a","worker-b",2));
    }
    {distributed::SQLiteDurableQueue reopened(path);auto recovered=reopened.inspect("durable-a");assert(recovered&&recovered->state==distributed::QueueState::Completed&&recovered->fencing_token==2);}
    std::filesystem::remove(path);
    const auto registry_path=(std::filesystem::temp_directory_path()/"phase4-worker-registry.sqlite").string();std::filesystem::remove(registry_path);{
      distributed::SQLiteWorkerRegistry registry_a(registry_path),registry_b(registry_path);distributed::WorkerRecord worker{"worker-1","instance-a","sha256:caps",0,100};auto generation_a=registry_a.register_worker(worker);assert(generation_a&&*generation_a==1);assert(registry_a.heartbeat("worker-1","instance-a",1,110));worker.instance_id="instance-b";worker.heartbeat_at_ms=120;auto generation_b=registry_b.register_worker(worker);assert(generation_b&&*generation_b==2);assert(!registry_a.heartbeat("worker-1","instance-a",1,130));assert(registry_b.heartbeat("worker-1","instance-b",2,130));assert(registry_a.alive(125).size()==1&&registry_a.alive(131).empty());assert(registry_a.set_quota("tenant-a",2));assert(registry_a.reserve("tenant-a"));assert(registry_b.reserve("tenant-a"));assert(!registry_a.reserve("tenant-a"));assert(!registry_a.set_quota("tenant-a",1));assert(registry_b.release("tenant-a"));auto quota=registry_a.quota("tenant-a");assert(quota&&quota->active==1&&quota->maximum_active==2);
    }
    {distributed::SQLiteWorkerRegistry reopened(registry_path);auto quota=reopened.quota("tenant-a");assert(quota&&quota->active==1);assert(reopened.release("tenant-a"));}
    std::filesystem::remove(registry_path);
    const auto control_path=(std::filesystem::temp_directory_path()/"phase4-control-plane.sqlite").string();std::filesystem::remove(control_path);{
      distributed::SQLiteWorkerRegistry registry(control_path);distributed::SQLiteDurableQueue scheduler_a(control_path),scheduler_b(control_path);assert(registry.set_quota("tenant-a",1));auto one=task;one.task_id="quota-a";one.idempotency_key="quota-a";auto two=task;two.task_id="quota-b";two.idempotency_key="quota-b";assert(scheduler_a.enqueue(one)&&scheduler_a.enqueue(two));auto lease_a=scheduler_a.claim_with_quota("worker-a","tenant-a",2000,10);assert(lease_a&&registry.quota("tenant-a")->active==1);assert(!scheduler_b.claim_with_quota("worker-b","tenant-a",2001,10));auto takeover=scheduler_b.claim_with_quota("worker-b","tenant-a",2010,10);assert(takeover&&takeover->task.task_id==lease_a->task.task_id&&takeover->fencing_token==lease_a->fencing_token+1&&registry.quota("tenant-a")->active==1);assert(!scheduler_a.ack_with_quota(lease_a->task.task_id,"worker-a",lease_a->fencing_token));assert(scheduler_b.ack_with_quota(takeover->task.task_id,"worker-b",takeover->fencing_token));assert(registry.quota("tenant-a")->active==0);auto lease_b=scheduler_a.claim_with_quota("worker-a","tenant-a",2020,10);assert(lease_b&&lease_b->task.task_id!="quota-a");assert(scheduler_a.nack_with_quota(lease_b->task.task_id,"worker-a",lease_b->fencing_token,2021));assert(registry.quota("tenant-a")->active==0);
    }
    std::filesystem::remove(control_path);
    const auto objects=(std::filesystem::temp_directory_path()/"phase4-object-store");std::filesystem::remove_all(objects);{
      distributed::FilesystemObjectStore store(objects,1024);std::string object_error;auto ref=store.put("tenant-a",std::string("artifact\0bytes",14),"application/octet-stream",{},&object_error);assert(ref&&ref->digest.rfind("sha256:",0)==0&&ref->size==14);auto replay=store.put("tenant-a",std::string("artifact\0bytes",14),"application/octet-stream",ref->digest,&object_error);assert(replay&&replay->digest==ref->digest);auto loaded=store.get(*ref,&object_error);assert(loaded&&*loaded==std::string("artifact\0bytes",14));assert(!store.put("tenant-a","different","text/plain",ref->digest,&object_error));auto other=store.put("tenant-b",std::string("artifact\0bytes",14),"application/octet-stream",{},&object_error);assert(other&&other->digest==ref->digest);const auto object_path=objects/"tenant-a"/ref->digest.substr(7,2)/ref->digest.substr(7);{std::ofstream corrupt(object_path,std::ios::binary|std::ios::trunc);corrupt<<"tampered-value";}assert(!store.get(*ref,&object_error));
    }
    std::filesystem::remove_all(objects);
    const auto schema_path=(std::filesystem::temp_directory_path()/"phase4-schema-migration.sqlite").string();std::filesystem::remove(schema_path);{
      distributed::SQLiteSchemaMigrationCoordinator first(schema_path),second(schema_path);std::string migration_error;assert(first.initialize("distributed-control",1,1,&migration_error));assert(!first.initialize("distributed-control",1,1,&migration_error));auto lease1=first.begin("distributed-control",1,"migrator-a",3000,10,&migration_error);assert(lease1&&lease1->fencing_token==1);assert(!second.begin("distributed-control",1,"migrator-b",3005,10,&migration_error));auto lease2=second.begin("distributed-control",1,"migrator-b",3010,10,&migration_error);assert(lease2&&lease2->fencing_token==2);assert(!first.commit("distributed-control","migrator-a",lease1->fencing_token,2,1,&migration_error));assert(second.renew("distributed-control","migrator-b",lease2->fencing_token,3011,10));assert(second.commit("distributed-control","migrator-b",lease2->fencing_token,2,1,&migration_error));assert(first.reader_compatible("distributed-control",1,&migration_error));auto lease3=first.begin("distributed-control",2,"migrator-c",3020,10,&migration_error);assert(lease3&&first.commit("distributed-control","migrator-c",lease3->fencing_token,3,2,&migration_error));assert(!second.reader_compatible("distributed-control",1,&migration_error));assert(second.reader_compatible("distributed-control",2,&migration_error));
    }
    {distributed::SQLiteSchemaMigrationCoordinator reopened(schema_path);auto schema=reopened.inspect("distributed-control");assert(schema&&schema->version==3&&schema->minimum_reader_version==2&&schema->migration_owner.empty());}
    std::filesystem::remove(schema_path);
    const auto leader_path=(std::filesystem::temp_directory_path()/"phase4-leader-election.sqlite").string();std::filesystem::remove(leader_path);{
      distributed::SQLiteLeaderElection first(leader_path),second(leader_path);auto leader1=first.acquire("scheduler","instance-a",4000,10);assert(leader1&&leader1->fencing_token==1&&first.is_current(*leader1,4001));assert(!second.acquire("scheduler","instance-b",4005,10));auto leader2=second.acquire("scheduler","instance-b",4010,10);assert(leader2&&leader2->fencing_token==2);assert(!first.is_current(*leader1,4010)&&!first.renew(*leader1,4010,10)&&!first.release(*leader1));assert(second.renew(*leader2,4011,20)&&second.is_current(*leader2,4020));assert(second.release(*leader2));auto leader3=first.acquire("scheduler","instance-c",4021,10);assert(leader3&&leader3->fencing_token==3);
    }
    {distributed::SQLiteLeaderElection reopened(leader_path);auto leader=reopened.inspect("scheduler");assert(leader&&leader->owner=="instance-c"&&leader->fencing_token==3);}
    std::filesystem::remove(leader_path);
    return 0;
}
