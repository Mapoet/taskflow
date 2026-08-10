#include <cassert>
#include <chrono>
#include <filesystem>

#include "phase4_judge_test_support.hpp"

int main(){using namespace phase4_judge_test;auto s=suite("task-f6e-contracts");auto b=run(s,false);auto c=run(s,true);
    assert(decode_evaluation_suite(encode(s)));assert(decode_candidate_evaluation_run(encode(b)));
    auto unknown=encode(s);unknown["payload"]["unknown"]=true;unknown["canonical_digest"]=contracts::embedded_digest(unknown).value();assert(!decode_evaluation_suite(unknown));
    auto nested=encode(s);nested["payload"]["cases"][0]["unknown"]=true;nested["canonical_digest"]=contracts::embedded_digest(nested).value();assert(!decode_evaluation_suite(nested));
    auto tampered=encode(c);tampered["payload"]["revision_id"]="tampered";assert(!decode_candidate_evaluation_run(tampered));
    JudgeCheckpoint cp;cp.metadata=s.metadata;cp.workflow_id="store-f6e";cp.suite_digest=encode(s).at("canonical_digest");cp.baseline_run_digest=encode(b).at("canonical_digest");cp.candidate_run_digest=encode(c).at("canonical_digest");cp.memory_snapshot_id="snapshot";cp.memory_view_digest="sha256:view";cp.updated_at="2026-08-10T00:00:00Z";
    assert(decode_judge_checkpoint(encode(cp)));InMemoryJudgeStore memory;assert(memory.create(cp));auto next=cp;next.revision=2;next.next_stage=JudgeStage::PrimaryJudging;assert(memory.compare_exchange(next,1));assert(memory.compare_exchange(next,1).status==JudgeStoreStatus::RevisionConflict);
    auto changed=next;changed.revision=3;changed.suite_digest="sha256:changed";assert(memory.compare_exchange(changed,2).status==JudgeStoreStatus::Invalid);
    auto suffix=std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());auto path=(std::filesystem::temp_directory_path()/("taskflow-f6e-contracts-"+suffix+".sqlite")).string();
    {SQLiteJudgeStore store(path);assert(store.create(cp));assert(store.compare_exchange(next,1));}
    {SQLiteJudgeStore store(path);auto loaded=store.load("tenant-a","store-f6e");assert(loaded&&loaded->revision==2&&loaded->checkpoint.next_stage==JudgeStage::PrimaryJudging);}
    std::filesystem::remove(path);std::filesystem::remove(path+"-wal");std::filesystem::remove(path+"-shm");return 0;}
