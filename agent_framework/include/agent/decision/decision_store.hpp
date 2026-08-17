#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "agent/decision/decision_types.hpp"

namespace agent_framework::decision {

class DecisionStore {
public:
    virtual ~DecisionStore()=default;
    virtual DecisionMutationResult create(DecisionRequest)=0;
    virtual std::optional<DecisionRequest> load(std::string_view tenant,
        std::string_view decision_id)=0;
    virtual std::optional<DecisionRequest> pending(std::string_view tenant,
        std::string_view session_id,std::string_view conversation_id)=0;
    virtual std::optional<DecisionRequest> latest(std::string_view tenant,
        std::string_view session_id,std::string_view conversation_id)=0;
    virtual DecisionMutationResult answer(std::string_view tenant,
        std::string_view decision_id,std::uint64_t expected_revision,
        std::string_view option_id,std::uint64_t now_ms)=0;
    virtual DecisionMutationResult cancel(std::string_view tenant,
        std::string_view decision_id,std::uint64_t expected_revision)=0;
    virtual DecisionMutationResult expire(std::string_view tenant,
        std::string_view decision_id,std::uint64_t expected_revision,
        std::uint64_t now_ms)=0;
};

class SQLiteDecisionStore final : public DecisionStore {
public:
    explicit SQLiteDecisionStore(std::string database_path);
    ~SQLiteDecisionStore() override;
    SQLiteDecisionStore(const SQLiteDecisionStore&)=delete;
    SQLiteDecisionStore& operator=(const SQLiteDecisionStore&)=delete;
    DecisionMutationResult create(DecisionRequest) override;
    std::optional<DecisionRequest> load(std::string_view,std::string_view) override;
    std::optional<DecisionRequest> pending(std::string_view,std::string_view,
                                           std::string_view) override;
    std::optional<DecisionRequest> latest(std::string_view,std::string_view,
                                          std::string_view) override;
    DecisionMutationResult answer(std::string_view,std::string_view,std::uint64_t,
                                  std::string_view,std::uint64_t) override;
    DecisionMutationResult cancel(std::string_view,std::string_view,std::uint64_t) override;
    DecisionMutationResult expire(std::string_view,std::string_view,std::uint64_t,
                                  std::uint64_t) override;
private:
    void migrate();
    void* db_{nullptr};
    std::string path_;
    std::mutex mutex_;
};

}  // namespace agent_framework::decision
