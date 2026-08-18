#pragma once

#include <mutex>
#include <string>

#include <httplib.hpp>
#include <nlohmann/json.hpp>

#include "agent/api/v1/http_routes.hpp"
#include "agent/api/v1/session_run_api.hpp"

namespace agent_framework::api::v1 {

class RuntimeSettingsStore {
public:
    RuntimeSettingsStore(std::string database_path, nlohmann::json defaults = {});
    ApiResult snapshot(const identity::RuntimeSubject& subject) const;
    ApiResult update(const identity::RuntimeSubject& subject,
                     std::uint64_t expected_revision,
                     const nlohmann::json& updates);

private:
    std::string path_;
    nlohmann::json defaults_;
    mutable std::mutex mutex_;
};

void register_runtime_settings_routes(httplib::Server&, RuntimeSettingsStore&,
                                      RuntimeSubjectResolver);

}  // namespace agent_framework::api::v1
