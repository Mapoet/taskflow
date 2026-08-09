#include <agent/agent_client/token_provider.hpp>
#include <agent/core/types.hpp>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent_framework {
namespace {

std::string hex_encode(const unsigned char* data, std::size_t size) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (std::size_t i = 0; i < size; ++i) out << std::setw(2) << static_cast<unsigned>(data[i]);
    return out.str();
}

std::vector<unsigned char> hex_decode(const std::string& value) {
    if (value.size() % 2 != 0) throw std::runtime_error("credential envelope is malformed");
    std::vector<unsigned char> result(value.size() / 2);
    for (std::size_t i = 0; i < result.size(); ++i) {
        unsigned int byte = 0;
        std::istringstream in(value.substr(i * 2, 2));
        in >> std::hex >> byte;
        if (!in || !in.eof()) throw std::runtime_error("credential envelope is malformed");
        result[i] = static_cast<unsigned char>(byte);
    }
    return result;
}

std::vector<std::string> split_envelope(const std::string& value) {
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(':', begin);
        parts.push_back(value.substr(begin, end == std::string::npos ? std::string::npos : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return parts;
}

json token_to_json(const OAuthToken& token) {
    json value = {{"v", 1}, {"access_token", token.access_token},
                  {"expires_at_epoch_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
                      token.expires_at.time_since_epoch()).count()},
                  {"token_type", token.token_type}, {"scope", token.scope}};
    if (token.refresh_token) value["refresh_token"] = *token.refresh_token;
    return value;
}

OAuthToken token_from_json(const json& value) {
    if (!value.is_object() || value.value("v", 0) != 1 || !value.contains("access_token") ||
        !value["access_token"].is_string() || !value.contains("expires_at_epoch_ms"))
        throw std::runtime_error("credential payload has unsupported schema");
    OAuthToken token;
    token.access_token = value["access_token"].get<std::string>();
    if (value.contains("refresh_token") && value["refresh_token"].is_string())
        token.refresh_token = value["refresh_token"].get<std::string>();
    token.expires_at = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(value["expires_at_epoch_ms"].get<std::int64_t>()));
    token.token_type = value.value("token_type", "Bearer");
    token.scope = value.value("scope", "");
    return token;
}

void atomic_write_private(const std::filesystem::path& path, const std::string& bytes) {
    const auto parent = path.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const auto temporary = path.string() + ".tmp." + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
#ifndef _WIN32
    const int fd = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, S_IRUSR | S_IWUSR);
    if (fd < 0) throw std::runtime_error("cannot create protected credential temporary file");
    bool ok = true;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) { if (errno == EINTR) continue; ok = false; break; }
        offset += static_cast<std::size_t>(written);
    }
    if (ok) ok = ::fsync(fd) == 0;
    if (::close(fd) != 0) ok = false;
    if (!ok) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw std::runtime_error("cannot durably write protected credential file");
    }
    std::filesystem::rename(temporary, path);
    ::chmod(path.c_str(), S_IRUSR | S_IWUSR);
    if (!parent.empty()) {
        const int directory = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (directory >= 0) { (void)::fsync(directory); (void)::close(directory); }
    }
#else
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("cannot write protected credential file");
    out.close();
    std::filesystem::rename(temporary, path);
#endif
}

} // namespace

std::optional<OAuthToken> MemoryCredentialStore::load() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return value_;
}
void MemoryCredentialStore::save(const OAuthToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    value_ = token;
}

Aes256GcmCredentialProtector::Aes256GcmCredentialProtector(std::vector<unsigned char> key)
    : key_(std::move(key)) {
    if (key_.size() != 32) throw std::invalid_argument("AES-256-GCM credential key must be 32 bytes");
}

