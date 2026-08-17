#pragma once

#include <optional>
#include <string_view>

#include "agent/conversation/task_intent.hpp"

namespace agent_framework::conversation {

enum class WorkShape { SingleTurn, BoundedTask, LongRunningTask, ContinuousTask };
enum class AssuranceTier { Basic, Functional, Professional, ProductionCertification };

std::string_view name(WorkShape);
std::optional<WorkShape> work_shape(std::string_view);
std::string_view name(AssuranceTier);
std::optional<AssuranceTier> assurance_tier(std::string_view);

struct TaskSemanticAxes {
    TaskIntentKind intent{TaskIntentKind::NewTask};
    WorkShape work_shape{WorkShape::SingleTurn};
    EffectClass effect_class{EffectClass::None};
    AssuranceTier assurance_tier{AssuranceTier::Basic};
};

}  // namespace agent_framework::conversation
