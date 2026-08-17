#pragma once

#include <mutex>

#include "agent/session/catalog_types.hpp"

namespace agent_framework::session {

class SessionCatalog {
public:
    virtual ~SessionCatalog() = default;
    virtual SessionMutationResult create(ProductSession) = 0;
    virtual std::optional<ProductSession> get(std::string_view tenant,
                                              std::string_view session) = 0;
    virtual SessionListPage list(const SessionListQuery&) = 0;
    virtual std::optional<SessionMember> member(std::string_view tenant,
        std::string_view session, std::string_view principal) = 0;
    virtual SessionMutationResult rename(std::string_view tenant,
        std::string_view session, std::uint64_t expected, std::string_view title) = 0;
    virtual SessionMutationResult organize(std::string_view tenant,
        std::string_view session, std::uint64_t expected, std::string_view folder,
        const std::vector<std::string>& tags, bool pinned) = 0;
    virtual SessionMutationResult transition(std::string_view tenant,
        std::string_view session, std::uint64_t expected, ProductSessionState) = 0;
    virtual SessionMutationResult put_member(const SessionMember&,
                                              std::uint64_t expected_revision) = 0;
};

class SQLiteSessionCatalog final : public SessionCatalog {
public:
    explicit SQLiteSessionCatalog(std::string database_path);
    ~SQLiteSessionCatalog() override;
    SQLiteSessionCatalog(const SQLiteSessionCatalog&) = delete;
    SQLiteSessionCatalog& operator=(const SQLiteSessionCatalog&) = delete;

    SessionMutationResult create(ProductSession) override;
    std::optional<ProductSession> get(std::string_view, std::string_view) override;
    SessionListPage list(const SessionListQuery&) override;
    std::optional<SessionMember> member(std::string_view, std::string_view,
        std::string_view) override;
    SessionMutationResult rename(std::string_view, std::string_view,
        std::uint64_t, std::string_view) override;
    SessionMutationResult organize(std::string_view, std::string_view,
        std::uint64_t, std::string_view, const std::vector<std::string>&, bool) override;
    SessionMutationResult transition(std::string_view, std::string_view,
        std::uint64_t, ProductSessionState) override;
    SessionMutationResult put_member(const SessionMember&, std::uint64_t) override;

private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    std::mutex mutex_;
};

}  // namespace agent_framework::session
