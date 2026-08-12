#include <cassert>
#include <filesystem>

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
    bool commit(const CrossStoreOperation&,const ParticipantPin&p,std::string*e)override{++commits;return !fail_commit&&check(p,e);}
    bool confirm(const CrossStoreOperation&,const ParticipantPin&p,std::string*e)override{return check(p,e);}
    bool compensate(const CrossStoreOperation&,const ParticipantPin&,std::string*)override{return reversible_;}
    bool check(const ParticipantPin&p,std::string*e){const bool ok=p.revision==revision_&&p.digest==digest_;if(!ok&&e)*e="pin drift";return ok;}
    std::string id_{"a"},digest_{"sha256:a"};std::uint64_t revision_{1};bool reversible_{false},fail_commit{false};int commits{0};
};
CrossStoreOperation operation(std::string id){return{id,"tenant","run","harness","test",id,"policy-r1",{}, {}};}
}

int main(){namespace fs=std::filesystem;using namespace agent_framework::harness;
 const auto root=fs::temp_directory_path()/("phase4-cross-store-"+std::to_string(agent_framework::internal::current_process_id()));std::error_code ec;fs::remove_all(root,ec);fs::create_directories(root);
 auto p=std::make_shared<Participant>();
 {SQLiteCoordinationJournal journal((root/"journal.sqlite3").string());CrossStoreCoordinator coordinator(journal,"policy-r1");std::vector<std::string> issues;assert(!coordinator.production_ready(&issues)&&issues.size()==5);assert(coordinator.register_participant(p));std::string error;assert(coordinator.execute(operation("op-1"),&error));auto receipt=journal.load("op-1");assert(receipt&&receipt->state==CoordinationState::Confirmed&&receipt->transition_sequence==4);assert(p->commits==1);assert(coordinator.execute(operation("op-1"),&error));assert(p->commits==1);}
 {SQLiteCoordinationJournal journal((root/"journal.sqlite3").string());CrossStoreCoordinator coordinator(journal,"policy-r1");assert(coordinator.register_participant(p));assert(coordinator.reconcile("op-1"));}
 auto failing=std::make_shared<Participant>();failing->id_="b";failing->digest_="sha256:b";failing->fail_commit=true;
 {SQLiteCoordinationJournal journal((root/"journal.sqlite3").string());CrossStoreCoordinator coordinator(journal,"policy-r1");assert(coordinator.register_participant(failing));std::string error;assert(!coordinator.execute(operation("op-2"),&error));auto receipt=journal.load("op-2");assert(receipt&&receipt->state==CoordinationState::ManualReview);}
 fs::remove_all(root,ec);
}
