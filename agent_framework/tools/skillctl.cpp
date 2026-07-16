#include <agent/skills/skill_command.hpp>
#include <agent/skills/skill_archive.hpp>
#include <agent/skills/skill_lifecycle.hpp>
#include <agent/skills/skill_remote_registry.hpp>
#include <agent/skills/skill_sbom.hpp>
#include <agent/skills/skill_supply_chain.hpp>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

using namespace agent_framework;
namespace {
namespace fs = std::filesystem;

int emit(const SkillCommandResponse& response) {
    if(response.raw_output) std::cout.write(
        response.raw_output->data(), static_cast<std::streamsize>(response.raw_output->size()));
    else std::cout << response.to_json().dump(2) << '\n';
    return static_cast<int>(response.exit);
}

SkillCommandResponse usage_error(const std::string& command, const std::string& message) {
    SkillCommandResponse response;
    response.exit = SkillCliExit::Usage;
    response.command = command;
    response.error = {{"code", "skillctl_usage_error"}, {"message", message},
                      {"details", {{"usage", skillctl_usage()}}}};
    return response;
}

SkillCommandResponse success(const std::string& command, nlohmann::json data) {
    SkillCommandResponse response;
    response.command = command;
    response.data = std::move(data);
    return response;
}

SkillCommandResponse integrity_error(const std::string& command, const std::string& message) {
    SkillCommandResponse response;
    response.exit = SkillCliExit::IntegrityFailed;
    response.command = command;
    response.error = {{"code", "skill_supply_chain_invalid"}, {"message", message},
                      {"details", nlohmann::json::object()}};
    return response;
}

std::optional<std::string> read_bounded(const fs::path& path, std::uint64_t limit,
                                        std::string& error) {
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    if(ec || size > limit) { error = "file is unavailable or oversized"; return std::nullopt; }
    std::string bytes(static_cast<std::size_t>(size), '\0');
    std::ifstream input(path, std::ios::binary);
    if(!input || (size && !input.read(bytes.data(), static_cast<std::streamsize>(size)))) {
        error = "file read failed";
        return std::nullopt;
    }
    return bytes;
}

bool write_file(const fs::path& path, const std::string& bytes, std::string& error) {
    std::error_code ec;
    if(!path.parent_path().empty()) fs::create_directories(path.parent_path(), ec);
    if(ec) { error = ec.message(); return false; }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if(!output) { error = "file write failed"; return false; }
    return true;
}

std::optional<SkillTrustStore> load_trust(const fs::path& path, std::string& error) {
    auto bytes = read_bounded(path, 4ull * 1024 * 1024, error);
    if(!bytes) return std::nullopt;
    try { return SkillTrustStore::from_json(nlohmann::json::parse(*bytes), &error); }
    catch(const std::exception& ex) { error = ex.what(); return std::nullopt; }
}

std::optional<SkillSignatureEnvelope> load_signature(const fs::path& path, std::string& error) {
    auto bytes = read_bounded(path, SkillRemoteRegistryClient::kMaxSignatureBytes, error);
    if(!bytes) return std::nullopt;
    try { return SkillSignatureEnvelope::from_json(nlohmann::json::parse(*bytes), &error); }
    catch(const std::exception& ex) { error = ex.what(); return std::nullopt; }
}

std::optional<SkillPackageMetadata> audit_metadata(const fs::path& archive, std::string& error) {
    auto inspected = inspect_skill_archive(archive);
    if(!inspected.ok) { error = inspected.error; return std::nullopt; }
    const auto temporary = fs::temp_directory_path() /
        ("skillctl-audit-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    auto extracted = extract_skill_archive(archive, temporary);
    if(!extracted.ok) { error = extracted.error; return std::nullopt; }
    auto sbom = skill_sha256_file(temporary / "META-INF/sbom.cdx.json", &error);
    auto provenance = skill_sha256_file(temporary / "META-INF/provenance.json", &error);
    std::error_code ec;
    fs::remove_all(temporary, ec);
    if(!sbom || !provenance) return std::nullopt;
    SkillPackageMetadata metadata;
    metadata.archive_digest = inspected.archive_digest;
    metadata.sbom_digest = *sbom;
    metadata.provenance_digest = *provenance;
    metadata.entry_count = inspected.entries.size();
    return metadata;
}

} // namespace

int main(int argc, char** argv) {
    std::vector<std::string> tokens;
    for(int i = 1; i < argc; ++i) tokens.emplace_back(argv[i]);
    const auto parsed = parse_skill_cli_arguments(tokens);
    if(!parsed) {
        std::cout << parsed.response.to_json().dump(2) << '\n';
        return static_cast<int>(parsed.response.exit);
    }
    if(parsed.arguments->help) {
        std::cout << skillctl_usage() << '\n';
        return 0;
    }

    const auto& invocation = *parsed.arguments;
    auto registry = std::make_shared<SkillRegistry>(invocation.root);
    registry->scan_or_reload();
    SkillCommandService service(registry, invocation.root / ".skill-cache");
    const auto& command = invocation.command;
    const auto& operands = invocation.operands;
    if(command == "list" && operands.empty()) return emit(service.list());
    if(command == "validate" && operands.empty()) return emit(service.validate());
    if(command == "show" && operands.size() == 1) return emit(service.show(operands[0]));
    if(command == "inspect" && !operands.empty() && operands.size() <= 2) {
        const bool resolved = operands.size() == 2 && operands[1] == "--resolved";
        if(operands.size() == 2 && !resolved)
            return emit(usage_error(command, "inspect accepts only --resolved"));
        return emit(service.inspect(operands[0], resolved));
    }
    if(command == "read" && operands.size() >= 3) {
        const auto kind = parse_skill_resource_kind(operands[1]);
        if(!kind) return emit(usage_error(command, "unknown resource kind: " + operands[1]));
        std::size_t max_bytes = 65536;
        bool raw = false;
        for(std::size_t index = 3; index < operands.size(); ++index) {
            if(operands[index] == "--raw") raw = true;
            else if(operands[index] == "--max-bytes" && index + 1 < operands.size())
                max_bytes = std::strtoull(operands[++index].c_str(), nullptr, 10);
            else if(index == 3 && !operands[index].starts_with("-"))
                max_bytes = std::strtoull(operands[index].c_str(), nullptr, 10);
            else return emit(usage_error(command, "invalid read option: " + operands[index]));
        }
        return emit(service.read(operands[0], *kind, operands[2], max_bytes, raw));
    }
    if(command == "lint") {
        std::string skill_id;
        bool warnings_as_errors = false;
        for(const auto& operand : operands) {
            if(operand == "--warnings-as-errors") warnings_as_errors = true;
            else if(skill_id.empty() && !operand.starts_with("-")) skill_id = operand;
            else return emit(usage_error(command, "invalid lint option: " + operand));
        }
        return emit(service.lint(skill_id, warnings_as_errors));
    }
    if(command == "graph" && operands.empty()) return emit(service.graph());
    if(command == "permissions" && !operands.empty()) {
        SkillPermissionGrant grant;
        for(std::size_t index = 1; index < operands.size(); index += 2) {
            if(index + 1 >= operands.size())
                return emit(usage_error(command, "permission grant value is missing"));
            const auto& option = operands[index];
            const auto& value = operands[index + 1];
            if(option == "--grant-tool") grant.tools.push_back(value);
            else if(option == "--grant-network") grant.network.push_back(value);
            else if(option == "--grant-env") grant.environment.push_back(value);
            else if(option == "--grant-read") grant.filesystem_read.push_back(value);
            else if(option == "--grant-write") grant.filesystem_write.push_back(value);
            else if(option == "--grant-secret") grant.secrets.push_back(value);
            else return emit(usage_error(command, "unknown permission option: " + option));
        }
        return emit(service.permissions(operands[0], grant));
    }
    if(command == "doctor" && !operands.empty()) {
        SkillDoctorOptions options;
        for(std::size_t index = 1; index < operands.size(); index += 2) {
            if(index + 1 >= operands.size())
                return emit(usage_error(command, "doctor option value is missing"));
            const auto& option = operands[index];
            const auto& value = operands[index + 1];
            if(option == "--runtime") options.available_runtimes.push_back(value);
            else if(option == "--model") options.available_models.push_back(value);
            else if(option == "--device") options.available_devices.push_back(value);
            else if(option == "--precision") options.available_precisions.push_back(value);
            else if(option == "--memory")
                options.available_memory_bytes = std::strtoull(value.c_str(), nullptr, 10);
            else if(option == "--grant-read") options.grants.filesystem_read.push_back(value);
            else if(option == "--grant-write") options.grants.filesystem_write.push_back(value);
            else return emit(usage_error(command, "unknown doctor option: " + option));
        }
        return emit(service.doctor(operands[0], options));
    }
    if(command == "reference" && operands.size() >= 3) {
        const auto& operation = operands[0];
        if(operation == "page") {
            std::uint64_t offset = 0;
            std::size_t max_bytes = 65536;
            for(std::size_t index = 3; index < operands.size(); ++index) {
                if(operands[index] == "--offset" && index + 1 < operands.size())
                    offset = std::strtoull(operands[++index].c_str(), nullptr, 10);
                else if(operands[index] == "--max-bytes" && index + 1 < operands.size())
                    max_bytes = std::strtoull(operands[++index].c_str(), nullptr, 10);
                else return emit(usage_error("reference page", "invalid reference page option"));
            }
            return emit(service.reference_page(operands[1], operands[2], offset, max_bytes));
        }
        if(operation == "search" && operands.size() >= 4) {
            std::size_t limit = 10;
            for(std::size_t index = 4; index < operands.size(); ++index) {
                if(operands[index] == "--limit" && index + 1 < operands.size())
                    limit = std::strtoull(operands[++index].c_str(), nullptr, 10);
                else return emit(usage_error("reference search", "invalid reference search option"));
            }
            return emit(service.reference_search(operands[1], operands[2], operands[3], limit));
        }
        return emit(usage_error("reference", "expected reference page or reference search"));
    }
    if(command == "cache" && !operands.empty()) {
        if(operands[0] == "status" && operands.size() == 1) return emit(service.cache_status());
        if(operands[0] == "verify" && operands.size() == 1) return emit(service.cache_verify());
        if(operands[0] == "gc" && operands.size() == 1) return emit(service.cache_gc());
        if(operands[0] == "pin" && operands.size() == 2)
            return emit(service.cache_pin(operands[1], true));
        if(operands[0] == "unpin" && operands.size() == 2)
            return emit(service.cache_pin(operands[1], false));
        return emit(usage_error("cache", "invalid cache operation"));
    }
    if(command == "model" && operands.size() >= 3 && operands[0] == "check") {
        SkillModelHostCapabilities host;
#if defined(__linux__)
        host.mmap_supported = true;
#endif
        for(std::size_t index = 3; index < operands.size(); ++index) {
            if(operands[index] == "--runtime" && index + 1 < operands.size())
                host.runtimes.push_back(operands[++index]);
            else if(operands[index] == "--device" && index + 1 < operands.size())
                host.devices.push_back(operands[++index]);
            else if(operands[index] == "--precision" && index + 1 < operands.size())
                host.precisions.push_back(operands[++index]);
            else if(operands[index] == "--memory" && index + 1 < operands.size())
                host.available_memory_bytes = std::strtoull(operands[++index].c_str(), nullptr, 10);
            else return emit(usage_error("model check", "invalid model check option"));
        }
        host.max_readonly_bytes = host.available_memory_bytes;
        return emit(service.model_check(operands[1], operands[2], host));
    }
    if(command == "test") {
        std::string skill_id;
        std::string filter;
        std::size_t jobs = 1;
        for(std::size_t index = 0; index < operands.size(); ++index) {
            if(operands[index] == "--filter" && index + 1 < operands.size())
                filter = operands[++index];
            else if(operands[index] == "--jobs" && index + 1 < operands.size())
                jobs = std::strtoull(operands[++index].c_str(), nullptr, 10);
            else if(skill_id.empty() && !operands[index].starts_with("-"))
                skill_id = operands[index];
            else return emit(usage_error(command, "invalid test option: " + operands[index]));
        }
        return emit(service.test(skill_id, filter, jobs));
    }
    if(command == "package" && !operands.empty() && operands[0] == "build") {
        if(operands.size() < 3) return emit(usage_error("package build", "expected SOURCE OUTPUT"));
        SkillProvenanceOptions options;
        for(std::size_t index = 3; index < operands.size(); ++index) {
            if(operands[index] == "--source" && index + 1 < operands.size()) options.source_uri = operands[++index];
            else if(operands[index] == "--revision" && index + 1 < operands.size()) options.source_revision = operands[++index];
            else if(operands[index] == "--builder" && index + 1 < operands.size()) options.builder_id = operands[++index];
            else return emit(usage_error("package build", "invalid package build option"));
        }
        auto built = build_audited_skill_archive(operands[1], operands[2], options);
        if(!built.ok) return emit(integrity_error("package build", built.error));
        return emit(success("package build", {{"output", fs::path(operands[2]).generic_string()},
            {"metadata", built.metadata.to_json()}, {"entries", built.archive.entries.size()}}));
    }
    if(command == "package" && operands.size() == 2 && operands[0] == "inspect") {
        auto inspected = inspect_skill_archive(operands[1]);
        if(!inspected.ok) return emit(integrity_error("package inspect", inspected.error));
        auto entries = nlohmann::json::array();
        for(const auto& entry : inspected.entries)
            entries.push_back({{"path", entry.path}, {"size", entry.size},
                               {"crc32", entry.crc32}, {"executable", entry.executable}});
        return emit(success("package inspect", {{"archiveDigest", inspected.archive_digest},
                                                 {"entries", std::move(entries)}}));
    }
    if(command == "package" && !operands.empty() && operands[0] == "sbom") {
        if(operands.size() != 2) return emit(usage_error("package sbom", "expected DIRECTORY"));
        auto inspected = inspect_skill_package(operands[1]);
        if(!inspected.ok || !inspected.package)
            return emit(integrity_error("package sbom", inspected.error.dump()));
        return emit(success("package sbom", {{"sbom", generate_skill_sbom(*inspected.package)}}));
    }
    if(command == "package" && !operands.empty() && operands[0] == "sign") {
        if(operands.size() < 3) return emit(usage_error("package sign", "expected ARCHIVE SIGNATURE"));
        fs::path key_path;
        std::string publisher, source_uri;
        for(std::size_t index = 3; index < operands.size(); ++index) {
            if(operands[index] == "--key" && index + 1 < operands.size()) key_path = operands[++index];
            else if(operands[index] == "--publisher" && index + 1 < operands.size()) publisher = operands[++index];
            else if(operands[index] == "--source" && index + 1 < operands.size()) source_uri = operands[++index];
            else return emit(usage_error("package sign", "invalid package sign option"));
        }
        std::string error;
        auto key = read_bounded(key_path, 64 * 1024, error);
        auto metadata = audit_metadata(operands[1], error);
        if(!key || !metadata || publisher.empty() || source_uri.empty())
            return emit(integrity_error("package sign", error.empty() ? "signing fields are required" : error));
        SkillSignatureEnvelope envelope;
        envelope.subject_digest = metadata->archive_digest;
        envelope.publisher = publisher;
        envelope.source_uri = source_uri;
        envelope.sbom_digest = metadata->sbom_digest;
        envelope.provenance_digest = metadata->provenance_digest;
        auto signed_package = sign_skill_subject(envelope, *key);
        key->assign(key->size(), '\0');
        if(!signed_package.ok) return emit(integrity_error("package sign", signed_package.error));
        if(!write_file(operands[2], signed_package.envelope.to_json().dump(2) + "\n", error))
            return emit(integrity_error("package sign", error));
        return emit(success("package sign", {{"signature", fs::path(operands[2]).generic_string()},
            {"archiveDigest", metadata->archive_digest}, {"keyId", signed_package.envelope.key_id},
            {"publisher", publisher}}));
    }
    if(command == "package" && !operands.empty() && operands[0] == "verify") {
        if(operands.size() < 3) return emit(usage_error("package verify", "expected ARCHIVE SIGNATURE"));
        fs::path trust_path;
        std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for(std::size_t index = 3; index < operands.size(); ++index) {
            if(operands[index] == "--trust" && index + 1 < operands.size()) trust_path = operands[++index];
            else if(operands[index] == "--now" && index + 1 < operands.size()) now = std::strtoll(operands[++index].c_str(), nullptr, 10);
            else return emit(usage_error("package verify", "invalid package verify option"));
        }
        std::string error;
        auto trust = load_trust(trust_path, error);
        auto signature = load_signature(operands[2], error);
        auto metadata = audit_metadata(operands[1], error);
        if(!trust || !signature || !metadata || signature->subject_digest != metadata->archive_digest)
            return emit(integrity_error("package verify", error.empty() ? "package identity mismatch" : error));
        auto audited = inspect_audited_skill_archive(operands[1], *metadata);
        auto verified = verify_skill_signature(*signature, *trust, SkillTrustRole::Package, now);
        if(!audited.ok || !verified.ok)
            return emit(integrity_error("package verify", !audited.ok ? audited.error : verified.error));
        return emit(success("package verify", {{"archiveDigest", metadata->archive_digest},
            {"keyId", signature->key_id}, {"publisher", signature->publisher}, {"verified", true}}));
    }
    if(command == "registry" && operands.size() >= 5 &&
       (operands[0] == "sync" || operands[0] == "resolve")) {
        const auto operation = operands[0];
        const fs::path index_path = operands[1];
        const fs::path signature_path = operands[2];
        const fs::path trust_path = operands[3];
        const std::string source_uri = operands[4];
        std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string error;
        auto index_bytes = read_bounded(index_path, SkillRemoteRegistryClient::kMaxIndexBytes, error);
        auto signature_bytes = read_bounded(signature_path, SkillRemoteRegistryClient::kMaxSignatureBytes, error);
        auto trust = load_trust(trust_path, error);
        if(!index_bytes || !signature_bytes || !trust)
            return emit(integrity_error("registry " + operation, error));
        auto transport = std::make_shared<SkillMemoryRegistryTransport>();
        transport->responses[source_uri] = *index_bytes;
        transport->responses[source_uri + ".sig"] = *signature_bytes;
        SkillRemoteRegistryClient client(*trust, transport);
        auto synced = client.sync(source_uri, source_uri + ".sig", now);
        if(!synced.ok) return emit(integrity_error("registry " + operation, synced.error));
        if(operation == "sync")
            return emit(success("registry sync", {{"indexDigest", synced.index_digest},
                                                   {"index", synced.index.to_json()}}));
        if(operands.size() != 7)
            return emit(usage_error("registry resolve", "expected INDEX SIG TRUST SOURCE ID VERSION"));
        auto artifact = client.resolve(synced.index, operands[5], operands[6], &error);
        if(!artifact) return emit(integrity_error("registry resolve", error));
        return emit(success("registry resolve", {{"packageId", artifact->package_id},
            {"version", artifact->version}, {"digest", artifact->digest},
            {"size", artifact->size}, {"mirrors", artifact->mirrors},
            {"signatureUri", artifact->signature_uri}, {"indexDigest", synced.index_digest}}));
    }
    if(command == "package" && operands.size() == 1)
        return emit(service.package(operands[0]));
    if((command == "install" || command == "update") && !operands.empty() &&
       fs::is_regular_file(operands[0])) {
        if(invocation.store.empty()) return emit(usage_error(command, "archive install requires --store"));
        fs::path signature_path, trust_path;
        std::string expected_digest, registry_digest;
        bool remote = false, allow_unsigned_local = false;
        std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        for(std::size_t index = 1; index < operands.size(); ++index) {
            if(operands[index] == "--signature" && index + 1 < operands.size()) signature_path = operands[++index];
            else if(operands[index] == "--trust" && index + 1 < operands.size()) trust_path = operands[++index];
            else if(operands[index] == "--digest" && index + 1 < operands.size()) expected_digest = operands[++index];
            else if(operands[index] == "--registry-digest" && index + 1 < operands.size()) registry_digest = operands[++index];
            else if(operands[index] == "--now" && index + 1 < operands.size()) now = std::strtoll(operands[++index].c_str(), nullptr, 10);
            else if(operands[index] == "--remote") remote = true;
            else if(operands[index] == "--allow-unsigned-local") allow_unsigned_local = true;
            else return emit(usage_error(command, "invalid archive lifecycle option"));
        }
        std::string error;
        auto archive = inspect_skill_archive(operands[0]);
        if(!archive.ok || (!expected_digest.empty() && archive.archive_digest != expected_digest))
            return emit(integrity_error(command, archive.ok ? "pinned digest mismatch" : archive.error));
        SkillInstallOptions options;
        options.remote = remote;
        options.allow_unsigned_local = allow_unsigned_local;
        options.verification_time = now;
        options.registry_digest = registry_digest;
        if(!signature_path.empty()) {
            auto signature = load_signature(signature_path, error);
            auto trust = load_trust(trust_path, error);
            if(!signature || !trust) return emit(integrity_error(command, error));
            options.signature = *signature;
            options.trust = *trust;
        }
        try {
            SkillLifecycleManager manager(registry, invocation.store);
            auto result = command == "install" ? manager.install(operands[0], options)
                                                : manager.update(operands[0], options);
            if(!result.ok || !result.package)
                return emit(integrity_error(command, result.error.dump()));
            const auto& package = *result.package;
            return emit(success(command, {{"package", {{"id", package.id},
                {"version", package.version.str()}, {"packageDigest", package.package_digest},
                {"archiveDigest", package.archive_digest}, {"publisher", package.publisher},
                {"keyId", package.key_id}, {"signatureDigest", package.signature_digest},
                {"sbomDigest", package.sbom_digest}, {"provenanceDigest", package.provenance_digest},
                {"registryDigest", package.registry_digest},
                {"legacyUnsigned", package.legacy_unsigned}}}}));
        } catch(const std::exception& ex) {
            return emit(integrity_error(command, ex.what()));
        }
    }
    if(command == "install" || command == "update") {
        if(operands.empty()) return emit(usage_error(command, "a package path is required"));
        std::string source_uri;
        std::string signature_identity;
        for(std::size_t index = 1; index < operands.size(); ++index) {
            if(operands[index] == "--source" && index + 1 < operands.size())
                source_uri = operands[++index];
            else if(operands[index] == "--signature" && index + 1 < operands.size())
                signature_identity = operands[++index];
            else return emit(usage_error(command, "invalid lifecycle option: " + operands[index]));
        }
        return emit(command == "install"
            ? service.install(invocation.store, operands[0], source_uri, signature_identity)
            : service.update(invocation.store, operands[0], source_uri, signature_identity));
    }
    if(command == "enable" && !operands.empty() && operands.size() <= 3) {
        std::string range = "*";
        if(operands.size() == 2) range = operands[1];
        else if(operands.size() == 3 && operands[1] == "--range") range = operands[2];
        else if(operands.size() == 3) return emit(usage_error(command, "enable accepts --range RANGE"));
        return emit(service.enable(invocation.store, operands[0], range));
    }
    if(command == "disable" && operands.size() == 1)
        return emit(service.disable(invocation.store, operands[0]));
    if(command == "remove" && operands.size() == 1)
        return emit(service.remove(invocation.store, operands[0]));
    if(command == "rollback" && operands.size() == 1)
        return emit(service.rollback(invocation.store, operands[0]));

    return emit(usage_error(command, "invalid command arguments"));
}
