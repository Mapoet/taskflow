#include <cassert>
#include <filesystem>
#include <set>

#include "agent/contracts/contract.hpp"
#include "agent/harness/cross_store_coordination.hpp"
#include "agent/internal/platform_io.hpp"

namespace {
using namespace agent_framework::harness;
class Participant final : public CrossStoreParticipant {
public:
    std::string id()const override{return id_;}
    std::string capability_manifest_digest()const override{return "sha256:"+id_;}
    bool supports(const CrossStoreOperation&)const noexcept override{return true;}
    std::optional<ParticipantPin> inspect(const CrossStoreOperation&,std::string*)override{return ParticipantPin{id_,revision_,digest_,reversible_};}
    bool prepare(const CrossStoreOperation&,const ParticipantPin&p,std::string*e)override{return check(p,e);}
    bool commit(const CrossStoreOperation&o,const ParticipantPin&p,std::string*e)override{
        if(fail_commit)return false;if(!check(p,e))return false;
        if(committed_keys.insert(o.idempotency_key).second)++commits;return true;}
    bool confirm(const CrossStoreOperation&,const ParticipantPin&p,std::string*e)override{return check(p,e);}
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*)override{return reversible_;}
    bool check(const ParticipantPin&p,std::string*e){const bool ok=p.revision==revision_&&p.digest==digest_;if(!ok&&e)*e="pin drift";return ok;}
    std::string id_{"a"},digest_{"sha256:a"};std::uint64_t revision_{1};bool reversible_{false},fail_commit{false};int commits{0};std::set<std::string> committed_keys;
};
CrossStoreOperation operation(std::string id){return{id,"tenant","run","harness","test",id,"policy-r1",{}, {}};}
}

int main(){namespace fs=std::filesystem;using namespace agent_framework::harness;
 const auto root=fs::temp_directory_path()/("phase4-cross-store-"+std::to_string(agent_framework::internal::current_process_id()));std::error_code ec;fs::remove_all(root,ec);fs::create_directories(root);
 auto p=std::make_shared<Participant>();
 {SQLiteCoordinationJournal journal((root/"journal.sqlite3").string());CrossStoreCoordinator coordinator(journal,"policy-r1");std::vector<std::string> issues;assert(!coordinator.production_ready(&issues)&&issues.size()==6);assert(coordinator.register_participant(p));std::string error;assert(coordinator.execute(operation("op-1"),&error));auto receipt=journal.load("op-1");assert(receipt&&receipt->state==CoordinationState::Confirmed&&receipt->transition_sequence==4);assert(p->commits==1);assert(coordinator.execute(operation("op-1"),&error));assert(p->commits==1);}
 {SQLiteCoordinationJournal journal((root/"journal.sqlite3").string());CrossStoreCoordinator coordinator(journal,"policy-r1");assert(coordinator.register_participant(p));assert(coordinator.reconcile("op-1"));}
 // Crash after every prepare/commit/confirm participant boundary. The durable
 // state remains recoverable and idempotent replay never duplicates effects.
 for(const auto stage:{std::string("after_prepare"),std::string("after_commit"),
                       std::string("after_confirm")}) {
  for(const auto crash_id:{std::string("a"),std::string("b")}) {
   const auto suffix=stage+"-"+crash_id;const auto path=root/(suffix+".sqlite3");
   auto first=std::make_shared<Participant>();
   auto second=std::make_shared<Participant>();second->id_="b";second->digest_="sha256:b";
   bool injected=false;
   {SQLiteCoordinationJournal journal(path.string());
    CrossStoreCoordinator coordinator(journal,"policy-r1",{},
      [&](std::string_view observed,std::string_view participant){
       if(!injected&&observed==stage&&participant==crash_id){injected=true;return false;}
       return true;});
    assert(coordinator.register_participant(first));assert(coordinator.register_participant(second));
    std::string error;assert(!coordinator.execute(operation("crash-"+suffix),&error));
    assert(error.find("coordination_fault_injected:")==0);
    auto receipt=journal.load("crash-"+suffix);assert(receipt);
    assert(receipt->state==(stage=="after_prepare"?CoordinationState::Prepared:
           stage=="after_commit"?CoordinationState::Committing:CoordinationState::Confirming));}
   {SQLiteCoordinationJournal journal(path.string());CrossStoreCoordinator recovered(journal,"policy-r1");
    assert(recovered.register_participant(first));assert(recovered.register_participant(second));
    assert(recovered.reconcile("crash-"+suffix));
    auto receipt=journal.load("crash-"+suffix);
    assert(receipt&&receipt->state==CoordinationState::Confirmed);
    assert(first->commits==1&&second->commits==1);
    assert(recovered.execute(operation("crash-"+suffix)));
    assert(first->commits==1&&second->commits==1);}
  }
 }
 auto failing=std::make_shared<Participant>();failing->id_="b";failing->digest_="sha256:b";failing->fail_commit=true;
 {SQLiteCoordinationJournal journal((root/"journal.sqlite3").string());CrossStoreCoordinator coordinator(journal,"policy-r1");assert(coordinator.register_participant(failing));std::string error;assert(!coordinator.execute(operation("op-2"),&error));auto receipt=journal.load("op-2");assert(receipt&&receipt->state==CoordinationState::ManualReview);}
 {using namespace agent_framework::conversation;
  SQLiteTaskRegistry tasks((root/"tasks.sqlite3").string());ConversationIdentity identity{"tenant","conversation"};
  PersistentTask task;task.identity=identity;task.task_id="task";task.root_turn_id="turn";
  task.current_turn_id="turn";task.current_run_id="run";
  TaskRequirementRevision requirement;requirement.identity=identity;requirement.task_id="task";
  requirement.turn_id="turn";requirement.content="objective";
  TurnTaskLink link;link.identity=identity;link.turn_id="turn";link.task_id="task";link.run_id="run";
  assert(tasks.create(task,requirement,link).ok);
  auto participant=std::make_shared<ConversationTaskCoordinationParticipant>(tasks,"task-registry-v1");
  auto op=operation("op-task");op.expected_refs={{"conversation_id","conversation"},{"task_id","task"}};
  SQLiteCoordinationJournal journal((root/"task-journal.sqlite3").string());
  CrossStoreCoordinator coordinator(journal,"policy-r1");assert(coordinator.register_participant(participant));
  std::string error;assert(coordinator.execute(op,&error));auto receipt=journal.load("op-task");
  assert(receipt&&receipt->state==CoordinationState::Confirmed&&
         receipt->operation.participants.front().participant_id=="conversation_task_registry");}
 fs::remove_all(root,ec);
}
