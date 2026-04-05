/**
 * @file jsonrpc_client.hpp
 * @brief Shared JSON-RPC POST helper + A2aRpcException (WP2.4)
 * @author Mapoet
 * @version 0.1
 * @date 2026-04-05
 */
#ifndef __AGENT_A2A_JSONRPC_CLIENT_H__
#define __AGENT_A2A_JSONRPC_CLIENT_H__

#include <agent/a2a/jsonrpc.hpp>
#include <agent/types.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace agent_framework {

class HttplibClient;

namespace a2a {

/**
 * @brief JSON-RPC error envelope mapped to an exception (HTTP 200 + error object).
 */
class A2aRpcException : public std::runtime_error {
public:
    A2aRpcException(int code, std::string message, json data);

    int code() const { return code_; }
    const json& data() const { return data_; }

private:
    int code_;
    json data_;
};

/**
 * @brief POST JSON-RPC 2.0 request; returns the `result` object or throws A2aRpcException.
 * @param full_rpc_url Absolute http(s) URL to JSON-RPC path
 * @param next_id Monotonic request id (per client / transport instance)
 */
json a2a_jsonrpc_post(
    HttplibClient& client,
    const std::string& full_rpc_url,
    std::string_view method,
    const json& params,
    const std::map<std::string, std::string>& headers,
    std::atomic<std::uint64_t>& next_id);

} // namespace a2a
} // namespace agent_framework

#endif // __AGENT_A2A_JSONRPC_CLIENT_H__
