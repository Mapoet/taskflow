#ifndef __AGENT_SKILL_COMMAND_H__
#define __AGENT_SKILL_COMMAND_H__

#include <agent/skill_loader.hpp>
#include <agent/skill_doctor.hpp>
#include <agent/skill_policy.hpp>
#include <agent/skill_registry.hpp>
#include <agent/skill_test_runner.hpp>
#include <agent/skill_resource_access.hpp>
#include <agent/skill_resource_cache.hpp>
#include <agent/skill_reference.hpp>
#include <agent/skill_model.hpp>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace agent_framework {

enum class SkillCliExit : int {
    Success = 0,
    ContractFailed = 2,
    NotFound = 3,
    OperationFailed = 4,
    IntegrityFailed = 5,
    DependencyUnavailable = 6,
    Usage = 64,
    Internal = 70,
};

struct SkillCommandResponse {
    SkillCliExit exit = SkillCliExit::Success;
    std::string command;
    nlohmann::json data = nlohmann::json::object();
    std::vector<SkillDiagnostic> diagnostics;
    nlohmann::json error = nullptr;
    std::optional<std::string> raw_output;

    bool ok() const noexcept;
    nlohmann::json to_json() const;
};

struct SkillCliArguments {
    std::filesystem::path root;
    std::filesystem::path store;
    std::string format = "json";
    std::string command;
    std::vector<std::string> operands;
    bool help = false;
};

struct SkillCliParseResult {
    std::optional<SkillCliArguments> arguments;
    SkillCommandResponse response;

    explicit operator bool() const noexcept { return arguments.has_value(); }
};

SkillCliParseResult parse_skill_cli_arguments(const std::vector<std::string>& tokens);
std::string skillctl_usage();

class SkillCommandService {
public:
    explicit SkillCommandService(std::shared_ptr<SkillRegistry> registry,
                                 std::filesystem::path cache_root = {});

    SkillCommandResponse list() const;
    SkillCommandResponse validate() const;
    SkillCommandResponse show(const std::string& skill_id) const;
    SkillCommandResponse inspect(const std::string& skill_id, bool resolved) const;
    SkillCommandResponse read(const std::string& skill_id, SkillResourceKind kind,
                              const std::string& relative_path, std::size_t max_bytes,
                              bool raw) const;
    SkillCommandResponse lint(const std::string& skill_id = {},
                              bool warnings_as_errors = false) const;
    SkillCommandResponse graph() const;
    SkillCommandResponse permissions(const std::string& skill_id,
                                     const SkillPermissionGrant& granted = {}) const;
    SkillCommandResponse doctor(const std::string& skill_id,
                                const SkillDoctorOptions& options = {}) const;
    SkillCommandResponse test(const std::string& skill_id = {},
                              const std::string& filter = {},
                              std::size_t jobs = 1) const;
    SkillCommandResponse package(const std::filesystem::path& package) const;
    SkillCommandResponse install(const std::filesystem::path& store,
                                 const std::filesystem::path& package,
                                 const std::string& source_uri = {},
                                 const std::string& signature_identity = {}) const;
    SkillCommandResponse update(const std::filesystem::path& store,
                                const std::filesystem::path& package,
                                const std::string& source_uri = {},
                                const std::string& signature_identity = {}) const;
    SkillCommandResponse enable(const std::filesystem::path& store,
                                const std::string& skill_id,
                                const std::string& range = "*") const;
    SkillCommandResponse disable(const std::filesystem::path& store,
                                 const std::string& skill_id) const;
    SkillCommandResponse remove(const std::filesystem::path& store,
                                const std::string& package_digest) const;
    SkillCommandResponse rollback(const std::filesystem::path& store,
                                  const std::string& skill_id) const;
    SkillCommandResponse reference_page(const std::string& skill_id,
                                        const std::string& resource_id,
                                        std::uint64_t offset,
                                        std::size_t max_bytes) const;
    SkillCommandResponse reference_search(const std::string& skill_id,
                                          const std::string& resource_id,
                                          const std::string& query,
                                          std::size_t limit) const;
    SkillCommandResponse cache_status() const;
    SkillCommandResponse cache_verify() const;
    SkillCommandResponse cache_gc() const;
    SkillCommandResponse cache_pin(const std::string& digest, bool pinned) const;
    SkillCommandResponse model_check(const std::string& skill_id,
                                     const std::string& resource_id,
                                     const SkillModelHostCapabilities& host) const;

private:
    std::shared_ptr<SkillRegistry> registry_;
    SkillLoader loader_;
    std::shared_ptr<SkillResourceAccess> resource_access_;
    std::shared_ptr<SkillResourceCache> resource_cache_;
    std::shared_ptr<SkillReferenceService> references_;
    std::shared_ptr<SkillModelService> models_;
};

std::optional<SkillResourceKind> parse_skill_resource_kind(const std::string& value);

} // namespace agent_framework

#endif // __AGENT_SKILL_COMMAND_H__
