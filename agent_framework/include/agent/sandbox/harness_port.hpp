#pragma once

#include <mutex>
#include <optional>

#include "agent/harness/runtime.hpp"
#include "agent/sandbox/provider.hpp"

namespace agent_framework::sandbox {

class SQLiteSandboxReceiptJournal {
public:
    explicit SQLiteSandboxReceiptJournal(std::string path);
    ~SQLiteSandboxReceiptJournal();
    SQLiteSandboxReceiptJournal(const SQLiteSandboxReceiptJournal&) = delete;
    SQLiteSandboxReceiptJournal& operator=(const SQLiteSandboxReceiptJournal&) = delete;
    bool record(std::string_view idempotency_key, const ExecResult& result,
                std::string* error = nullptr);
    std::optional<ExecResult> find(std::string_view idempotency_key);
private:
    void* db_{nullptr};
    std::mutex mutex_;
};

class SandboxExecutionHarnessPort final : public harness::HarnessStagePort {
public:
    SandboxExecutionHarnessPort(std::string port_id, SandboxProvider& provider,
                                SandboxSpec spec, SQLiteSandboxReceiptJournal& journal);
    std::string id() const override { return id_; }
    bool may_have_side_effects() const noexcept override { return true; }
    harness::HarnessStageResult execute(const harness::HarnessStageRequest& request) override;
    std::optional<harness::HarnessStageResult> reconcile(
        const harness::HarnessStageRequest& request) override;
private:
    harness::HarnessStageResult project(const ExecResult& result) const;
    std::string id_;
    SandboxProvider& provider_;
    SandboxSpec spec_;
    SQLiteSandboxReceiptJournal& journal_;
};
}  // namespace agent_framework::sandbox
