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
 turn.turn_id="turn-3";turn.run_id="run-3";
 auto suspended=commands.execute({TaskCommandKind::Suspend,turn});assert(suspended.ok);
 assert(tasks.load(turn.identity,turn.task_id)->state==TaskLifecycleState::Suspended);
 std::filesystem::remove_all(root,ec);
}
