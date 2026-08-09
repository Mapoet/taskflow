#include <agent/mcp_client/mcp_lifecycle.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent_framework {
namespace {
namespace fs = std::filesystem;

bool valid_id(const std::string& id) {
    if(id.empty() || id.size() > 128 || id == "." || id == "..") return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '-' || c == '.';
    });
}

bool digest_value(const std::string& value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isxdigit(c);
    });
}

std::vector<std::string> sorted_unique(std::vector<std::string> values) {
    std::sort(values.begin(), values.end());
    values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}

json transport_json(const CapabilityTransportDescriptor& value) {
    return {{"kind", value.kind}, {"endpoint", value.endpoint}, {"command", value.command},
            {"arguments", value.arguments}, {"executable_digest", value.executable_digest},
            {"environment_allowlist", sorted_unique(value.environment_allowlist)},
            {"credential_ref", value.credential_ref},
            {"working_directory", value.working_directory}};
}

CapabilityTransportDescriptor transport_from_json(const json& value) {
    CapabilityTransportDescriptor result;
    if(!value.is_object()) throw std::runtime_error("invalid MCP transport descriptor");
    result.kind = value.value("kind", "");
    result.endpoint = value.value("endpoint", "");
    result.command = value.value("command", "");
    result.arguments = value.value("arguments", std::vector<std::string>{});
    result.executable_digest = value.value("executable_digest", "");
    result.environment_allowlist = value.value("environment_allowlist", std::vector<std::string>{});
    result.credential_ref = value.value("credential_ref", "");
    result.working_directory = value.value("working_directory", "");
    return result;
}

std::string state_name(CapabilityLifecycleState state) {
    switch(state) {
        case CapabilityLifecycleState::Discovered: return "discovered";
        case CapabilityLifecycleState::Validated: return "validated";
        case CapabilityLifecycleState::Staged: return "staged";
        case CapabilityLifecycleState::Active: return "active";
        case CapabilityLifecycleState::Draining: return "draining";
        case CapabilityLifecycleState::Removed: return "removed";
        case CapabilityLifecycleState::Failed: return "failed";
    }
    return "failed";
}

CapabilityLifecycleState state_from_name(const std::string& value) {
    if(value == "discovered") return CapabilityLifecycleState::Discovered;
    if(value == "validated") return CapabilityLifecycleState::Validated;
    if(value == "staged") return CapabilityLifecycleState::Staged;
    if(value == "active") return CapabilityLifecycleState::Active;
    if(value == "draining") return CapabilityLifecycleState::Draining;
    if(value == "removed") return CapabilityLifecycleState::Removed;
    if(value == "failed") return CapabilityLifecycleState::Failed;
    throw std::runtime_error("invalid MCP lifecycle state");
}

json manifest_json(const CapabilityManifest& manifest) {
    return {{"id", manifest.id}, {"version", manifest.version}, {"origin", manifest.origin},
            {"digest", manifest.digest}, {"permissions", manifest.permissions},
            {"kind", manifest.kind}, {"transport", transport_json(manifest.transport)},
            {"tenant_visibility", manifest.tenant_visibility},
            {"dependency_lock", manifest.dependency_lock},
            {"signature", manifest.signature ? manifest.signature->to_json() : json(nullptr)}};
}

CapabilityManifest manifest_from_json(const json& value) {
    CapabilityManifest manifest;
    manifest.id = value.value("id", "");
    manifest.version = value.value("version", "");
    manifest.origin = value.value("origin", "");
    manifest.digest = value.value("digest", "");
    manifest.permissions = value.value("permissions", std::vector<std::string>{});
    manifest.kind = value.value("kind", "mcp");
    manifest.transport = transport_from_json(value.value("transport", json::object()));
    manifest.tenant_visibility = value.value("tenant_visibility", std::vector<std::string>{"default"});
    manifest.dependency_lock = value.value("dependency_lock", std::vector<std::string>{});
    if(value.contains("signature") && !value["signature"].is_null()) {
        std::string error;
        auto signature = SkillSignatureEnvelope::from_json(value["signature"], &error);
        if(!signature) throw std::runtime_error("invalid persisted MCP signature: " + error);
        manifest.signature = std::move(*signature);
    }
    return manifest;
}

