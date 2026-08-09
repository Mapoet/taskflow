#ifndef AGENT_TOKEN_PROVIDER_HPP
#define AGENT_TOKEN_PROVIDER_HPP
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
namespace agent_framework
{
    struct OAuthToken
    {
        std::string access_token;
        std::optional<std::string> refresh_token;
        std::chrono::system_clock::time_point expires_at{};
    };
    class CredentialStore
    {
    public:
        virtual ~CredentialStore() = default;
        virtual std::optional<OAuthToken> load() const = 0;
        virtual void save(const OAuthToken &) = 0;
    };
    class MemoryCredentialStore final : public CredentialStore
    {
        mutable std::mutex m_;
        std::optional<OAuthToken> v_;

    public:
        std::optional<OAuthToken> load() const override;
        void save(const OAuthToken &) override;
    };
    class TokenProvider
    {
    public:
        virtual ~TokenProvider() = default;
        virtual std::string access_token() = 0;
    };
    class RefreshingTokenProvider final : public TokenProvider
    {
    public:
        using Refresh = std::function<OAuthToken(const OAuthToken &)>;
        RefreshingTokenProvider(std::shared_ptr<CredentialStore>, Refresh);
        std::string access_token() override;

    private:
        std::shared_ptr<CredentialStore> store_;
        Refresh refresh_;
        std::mutex m_;
    };
}
#endif
