#include "agent/execution/artifact_executor.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>
#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"
#include "agent/skills/skill_supply_chain.hpp"

namespace fs = std::filesystem;
namespace agent_framework::execution {
namespace {
namespace sqlite = internal::sqlite;
std::string digest(std::string_view bytes) {
    return "sha256:" + skill_sha256_bytes(std::string(bytes)).value_or("unavailable");
}
std::optional<std::string> read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if(!in) return std::nullopt;
    std::ostringstream out; out << in.rdbuf(); return out.str();
}
bool below(const fs::path& root, const fs::path& candidate) {
    auto r = root.begin(), c = candidate.begin();
    for(; r != root.end(); ++r, ++c) if(c == candidate.end() || *r != *c) return false;
    return true;
}
fs::path jailed(const fs::path& root, const fs::path& relative, std::string* error) {
    if(relative.empty() || relative.is_absolute()) {
        if(error) *error = "artifact path must be non-empty and relative";
        return {};
    }
    std::error_code ec;
    const auto parent = fs::weakly_canonical(root / relative.parent_path(), ec);
    if(ec || !below(root, parent)) {
        if(error) *error = "artifact path escapes workspace";
        return {};
    }
    const auto target = parent / relative.filename();
    if(fs::is_symlink(fs::symlink_status(target, ec))) {
        if(error) *error = "symbolic-link artifact targets are forbidden";
        return {};
    }
    return target;
}
nlohmann::json encode_manifest(const ArtifactManifest& value) {
    auto artifacts = nlohmann::json::array();
    for(const auto& item : value.artifacts) artifacts.push_back({
        {"relative_path", item.relative_path}, {"content_digest", item.content_digest},
        {"size", item.size}, {"producer_action_id", item.producer_action_id}});
    return {{"run_id", value.run_id}, {"parent_manifest_digest", value.parent_manifest_digest},
            {"artifacts", artifacts}, {"workspace_diff_digest", value.workspace_diff_digest},
            {"manifest_digest", value.manifest_digest}};
}
ArtifactManifest decode_manifest(const nlohmann::json& value) {
    ArtifactManifest out;
    out.run_id = value.at("run_id").get<std::string>();
    out.parent_manifest_digest = value.at("parent_manifest_digest").get<std::string>();
    out.workspace_diff_digest = value.at("workspace_diff_digest").get<std::string>();
    out.manifest_digest = value.at("manifest_digest").get<std::string>();
    for(const auto& item : value.at("artifacts")) out.artifacts.push_back({
        item.at("relative_path").get<std::string>(), item.at("content_digest").get<std::string>(),
        item.at("size").get<std::uint64_t>(), item.at("producer_action_id").get<std::string>()});
    return out;
}
nlohmann::json encode_entry(const ArtifactJournalEntry& value) {
    const auto& r = value.receipt;
    nlohmann::json out = {{"succeeded", r.succeeded}, {"action_id", r.action_id},
            {"relative_path", r.relative_path}, {"idempotency_key", r.idempotency_key},
            {"effect_digest", r.effect_digest}, {"preimage_digest", r.preimage_digest},
            {"manifest", encode_manifest(r.manifest)}, {"error_code", r.error_code},
            {"error_message", r.error_message}};
    out["preimage"] = value.preimage ? nlohmann::json(*value.preimage) : nlohmann::json(nullptr);
    return out;
}
ArtifactJournalEntry decode_entry(const nlohmann::json& value) {
    ArtifactJournalEntry out;
    auto& r = out.receipt;
    r.succeeded = value.at("succeeded").get<bool>();
    r.action_id = value.at("action_id").get<std::string>();
    r.relative_path = value.at("relative_path").get<std::string>();
    r.idempotency_key = value.at("idempotency_key").get<std::string>();
    r.effect_digest = value.at("effect_digest").get<std::string>();
    r.preimage_digest = value.at("preimage_digest").get<std::string>();
    r.manifest = decode_manifest(value.at("manifest"));
    r.error_code = value.at("error_code").get<std::string>();
    r.error_message = value.at("error_message").get<std::string>();
    if(!value.at("preimage").is_null()) out.preimage = value.at("preimage").get<std::string>();
    return out;
}
}

std::optional<ArtifactJournalEntry> InMemoryArtifactJournal::load(std::string_view key) {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(std::string(key));
    return found == entries_.end() ? std::nullopt : std::optional(found->second);
}
bool InMemoryArtifactJournal::put_if_absent(const ArtifactJournalEntry& entry, std::string* error) {
    std::lock_guard lock(mutex_);
    if(entry.receipt.idempotency_key.empty()) { if(error) *error = "empty idempotency key"; return false; }
    return entries_.emplace(entry.receipt.idempotency_key, entry).second;
}

