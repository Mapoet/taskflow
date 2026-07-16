#include <agent/session/session_store.hpp>

#include <cassert>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <sqlite3.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif

using namespace agent_framework;

int main() {
    const auto dir = std::filesystem::temp_directory_path() / "agent_phase2_session_store";
    const auto path = dir / "session.sqlite";
    std::filesystem::remove_all(dir);
    {
        SQLiteSessionStore store(path.string());
        auto s = store.load_or_create("ctx-1");
        assert(s.revision == 0);
        s.checkpoint_id = "cp-1";
        s.state.iteration = 3;
        s.state.history.push_back(Message{"user", "hello", {}, {}, {}, 7});
        s.tool_commits.push_back({"call-1", 0, "committed", "sha256:test"});
        s.child_tasks.push_back({"child-1", "local", 0, "completed", {{"value", 7}}});
        const auto c1 = store.commit(s, 0);
        assert(c1.status == SessionCommitStatus::Committed && c1.revision == 1);
        assert(store.commit(s, 0).status == SessionCommitStatus::RevisionConflict);
    }
    {
        SQLiteSessionStore reopened(path.string());
        auto s = reopened.load_or_create("ctx-1");
        assert(s.revision == 1);
        assert(s.state.iteration == 3);
        assert(s.state.history.size() == 1 && s.state.history[0].content == "hello");
        assert(s.tool_commits.size() == 1);
        assert(s.child_tasks.size() == 1 && s.child_tasks[0].payload.at("value") == 7);
        assert(reopened.load_checkpoint("ctx-1", "cp-1").has_value());
    }
    std::filesystem::remove_all(dir);

#if !defined(_WIN32)
    const auto insecure = std::filesystem::temp_directory_path() / "agent_phase2_insecure";
    std::filesystem::remove_all(insecure);
    std::filesystem::create_directories(insecure);
    ::chmod(insecure.c_str(), 0755);
    bool permission_rejected = false;
    try {
        SQLiteSessionStore rejected((insecure / "session.sqlite").string());
    } catch (const std::runtime_error&) {
        permission_rejected = true;
    }
    assert(permission_rejected);
    std::filesystem::remove_all(insecure);
#endif

    const auto future_dir = std::filesystem::temp_directory_path() / "agent_phase2_future";
    std::filesystem::remove_all(future_dir);
    std::filesystem::create_directories(future_dir);
#if !defined(_WIN32)
    ::chmod(future_dir.c_str(), 0700);
#endif
    const auto future_path = future_dir / "session.sqlite";
    sqlite3* future_db = nullptr;
    assert(sqlite3_open(future_path.c_str(), &future_db) == SQLITE_OK);
    assert(sqlite3_exec(future_db,
        "CREATE TABLE schema_version(version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);"
        "INSERT INTO schema_version VALUES(99,'future');", nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(future_db);
#if !defined(_WIN32)
    ::chmod(future_path.c_str(), 0600);
#endif
    bool future_rejected = false;
    try {
        SQLiteSessionStore rejected(future_path.string());
    } catch (const std::runtime_error&) {
        future_rejected = true;
    }
    assert(future_rejected);
    std::filesystem::remove_all(future_dir);

    const auto corrupt_dir = std::filesystem::temp_directory_path() / "agent_phase2_corrupt";
    std::filesystem::remove_all(corrupt_dir);
    std::filesystem::create_directories(corrupt_dir);
#if !defined(_WIN32)
    ::chmod(corrupt_dir.c_str(), 0700);
#endif
    const auto corrupt_path = corrupt_dir / "session.sqlite";
    { std::ofstream out(corrupt_path, std::ios::binary); out << "not-a-sqlite-database"; }
#if !defined(_WIN32)
    ::chmod(corrupt_path.c_str(), 0600);
#endif
    bool corrupt_rejected = false;
    try {
        SQLiteSessionStore rejected(corrupt_path.string());
    } catch (const std::runtime_error&) {
        corrupt_rejected = true;
    }
    assert(corrupt_rejected);
    std::filesystem::remove_all(corrupt_dir);

    const auto busy_dir = std::filesystem::temp_directory_path() / "agent_phase2_busy";
    std::filesystem::remove_all(busy_dir);
    SQLiteSessionStoreOptions busy_options;
    busy_options.busy_timeout_ms = 20;
    SQLiteSessionStore busy_store((busy_dir / "session.sqlite").string(), busy_options);
    sqlite3* locker = nullptr;
    assert(sqlite3_open((busy_dir / "session.sqlite").c_str(), &locker) == SQLITE_OK);
    assert(sqlite3_exec(locker, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK);
    auto busy_snapshot = busy_store.load_or_create("busy");
    assert(busy_store.commit(busy_snapshot, 0).status == SessionCommitStatus::StoreBusy);
    sqlite3_exec(locker, "ROLLBACK", nullptr, nullptr, nullptr);
    sqlite3_close(locker);
    std::filesystem::remove_all(busy_dir);

    InMemorySessionStore memory;
    auto s = memory.load_or_create("mem");
    assert(memory.commit(s, 0).status == SessionCommitStatus::Committed);
    assert(memory.commit(s, 0).status == SessionCommitStatus::RevisionConflict);
    std::cout << "test_session_store: all passed\n";
}
