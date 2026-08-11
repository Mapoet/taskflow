#include "agent/eval/governance.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::eval {
namespace {
using json = nlohmann::json;
namespace sqlite = agent_framework::internal::sqlite;

json signature_json(const CampaignSignature& value) {
    return {{"algorithm", value.algorithm}, {"key_id", value.key_id},
            {"signed_digest", value.signed_digest}, {"signature", value.signature}};
}
CampaignSignature signature_from(const json& value) {
    return {value.at("algorithm"), value.at("key_id"), value.at("signed_digest"),
            value.at("signature")};
}
std::optional<CampaignSpec> spec_from(const std::string& bytes) {
    try {
        auto document = contracts::parse_typed_contract(json::parse(bytes),
                                                        "agent.eval_campaign_spec/v1");
        if(!document) return std::nullopt;
        const auto& p = document->payload;
        CampaignSpec out;
        out.metadata = document->metadata;
        out.campaign_id = p.at("campaign_id"); out.dataset_id = p.at("dataset_id");
        out.dataset_version = p.at("dataset_version");
        out.dataset_manifest_digest = p.at("dataset_manifest_digest");
        out.baseline_revision_digest = p.at("baseline_revision_digest");
        out.candidate_revision_digest = p.at("candidate_revision_digest");
        out.model_digest = p.at("model_digest"); out.prompt_digest = p.at("prompt_digest");
        out.profile_digest = p.at("profile_digest"); out.suite_digest = p.at("suite_digest");
        out.scheduled_at = p.at("scheduled_at"); return out;
    } catch(...) { return std::nullopt; }
}
std::optional<CampaignReport> report_from(const std::string& bytes) {
    try {
        auto document = contracts::parse_typed_contract(json::parse(bytes),
                                                        "agent.eval_campaign_report/v1");
        if(!document) return std::nullopt;
        const auto& p = document->payload;
        CampaignReport out; out.metadata = document->metadata;
        out.campaign_id = p.at("campaign_id");
        out.dataset_manifest_digest = p.at("dataset_manifest_digest");
        out.spec_digest = p.at("spec_digest"); out.passed = p.at("passed");
        out.metrics = p.at("metrics").get<std::map<std::string,double>>();
        out.quarantined_case_ids = p.at("quarantined_case_ids").get<std::vector<std::string>>();
        out.blockers = p.at("blockers").get<std::vector<std::string>>();
        out.completed_at = p.at("completed_at"); out.signature = signature_from(p.at("signature"));
        return out;
    } catch(...) { return std::nullopt; }
}
bool insert_immutable(sqlite3* db, const char* sql, const std::vector<std::string>& values,
                      std::string* error) {
    try {
        sqlite::Statement statement(db, sql);
        for(std::size_t i = 0; i < values.size(); ++i)
            sqlite::bind_text(statement.get(), static_cast<int>(i + 1), values[i]);
        if(sqlite::step(statement.get()) != SQLITE_DONE) throw std::runtime_error(sqlite3_errmsg(db));
        return true;
    } catch(const std::exception& ex) { if(error) *error = ex.what(); return false; }
}
}

nlohmann::json encode(const CampaignSpec& v) {
    return contracts::make_typed_contract(v.metadata, "agent.eval_campaign_spec/v1",
        {{"campaign_id",v.campaign_id},{"dataset_id",v.dataset_id},
         {"dataset_version",v.dataset_version},{"dataset_manifest_digest",v.dataset_manifest_digest},
         {"baseline_revision_digest",v.baseline_revision_digest},
         {"candidate_revision_digest",v.candidate_revision_digest},{"model_digest",v.model_digest},
         {"prompt_digest",v.prompt_digest},{"profile_digest",v.profile_digest},
         {"suite_digest",v.suite_digest},{"scheduled_at",v.scheduled_at}});
}
nlohmann::json encode(const CampaignReport& v) {
    return contracts::make_typed_contract(v.metadata, "agent.eval_campaign_report/v1",
        {{"campaign_id",v.campaign_id},{"dataset_manifest_digest",v.dataset_manifest_digest},
         {"spec_digest",v.spec_digest},{"passed",v.passed},{"metrics",v.metrics},
         {"quarantined_case_ids",v.quarantined_case_ids},{"blockers",v.blockers},
         {"completed_at",v.completed_at},{"signature",signature_json(v.signature)}});
}
std::string campaign_spec_digest(const CampaignSpec& value) {
    auto encoded = encode(value); return encoded.value("canonical_digest", "");
}
std::string campaign_report_signing_digest(const CampaignReport& value) {
    auto copy = value; copy.signature = {}; auto encoded = encode(copy);
    return encoded.value("canonical_digest", "");
}
bool validate_campaign_spec(const CampaignSpec& v, const DatasetManifest& manifest,
                            std::string* error) {
    const bool required = !v.campaign_id.empty() && !v.dataset_id.empty() &&
        !v.dataset_version.empty() && !v.dataset_manifest_digest.empty() &&
        !v.baseline_revision_digest.empty() && !v.candidate_revision_digest.empty() &&
        !v.model_digest.empty() && !v.prompt_digest.empty() && !v.profile_digest.empty() &&
        !v.suite_digest.empty() && !v.scheduled_at.empty();
    const auto manifest_digest = contracts::canonical_digest(encode(manifest)).value_or("");
    const bool bound = v.dataset_id == manifest.dataset_id && v.dataset_version == manifest.version &&
        (v.dataset_manifest_digest == encode(manifest).value("canonical_digest", "") ||
         v.dataset_manifest_digest == manifest_digest);
    if(!required || !bound) { if(error) *error = required ? "dataset manifest binding mismatch" : "campaign binding missing"; return false; }
    return true;
}
bool validate_campaign_report(const CampaignReport& v,
    const std::function<bool(const CampaignSignature&)>& verifier, std::string* error) {
    const auto digest = campaign_report_signing_digest(v);
    if(v.campaign_id.empty() || v.spec_digest.empty() || v.dataset_manifest_digest.empty() ||
       v.completed_at.empty() || v.signature.algorithm.empty() || v.signature.key_id.empty() ||
       v.signature.signature.empty() || v.signature.signed_digest != digest || !verifier ||
       !verifier(v.signature)) {
        if(error) *error = "campaign report signature or binding invalid";
        return false;
    }
    if(v.passed && !v.blockers.empty()) { if(error) *error = "passing report contains blockers"; return false; }
    return true;
}
std::vector<CampaignTrend> compare_campaign_reports(const CampaignReport& a,
                                                    const CampaignReport& b) {
    std::vector<CampaignTrend> out;
    for(const auto& [name, current] : b.metrics) {
        const auto found = a.metrics.find(name); if(found == a.metrics.end()) continue;
        out.push_back({name, found->second, current, current - found->second});
    }
    return out;
}

