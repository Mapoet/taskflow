#include <agent/agent_client/agent_client.hpp>
#include <agent/agent_client/token_provider.hpp>

#include <cassert>

using namespace agent_framework;

namespace {
class UnauthorizedOnceClient final : public HTTPClient {
public:
    json post(const std::string&, const json&,
              const std::map<std::string, std::string>&) override { return json::object(); }
    json get(const std::string&, const std::map<std::string, std::string>& headers) override {
        ++calls;
        observed_authorization.push_back(headers.at("Authorization"));
        if (calls == 1) throw HttpStatusError(401, "GET");
        return {{"ok", true}};
    }
    void get_sse(const std::string&, const std::map<std::string, std::string>&,
                 const std::function<void(std::string_view)>&, int,
                 const std::atomic<bool>*) override {}
    int calls = 0;
    std::vector<std::string> observed_authorization;
};
}

int main() {
    auto credentials = std::make_shared<MemoryCredentialStore>();
    credentials->save({"old-access", std::string("refresh-secret"),
                       std::chrono::system_clock::now() + std::chrono::hours(1)});
    int refreshes = 0;
    auto provider = std::make_shared<RefreshingTokenProvider>(credentials, [&](const OAuthToken&) {
        ++refreshes;
        return OAuthToken{"new-access", std::string("refresh-secret"),
                          std::chrono::system_clock::now() + std::chrono::hours(1)};
    });
    auto fake = std::make_unique<UnauthorizedOnceClient>();
    auto* observed = fake.get();
    AgentClient client("http://example.invalid", {}, std::move(fake));
    client.set_token_provider(provider);
    const auto result = client.get_push_notification_config("/agent", "task").get();
    assert(result["ok"] == true);
    assert(observed->calls == 2 && refreshes == 1);
    assert(observed->observed_authorization[0] == "Bearer old-access");
    assert(observed->observed_authorization[1] == "Bearer new-access");
}
