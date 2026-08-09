#include <agent/agent_client/token_provider.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

using namespace agent_framework;

namespace {
class FakeDeviceTransport final : public OAuthDeviceTransport {
public:
    DeviceAuthorizationResponse request_device_authorization(
        const std::string& client_id, const std::string& scope) override {
        assert(client_id == "client" && scope == "tasks.read tasks.write");
        return {"device-secret", "ABCD-EFGH", "https://issuer.example/device", std::nullopt,
                std::chrono::seconds(60), std::chrono::seconds(2)};
    }
    DeviceTokenPollResult poll_device_token(const std::string& client_id,
                                            const std::string& device_code) override {
        assert(client_id == "client" && device_code == "device-secret");
        ++polls;
        if (polls == 1) return {DeviceTokenPollStatus::AuthorizationPending, std::nullopt, ""};
        if (polls == 2) return {DeviceTokenPollStatus::SlowDown, std::nullopt, ""};
        return {DeviceTokenPollStatus::Authorized,
                OAuthToken{"device-access", std::string("device-refresh"),
                           std::chrono::system_clock::now() + std::chrono::hours(1)}, ""};
    }
    int polls = 0;
};
}

int main() {
    auto memory = std::make_shared<MemoryCredentialStore>();
    int refresh_calls = 0;
    memory->save({"old", std::string("refresh-secret"),
                  std::chrono::system_clock::now() - std::chrono::seconds(1)});
    RefreshingTokenProvider provider(memory, [&](const OAuthToken&) {
        ++refresh_calls;
        return OAuthToken{"new-" + std::to_string(refresh_calls), std::nullopt,
                          std::chrono::system_clock::now() + std::chrono::hours(1)};
    });
    std::vector<std::future<std::string>> requests;
    for (int i = 0; i < 8; ++i)
        requests.push_back(std::async(std::launch::async, [&] { return provider.access_token(); }));
    for (auto& request : requests) assert(request.get() == "new-1");
    assert(refresh_calls == 1);
    assert(provider.force_refresh() == "new-2" && refresh_calls == 2);
    assert(memory->load()->refresh_token == "refresh-secret");

    const auto root = std::filesystem::temp_directory_path() / "agent-wp36-credentials";
    std::filesystem::remove_all(root);
    const auto file = root / "credential.json";
    auto protector = std::make_shared<Aes256GcmCredentialProtector>(
        std::vector<unsigned char>(32, 0x42));
    FileCredentialStore persistent(file, protector);
    persistent.save({"disk-access", std::string("disk-refresh-secret"),
                     std::chrono::system_clock::now() + std::chrono::minutes(10),
                     "Bearer", "tasks.read"});
    std::ifstream stored(file, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(stored)), {});
    assert(bytes.find("disk-access") == std::string::npos);
    assert(bytes.find("disk-refresh-secret") == std::string::npos);
#ifndef _WIN32
    struct stat metadata{};
    assert(::stat(file.c_str(), &metadata) == 0);
    assert((metadata.st_mode & (S_IRWXG | S_IRWXO)) == 0);
#endif
    const auto restored = persistent.load();
    assert(restored && restored->access_token == "disk-access");
    assert(restored->refresh_token == "disk-refresh-secret");
    assert(restored->scope == "tasks.read");

    auto fake = std::make_shared<FakeDeviceTransport>();
    auto monotonic_now = std::chrono::steady_clock::time_point{};
    std::vector<std::chrono::milliseconds> sleeps;
    DeviceFlowOptions options;
    options.now = [&] { return monotonic_now; };
    options.sleep = [&](std::chrono::milliseconds duration) {
        sleeps.push_back(duration);
        monotonic_now += duration;
    };
    OAuthDeviceFlow flow(fake, options);
    const auto authorization = flow.begin("client", "tasks.read tasks.write");
    const auto token = flow.poll_until_authorized("client", authorization);
    assert(token.access_token == "device-access");
    assert((sleeps == std::vector<std::chrono::milliseconds>{
        std::chrono::seconds(2), std::chrono::seconds(2), std::chrono::seconds(7)}));

    bool cancelled = false;
    options.cancellation_requested = [&] { return cancelled; };
    options.sleep = [&](std::chrono::milliseconds) { cancelled = true; };
    OAuthDeviceFlow cancelled_flow(fake, options);
    bool saw_cancel = false;
    try { (void)cancelled_flow.poll_until_authorized("client", authorization); }
    catch (const std::runtime_error& error) {
        saw_cancel = std::string(error.what()).find("cancelled") != std::string::npos;
        assert(std::string(error.what()).find("device-secret") == std::string::npos);
    }
    assert(saw_cancel);
    std::filesystem::remove_all(root);
}
