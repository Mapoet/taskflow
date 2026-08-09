#include <agent/memory/memory.hpp>
#include <cassert>
#include <filesystem>
#include <iostream>
using namespace agent_framework;
int main() {
  const auto dir=(std::filesystem::temp_directory_path()/"agent-memory-wp34").string();
  std::filesystem::remove_all(dir);
  { MemoryStore s(std::make_unique<FileMemoryBackend>(dir)); Event e{1,"node","input",{{"session_id","s"},{"v",1}}}; s.store_event(e); MemorySummary x{"s","GNSS memory",{}, {},1,1}; s.store_long_term_memory("s",x); assert(s.get_short_term_memory("s").size()==1); assert(s.query_long_term_memory("GNSS").size()==1); }
  { MemoryStore s(std::make_unique<FileMemoryBackend>(dir)); assert(s.get_short_term_memory("s").size()==1); assert(s.query_long_term_memory("GNSS").size()==1); }
  const auto db=dir+".sqlite"; std::filesystem::remove(db);
  { MemoryStore s(std::make_unique<SQLiteMemoryBackend>(db)); Event e{1,"node","input",{{"session_id","s"}}}; Event out{2,"other","output",{{"session_id","s"}}}; s.store_event(e); s.store_event(out); MemorySummary x{"s","SQLite GNSS",{}, {},1,1}; s.store_long_term_memory("s",x); assert(s.get_short_term_memory("s").size()==2); assert(s.query_long_term_memory("GNSS").size()==1); }
  { SQLiteMemoryBackend sqlite(db); assert(sqlite.query_events("s", "node", 1, 1).size()==1); Message m{"tool","result","call-1","lookup",json{{"ok",true}},3}; sqlite.store_message(m); auto history=sqlite.get_conversation_history("default", 1); assert(history.size()==1 && history[0].tool_name == "lookup" && history[0].tool_result->at("ok") == true); }
  std::filesystem::remove_all(dir); std::cout<<"test_memory_store_wp34: ok\n";
}