SQLiteCampaignStore::SQLiteCampaignStore(std::string path, int busy_timeout_ms) {
    if(path.empty()) throw std::invalid_argument("campaign store path required");
    std::error_code ec; const std::filesystem::path file(path);
    if(file.has_parent_path()) std::filesystem::create_directories(file.parent_path(), ec);
    sqlite3* opened = nullptr;
    if(ec || sqlite3_open_v2(path.c_str(), &opened, SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const auto message = ec ? ec.message() : (opened ? sqlite3_errmsg(opened) : "sqlite open failed");
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened; sqlite3_busy_timeout(opened, busy_timeout_ms); migrate();
}
SQLiteCampaignStore::~SQLiteCampaignStore() { if(db_) sqlite3_close(sqlite::database(db_)); }
void SQLiteCampaignStore::migrate() {
    auto* db = sqlite::database(db_); sqlite::exec(db,"PRAGMA journal_mode=WAL");
    sqlite::exec(db,"PRAGMA synchronous=FULL");
    sqlite::exec(db,"CREATE TABLE IF NOT EXISTS eval_manifests(dataset_id TEXT NOT NULL,version TEXT NOT NULL,digest TEXT NOT NULL,json TEXT NOT NULL,PRIMARY KEY(dataset_id,version))");
    sqlite::exec(db,"CREATE TABLE IF NOT EXISTS eval_specs(campaign_id TEXT PRIMARY KEY,dataset_id TEXT NOT NULL,dataset_version TEXT NOT NULL,digest TEXT NOT NULL,json TEXT NOT NULL)");
    sqlite::exec(db,"CREATE TABLE IF NOT EXISTS eval_labels(label_id TEXT PRIMARY KEY,dataset_id TEXT NOT NULL,dataset_version TEXT NOT NULL,json TEXT NOT NULL)");
    sqlite::exec(db,"CREATE TABLE IF NOT EXISTS eval_quarantine(campaign_id TEXT NOT NULL,case_id TEXT NOT NULL,digest TEXT NOT NULL,json TEXT NOT NULL,PRIMARY KEY(campaign_id,case_id))");
    sqlite::exec(db,"CREATE TABLE IF NOT EXISTS eval_reports(campaign_id TEXT PRIMARY KEY,dataset_id TEXT NOT NULL,dataset_version TEXT NOT NULL,completed_at TEXT NOT NULL,json TEXT NOT NULL)");
    sqlite::exec(db,"CREATE TABLE IF NOT EXISTS eval_leases(campaign_id TEXT PRIMARY KEY,owner TEXT NOT NULL,expires_at INTEGER NOT NULL)");
}
bool SQLiteCampaignStore::put_manifest(const DatasetManifest& v,std::string* e){std::lock_guard l(mutex_);auto j=encode(v);return insert_immutable(sqlite::database(db_),"INSERT INTO eval_manifests VALUES(?,?,?,?)",{v.dataset_id,v.version,j.value("canonical_digest",""),contracts::canonical_json(j)},e);}
bool SQLiteCampaignStore::put_spec(const CampaignSpec& v,std::string* e){std::lock_guard l(mutex_);auto j=encode(v);return insert_immutable(sqlite::database(db_),"INSERT INTO eval_specs VALUES(?,?,?,?,?)",{v.campaign_id,v.dataset_id,v.dataset_version,j.value("canonical_digest",""),contracts::canonical_json(j)},e);}
bool SQLiteCampaignStore::put_label(const HumanLabel& v,std::string* e){std::lock_guard l(mutex_);return insert_immutable(sqlite::database(db_),"INSERT INTO eval_labels VALUES(?,?,?,?)",{v.label_id,v.dataset_id,v.dataset_version,contracts::canonical_json(encode(v))},e);}
bool SQLiteCampaignStore::put_quarantine(std::string_view id,const QuarantineDecision& v,std::string* e){std::lock_guard l(mutex_);return insert_immutable(sqlite::database(db_),"INSERT INTO eval_quarantine VALUES(?,?,?,?)",{std::string(id),v.case_id,v.digest,contracts::canonical_json(encode(v))},e);}
bool SQLiteCampaignStore::put_report(const CampaignReport& v,std::string* e){std::lock_guard l(mutex_);auto*db=sqlite::database(db_);sqlite::Statement query(db,"SELECT json FROM eval_specs WHERE campaign_id=?");sqlite::bind_text(query.get(),1,v.campaign_id);auto spec=sqlite::step(query.get())==SQLITE_ROW?spec_from(sqlite::column_text(query.get(),0)):std::nullopt;if(!spec){if(e)*e="campaign spec missing";return false;}if(v.spec_digest!=campaign_spec_digest(*spec)||v.signature.signed_digest!=campaign_report_signing_digest(v)){if(e)*e="report signature or spec binding mismatch";return false;}return insert_immutable(db,"INSERT INTO eval_reports VALUES(?,?,?,?,?)",{v.campaign_id,spec->dataset_id,spec->dataset_version,v.completed_at,contracts::canonical_json(encode(v))},e);}
std::optional<CampaignSpec> SQLiteCampaignStore::load_spec(std::string_view id)const{std::lock_guard l(mutex_);auto*db=sqlite::database(db_);sqlite::Statement s(db,"SELECT json FROM eval_specs WHERE campaign_id=?");sqlite::bind_text(s.get(),1,id);return sqlite::step(s.get())==SQLITE_ROW?spec_from(sqlite::column_text(s.get(),0)):std::nullopt;}
std::optional<CampaignReport> SQLiteCampaignStore::load_report(std::string_view id)const{std::lock_guard l(mutex_);auto*db=sqlite::database(db_);sqlite::Statement s(db,"SELECT json FROM eval_reports WHERE campaign_id=?");sqlite::bind_text(s.get(),1,id);return sqlite::step(s.get())==SQLITE_ROW?report_from(sqlite::column_text(s.get(),0)):std::nullopt;}
std::vector<CampaignReport> SQLiteCampaignStore::report_history(std::string_view id,std::string_view version)const{std::lock_guard l(mutex_);std::vector<CampaignReport>out;auto*db=sqlite::database(db_);sqlite::Statement s(db,"SELECT json FROM eval_reports WHERE dataset_id=? AND dataset_version=? ORDER BY completed_at,campaign_id");sqlite::bind_text(s.get(),1,id);sqlite::bind_text(s.get(),2,version);while(sqlite::step(s.get())==SQLITE_ROW)if(auto v=report_from(sqlite::column_text(s.get(),0)))out.push_back(std::move(*v));return out;}
bool SQLiteCampaignStore::acquire_lease(const CampaignLease&v,std::int64_t now,std::string*e){std::lock_guard l(mutex_);try{auto*db=sqlite::database(db_);sqlite::Transaction tx(db);sqlite::Statement del(db,"DELETE FROM eval_leases WHERE campaign_id=? AND expires_at<=?");sqlite::bind_text(del.get(),1,v.campaign_id);sqlite::bind_int64(del.get(),2,now);if(sqlite::step(del.get())!=SQLITE_DONE)throw std::runtime_error(sqlite3_errmsg(db));sqlite::Statement ins(db,"INSERT INTO eval_leases VALUES(?,?,?)");sqlite::bind_text(ins.get(),1,v.campaign_id);sqlite::bind_text(ins.get(),2,v.owner);sqlite::bind_int64(ins.get(),3,v.expires_at_epoch);if(sqlite::step(ins.get())!=SQLITE_DONE)throw std::runtime_error("campaign already leased");tx.commit();return true;}catch(const std::exception&x){if(e)*e=x.what();return false;}}
bool SQLiteCampaignStore::release_lease(std::string_view id,std::string_view owner){std::lock_guard l(mutex_);auto*db=sqlite::database(db_);sqlite::Statement s(db,"DELETE FROM eval_leases WHERE campaign_id=? AND owner=?");sqlite::bind_text(s.get(),1,id);sqlite::bind_text(s.get(),2,owner);return sqlite::step(s.get())==SQLITE_DONE&&sqlite::changes(db)==1;}
}  // namespace agent_framework::eval
