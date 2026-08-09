#include <agent/mcp_client/mcp_lifecycle.hpp>
#include <agent/toolbus/toolbus.hpp>

#include <openssl/evp.h>
#include <openssl/pem.h>

#include <filesystem>
#include <fstream>
#include <stdexcept>

using namespace agent_framework;
namespace fs = std::filesystem;

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

class FakeTransport final : public MCPTransportInterface {
public:
    bool connect(const std::string&) override { connected = true; return true; }
    void disconnect() override { connected = false; }
    bool is_connected() const override { return connected; }
    MCPTransport get_transport_type() const override { return MCPTransport::HTTP; }
    void send_notification(const json&) override {}
    json transceive(const json& request) override {
        const auto id = request.at("id");
        const auto method = request.at("method").get<std::string>();
        if(method == "initialize")
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"capabilities", {{"tools", json::object()}}}}}};
        if(method == "tools/list")
            return {{"jsonrpc", "2.0"}, {"id", id},
                    {"result", {{"tools", json::array()}}}};
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
    }
    bool connected = false;
};

std::shared_ptr<MCPClient> client() {
    return MCPClient::create_with_transport(std::make_unique<FakeTransport>());
}

std::string bio_string(BIO* bio) {
    char* data = nullptr;
    const auto size = BIO_get_mem_data(bio, &data);
    return std::string(data, static_cast<std::size_t>(size));
}

std::pair<std::string, std::string> keypair() {
    auto* context = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    require(context && EVP_PKEY_keygen_init(context) == 1, "cannot initialize Ed25519");
    EVP_PKEY* key = nullptr;
    require(EVP_PKEY_keygen(context, &key) == 1, "cannot generate Ed25519 key");
    EVP_PKEY_CTX_free(context);
    auto* private_bio = BIO_new(BIO_s_mem());
    auto* public_bio = BIO_new(BIO_s_mem());
    require(PEM_write_bio_PrivateKey(private_bio, key, nullptr, nullptr, 0, nullptr, nullptr) == 1,
            "cannot serialize private key");
    require(PEM_write_bio_PUBKEY(public_bio, key) == 1, "cannot serialize public key");
    auto private_key = bio_string(private_bio);
    auto public_key = bio_string(public_bio);
    BIO_free(private_bio); BIO_free(public_bio); EVP_PKEY_free(key);
    return {private_key, public_key};
}

CapabilityManifest unsigned_manifest(std::string id = "safe_mcp") {
    CapabilityManifest manifest;
    manifest.id = std::move(id);
    manifest.version = "1.0.0";
    manifest.origin = "https://mcp.example/capabilities/safe";
    manifest.permissions = {"resources.read", "tools.call"};
    manifest.transport.kind = "http";
    manifest.transport.endpoint = "https://mcp.example/rpc";
    manifest.transport.credential_ref = "mcp_fixture_credential";
    manifest.tenant_visibility = {"tenant-a"};
    manifest.dependency_lock = {"protocol:2025-03-26"};
    manifest.digest = capability_manifest_digest(manifest);
    return manifest;
}

CapabilityManifest signed_manifest(const std::string& private_key) {
    auto manifest = unsigned_manifest();
    SkillSignatureEnvelope envelope;
    envelope.subject_kind = "mcp-capability";
    envelope.subject_digest = manifest.digest;
    envelope.publisher = "org.example";
    envelope.source_uri = manifest.origin;
    envelope.sbom_digest = std::string(64, 'b');
    envelope.provenance_digest = std::string(64, 'c');
    auto signed_result = sign_skill_subject(std::move(envelope), private_key);
    require(signed_result.ok, "cannot sign capability manifest");
    manifest.signature = std::move(signed_result.envelope);
    return manifest;
}

std::string current_name(const fs::path& root) {
    std::ifstream input(root / "CURRENT");
    std::string name;
    std::getline(input, name);
    return name;
}
}