SQLiteArtifactJournal::SQLiteArtifactJournal(std::string path, int timeout) {
    if(path.empty()) throw std::invalid_argument("artifact journal path must not be empty");
    const fs::path file(path); std::error_code ec;
    if(file.has_parent_path()) fs::create_directories(file.parent_path(), ec);
    sqlite3* opened = nullptr;
    if(ec || sqlite3_open_v2(path.c_str(), &opened, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const auto message = ec ? ec.message() : (opened ? sqlite3_errmsg(opened) : "sqlite open failed");
        if(opened) sqlite3_close(opened);
        throw std::runtime_error(message);
    }
    db_ = opened; sqlite3_busy_timeout(opened, timeout);
    sqlite::exec(opened, "PRAGMA journal_mode=WAL");
    sqlite::exec(opened, "PRAGMA synchronous=FULL");
    sqlite::exec(opened, "CREATE TABLE IF NOT EXISTS phase4_artifact_journal("
                         "idempotency_key TEXT PRIMARY KEY,entry_json TEXT NOT NULL,"
                         "entry_digest TEXT NOT NULL,created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)");
#if !defined(_WIN32)
    fs::permissions(file, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, ec);
    if(ec) throw std::runtime_error(ec.message());
#endif
}
SQLiteArtifactJournal::~SQLiteArtifactJournal() { if(db_) sqlite3_close(sqlite::database(db_)); }
std::optional<ArtifactJournalEntry> SQLiteArtifactJournal::load(std::string_view key) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    sqlite::Statement query(db, "SELECT entry_json,entry_digest FROM phase4_artifact_journal WHERE idempotency_key=?");
    sqlite::bind_text(query.get(), 1, key); const int rc = sqlite3_step(query.get());
    if(rc == SQLITE_DONE) return std::nullopt;
    if(rc != SQLITE_ROW) throw std::runtime_error(sqlite3_errmsg(db));
    const auto document = sqlite::column_text(query.get(), 0);
    if(digest(document) != sqlite::column_text(query.get(), 1)) throw std::runtime_error("artifact journal digest mismatch");
    return decode_entry(nlohmann::json::parse(document));
}
bool SQLiteArtifactJournal::put_if_absent(const ArtifactJournalEntry& entry, std::string* error) {
    std::lock_guard lock(mutex_); auto* db = sqlite::database(db_);
    try {
        const auto document = encode_entry(entry).dump();
        sqlite::Statement insert(db, "INSERT INTO phase4_artifact_journal(idempotency_key,entry_json,entry_digest) VALUES(?,?,?)");
        sqlite::bind_text(insert.get(), 1, entry.receipt.idempotency_key);
        sqlite::bind_text(insert.get(), 2, document); sqlite::bind_text(insert.get(), 3, digest(document));
        const int rc = sqlite3_step(insert.get());
        if(rc == SQLITE_CONSTRAINT) return false;
        if(rc != SQLITE_DONE) { if(error) *error = sqlite3_errmsg(db); return false; }
        return true;
    } catch(const std::exception& e) { if(error) *error = e.what(); return false; }
}

WorkspaceArtifactExecutor::WorkspaceArtifactExecutor(fs::path root, ArtifactJournal* journal) {
    fs::create_directories(root);
    root_ = fs::weakly_canonical(root);
    journal_ = journal ? journal : &local_journal_;
}

fs::path WorkspaceArtifactExecutor::resolve(const fs::path& relative, std::string* error) const {
    return jailed(root_, relative, error);
}

ArtifactManifest WorkspaceArtifactExecutor::scan(std::string run_id, std::string parent,
                                                  std::string producer) const {
    ArtifactManifest result;
    result.run_id = std::move(run_id);
    result.parent_manifest_digest = std::move(parent);
    std::vector<fs::path> paths;
    for(const auto& item : fs::recursive_directory_iterator(root_))
        if(!item.is_symlink() && item.is_regular_file()) paths.push_back(fs::relative(item.path(), root_));
    std::sort(paths.begin(), paths.end());
    nlohmann::json canonical = nlohmann::json::array();
    for(const auto& relative : paths) {
        auto bytes = read(root_ / relative).value_or("");
        ArtifactEntry entry{relative.generic_string(), digest(bytes), bytes.size(), producer};
        result.artifacts.push_back(entry);
        canonical.push_back({{"path", entry.relative_path}, {"digest", entry.content_digest},
                             {"size", entry.size}});
    }
    result.workspace_diff_digest = digest(canonical.dump());
    result.manifest_digest = digest(nlohmann::json{{"run_id", result.run_id},
        {"parent", result.parent_manifest_digest}, {"artifacts", canonical},
        {"workspace_diff", result.workspace_diff_digest}}.dump());
    return result;
}

