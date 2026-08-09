#include <agent/agent_client/token_provider.hpp>
#include <functional>
#include <stdexcept>
namespace agent_framework
{
    std::optional<OAuthToken> MemoryCredentialStore::load() const
    {
        std::lock_guard<std::mutex> l(m_);
        return v_;
    }
    void MemoryCredentialStore::save(const OAuthToken &v)
    {
        std::lock_guard<std::mutex> l(m_);
        v_ = v;
    }
    RefreshingTokenProvider::RefreshingTokenProvider(std::shared_ptr<CredentialStore> s, Refresh r) : store_(std::move(s)), refresh_(std::move(r))
    {
        if (!store_ || !refresh_)
            throw std::invalid_argument("token provider requires store and refresh");
    }
    std::string RefreshingTokenProvider::access_token()
    {
        std::lock_guard<std::mutex> l(m_);
        auto t = store_->load();
        if (!t)
            throw std::runtime_error("no OAuth credential");
        if (t->access_token.empty() || std::chrono::system_clock::now() >= t->expires_at)
        {
            if (!t->refresh_token || t->refresh_token->empty())
                throw std::runtime_error("OAuth token expired without refresh token");
            auto fresh = refresh_(*t);
            if (fresh.access_token.empty())
                throw std::runtime_error("OAuth refresh returned empty access token");
            store_->save(fresh);
            t = std::move(fresh);
        }
        return t->access_token;
    }
}
