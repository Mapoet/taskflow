#pragma once

#include <agent/toolbus/toolbus.hpp>
#include <agent/sandbox/credential_broker.hpp>
#include <agent/distributed/object_store.hpp>

namespace agent_framework {
// Registers typed process/network compatibility tools. Process tools fail closed
// unless AGENT_FS_ROOT is configured and the Bubblewrap backend is available.
void register_builtin_process_tools_if_configured(ToolBus& bus);
/** Install the production credential resolver used by Curl/Wget. Values remain execution-local. */
void configure_network_tool_credentials(std::shared_ptr<sandbox::CredentialBroker> broker);
void configure_network_tool_object_store(std::shared_ptr<distributed::ObjectStore> store,
                                         std::string tenant_id = "local");
}
