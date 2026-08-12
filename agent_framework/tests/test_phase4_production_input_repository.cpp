#include <cassert>
#include <filesystem>

#include "agent/harness/sqlite_production_input_repository.hpp"
#include "agent/internal/platform_io.hpp"
#include "phase4_harness_test_support.hpp"

int main(){using namespace agent_framework;using namespace agent_framework::harness;
 const auto root=std::filesystem::temp_directory_path()/("p4-input-repo-"+std::to_string(internal::current_process_id()));std::error_code ec;std::filesystem::remove_all(root,ec);std::filesystem::create_directories(root);const auto path=root/"inputs.sqlite3";auto meta=phase4_harness_test::metadata();
 planning::TaskIntake intake;intake.metadata=meta;intake.user_goal="production goal";intake.requested_deliverables={"artifact"};
 assurance::AcceptanceContract contract;contract.metadata=meta;contract.plan_digest="sha256:plan";contract.criteria.push_back({"criterion",assurance::VerificationLayer::System,"works","system",{},"pass",true});
 const auto contract_digest=assurance::encode(contract).at("canonical_digest").get<std::string>();
 nlohmann::json artifact={{"artifact_id","a"},{"content","v1"}};const auto artifact_digest=contracts::canonical_digest(artifact).value();
 {SQLiteProductionWorkflowInputRepository repo(path.string());assert(repo.put_intake(intake));assert(repo.put_intake(intake).status==ProductionInputStatus::AlreadyExists);auto changed=intake;changed.user_goal="tampered";assert(repo.put_intake(changed).status==ProductionInputStatus::RevisionConflict);assert(repo.put_acceptance_contract(contract));assert(repo.put_artifact_manifest(meta.identity,artifact_digest,artifact));assert(repo.put_artifact_manifest(meta.identity,"sha256:wrong",artifact).status==ProductionInputStatus::Invalid);}
 {SQLiteProductionWorkflowInputRepository repo(path.string());auto loaded=repo.intake(meta.identity);assert(loaded&&loaded->user_goal=="production goal");assert(repo.acceptance_contract(meta.identity,contract_digest));assert(repo.artifact_manifest(meta.identity,artifact_digest)==artifact);auto other=meta.identity;other.tenant_id="other";assert(!repo.intake(other));}
 std::filesystem::remove_all(root,ec);}
