#pragma once
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
namespace agent_framework::tool_runtime {
enum class CancellationStage { None, Requested, Cooperative, Terminate, Kill, Reconciling, Cancelled, ManualReview };
enum class BackpressureMode { Block, DropEphemeral, Fail };
struct BackpressurePolicy { std::uint64_t maximum_pending_bytes{4*1024*1024},maximum_pending_events{1024};BackpressureMode mode{BackpressureMode::Block}; };
struct ExecutionControlEnvelope { std::string invocation_id,tenant_id;std::int64_t deadline_at_ms{0};std::uint64_t cancel_generation{0};BackpressurePolicy backpressure; };
struct CancellationIntent { ExecutionControlEnvelope control;CancellationStage stage{CancellationStage::None};std::uint64_t revision{0},fencing_token{0};std::string reason,owner,receipt_digest;std::int64_t requested_at_ms{0},next_escalation_at_ms{0},lease_expires_at_ms{0};bool effect_known{false}; };
struct ControlResult { bool committed{false};std::uint64_t revision{0},generation{0};std::string error;explicit operator bool()const noexcept{return committed;} };
struct BackpressureReservation { bool accepted{false},dropped{false};std::uint64_t pending_bytes{0},pending_events{0};std::string error; };
std::string_view name(CancellationStage);std::string_view name(BackpressureMode);
class ExecutionControlStore { public:virtual ~ExecutionControlStore()=default;virtual ControlResult create(const ExecutionControlEnvelope&)=0;virtual std::optional<CancellationIntent> load(std::string_view)=0;virtual ControlResult request_cancel(std::string_view,std::string,std::int64_t)=0;virtual std::optional<CancellationIntent> claim(std::string_view,std::string,std::int64_t,std::int64_t)=0;virtual ControlResult advance(std::string_view,std::string_view,std::uint64_t,std::uint64_t,CancellationStage,std::int64_t,bool,std::string)=0;virtual std::vector<CancellationIntent> recoverable(std::int64_t,std::size_t)=0;virtual BackpressureReservation reserve(std::string_view,std::uint64_t,std::uint64_t,bool)=0;virtual bool release(std::string_view,std::uint64_t,std::uint64_t)=0;};
class SQLiteExecutionControlStore final:public ExecutionControlStore {public:explicit SQLiteExecutionControlStore(std::string);~SQLiteExecutionControlStore();ControlResult create(const ExecutionControlEnvelope&)override;std::optional<CancellationIntent> load(std::string_view)override;ControlResult request_cancel(std::string_view,std::string,std::int64_t)override;std::optional<CancellationIntent> claim(std::string_view,std::string,std::int64_t,std::int64_t)override;ControlResult advance(std::string_view,std::string_view,std::uint64_t,std::uint64_t,CancellationStage,std::int64_t,bool,std::string)override;std::vector<CancellationIntent> recoverable(std::int64_t,std::size_t)override;BackpressureReservation reserve(std::string_view,std::uint64_t,std::uint64_t,bool)override;bool release(std::string_view,std::uint64_t,std::uint64_t)override;private:void migrate();void*db_{nullptr};std::mutex mutex_;};
}