int main() {
    auto bus = std::make_shared<ToolBus>();

    // Production is fail-closed for unsigned capabilities.
    bool rejected = false;
    try {
        McpCapabilityRegistry production(bus);
        production.stage(unsigned_manifest(), client());
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "unsigned production capability was accepted");

    // Explicit development mode remains available and is auditable.
    bool unsigned_audited = false;
    {
        McpCapabilityRegistry development(bus, [&](const CapabilityStatus& status) {
            if(status.failure.find("legacy_unsigned_allowed") != std::string::npos)
                unsigned_audited = true;
        }, false);
        auto manifest = unsigned_manifest("development_mcp");
        development.stage(manifest, client());
        development.activate(manifest.id);
        auto lease = development.acquire(manifest.id, 1, manifest.digest);
        bool pin_rejected = false;
        try { (void)development.acquire(manifest.id, 2, manifest.digest); }
        catch(const std::runtime_error&) { pin_rejected = true; }
        require(pin_rejected, "wrong capability revision pin accepted");
        development.drain(manifest.id);
        bool draining_rejected = false;
        try { (void)development.acquire(manifest.id); }
        catch(const std::runtime_error&) { draining_rejected = true; }
        require(draining_rejected, "draining capability accepted a new lease");
        require(!development.remove(manifest.id), "leased capability was removed");
        lease = CapabilityLease{};
        require(development.remove(manifest.id), "drained capability was not removed");
    }
    require(unsigned_audited, "development unsigned capability was not audited");

    const auto [private_key, public_key] = keypair();
    std::string key_error;
    const auto key_id = skill_public_key_id(public_key, &key_error);
    require(key_id.has_value(), "cannot derive capability key id");
    SkillTrustStore trust;
    trust.keys.push_back({*key_id, "org.example", public_key,
                          {SkillTrustRole::Capability},
                          {"https://mcp.example/capabilities/"}, 0, 0});
    const auto manifest = signed_manifest(private_key);

    // Any canonical security-field change invalidates the digest/signature binding.
    auto tampered = manifest;
    tampered.permissions.push_back("admin");
    rejected = false;
    try {
        McpCapabilityRegistry registry(bus);
        registry.set_trust_store(trust);
        registry.stage(tampered, client());
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "tampered permissions were accepted");

    auto unsafe = unsigned_manifest("unsafe_stdio");
    unsafe.transport = {};
    unsafe.transport.kind = "stdio";
    unsafe.transport.command = "/bin/sh";
    unsafe.transport.arguments = {"-c", "echo $(secret)"};
    unsafe.transport.executable_digest = std::string(64, 'a');
    unsafe.digest = capability_manifest_digest(unsafe);
    rejected = false;
    try {
        McpCapabilityRegistry registry(bus, {}, false);
        registry.stage(unsafe, client());
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "shell-expanding stdio descriptor was accepted");

    const auto registry_root = fs::temp_directory_path() / "mcp-registry-wp35-v2";
    std::error_code ignored;
    fs::remove_all(registry_root, ignored);
    {
        McpCapabilityRegistry registry(bus);
        registry.set_trust_store(trust);
        registry.stage(manifest, client());
        registry.save(registry_root.string()); // rollback generation: desired Staged
        registry.activate(manifest.id);
        auto lease = registry.acquire(manifest.id, 1, manifest.digest);
        registry.drain(manifest.id);
        registry.save(registry_root.string()); // current generation: desired Draining
        lease = CapabilityLease{};
    }

    // Corrupt CURRENT generation; load must fall back to the previous valid generation.
    const auto newest = current_name(registry_root);
    const auto newest_path = registry_root / "generations" / newest;
    json document;
    { std::ifstream input(newest_path); input >> document; }
    document["sha256"] = std::string(64, '0');
    { std::ofstream output(newest_path, std::ios::trunc); output << document.dump(2); }

    {
        McpCapabilityRegistry restored(bus);
        restored.set_trust_store(trust);
        restored.load(registry_root.string());
        const auto loaded = restored.status(manifest.id);
        require(loaded && loaded->state == CapabilityLifecycleState::Validated,
                "restart did not downgrade capability to Validated");
        require(loaded->manifest.signature.has_value(), "signature was lost during restart");
        require(!bus->has_mcp_service(manifest.id), "restart auto-activated MCP service");
        restored.rehydrate(manifest.id, [](const CapabilityManifest&) { return client(); });
        require(restored.status(manifest.id)->state == CapabilityLifecycleState::Staged,
                "rehydrate did not stage capability");
        restored.activate(manifest.id);
        require(bus->has_mcp_service(manifest.id), "rehydrated capability did not activate");
        restored.drain(manifest.id);
        require(restored.remove(manifest.id), "rehydrated capability was not removable");
        require(!restored.status(manifest.id), "removed tombstone exposed as active status");
    }

    // Lease cleanup after registry destruction must not dereference a dead registry.
    CapabilityLease late_lease;
    {
        auto late_bus = std::make_shared<ToolBus>();
        auto registry = std::make_unique<McpCapabilityRegistry>(late_bus, CapabilityAuditSink{}, false);
        auto late_manifest = unsigned_manifest("late_lease_mcp");
        registry->stage(late_manifest, client());
        registry->activate(late_manifest.id);
        late_lease = registry->acquire(late_manifest.id);
    }
    late_lease = CapabilityLease{};

    fs::remove_all(registry_root, ignored);
}
