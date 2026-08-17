#pragma once

#include <functional>
#include <optional>

#include <httplib.hpp>
#include "agent/api/v1/session_run_api.hpp"

namespace agent_framework::api::v1 {

using RuntimeSubjectResolver=std::function<std::optional<identity::RuntimeSubject>(
    const httplib::Request&)>;

void register_session_run_routes(httplib::Server&,SessionRunApi&,
                                 RuntimeSubjectResolver);

} // namespace agent_framework::api::v1
