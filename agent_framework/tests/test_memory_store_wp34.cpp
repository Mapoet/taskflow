#include <agent/memory/memory.hpp>

#include <filesystem>
#include <fstream>
#include <atomic>
#include <stdexcept>
#include <thread>
#include <sqlite3.h>

using namespace agent_framework;
namespace fs = std::filesystem;

namespace {
void require(bool condition, const char* message) {
    if(!condition) throw std::runtime_error(message);
}

std::string read_text(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string current_generation(const fs::path& session) {
    std::ifstream input(session / "CURRENT");
    std::string value;
    std::getline(input, value);
    return value;
}

void require_private_permissions(const fs::path& path, bool directory) {
    const auto permissions = fs::status(path).permissions();
    using P = fs::perms;
    require((permissions & P::owner_read) != P::none, "owner cannot read memory artifact");
    if(directory) require((permissions & P::owner_exec) != P::none, "owner cannot traverse memory directory");
    require((permissions & (P::group_all | P::others_all)) == P::none,
            "memory artifact accessible outside owner");
}
}

int main() {
    const auto root = fs::temp_directory_path() / "agent-memory-wp34-v2";
    std::error_code ignored;
    fs::remove_all(root, ignored);

    const auto session = root / "tenant-a" / "agent-a" / "s1";
    {
        MemoryStore store(std::make_unique<FileMemoryBackend>(
            root.string(), "tenant-a", "agent-a"));
        Event event{1, "node", "input",
                    {{"session_id", "s1"}, {"Authorization", "Bearer top-secret"}}};
        store.store_event(event);
        Message first{"user", "credential Bearer message-secret", {}, {}, {}, 2};
        store.store_message("s1", first);
        store.store_message("s2", Message{"user", "other session", {}, {}, {}, 3});
        MemorySummary summary{"s1", "GNSS memory Bearer summary-secret", {}, {}, 1, 4};
        store.store_long_term_memory("s1", summary);

        require(store.get_short_term_memory("s1").size() == 1, "file event missing");
        require(store.get_short_term_memory("s2").empty(), "events crossed sessions");
        require(store.get_conversation_history("s1").size() == 1, "file message missing");
        require(store.get_conversation_history("s2").size() == 1, "second session missing");
        require(store.query_long_term_memory("GNSS").size() == 1, "file summary missing");
    }

    require(fs::exists(session / "CURRENT"), "CURRENT was not published");
    const auto first_current = current_generation(session);
    const auto generation = session / "generations" / first_current;
    require_private_permissions(session, true);
    require_private_permissions(generation / "manifest.json", false);
    const auto persisted = read_text(generation / "events.jsonl") +
                           read_text(generation / "messages.jsonl") +
                           read_text(generation / "summaries.jsonl");
    require(persisted.find("top-secret") == std::string::npos &&
            persisted.find("message-secret") == std::string::npos &&
            persisted.find("summary-secret") == std::string::npos,
            "secret was persisted without redaction");

    // Digest mismatch in CURRENT must fall back to the preceding committed generation.
    {
        std::ofstream corrupt(generation / "events.jsonl", std::ios::app);
        corrupt << "corrupt-tail\n";
    }
    {
        FileMemoryBackend recovered(root.string(), "tenant-a", "agent-a");
        require(recovered.query_events("s1").size() == 1,
                "corrupt generation did not fall back");
        recovered.store_message("s1", Message{"assistant", "after recovery", {}, {}, {}, 5});
    }
    require(current_generation(session) != first_current,
            "recovery commit reused corrupt generation revision");
    require(fs::exists(session / "quarantine" / first_current),
            "corrupt committed generation was not quarantined");
    require(read_text(session / "audit.jsonl").find("generation_quarantined") != std::string::npos,
            "quarantine audit record missing");

    // Invalid CURRENT and abandoned temporary generations are ignored.
    fs::create_directories(session / "generations" / ".tmp-abandoned");
    {
        std::ofstream current(session / "CURRENT", std::ios::trunc);
        current << "../escape\n";
    }
    {
        FileMemoryBackend recovered(root.string(), "tenant-a", "agent-a");
        require(recovered.query_events("s1").size() == 1,
                "invalid CURRENT prevented historical recovery");
    }

    // Tenant isolation and path traversal rejection.
    {
        FileMemoryBackend isolated(root.string(), "tenant-b", "agent-a");
        require(isolated.query_events("s1").empty(), "events crossed tenants");
    }
    bool rejected = false;
    try {
        FileMemoryBackend backend(root.string(), "tenant-a", "agent-a");
        backend.store_message("../escape", Message{});
    } catch(const std::invalid_argument&) { rejected = true; }
    require(rejected, "path traversal session accepted");

    // A failed generation must never advance CURRENT or contaminate the committed snapshot.
    const auto before_fault = current_generation(session);
    bool injected = false;
    try {
        FileMemoryBackend faulty(root.string(), "tenant-a", "agent-a",
            [](std::string_view stage) {
                if(stage == "after_manifest") throw std::runtime_error("injected crash");
            });
        faulty.store_message("s1", Message{"assistant", "must-not-commit", {}, {}, {}, 6});
    } catch(const std::runtime_error&) { injected = true; }
    require(injected, "memory fault injector did not run");
    require(current_generation(session) == before_fault,
            "failed generation advanced CURRENT");
    {
        FileMemoryBackend recovered(root.string(), "tenant-a", "agent-a");
        const auto history = recovered.get_conversation_history("s1", 20);
        require(history.size() == 2 && history.back().content == "after recovery",
                "failed generation contaminated recovery");
    }

    // Derived indexes are rebuilt from committed records and unreferenced blobs are collected.
    const auto committed_before_rebuild = current_generation(session);
    fs::remove(session / "indexes" / (committed_before_rebuild + ".json"), ignored);
    {
        FileMemoryBackend recovered(root.string(), "tenant-a", "agent-a");
        (void)recovered.get_conversation_history("s1", 20);
    }
    require(fs::exists(session / "indexes" / (committed_before_rebuild + ".json")),
            "missing derived index was not rebuilt");
    fs::create_directories(session / "blobs");
    { std::ofstream orphan(session / "blobs" / "orphan"); orphan << "unreferenced"; }
    {
        FileMemoryBackend recovered(root.string(), "tenant-a", "agent-a");
        recovered.store_message("s1", Message{"assistant", "gc-trigger", {}, {}, {}, 7});
    }
    require(!fs::exists(session / "blobs" / "orphan"),
            "unreferenced memory blob was not collected");
    require(read_text(session / "audit.jsonl").find("unreferenced_blob_gc") != std::string::npos,
            "blob GC audit record missing");

    // Independent writers must reload under the process lock and compose their updates.
    constexpr int writes_per_thread = 8;
    std::atomic<bool> start{false};
    auto writer = [&](const char* prefix) {
        FileMemoryBackend backend(root.string(), "tenant-a", "agent-a");
        while(!start.load(std::memory_order_acquire)) std::this_thread::yield();
        for(int i = 0; i < writes_per_thread; ++i)
            backend.store_message("concurrent", Message{"user", std::string(prefix) + std::to_string(i), {}, {}, {}, 10 + i});
    };
    std::thread left(writer, "left-");
    std::thread right(writer, "right-");
    start.store(true, std::memory_order_release);
    left.join();
    right.join();
    {
        FileMemoryBackend recovered(root.string(), "tenant-a", "agent-a");
        require(recovered.get_conversation_history("concurrent", 100).size() ==
                    2 * writes_per_thread,
                "concurrent memory commits lost an update");
    }

    // SQLite uses the explicit session instead of the historical default bucket.
    const auto database_path = root.string() + ".sqlite";
    fs::remove(database_path, ignored);
    {
        SQLiteMemoryBackend sqlite(database_path, "tenant-a", "agent-a");
        sqlite.store_message("s1", Message{"tool", "result", "call-1", "lookup",
                                            json{{"ok", true}}, 3});
        sqlite.store_message("s2", Message{"user", "isolated", {}, {}, {}, 4});
        const auto history = sqlite.get_conversation_history("s1", 1);
        require(history.size() == 1 && history[0].tool_name == "lookup" &&
                history[0].tool_result->at("ok") == true,
                "SQLite message session contract failed");
        require(sqlite.get_conversation_history("s2", 2).size() == 1,
                "SQLite sessions crossed");
    }
    {
        SQLiteMemoryBackend isolated(database_path, "tenant-b", "agent-a");
        require(isolated.get_conversation_history("s1", 2).empty(),
                "SQLite messages crossed tenants");
    }

    // A v2 database is migrated transactionally and its legacy rows remain in default scope.
    const auto legacy_path = root.string() + "-legacy.sqlite";
    fs::remove(legacy_path, ignored);
    sqlite3* legacy = nullptr;
    require(sqlite3_open(legacy_path.c_str(), &legacy) == SQLITE_OK, "cannot create legacy sqlite");
    char* sqlite_error = nullptr;
    const char* legacy_sql =
        "CREATE TABLE memory_events(session TEXT,node TEXT,ts INTEGER,payload TEXT);"
        "CREATE TABLE memory_messages(session TEXT,ts INTEGER,payload TEXT);"
        "CREATE TABLE memory_summaries(session TEXT,summary TEXT,payload TEXT,updated INTEGER);"
        "CREATE INDEX memory_events_lookup ON memory_events(session,node,ts);"
        "CREATE INDEX memory_messages_lookup ON memory_messages(session,ts);"
        "PRAGMA user_version=2;";
    require(sqlite3_exec(legacy, legacy_sql, nullptr, nullptr, &sqlite_error) == SQLITE_OK,
            "cannot seed legacy sqlite");
    sqlite3_close(legacy);
    {
        SQLiteMemoryBackend migrated(legacy_path);
        migrated.store_message("legacy", Message{"user", "migrated", {}, {}, {}, 1});
        require(migrated.get_conversation_history("legacy", 2).size() == 1,
                "migrated sqlite is unusable");
    }
    require(sqlite3_open(legacy_path.c_str(), &legacy) == SQLITE_OK, "cannot inspect migrated sqlite");
    sqlite3_stmt* version_statement = nullptr;
    require(sqlite3_prepare_v2(legacy, "PRAGMA user_version", -1, &version_statement, nullptr) == SQLITE_OK &&
            sqlite3_step(version_statement) == SQLITE_ROW && sqlite3_column_int(version_statement, 0) == 3,
            "sqlite schema version was not migrated to v3");
    sqlite3_finalize(version_statement);
    sqlite3_close(legacy);

    fs::remove_all(root, ignored);
    fs::remove(database_path, ignored);
    fs::remove(legacy_path, ignored);
}