ArtifactExecutionReceipt WorkspaceArtifactExecutor::execute(
    std::string run_id, const ArtifactAction& action, const ArtifactManifest* parent) {
    ArtifactExecutionReceipt out;
    out.action_id = action.action_id; out.relative_path = action.relative_path.generic_string();
    out.idempotency_key = action.idempotency_key;
    if(!action.approved) { out.error_code = "action_not_approved"; return out; }
    if(action.action_id.empty() || action.idempotency_key.empty()) {
        out.error_code = "invalid_action_identity"; return out;
    }
    if(auto found = journal_->load(action.idempotency_key)) {
        out = found->receipt; out.replayed = true; return out;
    }
    std::string error;
    auto target = resolve(action.relative_path, &error);
    if(target.empty()) { out.error_code = "workspace_escape"; out.error_message = error; return out; }
    auto preimage = read(target);
    out.preimage_digest = preimage ? digest(*preimage) : digest("absent");
    fs::create_directories(target.parent_path());
    auto temporary = target; temporary += ".phase4-tmp";
    { std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
      if(!file || !(file << action.content)) { out.error_code = "artifact_write_failed"; return out; } }
    std::error_code ec; fs::rename(temporary, target, ec);
    if(ec) { fs::remove(temporary); out.error_code = "artifact_commit_failed"; out.error_message = ec.message(); return out; }
    out.succeeded = true;
    out.effect_digest = digest(action.action_id + "\n" + action.relative_path.generic_string() + "\n" + action.content);
    out.manifest = scan(std::move(run_id), parent ? parent->manifest_digest : "", action.action_id);
    if(parent) {
        for(auto& entry : out.manifest.artifacts) {
            if(entry.relative_path == action.relative_path.generic_string()) continue;
            const auto previous = std::find_if(parent->artifacts.begin(), parent->artifacts.end(),
                [&](const auto& item) { return item.relative_path == entry.relative_path &&
                                               item.content_digest == entry.content_digest; });
            if(previous != parent->artifacts.end()) entry.producer_action_id = previous->producer_action_id;
        }
    }
    std::string journal_error;
    if(!journal_->put_if_absent(ArtifactJournalEntry{out, preimage}, &journal_error)) {
        auto winner = journal_->load(action.idempotency_key);
        if(winner) { out = winner->receipt; out.replayed = true; }
        else { out.succeeded = false; out.error_code = "artifact_journal_failed"; out.error_message = journal_error; }
    }
    return out;
}

bool WorkspaceArtifactExecutor::rollback(const ArtifactExecutionReceipt& receipt, std::string* error) {
    auto found = journal_->load(receipt.idempotency_key);
    if(!found) { if(error) *error = "receipt not found"; return false; }
    auto target = resolve(found->receipt.relative_path, error); if(target.empty()) return false;
    if(found->preimage) { std::ofstream file(target, std::ios::binary | std::ios::trunc); file << *found->preimage; return !!file; }
    std::error_code ec; fs::remove(target, ec); return !ec;
}

FilesystemArtifactOracle::FilesystemArtifactOracle(fs::path root)
    : root_(fs::weakly_canonical(std::move(root))) {}

std::vector<OracleObservation> FilesystemArtifactOracle::verify(
    const ArtifactManifest& manifest, const std::vector<ArtifactRequirement>& requirements) const {
    std::vector<OracleObservation> out;
    for(const auto& requirement : requirements) {
        OracleObservation item;
        item.oracle_id = "filesystem-v1";
        item.requirement_id = requirement.requirement_id;
        item.artifact_manifest_digest = manifest.manifest_digest;
        std::string jail_error;
        auto path = jailed(root_, requirement.relative_path, &jail_error);
        auto bytes = path.empty() ? std::optional<std::string>{} : read(path);
        if(path.empty()) item.detail = jail_error;
        else if(!bytes) item.detail = "required artifact is missing";
        else {
            item.observed_digest = digest(*bytes);
            const auto listed = std::find_if(manifest.artifacts.begin(), manifest.artifacts.end(),
                [&](const auto& e) { return e.relative_path == requirement.relative_path.generic_string() &&
                                             e.content_digest == item.observed_digest; });
            if(listed == manifest.artifacts.end()) item.detail = "artifact is absent from or differs from manifest";
            else if(requirement.nonempty && bytes->empty()) item.detail = "artifact is empty";
            else if(requirement.expected_digest && *requirement.expected_digest != item.observed_digest)
                item.detail = "artifact digest does not match requirement";
            else item.passed = true;
        }
        if(!item.passed) item.finding_id = "artifact:" + requirement.requirement_id;
        out.push_back(std::move(item));
    }
    return out;
}

bool FilesystemArtifactOracle::reusable(const OracleObservation& observation,
                                        std::string_view current_manifest_digest) {
    return observation.passed && observation.artifact_manifest_digest == current_manifest_digest;
}
}  // namespace agent_framework::execution