bool unsafe_shell_text(const std::string& value) {
    return value.find("$('") != std::string::npos || value.find("$(") != std::string::npos ||
           value.find('`') != std::string::npos || value.find(";") != std::string::npos ||
           value.find("&&") != std::string::npos || value.find("||") != std::string::npos;
}

std::string http_host(const std::string& endpoint) {
    const auto scheme = endpoint.find("://");
    if(scheme == std::string::npos) return {};
    const auto begin = scheme + 3;
    const auto end = endpoint.find_first_of("/:?#", begin);
    return endpoint.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

bool private_host(std::string host) {
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return host == "localhost" || host == "::1" || host == "[::1]" ||
           host.rfind("127.", 0) == 0 || host.rfind("10.", 0) == 0 ||
           host.rfind("192.168.", 0) == 0 || host.rfind("169.254.", 0) == 0 ||
           host.rfind("172.16.", 0) == 0 || host.rfind("172.17.", 0) == 0 ||
           host.rfind("172.18.", 0) == 0 || host.rfind("172.19.", 0) == 0 ||
           host.rfind("172.2", 0) == 0 || host.rfind("172.30.", 0) == 0 ||
           host.rfind("172.31.", 0) == 0;
}

void validate_transport(const CapabilityTransportDescriptor& transport, bool development) {
    if(transport.kind == "http") {
        const bool secure = transport.endpoint.rfind("https://", 0) == 0;
        const bool local_development = development && transport.endpoint.rfind("http://", 0) == 0;
        if((!secure && !local_development) || transport.endpoint.find('@') != std::string::npos)
            throw std::invalid_argument("unsafe MCP HTTP endpoint");
        const auto host = http_host(transport.endpoint);
        if(host.empty() || (!development && private_host(host)))
            throw std::invalid_argument("MCP HTTP endpoint violates SSRF policy");
        if(!transport.command.empty() || !transport.arguments.empty())
            throw std::invalid_argument("HTTP MCP contains stdio fields");
    } else if(transport.kind == "stdio") {
        if(transport.command.empty() || !fs::path(transport.command).is_absolute() ||
           unsafe_shell_text(transport.command) || !digest_value(transport.executable_digest))
            throw std::invalid_argument("unsafe MCP stdio executable");
        for(const auto& argument : transport.arguments)
            if(unsafe_shell_text(argument)) throw std::invalid_argument("unsafe MCP stdio argument");
        for(const auto& variable : transport.environment_allowlist)
            if(!valid_id(variable)) throw std::invalid_argument("invalid MCP environment allowlist");
        if(!transport.endpoint.empty()) throw std::invalid_argument("stdio MCP contains HTTP endpoint");
    } else {
        throw std::invalid_argument("unsupported MCP transport kind");
    }
    if(!transport.credential_ref.empty() && !valid_id(transport.credential_ref))
        throw std::invalid_argument("invalid MCP credential reference");
}

bool durable_file(const fs::path& path) {
#if defined(_WIN32)
    (void)path; return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if(fd < 0) return false;
    const bool result = ::fsync(fd) == 0; ::close(fd); return result;
#endif
}

bool durable_directory(const fs::path& path) {
#if defined(_WIN32)
    (void)path; return true;
#else
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if(fd < 0) return false;
    const bool result = ::fsync(fd) == 0; ::close(fd); return result;
#endif
}

void secure_directory(const fs::path& path) {
    fs::create_directories(path);
#if !defined(_WIN32)
    if(::chmod(path.c_str(), 0700) != 0) throw std::runtime_error("cannot secure MCP registry");
#endif
}

class RegistryFileLock {
public:
    explicit RegistryFileLock(const fs::path& path) {
#if !defined(_WIN32)
        fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if(fd_ < 0 || ::flock(fd_, LOCK_EX) != 0) throw std::runtime_error("cannot lock MCP registry");
#else
        (void)path;
#endif
    }
    ~RegistryFileLock() {
#if !defined(_WIN32)
        if(fd_ >= 0) { (void)::flock(fd_, LOCK_UN); ::close(fd_); }
#endif
    }
private:
    int fd_ = -1;
};

std::string generation_name(std::uint64_t generation) {
    std::ostringstream output;
    output << std::setw(20) << std::setfill('0') << generation;
    return output.str();
}

std::vector<std::string> registry_candidates(const fs::path& root) {
    std::vector<std::string> result;
    std::ifstream current(root / "CURRENT");
    std::string selected;
    if(std::getline(current, selected) && valid_id(selected)) result.push_back(selected);
    std::error_code error;
    const auto generations = root / "generations";
    if(fs::exists(generations, error)) for(const auto& entry : fs::directory_iterator(generations)) {
        const auto name = entry.path().filename().string();
        if(entry.is_regular_file() && name.ends_with(".json")) result.push_back(name);
    }
    std::sort(result.begin(), result.end(), std::greater<>());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    if(!selected.empty()) {
        const auto found = std::find(result.begin(), result.end(), selected);
        if(found != result.end()) std::rotate(result.begin(), found, found + 1);
    }
    return result;
}
} // namespace

