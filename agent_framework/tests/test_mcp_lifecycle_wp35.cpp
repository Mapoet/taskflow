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

CapabilityManifest unsigned_manifest(std::string id = "safe_mcp",
                                     std::string version = "1.0.0") {
    CapabilityManifest manifest;
    manifest.id = std::move(id);
    manifest.version = std::move(version);
    manifest.origin = "https://mcp.example/capabilities/safe";
    manifest.permissions = {"resources.read", "tools.call"};
    manifest.transport.kind = "http";
    manifest.transport.endpoint = "https://mcp.example/rpc";
    manifest.transport.host_allowlist = {"mcp.example"};
    manifest.transport.credential_ref = "mcp_fixture_credential";
    manifest.tenant_visibility = {"tenant-a"};
    manifest.dependency_lock = {"protocol:2025-03-26"};
    manifest.digest = capability_manifest_digest(manifest);
    return manifest;
}

CapabilityManifest signed_manifest(const std::string& private_key,
                                   std::string id = "safe_mcp",
                                   std::string version = "1.0.0") {
    auto manifest = unsigned_manifest(std::move(id), std::move(version));
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

std::string file_digest(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    return skill_sha256_bytes(bytes).value_or("");
}
}

int main() {
    auto bus = std::make_shared<ToolBus>();
    std::error_code ignored;

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

    tampered = manifest;
    tampered.origin = "https://attacker.example/capabilities/safe";
    tampered.digest = capability_manifest_digest(tampered);
    rejected = false;
    try {
        McpCapabilityRegistry registry(bus);
        registry.set_trust_store(trust);
        registry.stage(tampered, client());
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "origin replacement was accepted with a reused signature");

    auto revoked_trust = trust;
    revoked_trust.revoked_keys.insert(*key_id);
    rejected = false;
    try {
        McpCapabilityRegistry registry(bus);
        registry.set_trust_store(revoked_trust);
        registry.stage(manifest, client());
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "revoked capability signing key was accepted");

    auto expired_trust = trust;
    expired_trust.keys.front().not_after = std::time(nullptr) - 1;
    rejected = false;
    try {
        McpCapabilityRegistry registry(bus);
        registry.set_trust_store(expired_trust);
        registry.stage(manifest, client());
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "expired capability signing key was accepted");

    auto private_endpoint = unsigned_manifest("private_http");
    private_endpoint.transport.endpoint = "https://127.0.0.1/rpc";
    private_endpoint.transport.host_allowlist = {"127.0.0.1"};
    private_endpoint.digest = capability_manifest_digest(private_endpoint);
    rejected = false;
    try {
        McpCapabilityRegistry registry(bus, {}, false);
        registry.set_development_unsigned_allowed(false);
        registry.stage(private_endpoint, client());
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "private MCP HTTP endpoint bypassed SSRF policy");

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

    auto safe_stdio = unsigned_manifest("safe_stdio");
    safe_stdio.transport = {};
    safe_stdio.transport.kind = "stdio";
    safe_stdio.transport.command = "/usr/bin/true";
    safe_stdio.transport.executable_digest = file_digest(safe_stdio.transport.command);
    safe_stdio.transport.environment_allowlist = {"LANG"};
    safe_stdio.transport.working_directory = "/tmp";
    safe_stdio.digest = capability_manifest_digest(safe_stdio);
    {
        McpCapabilityRegistry registry(bus, {}, false);
        registry.stage(safe_stdio, client());
        require(registry.status(safe_stdio.id).has_value(),
                "digest-locked safe stdio descriptor was rejected");
    }

    // Failed publication leaves the pre-existing ToolBus service intact and no alias exposed.
    {
        auto occupied = client();
        require(occupied->ping(), "cannot initialize occupied MCP fixture");
        bus->register_mcp_service("rollback_mcp@1", occupied);
        McpCapabilityRegistry registry(bus, {}, false);
        auto rollback_manifest = unsigned_manifest("rollback_mcp");
        registry.stage(rollback_manifest, client());
        bool activation_failed = false;
        try { registry.activate("rollback_mcp"); }
        catch(const std::exception&) { activation_failed = true; }
        require(activation_failed &&
                registry.status("rollback_mcp")->state == CapabilityLifecycleState::Failed,
                "failed activation did not enter Failed state");
        require(bus->has_mcp_service("rollback_mcp@1") &&
                !bus->has_mcp_service("rollback_mcp"),
                "failed activation damaged the prior ToolBus state");
        bus->unregister_mcp_service("rollback_mcp@1");
    }

    // Multiple revisions coexist: old and new sessions stay pinned independently.
    const auto pin_root = fs::temp_directory_path() / "mcp-registry-wp35-pins";
    fs::remove_all(pin_root, ignored);
    {
        McpCapabilityRegistry registry(bus);
        registry.set_trust_store(trust);
        const auto revision_one = signed_manifest(private_key, "versioned_mcp", "1.0.0");
        registry.stage(revision_one, client());
        registry.activate(revision_one.id, 1);
        auto old_lease = registry.acquire_for_session(
            revision_one.id, "tenant-a", "old-session", 0, revision_one.digest);
        require(bus->has_mcp_service("versioned_mcp@1"),
                "revision-one ToolBus service missing");

        const auto revision_two = signed_manifest(private_key, "versioned_mcp", "2.0.0");
        registry.stage(revision_two, client());
        registry.activate(revision_two.id, 2);
        auto new_lease = registry.acquire_for_session(
            revision_two.id, "tenant-a", "new-session", 0, revision_two.digest);
        auto old_lease_again = registry.acquire_for_session(
            revision_one.id, "tenant-a", "old-session", 0, revision_one.digest);
        require(registry.status("versioned_mcp", 1)->leases == 2 &&
                registry.status("versioned_mcp", 2)->leases == 1,
                "session revision pins did not select old/new revisions");
        require(bus->has_mcp_service("versioned_mcp@1") &&
                bus->has_mcp_service("versioned_mcp@2") &&
                bus->has_mcp_service("versioned_mcp"),
                "versioned ToolBus publication is incomplete");
        bool tenant_rejected = false;
        try {
            (void)registry.acquire_for_session(
                revision_two.id, "tenant-b", "forbidden-session");
        } catch(const std::runtime_error&) { tenant_rejected = true; }
        require(tenant_rejected, "cross-tenant capability lease was accepted");
        registry.save(pin_root.string());

        registry.drain("versioned_mcp", 1);
        bool drained_pin_rejected = false;
        try {
            (void)registry.acquire_for_session(
                revision_one.id, "tenant-a", "old-session");
        } catch(const std::runtime_error&) { drained_pin_rejected = true; }
        require(drained_pin_rejected, "draining revision accepted a new pinned lease");
        require(!registry.remove("versioned_mcp", 1),
                "draining revision was removed while leased");
        old_lease = CapabilityLease{};
        old_lease_again = CapabilityLease{};
        require(registry.remove("versioned_mcp", 1),
                "drained revision was not collected after leases completed");
        new_lease = CapabilityLease{};
        registry.drain("versioned_mcp", 2);
        require(registry.remove("versioned_mcp", 2), "new revision cleanup failed");
    }
    {
        McpCapabilityRegistry restored(bus);
        restored.set_trust_store(trust);
        restored.load(pin_root.string());
        restored.rehydrate("versioned_mcp", [](const CapabilityManifest&) { return client(); }, 1);
        restored.rehydrate("versioned_mcp", [](const CapabilityManifest&) { return client(); }, 2);
        restored.activate("versioned_mcp", 1);
        restored.activate("versioned_mcp", 2);
        auto restored_old = restored.acquire_for_session(
            "versioned_mcp", "tenant-a", "old-session", 0,
            restored.status("versioned_mcp", 1)->manifest.digest);
        require(restored.status("versioned_mcp", 1)->leases == 1 &&
                restored.status("versioned_mcp", 2)->leases == 0,
                "persisted session revision pin was not restored");
    }

    // Stale registry writers fail with a generation conflict instead of overwriting updates.
    {
        auto bus_a = std::make_shared<ToolBus>();
        auto bus_b = std::make_shared<ToolBus>();
        McpCapabilityRegistry writer_a(bus_a);
        McpCapabilityRegistry writer_b(bus_b);
        writer_a.set_trust_store(trust);
        writer_b.set_trust_store(trust);
        writer_a.load(pin_root.string());
        writer_b.load(pin_root.string());
        writer_a.stage(signed_manifest(private_key, "writer_a_mcp"), client());
        writer_a.save(pin_root.string());
        bool conflict = false;
        try { writer_b.save(pin_root.string()); }
        catch(const std::runtime_error&) { conflict = true; }
        require(conflict, "stale MCP registry writer silently overwrote a generation");
    }
    {
        auto changed_trust = trust;
        changed_trust.revoked_publishers.insert("unrelated.publisher");
        McpCapabilityRegistry mismatched(bus);
        mismatched.set_trust_store(changed_trust);
        bool mismatch_rejected = false;
        try { mismatched.load(pin_root.string()); }
        catch(const std::runtime_error&) { mismatch_rejected = true; }
        require(mismatch_rejected, "MCP registry trust-store revision mismatch was accepted");
    }
    fs::remove_all(pin_root, ignored);

    const auto registry_root = fs::temp_directory_path() / "mcp-registry-wp35-v2";
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
