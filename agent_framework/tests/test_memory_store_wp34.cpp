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
  const auto db=dir+".sqlite";
  { MemoryStore s(std::make_unique<SQLiteMemoryBackend>(db)); Event e{1,"node","input",{{"session_id","s"}}}; s.store_event(e); MemorySummary x{"s","SQLite GNSS",{}, {},1,1}; s.store_long_term_memory("s",x); assert(s.get_short_term_memory("s").size()==1); assert(s.query_long_term_memory("GNSS").size()==1); }
  std::filesystem::remove_all(dir); std::cout<<"test_memory_store_wp34: ok\n";
}