json canonical_capability_manifest(const CapabilityManifest& manifest) {
    return {{"schema_version", 1}, {"id", manifest.id}, {"version", manifest.version},
            {"origin", manifest.origin}, {"kind", manifest.kind},
            {"permissions", sorted_unique(manifest.permissions)},
            {"transport", transport_json(manifest.transport)},
            {"tenant_visibility", sorted_unique(manifest.tenant_visibility)},
            {"dependency_lock", sorted_unique(manifest.dependency_lock)}};
}

std::string capability_manifest_digest(const CapabilityManifest& manifest) {
    const auto digest = skill_sha256_bytes(canonical_capability_manifest(manifest).dump());
    if(!digest) throw std::runtime_error("SHA-256 unavailable for MCP manifest");
    return *digest;
}

struct McpCapabilityRegistry::RegistryState {
    struct Entry { CapabilityStatus status; std::shared_ptr<MCPClient> client; };
    mutable std::mutex mutex;
    std::map<std::string, Entry> entries;
    std::uint64_t generation = 0;
};

CapabilityLease::CapabilityLease(CapabilityLease&& other) noexcept
    : release_(std::move(other.release_)) {}
CapabilityLease& CapabilityLease::operator=(CapabilityLease&& other) noexcept {
    if(this != &other) {
        if(release_) release_();
        release_ = std::move(other.release_);
    }
    return *this;
}
CapabilityLease::~CapabilityLease() { if(release_) release_(); }

McpCapabilityRegistry::McpCapabilityRegistry(std::shared_ptr<ToolBus> toolbus,
                                             CapabilityAuditSink audit,
                                             bool require_signature)
    : toolbus_(std::move(toolbus)), audit_(std::move(audit)),
      require_signature_(require_signature),
      development_unsigned_allowed_(!require_signature),
      state_(std::make_shared<RegistryState>()) {
    if(!toolbus_) throw std::invalid_argument("MCP lifecycle requires ToolBus");
}

McpCapabilityRegistry::~McpCapabilityRegistry() {
    std::vector<std::pair<std::string, std::shared_ptr<MCPClient>>> clients;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        for(auto& [id, entry] : state_->entries) {
            if(entry.status.state == CapabilityLifecycleState::Active ||
               entry.status.state == CapabilityLifecycleState::Draining)
                clients.emplace_back(id, std::move(entry.client));
        }
    }
    for(auto& [id, client] : clients) {
        toolbus_->unregister_mcp_service(id);
        if(client) client->disconnect();
    }
}

