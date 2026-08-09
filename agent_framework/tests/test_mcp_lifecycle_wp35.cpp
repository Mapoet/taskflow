#include <agent/mcp_client/mcp_lifecycle.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <cassert>
#include <filesystem>
using namespace agent_framework;
class FakeTransport final : public MCPTransportInterface
{
public:
    bool connect(const std::string &) override
    {
        connected = true;
        return true;
    }
    void disconnect() override { connected = false; }
    bool is_connected() const override { return connected; }
    MCPTransport get_transport_type() const override { return MCPTransport::HTTP; }
    void send_notification(const json &) override {}
    json transceive(const json &r) override
    {
        auto id = r.at("id");
        auto m = r.at("method").get<std::string>();
        if (m == "initialize")
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"capabilities", {{"tools", json::object()}}}}}};
        if (m == "tools/list")
            return {{"jsonrpc", "2.0"}, {"id", id}, {"result", {{"tools", json::array()}}}};
        return {{"jsonrpc", "2.0"}, {"id", id}, {"result", json::object()}};
    }
    bool connected = false;
};
int main()
{
    auto bus = std::make_shared<ToolBus>();
    auto client = MCPClient::create_with_transport(std::make_unique<FakeTransport>());
    McpCapabilityRegistry registry(bus);
    CapabilityManifest manifest{"safe_mcp", "1.0", "fixture", std::string(64, 'a'), {}, "mcp"};
    registry.stage(manifest, client);
    assert(registry.status("safe_mcp")->state == CapabilityLifecycleState::Staged);
    registry.activate("safe_mcp");
    assert(bus->has_mcp_service("safe_mcp"));
    auto lease = registry.acquire("safe_mcp");
    registry.drain("safe_mcp");
    assert(!bus->has_mcp_service("safe_mcp"));
    assert(!registry.remove("safe_mcp"));
    lease = CapabilityLease{};
    const auto snapshot = (std::filesystem::temp_directory_path() / "mcp-registry-wp35.json").string();
    std::filesystem::remove(snapshot);
    registry.save(snapshot);
    McpCapabilityRegistry restored(bus);
    restored.load(snapshot);
    assert(restored.status("safe_mcp")->state == CapabilityLifecycleState::Staged);
    assert(registry.remove("safe_mcp"));
    assert(!registry.status("safe_mcp"));
    bool invalid = false;
    try
    {
        registry.stage({"../bad", "1", "fixture", std::string(64, 'a'), {}, "mcp"}, client);
    }
    catch (const std::invalid_argument &)
    {
        invalid = true;
    }
    assert(invalid);
    std::filesystem::remove(snapshot);
}
