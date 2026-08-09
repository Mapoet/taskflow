#include <agent/memory/memory.hpp>

#include <filesystem>
#include <fstream>
#include <stdexcept>

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

    // SQLite uses the explicit session instead of the historical default bucket.
    const auto database_path = root.string() + ".sqlite";
    fs::remove(database_path, ignored);
    {
        SQLiteMemoryBackend sqlite(database_path);
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

    fs::remove_all(root, ignored);
    fs::remove(database_path, ignored);
}
