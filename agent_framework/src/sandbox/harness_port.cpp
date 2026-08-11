#include "agent/sandbox/harness_port.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/contracts/contract.hpp"
#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::sandbox {
namespace {
namespace sqlite = internal::sqlite;
nlohmann::json receipt_json(const ExecResult& result) {
    return {{"exit_code", result.exit_code}, {"stdout_text", result.stdout_text},
            {"stderr_text", result.stderr_text}, {"timed_out", result.timed_out},
            {"manifest", encode(result.manifest)}};
}
ExecResult receipt_value(const nlohmann::json& value) {
    ExecResult result; result.exit_code=value.at("exit_code"); result.stdout_text=value.at("stdout_text");
    result.stderr_text=value.at("stderr_text"); result.timed_out=value.at("timed_out");
    auto manifest=decode_sandbox_manifest(value.at("manifest"));
    if(!manifest) throw std::runtime_error("invalid sandbox receipt manifest");
    result.manifest=std::move(*manifest); return result;
}
}

SQLiteSandboxReceiptJournal::SQLiteSandboxReceiptJournal(std::string path) {
    if(path.empty()) throw std::invalid_argument("sandbox journal path is required");
    std::error_code ec; const std::filesystem::path file(path);
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(),ec);
    sqlite3* opened=nullptr;
    if(ec||sqlite3_open_v2(path.c_str(),&opened,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK){
        const auto message=ec?ec.message():(opened?sqlite3_errmsg(opened):"sqlite open failed");if(opened)sqlite3_close(opened);throw std::runtime_error(message);}
    db_=opened;sqlite3_busy_timeout(opened,3000);sqlite::exec(opened,"PRAGMA journal_mode=WAL");sqlite::exec(opened,"PRAGMA synchronous=FULL");
    sqlite::exec(opened,"CREATE TABLE IF NOT EXISTS sandbox_receipts(idempotency_key TEXT PRIMARY KEY,document_json TEXT NOT NULL,document_digest TEXT NOT NULL)");
}
SQLiteSandboxReceiptJournal::~SQLiteSandboxReceiptJournal(){if(db_)sqlite3_close(sqlite::database(db_));}
bool SQLiteSandboxReceiptJournal::record(std::string_view key,const ExecResult& result,std::string* error){
    std::lock_guard lock(mutex_);try{auto*db=sqlite::database(db_);const auto document=receipt_json(result);const auto text=document.dump();const auto digest=contracts::canonical_digest(document).value_or("");
        sqlite::Statement insert(db,"INSERT INTO sandbox_receipts(idempotency_key,document_json,document_digest) VALUES(?,?,?) ON CONFLICT(idempotency_key) DO NOTHING");sqlite::bind_text(insert.get(),1,key);sqlite::bind_text(insert.get(),2,text);sqlite::bind_text(insert.get(),3,digest);
        if(sqlite::step(insert.get())!=SQLITE_DONE){if(error)*error=sqlite3_errmsg(db);return false;}
        sqlite::Statement verify(db,"SELECT document_digest FROM sandbox_receipts WHERE idempotency_key=?");sqlite::bind_text(verify.get(),1,key);if(sqlite::step(verify.get())!=SQLITE_ROW||sqlite::column_text(verify.get(),0)!=digest){if(error)*error="idempotency key receipt conflict";return false;}return true;
    }catch(const std::exception&e){if(error)*error=e.what();return false;}}
std::optional<ExecResult> SQLiteSandboxReceiptJournal::find(std::string_view key){std::lock_guard lock(mutex_);auto*db=sqlite::database(db_);sqlite::Statement query(db,"SELECT document_json,document_digest FROM sandbox_receipts WHERE idempotency_key=?");sqlite::bind_text(query.get(),1,key);if(sqlite::step(query.get())!=SQLITE_ROW)return std::nullopt;const auto text=sqlite::column_text(query.get(),0);const auto value=nlohmann::json::parse(text);if(contracts::canonical_digest(value).value_or("")!=sqlite::column_text(query.get(),1))throw std::runtime_error("sandbox receipt digest mismatch");return receipt_value(value);}

SandboxExecutionHarnessPort::SandboxExecutionHarnessPort(std::string id,SandboxProvider& provider,SandboxSpec spec,SQLiteSandboxReceiptJournal& journal)
    :id_(std::move(id)),provider_(provider),spec_(std::move(spec)),journal_(journal){if(id_.empty())throw std::invalid_argument("sandbox port id required");}
harness::HarnessStageResult SandboxExecutionHarnessPort::project(const ExecResult& result)const{
    harness::HarnessStageResult out;const auto manifest_digest=encode(result.manifest).at("canonical_digest").get<std::string>();out.invocation_manifest_digest=result.manifest.spec_digest;out.output_digest=result.manifest.workspace_output_digest;out.effect_receipt_digest=manifest_digest;
    if(result.timed_out){out.outcome=harness::StageOutcome::Retryable;out.error_code="sandbox_timeout";out.error_message="sandbox wall time exceeded";return out;}
    if(result.exit_code!=0){out.outcome=harness::StageOutcome::Failed;out.error_code="sandbox_exit_nonzero";out.error_message="sandbox exited with code "+std::to_string(result.exit_code);return out;}
    out.outcome=harness::StageOutcome::Succeeded;out.pins.artifact_manifest_digest=result.manifest.workspace_output_digest;return out;}
harness::HarnessStageResult SandboxExecutionHarnessPort::execute(const harness::HarnessStageRequest& request){
    if(auto prior=journal_.find(request.idempotency_key)) return project(*prior);
    SandboxSpec spec=spec_;
    spec.metadata.identity=request.checkpoint.metadata.identity;
    spec.workspace_base_digest=request.checkpoint.pins.artifact_manifest_digest.empty()
        ? spec.workspace_base_digest : request.checkpoint.pins.artifact_manifest_digest;
    std::string error;auto handle=provider_.create(spec,&error);if(!handle)return {harness::StageOutcome::Failed,{},"","","",{},"","sandbox_create_failed",error};
    auto result=provider_.exec(*handle,&error);std::string destroy_error;(void)provider_.destroy(*handle,&destroy_error);if(!result)return {harness::StageOutcome::ManualReview,{},"","","",{},"","sandbox_effect_unknown",error};
    if(!journal_.record(request.idempotency_key,*result,&error))
        return {harness::StageOutcome::ManualReview,{},"","","",{},"","sandbox_receipt_persist_failed",error};
    return project(*result);
}
std::optional<harness::HarnessStageResult> SandboxExecutionHarnessPort::reconcile(const harness::HarnessStageRequest& request){auto prior=journal_.find(request.idempotency_key);return prior?std::optional(project(*prior)):std::nullopt;}
}  // namespace agent_framework::sandbox
