#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/live/role_certification.hpp"

namespace agent_framework::live {

enum class LiveEvidenceLevel { OfflineControl, ProductionLike, ProductionCertified };

struct ProductionLiveBundle {
    LiveEvidenceLevel evidence_level{LiveEvidenceLevel::OfflineControl};
    LiveEnvironmentProfile environment;
    RoleLiveMatrix matrix;
    std::vector<std::string> mandatory_cell_ids;
    std::vector<std::string> mandatory_dependency_digests;
};

std::optional<ProductionLiveBundle> decode_production_live_bundle(
    const nlohmann::json& document, std::vector<std::string>* errors = nullptr);
std::vector<std::string> validate_production_live_bundle(const ProductionLiveBundle& bundle);

}  // namespace agent_framework::live
