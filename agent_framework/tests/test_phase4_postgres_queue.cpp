#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

#include "agent/distributed/postgres_durable_queue.hpp"
#include "agent/distributed/postgres_control_plane.hpp"
#include "agent/distributed/postgres_rolling_migration.hpp"
#include "agent/internal/postgres_utils.hpp"

namespace {
using agent_framework::distributed::PostgresDurableQueue;
using agent_framework::distributed::QueueState;
using agent_framework::distributed::QueueTask;
namespace pg = agent_framework::internal::postgres;

QueueTask task(std::string id, std::string tenant, int priority = 0) {
    QueueTask value;
    value.task_id = std::move(id);
    value.tenant_id = std::move(tenant);
    value.idempotency_key = "idem-" + value.task_id;
    value.payload_digest = "sha256:" + value.task_id;
    value.priority = priority;
    value.max_attempts = 3;
    return value;
}
}  // namespace

int main(int argc, char** argv) {
    const auto* raw = std::getenv("AGENT_TEST_POSTGRES_CONNINFO");
    if(!raw || !*raw) {
        std::cerr << "AGENT_TEST_POSTGRES_CONNINFO is required\n";
        return 77;
    }
    const std::string conninfo(raw);
    const std::string mode = argc > 1 ? argv[1] : "full";
    if(mode == "seed-restart") {
        pg::Connection connection(conninfo);
        pg::exec(connection.get(),
            "DROP TABLE IF EXISTS agent_distributed_queue;"
            "DROP TABLE IF EXISTS agent_tenant_quota;");
        PostgresDurableQueue queue(conninfo);
        std::string error;
        assert(queue.enqueue(task("restart-a", "restart"), &error));
        const auto lease = queue.claim("worker-before-restart", "restart", 200, &error);
        assert(lease && lease->fencing_token == 1);
        return 0;
    }
    if(mode == "verify-restart") {
        PostgresDurableQueue queue(conninfo);
        std::string error;
        const auto durable = queue.inspect("restart-a", &error);
        assert(durable && durable->state == QueueState::Leased && durable->fencing_token == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        const auto recovered = queue.claim("worker-after-restart", "restart", 500, &error);
        assert(recovered && recovered->fencing_token == 2);
        assert(!queue.ack("restart-a", "worker-before-restart", 1, &error));
        assert(queue.ack("restart-a", "worker-after-restart", 2, &error));
        return 0;
    }
    {
        pg::Connection connection(conninfo);
        pg::exec(connection.get(),
            "DROP TABLE IF EXISTS agent_distributed_queue;"
            "DROP TABLE IF EXISTS agent_tenant_quota;"
            "DROP TABLE IF EXISTS agent_worker_registry;"
            "DROP TABLE IF EXISTS agent_leader_election;"
            "DROP TABLE IF EXISTS agent_rolling_migration;"
            "DROP TABLE IF EXISTS agent_component_schema;"
            "DROP TABLE IF EXISTS pg_rollback;"
            "DROP TABLE IF EXISTS pg_records_next;"
            "DROP TABLE IF EXISTS pg_records;");
    }

    PostgresDurableQueue first(conninfo), second(conninfo);
    std::string error;
    assert(first.set_quota("tenant-a", 1, &error));
    assert(first.enqueue(task("quota-a", "tenant-a", 10), &error));
    assert(first.enqueue(task("quota-b", "tenant-a", 1), &error));
    assert(first.enqueue(task("quota-a", "tenant-a", 10), &error));
    auto conflict = task("different-id", "tenant-a");
    conflict.idempotency_key = "idem-quota-a";
    assert(!first.enqueue(conflict, &error));
    assert(error == "queue idempotency conflict");

    const auto lease1 = first.claim_with_quota("worker-a", "tenant-a", 120, &error);
    assert(lease1 && lease1->task.task_id == "quota-a" && lease1->fencing_token == 1);
    assert(first.quota("tenant-a", &error)->active == 1);
    assert(!second.claim_with_quota("worker-b", "tenant-a", 120, &error));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto takeover = second.claim_with_quota("worker-b", "tenant-a", 500, &error);
    assert(takeover && takeover->task.task_id == "quota-a");
    assert(takeover->fencing_token == lease1->fencing_token + 1);
    assert(first.quota("tenant-a", &error)->active == 1);
    assert(!first.ack_with_quota("quota-a", "worker-a", lease1->fencing_token, &error));
    assert(second.ack_with_quota("quota-a", "worker-b", takeover->fencing_token, &error));
    assert(first.quota("tenant-a", &error)->active == 0);

    // Independent libpq connections concurrently claim distinct rows through SKIP LOCKED.
    assert(first.enqueue(task("parallel-a", "parallel", 5), &error));
    assert(first.enqueue(task("parallel-b", "parallel", 5), &error));
    std::optional<agent_framework::distributed::Lease> parallel1, parallel2;
    std::thread one([&] { parallel1 = first.claim("worker-1", "parallel", 1000, &error); });
    std::thread two([&] { parallel2 = second.claim("worker-2", "parallel", 1000, &error); });
    one.join();
    two.join();
    assert(parallel1 && parallel2);
    assert(parallel1->task.task_id != parallel2->task.task_id);

    // A real child process acquires and exits without ACK; another process recovers it.
    assert(first.enqueue(task("crash-a", "crash"), &error));
    int pipefd[2];
    assert(pipe(pipefd) == 0);
    const auto child = fork();
    assert(child >= 0);
    if(child == 0) {
        close(pipefd[0]);
        PostgresDurableQueue child_queue(conninfo);
        std::string child_error;
        const auto lease = child_queue.claim("crashing-worker", "crash", 120, &child_error);
        const std::uint64_t token = lease ? lease->fencing_token : 0;
        const auto written = write(pipefd[1], &token, sizeof(token));
        close(pipefd[1]);
        _exit(written == static_cast<ssize_t>(sizeof(token)) && token != 0 ? 0 : 2);
    }
    close(pipefd[1]);
    std::uint64_t child_token = 0;
    assert(read(pipefd[0], &child_token, sizeof(child_token)) ==
           static_cast<ssize_t>(sizeof(child_token)));
    close(pipefd[0]);
    int status = 0;
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(!first.claim("early-worker", "crash", 500, &error));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto recovered = second.claim("recovery-worker", "crash", 500, &error);
    assert(recovered && recovered->fencing_token == child_token + 1);
    assert(!first.ack("crash-a", "crashing-worker", child_token, &error));
    assert(second.ack("crash-a", "recovery-worker", recovered->fencing_token, &error));
    assert(first.inspect("crash-a", &error)->state == QueueState::Completed);

    agent_framework::distributed::PostgresWorkerRegistry registry1(conninfo), registry2(conninfo);
    const auto generation1 = registry1.register_worker(
        "worker-shared", "instance-a", "sha256:caps", &error);
    assert(generation1 && *generation1 == 1);
    assert(registry2.heartbeat("worker-shared", "instance-a", *generation1, &error));
    const auto generation2 = registry2.register_worker(
        "worker-shared", "instance-b", "sha256:caps-v2", &error);
    assert(generation2 && *generation2 == 2);
    assert(!registry1.heartbeat("worker-shared", "instance-a", *generation1, &error));
    const auto workers = registry1.alive(1000, &error);
    assert(workers.size() == 1 && workers.front().instance_id == "instance-b");

    agent_framework::distributed::PostgresLeaderElection election1(conninfo), election2(conninfo);
    const auto leader1 = election1.acquire("scheduler", "instance-a", 120, &error);
    assert(leader1 && leader1->fencing_token == 1 && election1.is_current(*leader1, &error));
    assert(!election2.acquire("scheduler", "instance-b", 120, &error));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto leader2 = election2.acquire("scheduler", "instance-b", 500, &error);
    assert(leader2 && leader2->fencing_token == leader1->fencing_token + 1);
    assert(!election1.is_current(*leader1, &error));
    assert(!election1.renew(*leader1, 500, &error));
    assert(!election1.release(*leader1, &error));
    assert(election2.renew(*leader2, 500, &error));
    assert(election2.release(*leader2, &error));

    // Terminate a live queue connection in PostgreSQL. The same C++ object must
    // reset its libpq session and continue without reconstructing the scheduler.
    const auto reconnect_conninfo = conninfo + " application_name=phase4_reconnect_probe";
    PostgresDurableQueue reconnecting(reconnect_conninfo);
    assert(reconnecting.enqueue(task("reconnect-a", "reconnect"), &error));
    {
        pg::Connection administrator(conninfo);
        const auto killed = pg::exec(administrator.get(),
            "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
            "WHERE application_name='phase4_reconnect_probe' AND pid<>pg_backend_pid()");
        assert(killed.rows() >= 1);
    }
    assert(!reconnecting.inspect("reconnect-a", &error));
    assert(!error.empty());
    error.clear();
    assert(reconnecting.inspect("reconnect-a", &error));

    using agent_framework::distributed::PostgresRollingMigrationExecutor;
    using agent_framework::distributed::RollingMigrationPhase;
    using agent_framework::distributed::RollingMigrationPlan;
    PostgresRollingMigrationExecutor migration(conninfo);
    assert(migration.initialize("pg_records", 1, 1, &error));
    {
        pg::Connection connection(conninfo);
        pg::exec(connection.get(),
            "CREATE TABLE pg_records(id bigint PRIMARY KEY,old_value text NOT NULL);"
            "INSERT INTO pg_records VALUES(1,'alpha'),(2,'beta')");
    }
    const auto migration_lease = migration.begin("pg_records", 1, "migrator-a", 5000, &error);
    assert(migration_lease && migration_lease->fencing_token == 1);
    const RollingMigrationPlan migration_plan{
        "pg_records", 1, 2, 2, "sha256:pg-records-v2",
        "ALTER TABLE pg_records ADD COLUMN new_value text",
        "UPDATE pg_records SET new_value=upper(old_value)",
        "CREATE TABLE pg_records_next(id bigint PRIMARY KEY,new_value text NOT NULL);"
        "INSERT INTO pg_records_next SELECT id,new_value FROM pg_records;"
        "DROP TABLE pg_records;ALTER TABLE pg_records_next RENAME TO pg_records"};
    assert(migration.prepare(migration_plan, "migrator-a",
                             migration_lease->fencing_token, &error));
    assert(!migration.backfill(migration_plan, "migrator-a",
                               migration_lease->fencing_token, &error));
    assert(migration.expand(migration_plan, "migrator-a",
                            migration_lease->fencing_token, &error));
    assert(migration.expand(migration_plan, "migrator-a",
                            migration_lease->fencing_token, &error));
    {
        PostgresRollingMigrationExecutor restarted(conninfo);
        assert(restarted.inspect("pg_records", &error)->phase == RollingMigrationPhase::Expanded);
        assert(restarted.backfill(migration_plan, "migrator-a",
                                  migration_lease->fencing_token, &error));
    }
    assert(migration.contract(migration_plan, "migrator-a",
                              migration_lease->fencing_token, &error));
    assert(migration.contract(migration_plan, "migrator-a",
                              migration_lease->fencing_token, &error));
    assert(!migration.reader_compatible("pg_records", 1, &error));
    assert(migration.reader_compatible("pg_records", 2, &error));
    {
        pg::Connection connection(conninfo);
        const auto data = pg::exec(connection.get(),
            "SELECT string_agg(new_value,',' ORDER BY id) FROM pg_records");
        assert(data.rows() == 1 && data.value(0, 0) == "ALPHA,BETA");
        const auto old_column = pg::exec(connection.get(),
            "SELECT 1 FROM information_schema.columns WHERE table_name='pg_records' "
            "AND column_name='old_value'");
        assert(old_column.rows() == 0);
    }

    // Multiple DDL statements and the phase transition are one transaction.
    // A later statement failure must roll back the earlier schema mutation.
    assert(migration.initialize("pg_rollback", 1, 1, &error));
    {
        pg::Connection connection(conninfo);
        pg::exec(connection.get(),
                 "CREATE TABLE pg_rollback(id bigint PRIMARY KEY,value text NOT NULL)");
    }
    const auto rollback_lease = migration.begin("pg_rollback", 1, "migrator-old", 120, &error);
    assert(rollback_lease && rollback_lease->fencing_token == 1);
    const RollingMigrationPlan rollback_plan{
        "pg_rollback", 1, 2, 2, "sha256:pg-rollback-v2",
        "ALTER TABLE pg_rollback ADD COLUMN temporary_value text;"
        "INSERT INTO pg_missing_table VALUES(1)",
        "UPDATE pg_rollback SET temporary_value=upper(value)",
        "ALTER TABLE pg_rollback DROP COLUMN value;"
        "ALTER TABLE pg_rollback RENAME COLUMN temporary_value TO value"};
    assert(migration.prepare(rollback_plan, "migrator-old",
                             rollback_lease->fencing_token, &error));
    assert(!migration.expand(rollback_plan, "migrator-old",
                             rollback_lease->fencing_token, &error));
    assert(migration.inspect("pg_rollback", &error)->phase == RollingMigrationPhase::Prepared);
    {
        pg::Connection connection(conninfo);
        const auto temporary_column = pg::exec(connection.get(),
            "SELECT 1 FROM information_schema.columns WHERE table_name='pg_rollback' "
            "AND column_name='temporary_value'");
        assert(temporary_column.rows() == 0);
    }

    // PostgreSQL time is the lease authority. After expiry a new owner receives
    // a higher token, while the old owner cannot advance the durable phase.
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const auto rollback_takeover = migration.begin("pg_rollback", 1, "migrator-new", 500, &error);
    assert(rollback_takeover &&
           rollback_takeover->fencing_token == rollback_lease->fencing_token + 1);
    assert(!migration.expand(rollback_plan, "migrator-old",
                             rollback_lease->fencing_token, &error));
    assert(migration.inspect("pg_rollback", &error)->phase == RollingMigrationPhase::Prepared);
    return 0;
}