void McpCapabilityRegistry::notify(const CapabilityStatus& status) const noexcept {
    try { if(audit_) audit_(status); } catch(...) {}
}

void McpCapabilityRegistry::set_trust_store(SkillTrustStore trust, bool required) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    trust_ = std::move(trust);
    require_signature_ = required;
    development_unsigned_allowed_ = !required;
}

void McpCapabilityRegistry::set_development_unsigned_allowed(bool allowed) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    development_unsigned_allowed_ = allowed;
}

void McpCapabilityRegistry::validate_manifest(const CapabilityManifest& manifest) const {
    if(manifest.kind != "mcp" || !valid_id(manifest.id) || manifest.version.empty() ||
       manifest.origin.empty() || !digest_value(manifest.digest) ||
       manifest.digest != capability_manifest_digest(manifest) ||
       manifest.permissions != sorted_unique(manifest.permissions) ||
       manifest.tenant_visibility.empty())
        throw std::invalid_argument("invalid MCP capability manifest");
    for(const auto& tenant : manifest.tenant_visibility)
        if(!valid_id(tenant)) throw std::invalid_argument("invalid MCP tenant visibility");
    validate_transport(manifest.transport, development_unsigned_allowed_);
    if(require_signature_ && !manifest.signature && !development_unsigned_allowed_)
        throw std::invalid_argument("MCP manifest signature required");
    if(manifest.signature) {
        if(!trust_) throw std::invalid_argument("MCP trust store required");
        const auto& signature = *manifest.signature;
        if(signature.subject_kind != "mcp-capability" ||
           signature.subject_digest != manifest.digest || signature.source_uri != manifest.origin)
            throw std::invalid_argument("MCP manifest signature identity mismatch");
        const auto result = verify_skill_signature(signature, *trust_,
                                                   SkillTrustRole::Capability,
                                                   std::time(nullptr));
        if(!result.ok) throw std::invalid_argument("MCP manifest signature rejected: " + result.error);
    }
}

void McpCapabilityRegistry::stage(CapabilityManifest manifest, std::shared_ptr<MCPClient> client) {
    if(!client) throw std::invalid_argument("MCP client required");
    validate_manifest(manifest);
    CapabilityStatus status;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto found = state_->entries.find(manifest.id);
        std::uint64_t revision = 1;
        if(found != state_->entries.end()) {
            if(found->second.status.state != CapabilityLifecycleState::Removed)
                throw std::invalid_argument("capability already exists");
            revision = found->second.status.revision + 1;
        }
        status = {std::move(manifest), CapabilityLifecycleState::Staged, 0, {}, revision,
                  CapabilityLifecycleState::Staged};
        const auto capability_id = status.manifest.id;
        state_->entries[capability_id] = RegistryState::Entry{status, std::move(client)};
    }
    notify(status);
    if(!status.manifest.signature && development_unsigned_allowed_) {
        auto audit = status;
        audit.failure = "legacy_unsigned_allowed:development";
        notify(audit);
    }
}

void McpCapabilityRegistry::rehydrate(const std::string& id, McpClientFactory factory) {
    if(!factory) throw std::invalid_argument("MCP client factory required");
    CapabilityManifest manifest;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto found = state_->entries.find(id);
        if(found == state_->entries.end() || found->second.status.state == CapabilityLifecycleState::Removed)
            throw std::runtime_error("capability is not recoverable");
        manifest = found->second.status.manifest;
    }
    validate_manifest(manifest);
    auto client = factory(manifest);
    if(!client || !client->ping()) throw std::runtime_error("MCP rehydrate healthcheck failed");
    CapabilityStatus status;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& entry = state_->entries.at(id);
        entry.client = std::move(client);
        entry.status.state = CapabilityLifecycleState::Staged;
        entry.status.failure.clear();
        status = entry.status;
    }
    notify(status);
}

