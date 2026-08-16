#ifdef NDEBUG
#undef NDEBUG
#endif
#include "agent/distributed/durable_queue.hpp"
#include "agent/distributed/remote_queue.hpp"

#include <cassert>
#include <csignal>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace agent_framework::distributed;

namespace {
struct ServerProcess { pid_t pid{-1}; int port{-1}; };

ServerProcess start_server(const std::string& path, const std::string& token) {
    int channel[2];
    assert(::pipe(channel) == 0);
    const pid_t pid = ::fork();
    assert(pid >= 0);
    if(pid == 0) {
        ::close(channel[0]);
        SQLiteDurableQueue queue(path);
        RemoteQueueServer server(queue, token);
        const int port = server.bind("127.0.0.1");
        const auto written = ::write(channel[1], &port, sizeof(port));
        ::close(channel[1]);
        if(port <= 0 || written != static_cast<ssize_t>(sizeof(port))) ::_exit(3);
        ::_exit(server.listen_after_bind() ? 0 : 4);
    }
    ::close(channel[1]);
    int port = -1;
    assert(::read(channel[0], &port, sizeof(port)) == static_cast<ssize_t>(sizeof(port)));
    ::close(channel[0]);
    assert(port > 0);
    return {pid, port};
}

void kill_server(ServerProcess server) {
    assert(::kill(server.pid, SIGKILL) == 0);
    int status = 0;
    assert(::waitpid(server.pid, &status, 0) == server.pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
}
}  // namespace

int main() {
    const auto path = (std::filesystem::temp_directory_path() /
        ("phase4-remote-queue-" + std::to_string(::getpid()) + ".sqlite")).string();
    const std::string token = "test-opaque-bearer";
    {
        SQLiteWorkerRegistry registry(path);
        SQLiteDurableQueue queue(path);
        assert(registry.set_quota("tenant-a", 1));
        QueueTask task;
        task.task_id = "remote-task";
        task.tenant_id = "tenant-a";
        task.idempotency_key = "remote-idempotency";
        task.payload_digest = "sha256:remote-payload";
        task.available_at_ms = 1000;
        assert(queue.enqueue(task));
        RemoteQueueServer loopback_only(queue, token);
        assert(loopback_only.bind("0.0.0.0") == -1);
    }

    auto first = start_server(path, token);
    RemoteQueueClient client("127.0.0.1", first.port, token);
    RemoteQueueClient unauthorized("127.0.0.1", first.port, "wrong-token");
    std::string error;
    assert(!unauthorized.inspect("remote-task", &error));
    assert(!error.empty());

    QueueTask replay;
    replay.task_id = "remote-task";
    replay.tenant_id = "tenant-a";
    replay.idempotency_key = "remote-idempotency";
    replay.payload_digest = "sha256:remote-payload";
    replay.available_at_ms = 1000;
    assert(client.enqueue(replay, &error));
    replay.payload_digest = "sha256:conflicting-payload";
    assert(!client.enqueue(replay, &error));
    assert(error == "queue idempotency conflict");
    assert(!client.claim("worker-a", "tenant-a", -1));
    assert(!client.claim("worker-a", "tenant-a", INT64_MAX));

    auto old_lease = client.claim("worker-a", "tenant-a", 500, &error);
    assert(old_lease && old_lease->fencing_token == 1);
    kill_server(first);
    error.clear();
    assert(!client.inspect("remote-task", &error));
    assert(error == "remote queue transport unavailable");

    auto second = start_server(path, token);
    RemoteQueueClient recovered("127.0.0.1", second.port, token);
    assert(!recovered.claim("worker-b", "tenant-a", 500));
    ::usleep(550000);
    assert(!recovered.ack("remote-task", "worker-a", old_lease->fencing_token));
    auto new_lease = recovered.claim("worker-b", "tenant-a", 500, &error);
    assert(new_lease && new_lease->fencing_token == 2);
    assert(!recovered.ack("remote-task", "worker-a", old_lease->fencing_token));
    assert(recovered.ack("remote-task", "worker-b", new_lease->fencing_token));
    auto completed = recovered.inspect("remote-task", &error);
    assert(completed && completed->state == QueueState::Completed);
    {
        SQLiteWorkerRegistry registry(path);
        auto quota = registry.quota("tenant-a");
        assert(quota && quota->active == 0);
    }
    kill_server(second);
    std::filesystem::remove(path);
    std::filesystem::remove(path + "-wal");
    std::filesystem::remove(path + "-shm");
}
