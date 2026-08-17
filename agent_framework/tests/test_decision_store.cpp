#include <cassert>
#include <barrier>
#include <filesystem>
#include <thread>
#include <sqlite3.h>

#include "agent/decision/decision_store.hpp"

using namespace agent_framework;

namespace {
decision::DecisionRequest request(std::string id="decision-1") {
    decision::DecisionRequest value;
    value.subject={"tenant","org","principal","project","workspace","session",
                   "conversation","task","run","turn","agent",3,true,false};
    value.decision_id=std::move(id);value.kind=decision::DecisionKind::TaskSemantics;
    value.resume_payload={{"task_intent","initial_request"}};
    value.question="Research only or implement?";
    value.options={{"research","Research only","Do not modify files",
                    {{"work_shape","long_running_task"},{"effect_class","read_only"}}},
                   {"implement","Implement","Modify and test",
                    {{"work_shape","bounded_task"},{"effect_class","workspace_write"}}}};
    value.recommended_option_id="research";value.expires_at_ms=1000;
    value.origin_digest="sha256:origin";value.created_at="100";value.updated_at="100";
    return value;
}
}

int main() {
    const auto path=(std::filesystem::temp_directory_path()/"agent-decision-store.sqlite").string();
    std::filesystem::remove(path);
    {
        decision::SQLiteDecisionStore store(path);auto value=request();
        auto created=store.create(value);assert(created.ok&&created.revision==1);
        assert(store.create(value).ok); // payload-identical replay
        auto conflict=value;conflict.origin_digest="sha256:other";
        assert(!store.create(conflict).ok);
        auto injection=request("decision-injection");
        injection.options[0].semantic_patch={{"shell_command","rm -rf workspace"}};
        assert(!store.create(injection).ok);
        auto pending=store.pending("tenant","session","conversation");
        assert(pending&&pending->options.size()==2&&pending->subject.authorization_revision==3&&
               pending->subject.agent_id=="agent");
        assert(store.latest("tenant","session","conversation")->decision_id=="decision-1");
        assert(!store.answer("tenant","decision-1",1,"unknown",200).ok);
        auto answered=store.answer("tenant","decision-1",1,"implement",200);
        assert(answered.ok&&answered.state==decision::DecisionState::Answered);
        assert(!store.answer("tenant","decision-1",1,"implement",200).ok);
    }
    {
        decision::SQLiteDecisionStore reopened(path);
        auto stored=reopened.load("tenant","decision-1");
        assert(stored&&stored->state==decision::DecisionState::Answered&&
               stored->selected_option_id=="implement"&&
               stored->options[1].semantic_patch.at("effect_class")=="workspace_write");
        auto expiring=request("decision-expire");expiring.expires_at_ms=100;
        assert(reopened.create(expiring).ok);
        assert(!reopened.expire("tenant","decision-expire",1,99).ok);
        assert(reopened.expire("tenant","decision-expire",1,100).ok);
        auto cancelling=request("decision-cancel");assert(reopened.create(cancelling).ok);
        assert(reopened.cancel("tenant","decision-cancel",1).ok);
    }
    {
        auto raced=request("decision-race");
        decision::SQLiteDecisionStore setup(path);assert(setup.create(raced).ok);
        decision::SQLiteDecisionStore first(path),second(path);std::barrier start(3);
        decision::DecisionMutationResult a,b;
        std::thread one([&]{start.arrive_and_wait();a=first.answer("tenant","decision-race",1,"research",200);});
        std::thread two([&]{start.arrive_and_wait();b=second.answer("tenant","decision-race",1,"implement",200);});
        start.arrive_and_wait();one.join();two.join();assert(a.ok!=b.ok);
    }
    {
        const auto migrated_path=(std::filesystem::temp_directory_path()/"agent-decision-store-v0.sqlite").string();
        std::filesystem::remove(migrated_path);sqlite3* db=nullptr;
        assert(sqlite3_open(migrated_path.c_str(),&db)==SQLITE_OK);
        const char* legacy="CREATE TABLE durable_decisions(tenant TEXT NOT NULL,decision_id TEXT NOT NULL,organization_id TEXT NOT NULL,principal_id TEXT NOT NULL,project_id TEXT NOT NULL,workspace_id TEXT NOT NULL,session_id TEXT NOT NULL,conversation_id TEXT NOT NULL,task_id TEXT NOT NULL,run_id TEXT NOT NULL,turn_id TEXT NOT NULL,kind TEXT NOT NULL,question TEXT NOT NULL,options_json TEXT NOT NULL,recommended_option_id TEXT NOT NULL,selected_option_id TEXT NOT NULL DEFAULT '',revision INTEGER NOT NULL,expires_at_ms INTEGER NOT NULL,state TEXT NOT NULL,origin_digest TEXT NOT NULL,created_at TEXT NOT NULL,updated_at TEXT NOT NULL,PRIMARY KEY(tenant,decision_id))";
        assert(sqlite3_exec(db,legacy,nullptr,nullptr,nullptr)==SQLITE_OK);sqlite3_close(db);
        decision::SQLiteDecisionStore migrated(migrated_path);auto value=request("migrated-decision");
        assert(migrated.create(value).ok);auto loaded=migrated.load("tenant","migrated-decision");
        assert(loaded&&loaded->subject.agent_id=="agent"&&loaded->subject.authorization_revision==3&&
               loaded->resume_payload.at("task_intent")=="initial_request");
        std::filesystem::remove(migrated_path);
    }
    std::filesystem::remove(path);
}
