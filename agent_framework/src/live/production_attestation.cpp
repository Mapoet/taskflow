#include "agent/live/production_attestation.hpp"

#include <algorithm>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::live {
namespace sql = internal::sqlite;

namespace {
std::string packed(const std::vector<std::string>& value) { return nlohmann::json(value).dump(); }
std::vector<std::string> unpacked(std::string_view value) {
    try { return nlohmann::json::parse(value).get<std::vector<std::string>>(); }
    catch(...) { throw std::runtime_error("attestation digest list is invalid"); }
}
bool same_set(std::vector<std::string> left, std::vector<std::string> right) {
    std::sort(left.begin(), left.end());
    std::sort(right.begin(), right.end());
    return left == right;
}
void fail(std::string* error, std::string message) { if(error) *error = std::move(message); }
}

SQLiteProductionAttestationStore::SQLiteProductionAttestationStore(std::string path,
                                                                    int busy_timeout_ms) {
    sqlite3* database = nullptr;
    if(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                       SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = database ? sqlite3_errmsg(database) : "sqlite open failed";
        if(database) sqlite3_close(database);
        throw std::runtime_error(message);
    }
    db_ = database;
    sqlite3_busy_timeout(database, busy_timeout_ms);
    sql::exec(database, "PRAGMA journal_mode=WAL;PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS agent_production_attestation("
        "tenant_id TEXT NOT NULL,attestation_id TEXT NOT NULL,environment_digest TEXT NOT NULL,"
        "matrix_digest TEXT NOT NULL,cell_id TEXT NOT NULL,spec_digest TEXT NOT NULL,"
        "invocation_id TEXT NOT NULL,invocation_manifest_digest TEXT NOT NULL,result_digest TEXT NOT NULL,"
        "evidence_digests TEXT NOT NULL,oracle_digests TEXT NOT NULL,trace_id TEXT NOT NULL,"
        "source TEXT NOT NULL,source_signature_digest TEXT NOT NULL,recorded_at TEXT NOT NULL,"
        "PRIMARY KEY(tenant_id,invocation_id),UNIQUE(tenant_id,attestation_id));");
}

SQLiteProductionAttestationStore::~SQLiteProductionAttestationStore() {
    if(db_) sqlite3_close(sql::database(db_));
}

bool SQLiteProductionAttestationStore::append(const ProductionCellAttestation& value,
                                               std::string* error) {
    std::lock_guard lock(mutex_);
    if(value.tenant_id.empty() || value.attestation_id.empty() || value.environment_digest.empty() ||
       value.matrix_digest.empty() || value.cell_id.empty() || value.spec_digest.empty() ||
       value.invocation_id.empty() || value.invocation_manifest_digest.empty() ||
       value.result_digest.empty() || value.evidence_digests.empty() || value.trace_id.empty() ||
       value.source.empty() || value.source_signature_digest.empty() || value.recorded_at.empty()) {
        fail(error, "production attestation is incomplete");
        return false;
    }
    try {
        sql::Statement statement(sql::database(db_),
            "INSERT INTO agent_production_attestation VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
        const std::vector<std::string> values = {value.tenant_id, value.attestation_id,
            value.environment_digest, value.matrix_digest, value.cell_id, value.spec_digest,
            value.invocation_id, value.invocation_manifest_digest, value.result_digest,
            packed(value.evidence_digests), packed(value.oracle_digests), value.trace_id,
            value.source, value.source_signature_digest, value.recorded_at};
        for(std::size_t index = 0; index < values.size(); ++index)
            sql::bind_text(statement.get(), static_cast<int>(index + 1), values[index]);
        if(sql::step(statement.get()) != SQLITE_DONE) {
            fail(error, sqlite3_errmsg(sql::database(db_)));
            return false;
        }
        return true;
    } catch(const std::exception& e) { fail(error, e.what()); return false; }
}

std::optional<ProductionCellAttestation> SQLiteProductionAttestationStore::find(
    std::string_view tenant_id, std::string_view invocation_id, std::string* error) {
    std::lock_guard lock(mutex_);
    try {
        sql::Statement statement(sql::database(db_),
            "SELECT attestation_id,environment_digest,matrix_digest,cell_id,spec_digest,"
            "invocation_manifest_digest,result_digest,evidence_digests,oracle_digests,trace_id,"
            "source,source_signature_digest,recorded_at FROM agent_production_attestation "
            "WHERE tenant_id=? AND invocation_id=?");
        sql::bind_text(statement.get(), 1, tenant_id);
        sql::bind_text(statement.get(), 2, invocation_id);
        if(sql::step(statement.get()) != SQLITE_ROW) return std::nullopt;
        ProductionCellAttestation out;
        out.tenant_id = std::string(tenant_id); out.invocation_id = std::string(invocation_id);
        out.attestation_id = sql::column_text(statement.get(), 0);
        out.environment_digest = sql::column_text(statement.get(), 1);
        out.matrix_digest = sql::column_text(statement.get(), 2);
        out.cell_id = sql::column_text(statement.get(), 3);
        out.spec_digest = sql::column_text(statement.get(), 4);
        out.invocation_manifest_digest = sql::column_text(statement.get(), 5);
        out.result_digest = sql::column_text(statement.get(), 6);
        out.evidence_digests = unpacked(sql::column_text(statement.get(), 7));
        out.oracle_digests = unpacked(sql::column_text(statement.get(), 8));
        out.trace_id = sql::column_text(statement.get(), 9);
        out.source = sql::column_text(statement.get(), 10);
        out.source_signature_digest = sql::column_text(statement.get(), 11);
        out.recorded_at = sql::column_text(statement.get(), 12);
        return out;
    } catch(const std::exception& e) { fail(error, e.what()); return std::nullopt; }
}

std::string production_cell_result_digest(const LiveCellResult& result) {
    const auto digest = contracts::embedded_digest(encode(result));
    if(!digest) throw std::runtime_error("unable to digest live cell result");
    return *digest;
}

StoreBackedCellEvidenceVerifier::StoreBackedCellEvidenceVerifier(
    ProductionAttestationStore& store, std::string expected_matrix_digest)
    : store_(store), expected_matrix_digest_(std::move(expected_matrix_digest)) {}

bool StoreBackedCellEvidenceVerifier::verify(const LiveEnvironmentProfile& environment,
    const LiveCellSpec& spec, const LiveCellResult& result, std::string* error) {
    if(!result.executed || result.invocation_id.empty()) { fail(error, "cell was not executed"); return false; }
    auto attestation = store_.find(environment.metadata.identity.tenant_id, result.invocation_id, error);
    if(!attestation) { if(error && error->empty()) *error = "attestation missing"; return false; }
    if(attestation->environment_digest != role_environment_digest(environment) ||
       attestation->matrix_digest != expected_matrix_digest_ ||
       attestation->cell_id != spec.cell_id ||
       attestation->spec_digest != live_cell_spec_digest(spec) ||
       attestation->invocation_manifest_digest != result.invocation_manifest_digest ||
       attestation->result_digest != production_cell_result_digest(result) ||
       !same_set(attestation->evidence_digests, result.evidence_digests) ||
       !same_set(attestation->oracle_digests, result.oracle_digests)) {
        fail(error, "attestation binding mismatch");
        return false;
    }
    if(spec.strong_oracle_required && attestation->oracle_digests.empty()) {
        fail(error, "strong oracle attestation missing");
        return false;
    }
    return true;
}

}  // namespace agent_framework::live
