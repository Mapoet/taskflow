#include <cassert>
#include <cstdint>
#include <filesystem>

#include <sys/wait.h>
#include <unistd.h>

#include "agent/distributed/durable_queue.hpp"

int main() {
    using namespace agent_framework::distributed;
    const auto path=(std::filesystem::temp_directory_path()/"phase4-distributed-chaos.sqlite").string();
    std::filesystem::remove(path);
    SQLiteWorkerRegistry registry(path); SQLiteDurableQueue parent(path);
    assert(registry.set_quota("tenant",1));
    QueueTask task;task.task_id="crash-task";task.tenant_id="tenant";
    task.idempotency_key="crash-idempotency";task.payload_digest="sha256:payload";
    assert(parent.enqueue(task));
    int channel[2];assert(::pipe(channel)==0);const auto pid=::fork();assert(pid>=0);
    if(pid==0){::close(channel[0]);SQLiteDurableQueue worker(path);auto lease=worker.claim_with_quota("crashing-worker","tenant",1000,20);if(!lease)::_exit(2);const auto token=lease->fencing_token;const auto written=::write(channel[1],&token,sizeof(token));(void)written;::_exit(0);}
    ::close(channel[1]);std::uint64_t stale_token=0;const auto received=::read(channel[0],&stale_token,sizeof(stale_token));::close(channel[0]);int status=0;assert(::waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);assert(received==static_cast<ssize_t>(sizeof(stale_token))&&stale_token==1);assert(registry.quota("tenant")->active==1);assert(!parent.claim_with_quota("early-worker","tenant",1010,20));auto recovered=parent.claim_with_quota("recovery-worker","tenant",1020,20);assert(recovered&&recovered->fencing_token==2);assert(registry.quota("tenant")->active==1);assert(!parent.ack_with_quota("crash-task","crashing-worker",stale_token));assert(parent.ack_with_quota("crash-task","recovery-worker",recovered->fencing_token));assert(registry.quota("tenant")->active==0);
    std::filesystem::remove(path);return 0;
}
