#include "agent/harness/sqlite_production_input_repository.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/contracts/contract.hpp"
#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::harness {
namespace {
namespace sqlite = internal::sqlite;
using json = nlohmann::json;

json identity_json(const contracts::ContractIdentity& id) {
    return contracts::identity_to_json(id);
}
json memory_scope_json(const memory_v2::MemoryScope& s) {
    return {{"tenant_id",s.tenant_id},{"organization_id",s.organization_id},
        {"principal_id",s.principal_id},{"agent_id",s.agent_id},{"project_id",s.project_id},
        {"workspace_id",s.workspace_id},{"path_scope",s.path_scope},{"task_id",s.task_id},
        {"run_id",s.run_id},{"turn_id",s.turn_id},{"level",static_cast<int>(s.level)}};
}
memory_v2::MemoryScope memory_scope_value(const json& v) {
    memory_v2::MemoryScope s; s.tenant_id=v.at("tenant_id"); s.organization_id=v.at("organization_id");
    s.principal_id=v.at("principal_id"); s.agent_id=v.at("agent_id"); s.project_id=v.at("project_id");
    s.workspace_id=v.at("workspace_id"); s.path_scope=v.at("path_scope"); s.task_id=v.at("task_id");
    s.run_id=v.at("run_id"); s.turn_id=v.at("turn_id");
    s.level=static_cast<memory_v2::MemoryLevel>(v.at("level").get<int>()); return s;
}
json memory_input_json(const memory_v2::workflows::MemoryWorkflowInput& v) {
    json sources=json::array();
    for(const auto& s:v.sources) sources.push_back({{"artifact_id",s.artifact_id},
        {"source_kind",s.source_kind},{"source_locator",s.source_locator},
        {"source_digest",s.source_digest},{"scope",memory_scope_json(s.scope)},
        {"acl_principals",s.acl_principals},{"content",s.content},
        {"evidence_ids",s.evidence_ids},{"event_time",s.event_time},
        {"trusted_instruction",s.trusted_instruction}});
    return {{"metadata",identity_json(v.metadata.identity)},{"workflow_id",v.workflow_id},
        {"subject",memory_scope_json(v.subject)},{"workflow_phase",v.workflow_phase},
        {"workflow_event",v.workflow_event},{"risk_level",v.risk_level},{"query",v.query},
        {"sources",std::move(sources)}};
}
memory_v2::workflows::MemoryWorkflowInput memory_input_value(const json& v) {
    memory_v2::workflows::MemoryWorkflowInput out;
    out.metadata.identity=*contracts::identity_from_json(v.at("metadata"));
    out.workflow_id=v.at("workflow_id"); out.subject=memory_scope_value(v.at("subject"));
    out.workflow_phase=v.at("workflow_phase"); out.workflow_event=v.at("workflow_event");
    out.risk_level=v.at("risk_level"); out.query=v.at("query");
    for(const auto& x:v.at("sources")) { memory_v2::workflows::MemorySourceArtifact s;
        s.artifact_id=x.at("artifact_id"); s.source_kind=x.at("source_kind");
        s.source_locator=x.at("source_locator"); s.source_digest=x.at("source_digest");
        s.scope=memory_scope_value(x.at("scope")); s.acl_principals=x.at("acl_principals").get<std::vector<std::string>>();
        s.content=x.at("content"); s.evidence_ids=x.at("evidence_ids").get<std::vector<std::string>>();
        s.event_time=x.at("event_time"); s.trusted_instruction=x.at("trusted_instruction"); out.sources.push_back(std::move(s)); }
    return out;
}
std::string digest(const json& v) { return contracts::canonical_digest(v).value_or(""); }
}

