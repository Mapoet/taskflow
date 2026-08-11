#include "agent/telemetry/durable_spool.hpp"

#include <filesystem>
#include <stdexcept>

#include <sqlite3.h>

#include "agent/internal/sqlite_utils.hpp"

namespace agent_framework::telemetry {
namespace { namespace sqlite=internal::sqlite;
nlohmann::json span_json(const SpanRecord&s){return{{"context",encode(s.context)},{"name",s.name},{"started_at",s.started_at},{"finished_at",s.finished_at},{"status",s.status},{"attributes",s.attributes}};}
SpanRecord span_value(const nlohmann::json&v){SpanRecord s;auto c=decode_correlation_context(v.at("context"));if(!c)throw std::runtime_error("invalid spooled span");s.context=std::move(*c);s.name=v.at("name");s.started_at=v.at("started_at");s.finished_at=v.at("finished_at");s.status=v.at("status");s.attributes=v.at("attributes").get<std::map<std::string,std::string>>();return s;}
}
SQLiteTelemetrySpool::SQLiteTelemetrySpool(std::string path,std::shared_ptr<TelemetrySink> downstream):downstream_(std::move(downstream)){
    if(path.empty()||!downstream_)throw std::invalid_argument("spool path and downstream are required");std::error_code ec;const std::filesystem::path file(path);if(file.has_parent_path())std::filesystem::create_directories(file.parent_path(),ec);sqlite3*opened=nullptr;if(ec||sqlite3_open_v2(path.c_str(),&opened,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_FULLMUTEX,nullptr)!=SQLITE_OK){const auto message=ec?ec.message():(opened?sqlite3_errmsg(opened):"sqlite open failed");if(opened)sqlite3_close(opened);throw std::runtime_error(message);}db_=opened;sqlite3_busy_timeout(opened,3000);sqlite::exec(opened,"PRAGMA journal_mode=WAL");sqlite::exec(opened,"PRAGMA synchronous=FULL");sqlite::exec(opened,"CREATE TABLE IF NOT EXISTS telemetry_spool(sequence INTEGER PRIMARY KEY AUTOINCREMENT,kind TEXT NOT NULL,document_json TEXT NOT NULL)");}
SQLiteTelemetrySpool::~SQLiteTelemetrySpool(){if(db_)sqlite3_close(sqlite::database(db_));}
bool SQLiteTelemetrySpool::append(std::string_view kind,const nlohmann::json&document){auto*db=sqlite::database(db_);sqlite::Statement statement(db,"INSERT INTO telemetry_spool(kind,document_json) VALUES(?,?)");sqlite::bind_text(statement.get(),1,kind);sqlite::bind_text(statement.get(),2,document.dump());return sqlite::step(statement.get())==SQLITE_DONE;}
bool SQLiteTelemetrySpool::export_span(const SpanRecord&span){std::lock_guard lock(mutex_);return append("span",span_json(span));}
bool SQLiteTelemetrySpool::export_metric(const MetricResult&metric){std::lock_guard lock(mutex_);return append("metric",encode(metric));}
bool SQLiteTelemetrySpool::flush(){std::lock_guard lock(mutex_);auto*db=sqlite::database(db_);sqlite::Statement query(db,"SELECT sequence,kind,document_json FROM telemetry_spool ORDER BY sequence");std::vector<std::int64_t> delivered;while(sqlite::step(query.get())==SQLITE_ROW){const auto sequence=sqlite::column_int64(query.get(),0);const auto kind=sqlite::column_text(query.get(),1);const auto document=nlohmann::json::parse(sqlite::column_text(query.get(),2));bool ok=false;if(kind=="span")ok=downstream_->export_span(span_value(document));else if(kind=="metric"){auto metric=decode_metric_result(document);ok=metric&&downstream_->export_metric(*metric);}if(!ok)return false;delivered.push_back(sequence);}if(!downstream_->flush())return false;if(delivered.empty())return true;sqlite::Transaction transaction(db);sqlite::Statement erase(db,"DELETE FROM telemetry_spool WHERE sequence=?");for(const auto sequence:delivered){sqlite::bind_int64(erase.get(),1,sequence);if(sqlite::step(erase.get())!=SQLITE_DONE)return false;sqlite::reset(erase.get());sqlite::clear_bindings(erase.get());}transaction.commit();return true;}
std::size_t SQLiteTelemetrySpool::pending()const{std::lock_guard lock(mutex_);auto*db=sqlite::database(db_);sqlite::Statement query(db,"SELECT COUNT(*) FROM telemetry_spool");return sqlite::step(query.get())==SQLITE_ROW?static_cast<std::size_t>(sqlite::column_int64(query.get(),0)):0;}
}  // namespace agent_framework::telemetry
