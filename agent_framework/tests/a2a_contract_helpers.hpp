/**
 * @file a2a_contract_helpers.hpp
 * @brief WP2.6 fixture helpers: canonical JSON, key stripping, paths (tests only)
 */
#ifndef AGENT_TESTS_A2A_CONTRACT_HELPERS_HPP
#define AGENT_TESTS_A2A_CONTRACT_HELPERS_HPP

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace agent_tests {
namespace a2a_contract {

using json = nlohmann::json;

inline std::string read_text_file(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        throw std::runtime_error("read_text_file: cannot open " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

inline json read_json_file(const std::string& path) {
    return json::parse(read_text_file(path));
}

/** @brief Stable dump for equality (parse both sides through json::parse(dump()) for key order). */
inline std::string canonical_json_string(const json& j) {
    return j.dump();
}

inline void remove_keys_recursive(json& j, const std::vector<std::string>& keys) {
    if (j.is_object()) {
        for (const auto& k : keys) {
            j.erase(k);
        }
        for (auto it = j.begin(); it != j.end(); ++it) {
            remove_keys_recursive(*it, keys);
        }
    } else if (j.is_array()) {
        for (auto& el : j) {
            remove_keys_recursive(el, keys);
        }
    }
}

inline bool json_equal_after_canonical(const json& a, const json& b,
                                       const std::vector<std::string>& ignore_keys) {
    json ca = a;
    json cb = b;
    remove_keys_recursive(ca, ignore_keys);
    remove_keys_recursive(cb, ignore_keys);
    return canonical_json_string(ca) == canonical_json_string(cb);
}

} // namespace a2a_contract
} // namespace agent_tests

#endif
