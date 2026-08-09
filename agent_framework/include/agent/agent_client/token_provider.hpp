#ifndef AGENT_TOKEN_PROVIDER_HPP
#define AGENT_TOKEN_PROVIDER_HPP

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace agent_framework {

struct OAuthToken {
    std::string access_token;
    std::optional<std::string> refresh_token;
    std::chrono::system_clock::time_point expires_at{};
    std::string token_type{"Bearer"};
    std::string scope;
};

class CredentialStore {
public:
    virtual ~CredentialStore() = default;
    virtual std::optional<OAuthToken> load() const = 0;
    virtual void save(const OAuthToken& token) = 0;
};

class MemoryCredentialStore final : public CredentialStore {
public:
    std::optional<OAuthToken> load() const override;
    void save(const OAuthToken& token) override;
private:
    mutable std::mutex mutex_;
    std::optional<OAuthToken> value_;
};

/** Protects the complete credential payload before it reaches persistent storage. */
class CredentialProtector {
public:
    virtual ~CredentialProtector() = default;
    virtual std::string protect(const std::string& plaintext) const = 0;
    virtual std::string unprotect(const std::string& protected_value) const = 0;
};

/** AES-256-GCM protector. The caller must obtain the 32-byte key from a secret facility. */
class Aes256GcmCredentialProtector final : public CredentialProtector {
public:
    explicit Aes256GcmCredentialProtector(std::vector<unsigned char> key);
    std::string protect(const std::string& plaintext) const override;
    std::string unprotect(const std::string& protected_value) const override;
private:
    std::vector<unsigned char> key_;
};

/** Atomic, permission-restricted, versioned local credential store. */
class FileCredentialStore final : public CredentialStore {
public:
    FileCredentialStore(std::filesystem::path path,
                        std::shared_ptr<const CredentialProtector> protector);
    std::optional<OAuthToken> load() const override;
    void save(const OAuthToken& token) override;
    const std::filesystem::path& path() const noexcept { return path_; }
private:
    std::filesystem::path path_;
    std::shared_ptr<const CredentialProtector> protector_;
    mutable std::mutex mutex_;
};

class TokenProvider {
public:
    virtual ~TokenProvider() = default;
    virtual std::string access_token() = 0;
    /** Force a refresh even when the cached token has not expired (for HTTP 401 recovery). */
    virtual std::string force_refresh() { return access_token(); }
};

class RefreshingTokenProvider final : public TokenProvider {
public:
    using Refresh = std::function<OAuthToken(const OAuthToken&)>;
    RefreshingTokenProvider(std::shared_ptr<CredentialStore> store, Refresh refresh,
                            std::chrono::seconds expiry_skew = std::chrono::seconds(30));
    std::string access_token() override;
    std::string force_refresh() override;
private:
    std::string get(bool force);
    std::shared_ptr<CredentialStore> store_;
    Refresh refresh_;
    std::chrono::seconds expiry_skew_;
    std::mutex mutex_;
};

struct DeviceAuthorizationResponse {
    std::string device_code;
    std::string user_code;
    std::string verification_uri;
    std::optional<std::string> verification_uri_complete;
    std::chrono::seconds expires_in{600};
    std::chrono::seconds interval{5};
};

enum class DeviceTokenPollStatus {
    Authorized,
    AuthorizationPending,
    SlowDown,
    AccessDenied,
    ExpiredToken,
    Failed
};

struct DeviceTokenPollResult {
    DeviceTokenPollStatus status{DeviceTokenPollStatus::Failed};
    std::optional<OAuthToken> token;
    /** Stable OAuth error code only; transports must not place credentials in this field. */
    std::string error_code;
};

class OAuthDeviceTransport {
public:
    virtual ~OAuthDeviceTransport() = default;
    virtual DeviceAuthorizationResponse request_device_authorization(
        const std::string& client_id, const std::string& scope) = 0;
    virtual DeviceTokenPollResult poll_device_token(
        const std::string& client_id, const std::string& device_code) = 0;
};

struct DeviceFlowOptions {
    std::function<std::chrono::steady_clock::time_point()> now =
        [] { return std::chrono::steady_clock::now(); };
    std::function<void(std::chrono::milliseconds)> sleep =
        [](std::chrono::milliseconds duration) { std::this_thread::sleep_for(duration); };
    std::function<bool()> cancellation_requested;
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::chrono::seconds slow_down_increment{5};
};

class OAuthDeviceFlow {
public:
    OAuthDeviceFlow(std::shared_ptr<OAuthDeviceTransport> transport,
                    DeviceFlowOptions options = {});
    DeviceAuthorizationResponse begin(const std::string& client_id, const std::string& scope);
    OAuthToken poll_until_authorized(const std::string& client_id,
                                     const DeviceAuthorizationResponse& authorization);
private:
    std::shared_ptr<OAuthDeviceTransport> transport_;
    DeviceFlowOptions options_;
};

} // namespace agent_framework
#endif
