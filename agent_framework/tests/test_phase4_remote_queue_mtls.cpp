#ifdef NDEBUG
#undef NDEBUG
#endif
#include "agent/distributed/durable_queue.hpp"
#include "agent/distributed/remote_object_store.hpp"
#include "agent/distributed/remote_queue.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace agent_framework::distributed;

namespace {
int run_openssl(const std::vector<std::string>& arguments) {
    const pid_t pid = ::fork();
    assert(pid >= 0);
    if(pid == 0) {
        const int input = ::open("/dev/null", O_RDONLY);
        const int sink = ::open("/dev/null", O_WRONLY);
        if(input >= 0) { ::dup2(input, STDIN_FILENO); ::close(input); }
        if(sink >= 0) { ::dup2(sink, STDOUT_FILENO); ::dup2(sink, STDERR_FILENO); ::close(sink); }
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(PHASE4_OPENSSL_EXECUTABLE));
        for(const auto& value : arguments) argv.push_back(const_cast<char*>(value.c_str()));
        argv.push_back(nullptr);
        ::execv(PHASE4_OPENSSL_EXECUTABLE, argv.data());
        ::_exit(127);
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while(::waitpid(pid, &status, WNOHANG) == 0) {
        if(std::chrono::steady_clock::now() >= deadline) {
            ::kill(pid, SIGKILL);
            assert(::waitpid(pid, &status, 0) == pid);
            return 124;
        }
        ::usleep(10000);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

void generate_identity(const std::filesystem::path& root, std::string name,
                       const std::filesystem::path& ca_cert,
                       const std::filesystem::path& ca_key, std::string extensions) {
    const auto key = root / (name + ".key");
    const auto csr = root / (name + ".csr");
    const auto cert = root / (name + ".crt");
    const auto ext = root / (name + ".ext");
    { std::ofstream output(ext); output << extensions; }
    assert(run_openssl({"req", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-keyout", key.string(), "-out", csr.string(), "-subj", "/CN=" + name}) == 0);
    assert(run_openssl({"x509", "-req", "-in", csr.string(), "-CA", ca_cert.string(),
        "-CAkey", ca_key.string(), "-CAcreateserial", "-out", cert.string(),
        "-days", "1", "-sha256", "-extfile", ext.string()}) == 0);
}

struct ServerProcess { pid_t pid{-1}; int port{-1}; };
ServerProcess start_server(const std::string& database, const std::string& token,
                           const RemoteQueueTlsServerConfig& tls) {
    int channel[2]; assert(::pipe(channel) == 0);
    const pid_t pid = ::fork(); assert(pid >= 0);
    if(pid == 0) {
        ::close(channel[0]);
        SQLiteDurableQueue queue(database);
        RemoteQueueServer server(queue, token, tls);
        const int port = server.bind("0.0.0.0");
        const auto written = ::write(channel[1], &port, sizeof(port));
        ::close(channel[1]);
        if(port <= 0 || written != static_cast<ssize_t>(sizeof(port))) ::_exit(3);
        ::_exit(server.listen_after_bind() ? 0 : 4);
    }
    ::close(channel[1]);
    int port = -1; assert(::read(channel[0], &port, sizeof(port)) == static_cast<ssize_t>(sizeof(port)));
    ::close(channel[0]);
    if(port <= 0) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        std::fprintf(stderr, "mTLS server bind failed: port=%d child_status=%d\n", port, status);
        std::abort();
    }
    return {pid, port};
}
ServerProcess start_object_server(const std::filesystem::path& objects,
                                  const std::string& token,
                                  const RemoteQueueTlsServerConfig& tls) {
    int channel[2]; assert(::pipe(channel) == 0);
    const pid_t pid = ::fork(); assert(pid >= 0);
    if(pid == 0) {
        ::close(channel[0]);
        FilesystemObjectStore store(objects);
        RemoteObjectStoreServer server(store, token, tls);
        const int port = server.bind("0.0.0.0");
        const auto written = ::write(channel[1], &port, sizeof(port));
        ::close(channel[1]);
        if(port <= 0 || written != static_cast<ssize_t>(sizeof(port))) ::_exit(3);
        ::_exit(server.listen_after_bind() ? 0 : 4);
    }
    ::close(channel[1]);
    int port=-1;assert(::read(channel[0],&port,sizeof(port))==static_cast<ssize_t>(sizeof(port)));
    ::close(channel[0]);assert(port>0);return {pid,port};
}
void stop(ServerProcess server) {
    assert(::kill(server.pid, SIGKILL) == 0);
    int status = 0; assert(::waitpid(server.pid, &status, 0) == server.pid);
}
}  // namespace

int main() {
    auto stage = [](const char* value) { std::fprintf(stderr, "mtls-stage:%s\n", value); std::fflush(stderr); };
    std::string template_path = (std::filesystem::temp_directory_path() /
        "phase4-mtls-queue-XXXXXX").string();
    std::vector<char> template_buffer(template_path.begin(), template_path.end());
    template_buffer.push_back('\0');
    const char* created = ::mkdtemp(template_buffer.data());
    assert(created);
    const std::filesystem::path root(created);
    const auto ca_key = root / "ca.key";
    const auto ca_cert = root / "ca.crt";
    assert(run_openssl({"req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-keyout", ca_key.string(), "-out", ca_cert.string(), "-subj",
        "/CN=Phase4 Test CA", "-days", "1"}) == 0);
    generate_identity(root, "localhost", ca_cert, ca_key,
        "subjectAltName=DNS:localhost\nextendedKeyUsage=serverAuth\n");
    generate_identity(root, "client", ca_cert, ca_key, "extendedKeyUsage=clientAuth\n");
    const auto wrong_key = root / "wrong-ca.key";
    const auto wrong_ca = root / "wrong-ca.crt";
    assert(run_openssl({"req", "-x509", "-newkey", "rsa:2048", "-sha256", "-nodes",
        "-keyout", wrong_key.string(), "-out", wrong_ca.string(), "-subj",
        "/CN=Wrong CA", "-days", "1"}) == 0);
    stage("certificates");

    const auto database = (root / "queue.sqlite").string();
    {
        SQLiteWorkerRegistry registry(database); assert(registry.set_quota("tenant", 1));
    }
    const std::string token = "mtls-bearer";
    RemoteQueueTlsServerConfig server_tls{(root / "localhost.crt").string(),
        (root / "localhost.key").string(), ca_cert.string()};
    auto server = start_server(database, token, server_tls);
    stage("server");
    RemoteQueueTlsClientConfig trusted{ca_cert.string(), (root / "client.crt").string(),
        (root / "client.key").string()};
    RemoteQueueClient client("localhost", server.port, token, trusted);
    QueueTask task{"mtls-task", "tenant", "mtls-key", "sha256:mtls"};
    assert(client.enqueue(task));
    auto lease = client.claim("worker", "tenant", 1000);
    assert(lease && lease->fencing_token == 1);
    stage("trusted-client");
    const int downgrade = run_openssl({"s_client", "-connect", "localhost:" + std::to_string(server.port),
        "-cert", (root / "client.crt").string(), "-key", (root / "client.key").string(),
        "-CAfile", ca_cert.string(), "-verify_return_error", "-no_ign_eof", "-nocommands",
        "-tls1"});
    assert(downgrade != 0 && downgrade != 124);
    stage("downgrade");

    std::string error;
    bool missing_client_rejected = false;
    try {
        RemoteQueueClient no_client_cert("localhost", server.port, token,
            RemoteQueueTlsClientConfig{ca_cert.string(), "", ""});
    } catch(const std::invalid_argument&) { missing_client_rejected = true; }
    assert(missing_client_rejected);
    stage("missing-client-cert");
    RemoteQueueClient wrong_trust("localhost", server.port, token,
        RemoteQueueTlsClientConfig{wrong_ca.string(), (root / "client.crt").string(),
                                   (root / "client.key").string()});
    assert(!wrong_trust.inspect("mtls-task", &error));
    assert(error == "remote queue transport unavailable");
    stage("wrong-ca");
    RemoteQueueClient wrong_hostname("127.0.0.1", server.port, token, trusted);
    assert(!wrong_hostname.inspect("mtls-task", &error));
    stage("hostname");
    assert(client.ack("mtls-task", "worker", lease->fencing_token));
    stage("ack");
    stop(server);
    stage("stop");

    const auto object_root = root / "objects";
    auto object_server = start_object_server(object_root, token, server_tls);
    RemoteObjectStoreClient objects("localhost", object_server.port, token, trusted);
    const std::string binary("remote\0object", 13);
    auto first = objects.put("tenant-a", binary, "application/octet-stream");
    assert(first && objects.get(*first) == std::optional<std::string>(binary));
    auto replay = objects.put("tenant-a", binary, "application/octet-stream", first->digest);
    assert(replay && replay->digest == first->digest);
    auto isolated = objects.put("tenant-b", binary, "application/octet-stream");
    assert(isolated && isolated->tenant_id == "tenant-b");
    std::string object_error;
    RemoteObjectStoreClient unauthorized_objects("localhost", object_server.port, "wrong", trusted);
    assert(!unauthorized_objects.get(*first, &object_error));
    assert(object_error == "unauthorized");
    const auto stored = object_root / "tenant-a" / first->digest.substr(7,2) /
                        first->digest.substr(7);
    { std::ofstream corrupt(stored, std::ios::binary | std::ios::trunc); corrupt << "tampered"; }
    assert(!objects.get(*first, &object_error));
    assert(object_error == "object size verification failed" ||
           object_error == "object integrity verification failed");
    stop(object_server);
    stage("remote-object");
    std::filesystem::remove_all(root);
}