void McpCapabilityRegistry::rebind(const std::string& id, std::shared_ptr<MCPClient> client) {
    rehydrate(id, [client = std::move(client)](const CapabilityManifest&) { return client; });
}

void McpCapabilityRegistry::activate(const std::string& id) {
    std::shared_ptr<MCPClient> client;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        const auto found = state_->entries.find(id);
        if(found == state_->entries.end() || found->second.status.state != CapabilityLifecycleState::Staged)
            throw std::runtime_error("capability is not staged");
        client = found->second.client;
    }
    try {
        if(!client || !client->ping()) throw std::runtime_error("MCP healthcheck failed");
        toolbus_->register_mcp_service(id, client);
        CapabilityStatus status;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            auto& entry = state_->entries.at(id);
            entry.status.state = CapabilityLifecycleState::Active;
            entry.status.desired_state = CapabilityLifecycleState::Active;
            entry.status.failure.clear();
            status = entry.status;
        }
        notify(status);
    } catch(const std::exception& error) {
        toolbus_->unregister_mcp_service(id);
        CapabilityStatus status;
        {
            std::lock_guard<std::mutex> lock(state_->mutex);
            auto& entry = state_->entries.at(id);
            entry.status.state = CapabilityLifecycleState::Failed;
            entry.status.failure = error.what();
            status = entry.status;
        }
        notify(status);
        throw;
    }
}

void McpCapabilityRegistry::drain(const std::string& id) {
    CapabilityStatus status;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto& entry = state_->entries.at(id);
        if(entry.status.state != CapabilityLifecycleState::Active)
            throw std::runtime_error("capability is not active");
        entry.status.state = CapabilityLifecycleState::Draining;
        entry.status.desired_state = CapabilityLifecycleState::Draining;
        status = entry.status;
    }
    toolbus_->unregister_mcp_service(id);
    notify(status);
}

bool McpCapabilityRegistry::remove(const std::string& id) {
    CapabilityStatus status;
    std::shared_ptr<MCPClient> client;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto found = state_->entries.find(id);
        if(found == state_->entries.end() || found->second.status.state == CapabilityLifecycleState::Removed)
            return true;
        if(found->second.status.leases) return false;
        if(found->second.status.state == CapabilityLifecycleState::Active)
            throw std::runtime_error("drain capability before removal");
        found->second.status.state = CapabilityLifecycleState::Removed;
        found->second.status.desired_state = CapabilityLifecycleState::Removed;
        status = found->second.status;
        client = std::move(found->second.client);
    }
    if(client) client->disconnect();
    notify(status);
    return true;
}

CapabilityLease McpCapabilityRegistry::acquire(const std::string& id,
                                                std::uint64_t expected_revision,
                                                std::string expected_digest) {
    std::uint64_t revision = 0;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        auto found = state_->entries.find(id);
        if(found == state_->entries.end() || found->second.status.state != CapabilityLifecycleState::Active)
            throw std::runtime_error("capability is not active");
        if((expected_revision && found->second.status.revision != expected_revision) ||
           (!expected_digest.empty() && found->second.status.manifest.digest != expected_digest))
            throw std::runtime_error("capability revision pin mismatch");
        ++found->second.status.leases;
        revision = found->second.status.revision;
    }
    std::weak_ptr<RegistryState> weak = state_;
    return CapabilityLease([weak, id, revision] {
        if(auto state = weak.lock()) {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto found = state->entries.find(id);
            if(found != state->entries.end() && found->second.status.revision == revision &&
               found->second.status.leases) --found->second.status.leases;
        }
    });
}

std::optional<CapabilityStatus> McpCapabilityRegistry::status(const std::string& id) const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->entries.find(id);
    if(found == state_->entries.end() || found->second.status.state == CapabilityLifecycleState::Removed)
        return std::nullopt;
    return found->second.status;
}