std::string Aes256GcmCredentialProtector::protect(const std::string& plaintext) const {
    std::array<unsigned char, 12> nonce{};
    std::array<unsigned char, 16> tag{};
    if (RAND_bytes(nonce.data(), nonce.size()) != 1)
        throw std::runtime_error("credential encryption random source failed");
    std::vector<unsigned char> ciphertext(plaintext.size() + 16);
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (!raw) throw std::runtime_error("credential encryption initialization failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw, EVP_CIPHER_CTX_free);
    int written = 0, final_written = 0;
    const bool ok = EVP_EncryptInit_ex(raw, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(raw, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
        EVP_EncryptInit_ex(raw, nullptr, nullptr, key_.data(), nonce.data()) == 1 &&
        EVP_EncryptUpdate(raw, ciphertext.data(), &written,
                          reinterpret_cast<const unsigned char*>(plaintext.data()), plaintext.size()) == 1 &&
        EVP_EncryptFinal_ex(raw, ciphertext.data() + written, &final_written) == 1 &&
        EVP_CIPHER_CTX_ctrl(raw, EVP_CTRL_GCM_GET_TAG, tag.size(), tag.data()) == 1;
    if (!ok) throw std::runtime_error("credential encryption failed");
    ciphertext.resize(static_cast<std::size_t>(written + final_written));
    return "agcm1:" + hex_encode(nonce.data(), nonce.size()) + ':' +
        hex_encode(tag.data(), tag.size()) + ':' + hex_encode(ciphertext.data(), ciphertext.size());
}

std::string Aes256GcmCredentialProtector::unprotect(const std::string& protected_value) const {
    const auto parts = split_envelope(protected_value);
    if (parts.size() != 4 || parts[0] != "agcm1")
        throw std::runtime_error("credential envelope has unsupported schema");
    const auto nonce = hex_decode(parts[1]);
    const auto tag = hex_decode(parts[2]);
    const auto ciphertext = hex_decode(parts[3]);
    if (nonce.size() != 12 || tag.size() != 16)
        throw std::runtime_error("credential envelope is malformed");
    std::vector<unsigned char> plaintext(ciphertext.size() + 16);
    EVP_CIPHER_CTX* raw = EVP_CIPHER_CTX_new();
    if (!raw) throw std::runtime_error("credential decryption initialization failed");
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> context(raw, EVP_CIPHER_CTX_free);
    int written = 0, final_written = 0;
    const bool initialized = EVP_DecryptInit_ex(raw, EVP_aes_256_gcm(), nullptr, nullptr, nullptr) == 1 &&
        EVP_CIPHER_CTX_ctrl(raw, EVP_CTRL_GCM_SET_IVLEN, nonce.size(), nullptr) == 1 &&
        EVP_DecryptInit_ex(raw, nullptr, nullptr, key_.data(), nonce.data()) == 1 &&
        EVP_DecryptUpdate(raw, plaintext.data(), &written, ciphertext.data(), ciphertext.size()) == 1 &&
        EVP_CIPHER_CTX_ctrl(raw, EVP_CTRL_GCM_SET_TAG, tag.size(), const_cast<unsigned char*>(tag.data())) == 1;
    if (!initialized || EVP_DecryptFinal_ex(raw, plaintext.data() + written, &final_written) != 1)
        throw std::runtime_error("credential authentication failed");
    plaintext.resize(static_cast<std::size_t>(written + final_written));
    return std::string(reinterpret_cast<const char*>(plaintext.data()), plaintext.size());
}

FileCredentialStore::FileCredentialStore(std::filesystem::path path,
                                         std::shared_ptr<const CredentialProtector> protector)
    : path_(std::move(path)), protector_(std::move(protector)) {
    if (path_.empty() || !protector_) throw std::invalid_argument("file credential store requires path and protector");
}
std::optional<OAuthToken> FileCredentialStore::load() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!std::filesystem::exists(path_)) return std::nullopt;
#ifndef _WIN32
    struct stat metadata{};
    if (::stat(path_.c_str(), &metadata) != 0 || (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0)
        throw std::runtime_error("credential file permissions are not private");
#endif
    std::ifstream in(path_, std::ios::binary);
    std::ostringstream bytes;
    bytes << in.rdbuf();
    if (!in.good() && !in.eof()) throw std::runtime_error("cannot read protected credential file");
    const auto envelope = json::parse(bytes.str());
    if (!envelope.is_object() || envelope.value("v", 0) != 1 || !envelope.contains("protected"))
        throw std::runtime_error("credential file has unsupported schema");
    return token_from_json(json::parse(protector_->unprotect(envelope["protected"].get<std::string>())));
}
void FileCredentialStore::save(const OAuthToken& token) {
    std::lock_guard<std::mutex> lock(mutex_);
    const json envelope = {{"v", 1}, {"protected", protector_->protect(token_to_json(token).dump())}};
    atomic_write_private(path_, envelope.dump());
}

RefreshingTokenProvider::RefreshingTokenProvider(std::shared_ptr<CredentialStore> store, Refresh refresh,
                                                 std::chrono::seconds expiry_skew)
    : store_(std::move(store)), refresh_(std::move(refresh)), expiry_skew_(expiry_skew) {
    if (!store_ || !refresh_) throw std::invalid_argument("token provider requires store and refresh");
    if (expiry_skew_ < std::chrono::seconds::zero())
        throw std::invalid_argument("token expiry skew cannot be negative");
}
std::string RefreshingTokenProvider::access_token() { return get(false); }
std::string RefreshingTokenProvider::force_refresh() { return get(true); }
std::string RefreshingTokenProvider::get(bool force) {
    // The mutex spans refresh and persistence, making refresh single-flight for this provider.
    std::lock_guard<std::mutex> lock(mutex_);
    auto token = store_->load();
    if (!token) throw std::runtime_error("OAuth credential is unavailable");
    if (force || token->access_token.empty() ||
        std::chrono::system_clock::now() + expiry_skew_ >= token->expires_at) {
        if (!token->refresh_token || token->refresh_token->empty())
            throw std::runtime_error("OAuth credential cannot be refreshed");
        OAuthToken fresh = refresh_(*token);
        if (fresh.access_token.empty()) throw std::runtime_error("OAuth refresh returned no access token");
        if (!fresh.refresh_token) fresh.refresh_token = token->refresh_token;
        store_->save(fresh);
        token = std::move(fresh);
    }
    return token->access_token;
}

OAuthDeviceFlow::OAuthDeviceFlow(std::shared_ptr<OAuthDeviceTransport> transport, DeviceFlowOptions options)
    : transport_(std::move(transport)), options_(std::move(options)) {
    if (!transport_ || !options_.now || !options_.sleep)
        throw std::invalid_argument("OAuth device flow requires transport, clock, and sleeper");
}
DeviceAuthorizationResponse OAuthDeviceFlow::begin(const std::string& client_id, const std::string& scope) {
    if (client_id.empty()) throw std::invalid_argument("OAuth device flow requires client_id");
    auto response = transport_->request_device_authorization(client_id, scope);
    if (response.device_code.empty() || response.user_code.empty() || response.verification_uri.empty() ||
        response.expires_in <= std::chrono::seconds::zero())
        throw std::runtime_error("OAuth device authorization response is incomplete");
    if (response.interval <= std::chrono::seconds::zero()) response.interval = std::chrono::seconds(5);
    return response;
}
OAuthToken OAuthDeviceFlow::poll_until_authorized(
    const std::string& client_id, const DeviceAuthorizationResponse& authorization) {
    auto interval = authorization.interval;
    const auto expires_at = options_.now() + authorization.expires_in;
    while (true) {
        if (options_.cancellation_requested && options_.cancellation_requested())
            throw std::runtime_error("OAuth device authorization cancelled");
        const auto now = options_.now();
        const auto deadline = options_.deadline ? std::min(expires_at, *options_.deadline) : expires_at;
        if (now >= deadline) throw std::runtime_error("OAuth device authorization expired");
        options_.sleep(std::chrono::duration_cast<std::chrono::milliseconds>(interval));
        if (options_.cancellation_requested && options_.cancellation_requested())
            throw std::runtime_error("OAuth device authorization cancelled");
        const auto result = transport_->poll_device_token(client_id, authorization.device_code);
        switch (result.status) {
            case DeviceTokenPollStatus::Authorized:
                if (!result.token || result.token->access_token.empty())
                    throw std::runtime_error("OAuth device token response is incomplete");
                return *result.token;
            case DeviceTokenPollStatus::AuthorizationPending: break;
            case DeviceTokenPollStatus::SlowDown: interval += options_.slow_down_increment; break;
            case DeviceTokenPollStatus::AccessDenied:
                throw std::runtime_error("OAuth device authorization denied");
            case DeviceTokenPollStatus::ExpiredToken:
                throw std::runtime_error("OAuth device authorization expired");
            case DeviceTokenPollStatus::Failed:
                throw std::runtime_error("OAuth device authorization failed: " +
                                         (result.error_code.empty() ? std::string("unknown") : result.error_code));
        }
    }
}

} // namespace agent_framework
