#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <filesystem>
#include "agent/conversation/task_command_service.hpp"
#include "agent/internal/platform_io.hpp"

int main(){using namespace agent_framework;using namespace conversation;
 const auto root=std::filesystem::temp_directory_path()/("task-command-"+std::to_string(internal::current_process_id()));
 std::error_code ec;std::filesystem::remove_all(root,ec);std::filesystem::create_directories(root);
 SQLiteTaskRegistry tasks((root/"tasks.sqlite3").string());TaskCommandService commands(tasks);
 TurnRequest turn{{"tenant","conversation"},"turn-1","initial",TaskExecutionProfile::CodeChange,10};turn.task_id="task";turn.run_id="run-1";
 auto started=commands.execute({TaskCommandKind::Start,turn});assert(started.ok);
 const auto before=*tasks.load(turn.identity,turn.task_id);const auto runs_before=tasks.runs(turn.identity,turn.task_id).size();
 auto status=commands.execute({TaskCommandKind::Status,turn});assert(status.ok&&status.read_only);
 auto output=commands.execute({TaskCommandKind::Output,turn});assert(output.ok&&output.read_only);
 const auto after=*tasks.load(turn.identity,turn.task_id);assert(after.revision==before.revision);
 assert(tasks.requirements(turn.identity,turn.task_id).size()==1);assert(tasks.runs(turn.identity,turn.task_id).size()==runs_before);
 turn.turn_id="turn-2";turn.run_id="run-2";turn.input="preserve ABI";
 auto amended=commands.execute({TaskCommandKind::Amend,turn});assert(amended.ok);
 assert(tasks.requirements(turn.identity,turn.task_id).size()==2);
 turn.turn_id="turn-attach";turn.run_id="run-attach";
 auto attached=commands.execute({TaskCommandKind::Attach,turn});assert(attached.ok);
 turn.turn_id="turn-replan";turn.run_id="run-replan";turn.input="replan for portability";
 auto replanned=commands.execute({TaskCommandKind::Replan,turn});assert(replanned.ok);
 turn.turn_id="turn-3";turn.run_id="run-3";
 auto suspended=commands.execute({TaskCommandKind::Suspend,turn});assert(suspended.ok);
 assert(tasks.load(turn.identity,turn.task_id)->state==TaskLifecycleState::Suspended);
 turn.turn_id="turn-continue";turn.run_id="run-continue";turn.input="continue";
 auto continued=commands.execute({TaskCommandKind::Continue,turn});assert(continued.ok);
 assert(tasks.load(turn.identity,turn.task_id)->state==TaskLifecycleState::Active);
 TaskCommandPolicy policy;TaskCommandService secured(tasks,nullptr,&policy);
 TaskCommandPrincipal principal{"operator",turn.identity,{"task:read","task:write","task:control"},true};
 TaskCommandRequest secured_status{TaskCommandKind::Status,turn};secured_status.principal=principal;
 auto authorized_status=secured.execute(secured_status);assert(authorized_status.ok);
 assert(authorized_status.payload["actions"].is_array());
 auto cross=principal;cross.identity.tenant_id="other";secured_status.principal=cross;
 assert(secured.execute(secured_status).error=="task_command_cross_tenant_forbidden");
 auto read_only=principal;read_only.scopes={"task:read"};
 TaskCommandRequest forbidden{TaskCommandKind::Cancel,turn};forbidden.principal=read_only;
 assert(secured.execute(forbidden).error=="task_command_scope_forbidden:task:control");
 TaskCommandRequest stale{TaskCommandKind::Continue,turn};stale.principal=principal;
 stale.expected_task_revision=1;auto stale_result=secured.execute(stale);
 assert(stale_result.error=="task_command_stale_revision");
 assert(stale_result.task_revision==tasks.load(turn.identity,turn.task_id)->revision);
 assert(stale_result.payload["expected_revision"]==1);
 assert(stale_result.payload["current_revision"]==stale_result.task_revision);
 assert(stale_result.payload["retryable"]==true);
 auto current=*tasks.load(turn.identity,turn.task_id);
 assert(tasks.transition(turn.identity,turn.task_id,current.revision,
     TaskLifecycleState::Closed,"completed_verified").ok);
 TaskCommandRequest terminal_continue{TaskCommandKind::Continue,turn};
 terminal_continue.principal=principal;
 assert(secured.execute(terminal_continue).error=="task_command_not_allowed_in_state:closed");
 TurnRequest cancel_turn{{"tenant","conversation"},"turn-cancel-start","cancel target",
     TaskExecutionProfile::CodeChange,10};cancel_turn.task_id="task-cancel";
 cancel_turn.run_id="run-cancel-start";
 assert(commands.execute({TaskCommandKind::Start,cancel_turn}).ok);
 cancel_turn.turn_id="turn-cancel";cancel_turn.run_id="run-cancel";
 assert(commands.execute({TaskCommandKind::Cancel,cancel_turn}).ok);
 assert(tasks.load(cancel_turn.identity,cancel_turn.task_id)->state==TaskLifecycleState::Cancelled);
 auto final_status=secured.execute(secured_status={TaskCommandKind::Status,turn});
 assert(final_status.error=="task_command_authentication_required");
 std::filesystem::remove_all(root,ec);
}
