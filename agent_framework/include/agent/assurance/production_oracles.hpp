#pragma once

#include <filesystem>
#include <map>
#include <set>

#include "agent/assurance/professional_workflow.hpp"
#include "agent/sandbox/provider.hpp"

namespace agent_framework::assurance {

struct RepositoryOracleRule {
    std::string criterion_id;
    std::string source_kind;
    std::filesystem::path relative_path;
    bool must_exist{true};
    bool must_be_nonempty{true};
    std::optional<std::string> expected_digest;
};

class RepositoryEvidenceOracle final : public DeterministicOracle {
public:
    RepositoryEvidenceOracle(std::filesystem::path root,
                             std::vector<RepositoryOracleRule> rules,
                             std::string revision);
    std::string id() const override { return "production.repository"; }
    std::vector<std::string> source_kinds() const override;
    bool production_ready() const noexcept override { return ready_; }
    bool supports(std::string_view criterion_id,
                  std::string_view source_kind) const override;
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    OracleResult collect(const OracleContext&) override;
private:
    std::filesystem::path root_;
    std::vector<RepositoryOracleRule> rules_;
    std::string revision_, manifest_digest_;
    bool ready_{false};
};

struct SandboxOracleRule {
    std::string rule_id;
    std::string criterion_id;
    std::string source_kind;
    std::vector<std::string> command;
    std::uint64_t wall_time_ms{60000};
    std::set<int> passing_exit_codes{0};
};

class SandboxCommandOracle final : public DeterministicOracle {
public:
    SandboxCommandOracle(sandbox::SandboxProvider& provider,
                         std::filesystem::path workspace,
                         std::vector<SandboxOracleRule> rules,
                         std::string policy_revision,
                         std::string revision);
    std::string id() const override { return "production.sandbox-command"; }
    std::vector<std::string> source_kinds() const override;
    bool production_ready() const noexcept override { return ready_; }
    bool supports(std::string_view criterion_id,
                  std::string_view source_kind) const override;
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    OracleResult collect(const OracleContext&) override;
private:
    sandbox::SandboxProvider& provider_;
    std::filesystem::path workspace_;
    std::vector<SandboxOracleRule> rules_;
    std::string policy_revision_, revision_, manifest_digest_;
    bool ready_{false};
};

class DomainOracleAdapter {
public:
    virtual ~DomainOracleAdapter() = default;
    virtual std::string id() const = 0;
    virtual std::string revision() const = 0;
    virtual OracleResult verify(const OracleContext&, const Criterion&) = 0;
};

class PolicyBoundDomainOracle final : public DeterministicOracle {
public:
    PolicyBoundDomainOracle(std::string source_kind,
                            std::shared_ptr<DomainOracleAdapter> adapter,
                            std::string policy_revision);
    std::string id() const override;
    std::vector<std::string> source_kinds() const override { return {source_kind_}; }
    bool production_ready() const noexcept override { return ready_; }
    std::string capability_manifest_digest() const override { return manifest_digest_; }
    OracleResult collect(const OracleContext&) override;
private:
    std::string source_kind_, policy_revision_, manifest_digest_;
    std::shared_ptr<DomainOracleAdapter> adapter_;
    bool ready_{false};
};

}  // namespace agent_framework::assurance
