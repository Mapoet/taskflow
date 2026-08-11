#pragma once

#include <string>
#include <string_view>

#include "agent/live/role_certification.hpp"

namespace agent_framework::live {

bool verify_ed25519_signature(const SignatureEnvelope& envelope,
                              std::string_view public_key_pem,
                              std::string* error = nullptr);

}  // namespace agent_framework::live
