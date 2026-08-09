/**
 * @file memory_store.cpp
 * @brief Memory store implementation
 */

#include <agent/memory/memory.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <stdexcept>

namespace agent_framework {
namespace {
json event_json(const Event& e) { return {{"v", 1}, {"timestamp", e.timestamp}, {"node_name", e.node_name}, {"event_type", e.event_type}, {"data", e.data}}; }
Event parse_event(const json& j) { return {j.value("timestamp", std::time_t{}), j.value("node_name", ""), j.value("event_type", ""), j.value("data", json::object())}; }
json summary_json(const MemorySummary& s) { return {{"v",1},{"session_id",s.session_id},{"summary",s.summary},{"keywords",s.keywords},{"embedding",s.summary_embedding},{"created_at",s.created_at},{"updated_at",s.updated_at}}; }
MemorySummary parse_summary(const json& j) { return {j.value("session_id", ""),j.value("summary", ""),j.value("keywords", std::vector<std::string>{}),j.value("embedding", Embedding{}),j.value("created_at", std::time_t{}),j.value("updated_at", std::time_t{})}; }
void append_jsonl(const std::string& path, const json& j) { std::filesystem::create_directories(std::filesystem::path(path).parent_path()); std::ofstream out(path, std::ios::app); if(!out) throw std::runtime_error("cannot open memory log"); out << j.dump() << '\n'; }
std::vector<json> read_jsonl(const std::string& path) { std::vector<json> r; std::ifstream in(path); for(std::string line; std::getline(in,line);) { try { if(!line.empty()) r.push_back(json::parse(line)); } catch(...) { break; } } return r; }
}

FileMemoryBackend::FileMemoryBackend(const std::string& data_dir) : data_dir_(data_dir) { if(data_dir.empty()) throw std::invalid_argument("memory data directory must not be empty"); std::filesystem::create_directories(data_dir_); }
std::string FileMemoryBackend::get_event_log_path(const std::string& session_id) const { return (std::filesystem::path(data_dir_) / session_id / "events.v1.jsonl").string(); }
void FileMemoryBackend::append_event_to_file(const Event& event, const std::string& path) { append_jsonl(path, event_json(event)); }
void FileMemoryBackend::store_event(const Event& event) { std::lock_guard<std::mutex> l(file_mutex_); append_event_to_file(event, get_event_log_path(event.data.value("session_id", "default"))); }
std::vector<Event> FileMemoryBackend::query_events(const std::string& session_id,const std::string& node,std::time_t begin,std::time_t end) { std::lock_guard<std::mutex> l(file_mutex_); std::vector<Event> r; for(const auto& j:read_jsonl(get_event_log_path(session_id))) { auto e=parse_event(j); if((node.empty()||e.node_name==node)&&(begin==0||e.timestamp>=begin)&&(end==0||e.timestamp<=end)) r.push_back(std::move(e)); } return r; }
void FileMemoryBackend::store_message(const Message& m) { std::lock_guard<std::mutex> l(file_mutex_); append_jsonl((std::filesystem::path(data_dir_)/"messages.v1.jsonl").string(), {{"role",m.role},{"content",m.content},{"timestamp",m.timestamp}}); }
std::vector<Message> FileMemoryBackend::get_conversation_history(const std::string&,int max) { std::lock_guard<std::mutex> l(file_mutex_); std::vector<Message> r; for(const auto& j:read_jsonl((std::filesystem::path(data_dir_)/"messages.v1.jsonl").string())) r.push_back({j.value("role", ""),j.value("content", ""),{}, {}, {},j.value("timestamp",std::time_t{})}); if(max>0&&r.size()>static_cast<size_t>(max)) r.erase(r.begin(),r.end()-max); return r; }
void FileMemoryBackend::store_memory_summary(const MemorySummary& s) { std::lock_guard<std::mutex> l(file_mutex_); append_jsonl((std::filesystem::path(data_dir_)/"summaries.v1.jsonl").string(),summary_json(s)); }
std::vector<MemorySummary> FileMemoryBackend::query_memory_summaries(const std::string& q,int top) { std::lock_guard<std::mutex> l(file_mutex_); std::vector<MemorySummary> r; for(const auto& j:read_jsonl((std::filesystem::path(data_dir_)/"summaries.v1.jsonl").string())) { auto s=parse_summary(j); if(q.empty()||s.summary.find(q)!=std::string::npos) r.push_back(std::move(s)); if(top>0&&r.size()>=static_cast<size_t>(top)) break; } return r; }
void FileMemoryBackend::cleanup_expired_data(std::time_t) {}

InMemoryBackend::InMemoryBackend() = default;
void InMemoryBackend::store_event(const Event& e) { std::lock_guard<std::mutex> l(data_mutex_); events_[e.data.value("session_id", "default")].push_back(e); }
std::vector<Event> InMemoryBackend::query_events(const std::string& s,const std::string& n,std::time_t b,std::time_t e) { std::lock_guard<std::mutex> l(data_mutex_); std::vector<Event> r; for(const auto& x:events_[s]) if((n.empty()||x.node_name==n)&&(b==0||x.timestamp>=b)&&(e==0||x.timestamp<=e)) r.push_back(x); return r; }
void InMemoryBackend::store_message(const Message& m) { std::lock_guard<std::mutex> l(data_mutex_); messages_["default"].push_back(m); }
std::vector<Message> InMemoryBackend::get_conversation_history(const std::string&,int max) { std::lock_guard<std::mutex> l(data_mutex_); auto r=messages_["default"]; if(max>0&&r.size()>static_cast<size_t>(max)) r.erase(r.begin(),r.end()-max); return r; }
void InMemoryBackend::store_memory_summary(const MemorySummary& s) { std::lock_guard<std::mutex> l(data_mutex_); summaries_.push_back(s); }
std::vector<MemorySummary> InMemoryBackend::query_memory_summaries(const std::string& q,int top) { std::lock_guard<std::mutex> l(data_mutex_); std::vector<MemorySummary> r; for(const auto& s:summaries_) if(q.empty()||s.summary.find(q)!=std::string::npos) {r.push_back(s); if(top>0&&r.size()>=static_cast<size_t>(top)) break;} return r; }
void InMemoryBackend::cleanup_expired_data(std::time_t t) { std::lock_guard<std::mutex> l(data_mutex_); summaries_.erase(std::remove_if(summaries_.begin(),summaries_.end(),[&](const auto& s){return s.updated_at<t;}),summaries_.end()); }

SQLiteMemoryBackend::SQLiteMemoryBackend(const std::string& path) : db_path_(path), db_(nullptr) { sqlite3* handle=nullptr; if(sqlite3_open(path.c_str(), &handle)!=SQLITE_OK) throw std::runtime_error("cannot open memory sqlite"); db_=handle; init_database(); }
SQLiteMemoryBackend::~SQLiteMemoryBackend(){ if(db_) sqlite3_close(static_cast<sqlite3*>(db_)); }
void SQLiteMemoryBackend::execute_sql(const std::string& sql,const std::vector<std::string>&){ char* e=nullptr; if(sqlite3_exec(static_cast<sqlite3*>(db_),sql.c_str(),nullptr,nullptr,&e)!=SQLITE_OK){std::string m=e?e:"sqlite error";sqlite3_free(e);throw std::runtime_error(m);} }
void SQLiteMemoryBackend::init_database(){ execute_sql("CREATE TABLE IF NOT EXISTS memory_events(session TEXT,node TEXT,ts INTEGER,payload TEXT); CREATE TABLE IF NOT EXISTS memory_summaries(session TEXT,summary TEXT,payload TEXT,updated INTEGER);"); }
void SQLiteMemoryBackend::store_event(const Event&e){std::lock_guard<std::mutex>l(db_mutex_);auto*d=static_cast<sqlite3*>(db_);sqlite3_stmt*s=nullptr;sqlite3_prepare_v2(d,"INSERT INTO memory_events VALUES(?,?,?,?)",-1,&s,nullptr);auto p=event_json(e).dump();auto sid=e.data.value("session_id","default");sqlite3_bind_text(s,1,sid.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,e.node_name.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,3,e.timestamp);sqlite3_bind_text(s,4,p.c_str(),-1,SQLITE_TRANSIENT);sqlite3_step(s);sqlite3_finalize(s);}
std::vector<Event> SQLiteMemoryBackend::query_events(const std::string&sid,const std::string&,std::time_t,std::time_t){std::lock_guard<std::mutex>l(db_mutex_);std::vector<Event>r;sqlite3_stmt*s=nullptr;sqlite3_prepare_v2(static_cast<sqlite3*>(db_),"SELECT payload FROM memory_events WHERE session=? ORDER BY ts",-1,&s,nullptr);sqlite3_bind_text(s,1,sid.c_str(),-1,SQLITE_TRANSIENT);while(sqlite3_step(s)==SQLITE_ROW)r.push_back(parse_event(json::parse(reinterpret_cast<const char*>(sqlite3_column_text(s,0)))));sqlite3_finalize(s);return r;}
void SQLiteMemoryBackend::store_message(const Message&m){(void)m;} std::vector<Message> SQLiteMemoryBackend::get_conversation_history(const std::string&,int){return{};}
void SQLiteMemoryBackend::store_memory_summary(const MemorySummary&x){std::lock_guard<std::mutex>l(db_mutex_);auto*d=static_cast<sqlite3*>(db_);sqlite3_stmt*s=nullptr;sqlite3_prepare_v2(d,"INSERT INTO memory_summaries VALUES(?,?,?,?)",-1,&s,nullptr);auto p=summary_json(x).dump();sqlite3_bind_text(s,1,x.session_id.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,2,x.summary.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_text(s,3,p.c_str(),-1,SQLITE_TRANSIENT);sqlite3_bind_int64(s,4,x.updated_at);sqlite3_step(s);sqlite3_finalize(s);}
std::vector<MemorySummary> SQLiteMemoryBackend::query_memory_summaries(const std::string&q,int top){std::lock_guard<std::mutex>l(db_mutex_);std::vector<MemorySummary>r;sqlite3_stmt*s=nullptr;sqlite3_prepare_v2(static_cast<sqlite3*>(db_),"SELECT payload FROM memory_summaries WHERE summary LIKE ? ORDER BY updated DESC",-1,&s,nullptr);auto like="%"+q+"%";sqlite3_bind_text(s,1,like.c_str(),-1,SQLITE_TRANSIENT);while(sqlite3_step(s)==SQLITE_ROW&&(top<=0||r.size()<static_cast<size_t>(top)))r.push_back(parse_summary(json::parse(reinterpret_cast<const char*>(sqlite3_column_text(s,0)))));sqlite3_finalize(s);return r;}
void SQLiteMemoryBackend::cleanup_expired_data(std::time_t t){std::lock_guard<std::mutex>l(db_mutex_);sqlite3_stmt*s=nullptr;sqlite3_prepare_v2(static_cast<sqlite3*>(db_),"DELETE FROM memory_summaries WHERE updated<?",-1,&s,nullptr);sqlite3_bind_int64(s,1,t);sqlite3_step(s);sqlite3_finalize(s);}

MemoryStore::MemoryStore(std::unique_ptr<MemoryBackend> b):backend_(std::move(b)){if(!backend_)throw std::invalid_argument("memory backend required");}
void MemoryStore::store_event(const Event& e){std::lock_guard<std::mutex>l(backend_mutex_);backend_->store_event(e);} std::vector<Message> MemoryStore::get_conversation_history(const std::string& s,int n){std::lock_guard<std::mutex>l(backend_mutex_);return backend_->get_conversation_history(s,n);} std::vector<Event> MemoryStore::get_short_term_memory(const std::string&s){std::lock_guard<std::mutex>l(backend_mutex_);return backend_->query_events(s);} void MemoryStore::store_long_term_memory(const std::string& s,const MemorySummary& x){auto c=x;c.session_id=s;std::lock_guard<std::mutex>l(backend_mutex_);backend_->store_memory_summary(c);} std::vector<MemorySummary> MemoryStore::query_long_term_memory(const std::string&q,int n){std::lock_guard<std::mutex>l(backend_mutex_);return backend_->query_memory_summaries(q,n);} void MemoryStore::switch_backend(std::unique_ptr<MemoryBackend>b){if(!b)throw std::invalid_argument("memory backend required");std::lock_guard<std::mutex>l(backend_mutex_);backend_=std::move(b);}

} // namespace agent_framework