SQLiteProductionWorkflowInputRepository::SQLiteProductionWorkflowInputRepository(std::string path)
    : path_(std::move(path)) {
    if(path_.empty()) throw std::invalid_argument("production input repository path is required");
    const std::filesystem::path file(path_); std::error_code ec;
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    if(ec) throw std::runtime_error(ec.message());
    sqlite3* db=nullptr;
    if(sqlite3_open_v2(path_.c_str(),&db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK) {
        const std::string message=db?sqlite3_errmsg(db):"sqlite open failed"; if(db) sqlite3_close(db); throw std::runtime_error(message); }
    db_=db; sqlite3_busy_timeout(db,3000); sqlite::exec(db,"PRAGMA journal_mode=WAL");
    sqlite::exec(db,"PRAGMA synchronous=FULL"); migrate();
}
SQLiteProductionWorkflowInputRepository::~SQLiteProductionWorkflowInputRepository(){if(db_)sqlite3_close(sqlite::database(db_));}
void SQLiteProductionWorkflowInputRepository::migrate(){auto*db=sqlite::database(db_);
    sqlite::exec(db,"CREATE TABLE IF NOT EXISTS phase4_production_inputs(kind TEXT NOT NULL,tenant_id TEXT NOT NULL,task_id TEXT NOT NULL,run_id TEXT NOT NULL,lookup_digest TEXT NOT NULL,revision INTEGER NOT NULL,document_json TEXT NOT NULL,document_digest TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,PRIMARY KEY(kind,tenant_id,task_id,run_id,lookup_digest,revision))");
    sqlite::exec(db,"CREATE UNIQUE INDEX IF NOT EXISTS phase4_production_input_latest ON phase4_production_inputs(kind,tenant_id,task_id,run_id,lookup_digest,revision)");}

ProductionInputCommit SQLiteProductionWorkflowInputRepository::put(std::string_view kind,
    const contracts::ContractIdentity& id,std::string_view lookup,std::uint64_t revision,const json& document){
    if(kind.empty()||id.tenant_id.empty()||id.task_id.empty()||id.run_id.empty()||lookup.empty()||revision==0)
        return {ProductionInputStatus::Invalid,0,{},"kind, identity, lookup digest and revision are required"};
    const auto d=digest(document); if(d.empty()) return {ProductionInputStatus::Invalid,0,{},"document digest failed"};
    std::lock_guard lock(mutex_); auto*db=sqlite::database(db_);
    sqlite::Statement query(db,"SELECT document_digest FROM phase4_production_inputs WHERE kind=? AND tenant_id=? AND task_id=? AND run_id=? AND lookup_digest=? AND revision=?");
    sqlite::bind_text(query.get(),1,kind);sqlite::bind_text(query.get(),2,id.tenant_id);sqlite::bind_text(query.get(),3,id.task_id);sqlite::bind_text(query.get(),4,id.run_id);sqlite::bind_text(query.get(),5,lookup);sqlite::bind_uint64(query.get(),6,revision);
    if(sqlite::step(query.get())==SQLITE_ROW){const auto existing=sqlite::column_text(query.get(),0);return {existing==d?ProductionInputStatus::AlreadyExists:ProductionInputStatus::RevisionConflict,revision,d,existing==d?"":"immutable revision digest mismatch"};}
    sqlite::Statement insert(db,"INSERT INTO phase4_production_inputs(kind,tenant_id,task_id,run_id,lookup_digest,revision,document_json,document_digest) VALUES(?,?,?,?,?,?,?,?)");
    sqlite::bind_text(insert.get(),1,kind);sqlite::bind_text(insert.get(),2,id.tenant_id);sqlite::bind_text(insert.get(),3,id.task_id);sqlite::bind_text(insert.get(),4,id.run_id);sqlite::bind_text(insert.get(),5,lookup);sqlite::bind_uint64(insert.get(),6,revision);sqlite::bind_text(insert.get(),7,document.dump());sqlite::bind_text(insert.get(),8,d);
    if(sqlite::step(insert.get())!=SQLITE_DONE)
        return {ProductionInputStatus::Error,0,d,sqlite3_errmsg(db)};
    return {ProductionInputStatus::Committed,revision,d,{}};}

std::optional<json> SQLiteProductionWorkflowInputRepository::get(std::string_view kind,const contracts::ContractIdentity&id,std::string_view lookup){
    std::lock_guard lock(mutex_);auto*db=sqlite::database(db_);sqlite::Statement q(db,"SELECT document_json,document_digest FROM phase4_production_inputs WHERE kind=? AND tenant_id=? AND task_id=? AND run_id=? AND lookup_digest=? ORDER BY revision DESC LIMIT 1");
    sqlite::bind_text(q.get(),1,kind);sqlite::bind_text(q.get(),2,id.tenant_id);sqlite::bind_text(q.get(),3,id.task_id);sqlite::bind_text(q.get(),4,id.run_id);sqlite::bind_text(q.get(),5,lookup);if(sqlite::step(q.get())!=SQLITE_ROW)return std::nullopt;
    const auto text=sqlite::column_text(q.get(),0),stored=sqlite::column_text(q.get(),1);auto value=json::parse(text);if(digest(value)!=stored)throw std::runtime_error("production input repository digest mismatch");return value;}

ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_intake(const planning::TaskIntake&v,std::uint64_t r){return put("intake",v.metadata.identity,"current",r,planning::encode(v));}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_memory_input(const memory_v2::workflows::MemoryWorkflowInput&v,std::string a,std::uint64_t r){return put("memory_input",v.metadata.identity,a,r,memory_input_json(v));}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_acceptance_contract(const assurance::AcceptanceContract&v){auto d=assurance::encode(v);return put("acceptance_contract",v.metadata.identity,d.at("canonical_digest").get<std::string>(),v.revision,d);}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_task_context(const contracts::ContractIdentity&i,std::string p,json v,std::uint64_t r){return put("task_context",i,p,r,v);}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_artifact_manifest(const contracts::ContractIdentity&i,std::string a,json v,std::uint64_t r){if(digest(v)!=a)return {ProductionInputStatus::Invalid,0,{},"artifact lookup digest must match document"};return put("artifact_manifest",i,a,r,v);}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_acceptance_report(const assurance::AcceptanceReport&v,std::string d,std::uint64_t r){auto j=assurance::encode(v);if(digest(j)!=d)return {ProductionInputStatus::Invalid,0,{},"report lookup digest must match document"};return put("acceptance_report",v.metadata.identity,d,r,j);}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_assurance_checkpoint(const assurance::AssuranceCheckpoint&v,std::string d){return put("assurance_checkpoint",v.metadata.identity,d,v.revision,assurance::encode(v));}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_impact_inventory(const remediation::ImpactInventory&v,std::uint64_t r){return put("impact_inventory",v.metadata.identity,v.artifact_manifest_digest,r,remediation::encode(v));}

ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_evaluation_input(const JudgeWorkflowInput&v,std::uint64_t r){if(!v.datasets)return {ProductionInputStatus::Invalid,0,{},"dataset registry required"};json cases=json::array();for(const auto&s:v.suite.cases){auto c=v.datasets->case_for_scoring(s.case_id);if(!c)return {ProductionInputStatus::Invalid,0,{},"suite dataset case missing"};cases.push_back(eval::encode(*c));}json doc={{"suite",eval::encode(v.suite)},{"baseline",eval::encode(v.baseline)},{"candidate",eval::encode(v.candidate)},{"cases",cases},{"subject",memory_scope_json(v.subject)}};return put("evaluation_input",v.suite.metadata.identity,"current",r,doc);}
ProductionInputCommit SQLiteProductionWorkflowInputRepository::put_plan_node_descriptor(
    const tool_runtime::PlanNodeExecutionDescriptor&v,std::uint64_t r){json doc={{"plan_digest",v.plan_digest},{"node_id",v.node_id},{"executor_id",v.executor_id},{"executor_revision",v.executor_revision},{"input",v.input},{"granted_capabilities",v.granted_capabilities},{"approval_decision_id",v.approval_decision_id},{"side_effecting",v.side_effecting}};const auto d=digest(doc);if(d!=v.descriptor_digest)return{ProductionInputStatus::Invalid,0,{},"descriptor digest mismatch"};return put("plan_node_descriptor",v.metadata.identity,v.plan_digest+":"+v.node_id,r,doc);}

std::optional<planning::TaskIntake> SQLiteProductionWorkflowInputRepository::intake(const contracts::ContractIdentity&i){auto v=get("intake",i,"current");return v?planning::decode_task_intake(*v):std::nullopt;}
std::optional<memory_v2::workflows::MemoryWorkflowInput> SQLiteProductionWorkflowInputRepository::memory_input(const contracts::ContractIdentity&i,std::string_view a){auto v=get("memory_input",i,a);try{return v?std::optional(memory_input_value(*v)):std::nullopt;}catch(...){return std::nullopt;}}
std::optional<assurance::AcceptanceContract> SQLiteProductionWorkflowInputRepository::acceptance_contract(const contracts::ContractIdentity&i,std::string_view d){auto v=get("acceptance_contract",i,d);return v?assurance::decode_acceptance_contract(*v):std::nullopt;}
std::optional<json> SQLiteProductionWorkflowInputRepository::task_context(const contracts::ContractIdentity&i,std::string_view d){return get("task_context",i,d);}
std::optional<json> SQLiteProductionWorkflowInputRepository::artifact_manifest(const contracts::ContractIdentity&i,std::string_view d){return get("artifact_manifest",i,d);}
std::optional<assurance::AcceptanceReport> SQLiteProductionWorkflowInputRepository::acceptance_report(const contracts::ContractIdentity&i,std::string_view d){auto v=get("acceptance_report",i,d);return v?assurance::decode_acceptance_report(*v):std::nullopt;}
std::optional<assurance::AssuranceCheckpoint> SQLiteProductionWorkflowInputRepository::assurance_checkpoint(const contracts::ContractIdentity&i,std::string_view d){auto v=get("assurance_checkpoint",i,d);return v?assurance::decode_assurance_checkpoint(*v):std::nullopt;}
std::optional<remediation::ImpactInventory> SQLiteProductionWorkflowInputRepository::impact_inventory(const contracts::ContractIdentity&i,std::string_view d){auto v=get("impact_inventory",i,d);return v?remediation::decode_impact_inventory(*v):std::nullopt;}
std::optional<JudgeWorkflowInput> SQLiteProductionWorkflowInputRepository::evaluation_input(const contracts::ContractIdentity&i){auto v=get("evaluation_input",i,"current");if(!v)return std::nullopt;JudgeWorkflowInput out;out.suite=*eval::decode_evaluation_suite(v->at("suite"));out.baseline=*eval::decode_candidate_evaluation_run(v->at("baseline"));out.candidate=*eval::decode_candidate_evaluation_run(v->at("candidate"));auto registry=std::make_shared<eval::DatasetRegistry>();for(const auto&x:v->at("cases")){auto c=eval::decode_dataset_case(x);if(!c||!registry->register_case(*c))return std::nullopt;}out.datasets=registry;out.subject=memory_scope_value(v->at("subject"));return out;}
std::optional<tool_runtime::PlanNodeExecutionDescriptor> SQLiteProductionWorkflowInputRepository::descriptor(const contracts::ContractIdentity&i,std::string_view p,std::string_view n){auto v=get("plan_node_descriptor",i,std::string(p)+":"+std::string(n));if(!v)return{};tool_runtime::PlanNodeExecutionDescriptor d;d.metadata.identity=i;d.plan_digest=v->at("plan_digest");d.node_id=v->at("node_id");d.executor_id=v->at("executor_id");d.executor_revision=v->at("executor_revision");d.input=v->at("input");d.granted_capabilities=v->at("granted_capabilities").get<std::vector<std::string>>();d.approval_decision_id=v->at("approval_decision_id");d.side_effecting=v->at("side_effecting");d.descriptor_digest=digest(*v);return d;}
}  // namespace agent_framework::harness
