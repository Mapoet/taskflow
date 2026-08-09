#ifndef AGENT_TOOL_EFFECT_JOURNAL_HPP
#define AGENT_TOOL_EFFECT_JOURNAL_HPP
#include <agent/core/types.hpp>
#include <map>
#include <mutex>
#include <optional>
namespace agent_framework { enum class ToolEffectStatus { Started, Completed, Committed, ManualReview }; struct ToolEffectRecord { std::string task_id, session_id, tool_call_id, idempotency_key, request_digest, result_digest; std::size_t attempt=0; ToolEffectStatus status=ToolEffectStatus::Started; }; class ToolEffectJournal { public: bool start(ToolEffectRecord); bool complete(const std::string&,const std::string&); bool commit(const std::string&); std::optional<ToolEffectRecord> find_idempotency(const std::string&) const; std::vector<ToolEffectRecord> recoverable() const; private: mutable std::mutex m_; std::map<std::string,ToolEffectRecord> by_key_; }; }
#endif
