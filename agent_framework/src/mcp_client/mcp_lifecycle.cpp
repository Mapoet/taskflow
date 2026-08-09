#include <agent/mcp_client/mcp_lifecycle.hpp>
#include <agent/toolbus/toolbus.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace agent_framework
{
    namespace
    {
        bool valid_id(const std::string &id)
        {
            if (id.empty() || id.size() > 128)
                return false;
            for (unsigned char c : id)
                if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.'))
                    return false;
            return true;
        }
    }
    CapabilityLease::CapabilityLease(CapabilityLease &&o) noexcept : release_(std::move(o.release_)) {}
    CapabilityLease &CapabilityLease::operator=(CapabilityLease &&o) noexcept
    {
        if (this != &o)
        {
            if (release_)
                release_();
            release_ = std::move(o.release_);
        }
        return *this;
    }
    CapabilityLease::~CapabilityLease()
    {
        if (release_)
            release_();
    }
    McpCapabilityRegistry::McpCapabilityRegistry(std::shared_ptr<ToolBus> b, CapabilityAuditSink a) : toolbus_(std::move(b)), audit_(std::move(a))
    {
        if (!toolbus_)
            throw std::invalid_argument("MCP lifecycle requires ToolBus");
    }
    void McpCapabilityRegistry::notify(const CapabilityStatus &s) const noexcept
    {
        try
        {
            if (audit_)
                audit_(s);
        }
        catch (...)
        {
        }
    }
    void McpCapabilityRegistry::set_trust_store(SkillTrustStore trust, bool required)
    {
        std::lock_guard<std::mutex> l(mutex_);
        trust_ = std::move(trust);
        require_signature_ = required;
    }
    void McpCapabilityRegistry::stage(CapabilityManifest m, std::shared_ptr<MCPClient> c)
    {
        if (m.kind != "mcp" || !valid_id(m.id) || m.version.empty() || m.origin.empty() || m.digest.size() != 64 || !c)
            throw std::invalid_argument("invalid MCP capability manifest");
        {
            std::lock_guard<std::mutex> l(mutex_);
            if (require_signature_ && !m.signature)
                throw std::invalid_argument("MCP manifest signature required");
            if (m.signature && trust_)
            {
                if (m.signature->subject_kind != "mcp-capability" || m.signature->subject_digest != m.digest || m.signature->source_uri != m.origin)
                    throw std::invalid_argument("MCP manifest signature identity mismatch");
                auto r = verify_skill_signature(*m.signature, *trust_, SkillTrustRole::Package, std::time(nullptr));
                if (!r.ok)
                    throw std::invalid_argument("MCP manifest signature rejected: " + r.error);
            }
            if (entries_.contains(m.id))
                throw std::invalid_argument("capability already exists");
        }
        CapabilityStatus s{m, CapabilityLifecycleState::Staged, 0, {}};
        {
            std::lock_guard<std::mutex> l(mutex_);
            entries_.emplace(m.id, Entry{s, std::move(c)});
        }
        notify(s);
    }
    void McpCapabilityRegistry::activate(const std::string &id)
    {
        std::shared_ptr<MCPClient> c;
        {
            std::lock_guard<std::mutex> l(mutex_);
            auto i = entries_.find(id);
            if (i == entries_.end() || i->second.status.state != CapabilityLifecycleState::Staged)
                throw std::runtime_error("capability is not staged");
            c = i->second.client;
        }
        try
        {
            if (!c || !c->ping())
                throw std::runtime_error("MCP healthcheck failed");
            toolbus_->register_mcp_service(id, c);
            CapabilityStatus s;
            {
                std::lock_guard<std::mutex> l(mutex_);
                auto &i = entries_.at(id);
                i.status.state = CapabilityLifecycleState::Active;
                s = i.status;
            }
            notify(s);
        }
        catch (const std::exception &e)
        {
            CapabilityStatus s;
            {
                std::lock_guard<std::mutex> l(mutex_);
                auto &i = entries_.at(id);
                i.status.state = CapabilityLifecycleState::Failed;
                i.status.failure = e.what();
                s = i.status;
            }
            notify(s);
            throw;
        }
    }
    void McpCapabilityRegistry::drain(const std::string &id)
    {
        CapabilityStatus s;
        {
            std::lock_guard<std::mutex> l(mutex_);
            auto &i = entries_.at(id);
            if (i.status.state != CapabilityLifecycleState::Active)
                throw std::runtime_error("capability is not active");
            i.status.state = CapabilityLifecycleState::Draining;
            s = i.status;
        }
        toolbus_->unregister_mcp_service(id);
        notify(s);
    }
    bool McpCapabilityRegistry::remove(const std::string &id)
    {
        CapabilityStatus s;
        std::shared_ptr<MCPClient> c;
        {
            std::lock_guard<std::mutex> l(mutex_);
            auto i = entries_.find(id);
            if (i == entries_.end())
                return true;
            if (i->second.status.leases)
                return false;
            if (i->second.status.state == CapabilityLifecycleState::Active)
                throw std::runtime_error("drain capability before removal");
            i->second.status.state = CapabilityLifecycleState::Removed;
            s = i->second.status;
            c = i->second.client;
            entries_.erase(i);
        }
        if (c)
            c->disconnect();
        notify(s);
        return true;
    }
    CapabilityLease McpCapabilityRegistry::acquire(const std::string &id)
    {
        std::lock_guard<std::mutex> l(mutex_);
        auto i = entries_.find(id);
        if (i == entries_.end() || i->second.status.state != CapabilityLifecycleState::Active)
            throw std::runtime_error("capability is not active");
        ++i->second.status.leases;
        return CapabilityLease([this, id]
                               {std::lock_guard<std::mutex>l(mutex_);auto i=entries_.find(id);if(i!=entries_.end()&&i->second.status.leases)--i->second.status.leases; });
    }
    std::optional<CapabilityStatus> McpCapabilityRegistry::status(const std::string &id) const
    {
        std::lock_guard<std::mutex> l(mutex_);
        auto i = entries_.find(id);
        return i == entries_.end() ? std::nullopt : std::optional<CapabilityStatus>(i->second.status);
    }
    void McpCapabilityRegistry::save(const std::string &path) const
    {
        json a = json::array();
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (const auto &[_, e] : entries_)
            {
                const auto &m = e.status.manifest;
                a.push_back({{"id", m.id}, {"version", m.version}, {"origin", m.origin}, {"digest", m.digest}, {"permissions", m.permissions}, {"kind", m.kind}, {"signature", m.signature ? m.signature->to_json() : json(nullptr)}});
            }
        }
        auto p = std::filesystem::path(path);
        std::filesystem::create_directories(p.parent_path());
        auto tmp = path + ".tmp";
        std::ofstream o(tmp);
        if (!o)
            throw std::runtime_error("cannot write MCP registry");
        o << json{{"schema_version", 1}, {"capabilities", a}}.dump();
        o.close();
        std::filesystem::rename(tmp, p);
    }
    void McpCapabilityRegistry::load(const std::string &path)
    {
        std::ifstream in(path);
        if (!in)
            return;
        json d;
        in >> d;
        if (d.value("schema_version", 0) != 1 || !d["capabilities"].is_array())
            throw std::runtime_error("invalid MCP registry snapshot");
        for (const auto &j : d["capabilities"])
        {
            CapabilityManifest m{j.value("id", ""), j.value("version", ""), j.value("origin", ""), j.value("digest", ""), j.value("permissions", std::vector<std::string>{}), j.value("kind", "mcp"), {}};
            if (m.kind != "mcp" || !valid_id(m.id) || m.digest.size() != 64)
                throw std::runtime_error("invalid persisted MCP manifest");
            CapabilityStatus s{m, CapabilityLifecycleState::Staged, 0, "restart_requires_restage"};
            std::lock_guard<std::mutex> l(mutex_);
            entries_.emplace(m.id, Entry{s, {}});
        }
    }
}