void McpCapabilityRegistry::save(const std::string& path) const {
    const fs::path root(path);
    secure_directory(root);
    secure_directory(root / "generations");
    RegistryFileLock file_lock(root / "lock");
    json records = json::array();
    std::uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        generation = ++state_->generation;
        for(const auto& [id, entry] : state_->entries) {
            (void)id;
            records.push_back({{"manifest", manifest_json(entry.status.manifest)},
                               {"desired_state", state_name(entry.status.desired_state)},
                               {"revision", entry.status.revision}});
        }
    }
    json payload{{"schema_version", 2}, {"generation", generation}, {"records", records}};
    const auto digest = skill_sha256_bytes(payload.dump());
    if(!digest) throw std::runtime_error("cannot digest MCP registry");
    const json document{{"payload", payload}, {"sha256", *digest}};
    const auto name = generation_name(generation) + ".json";
#if defined(_WIN32)
    const int process_id = 0;
#else
    const int process_id = ::getpid();
#endif
    const auto temporary = root / "generations" /
        (".tmp-" + std::to_string(process_id) + "-" + name);
    const auto committed = root / "generations" / name;
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << document.dump(2) << '\n'; output.flush();
        if(!output) throw std::runtime_error("cannot write MCP registry generation");
    }
#if !defined(_WIN32)
    (void)::chmod(temporary.c_str(), 0600);
#endif
    if(!durable_file(temporary)) throw std::runtime_error("MCP registry fsync failed");
    fs::rename(temporary, committed);
    if(!durable_directory(root / "generations")) throw std::runtime_error("MCP generation publish failed");
    const auto current_tmp = root / ("CURRENT.tmp-" + std::to_string(process_id));
    { std::ofstream output(current_tmp, std::ios::trunc); output << name << '\n'; output.flush(); }
#if !defined(_WIN32)
    (void)::chmod(current_tmp.c_str(), 0600);
#endif
    if(!durable_file(current_tmp)) throw std::runtime_error("MCP CURRENT fsync failed");
    fs::rename(current_tmp, root / "CURRENT");
    if(!durable_directory(root)) throw std::runtime_error("MCP registry commit failed");
}

void McpCapabilityRegistry::load(const std::string& path) {
    const fs::path root(path);
    if(!fs::exists(root)) return;
    std::exception_ptr last_error;
    for(const auto& candidate : registry_candidates(root)) {
        try {
            json document; std::ifstream input(root / "generations" / candidate); input >> document;
            const auto payload = document.at("payload");
            const auto digest = skill_sha256_bytes(payload.dump());
            if(!digest || *digest != document.value("sha256", "") ||
               payload.value("schema_version", 0) != 2 || !payload["records"].is_array())
                throw std::runtime_error("invalid MCP registry generation");
            std::map<std::string, RegistryState::Entry> restored;
            for(const auto& record : payload["records"]) {
                auto manifest = manifest_from_json(record.at("manifest"));
                validate_manifest(manifest);
                const auto desired = state_from_name(record.value("desired_state", "staged"));
                const auto revision = record.value("revision", std::uint64_t{0});
                if(!revision || restored.contains(manifest.id))
                    throw std::runtime_error("duplicate or invalid MCP registry record");
                CapabilityStatus status{std::move(manifest),
                    desired == CapabilityLifecycleState::Removed ? CapabilityLifecycleState::Removed
                                                                 : CapabilityLifecycleState::Validated,
                    0, "restart_requires_rehydrate", revision, desired};
                const auto capability_id = status.manifest.id;
                restored.emplace(capability_id,
                                 RegistryState::Entry{std::move(status), {}});
            }
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                state_->entries = std::move(restored);
                state_->generation = payload.value("generation", std::uint64_t{0});
            }
            return;
        } catch(...) { last_error = std::current_exception(); }
    }
    if(last_error) std::rethrow_exception(last_error);
    throw std::runtime_error("no valid MCP registry generation");
}

} // namespace agent_framework
