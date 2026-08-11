#include "agent/live/production_bundle.hpp"

#include <algorithm>
#include <set>

namespace agent_framework::live {
namespace {
void add(std::vector<std::string>* errors, std::string value) {
    if(errors) errors->push_back(std::move(value));
}
std::optional<LiveEvidenceLevel> level(std::string_view value) {
    if(value == "offline-control") return LiveEvidenceLevel::OfflineControl;
    if(value == "production-like") return LiveEvidenceLevel::ProductionLike;
    if(value == "production-certified") return LiveEvidenceLevel::ProductionCertified;
    return std::nullopt;
}
}

std::optional<ProductionLiveBundle> decode_production_live_bundle(
    const nlohmann::json& document, std::vector<std::string>* errors) {
    if(!document.is_object()) { add(errors, "bundle must be an object"); return std::nullopt; }
    static const std::set<std::string> fields = {"evidence_level", "environment", "matrix",
        "mandatory_cell_ids", "mandatory_dependency_digests"};
    for(auto it = document.begin(); it != document.end(); ++it)
        if(!fields.count(it.key())) { add(errors, "unknown bundle field:" + it.key()); return std::nullopt; }
    for(const auto* field : {"evidence_level", "environment", "matrix", "mandatory_cell_ids",
                              "mandatory_dependency_digests"})
        if(!document.contains(field)) { add(errors, std::string("missing bundle field:") + field); return std::nullopt; }
    if(!document.at("evidence_level").is_string()) { add(errors, "invalid evidence_level"); return std::nullopt; }
    const auto parsed_level = level(document.at("evidence_level").get<std::string>());
    if(!parsed_level) { add(errors, "unsupported evidence_level"); return std::nullopt; }
    std::vector<contracts::ContractIssue> issues;
    auto environment = decode_live_environment_profile(document.at("environment"), {}, &issues);
    auto matrix = decode_role_live_matrix(document.at("matrix"), {}, &issues);
    if(!environment || !matrix) {
        for(const auto& issue : issues) add(errors, issue.code + ":" + issue.path);
        return std::nullopt;
    }
    try {
        ProductionLiveBundle out;
        out.evidence_level = *parsed_level;
        out.environment = std::move(*environment);
        out.matrix = std::move(*matrix);
        out.mandatory_cell_ids = document.at("mandatory_cell_ids").get<std::vector<std::string>>();
        out.mandatory_dependency_digests =
            document.at("mandatory_dependency_digests").get<std::vector<std::string>>();
        const auto validation = validate_production_live_bundle(out);
        if(!validation.empty()) { if(errors) errors->insert(errors->end(), validation.begin(), validation.end()); return std::nullopt; }
        return out;
    } catch(...) { add(errors, "bundle list field has invalid type"); return std::nullopt; }
}

std::vector<std::string> validate_production_live_bundle(const ProductionLiveBundle& bundle) {
    auto errors = validate_live_contract(bundle.environment, bundle.matrix);
    if(bundle.evidence_level == LiveEvidenceLevel::ProductionCertified &&
       bundle.environment.endpoint_class != "production")
        errors.push_back("production-certified requires production endpoint_class");
    if(bundle.mandatory_cell_ids.empty()) errors.push_back("mandatory cell policy is empty");
    std::set<std::string> declared;
    for(const auto& cell : bundle.matrix.cells) declared.insert(cell.cell_id);
    for(const auto& id : bundle.mandatory_cell_ids) {
        const auto found = std::find_if(bundle.matrix.cells.begin(), bundle.matrix.cells.end(),
            [&](const auto& cell) { return cell.cell_id == id; });
        if(!declared.count(id)) errors.push_back("mandatory cell missing:" + id);
        else if(!found->required) errors.push_back("mandatory cell is optional:" + id);
    }
    if(bundle.mandatory_dependency_digests.empty())
        errors.push_back("mandatory dependency policy is empty");
    for(const auto& digest : bundle.mandatory_dependency_digests)
        if(std::find(bundle.environment.dependency_digests.begin(),
                     bundle.environment.dependency_digests.end(), digest) ==
           bundle.environment.dependency_digests.end())
            errors.push_back("mandatory dependency missing:" + digest);
    if(bundle.evidence_level == LiveEvidenceLevel::ProductionCertified &&
       bundle.environment.secret_refs.empty())
        errors.push_back("production-certified requires opaque secret references");
    for(const auto& reference : bundle.environment.secret_refs)
        if(reference.find("://") == std::string::npos)
            errors.push_back("secret reference is not opaque:" + reference);
    return errors;
}

}  // namespace agent_framework::live
