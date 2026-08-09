#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

#include <sqlite3.h>

namespace agent_framework::internal::sqlite {

inline sqlite3* database(void* value) noexcept {
    return static_cast<sqlite3*>(value);
}

inline void exec(sqlite3* database, const char* sql) {
    char* error = nullptr;
    const int status = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if(status == SQLITE_OK) return;
    const std::string message = error ? error : sqlite3_errmsg(database);
    sqlite3_free(error);
    throw std::runtime_error(message);
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) {
        if(sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK)
            throw std::runtime_error(sqlite3_errmsg(database));
    }
    ~Statement() { if(statement_) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    sqlite3_stmt* get() const noexcept { return statement_; }

private:
    sqlite3_stmt* statement_{nullptr};
};

inline void bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
    // An empty string_view may expose a null data pointer. SQLite would bind SQL NULL,
    // not TEXT '', so always provide a valid pointer for the empty value.
    const char* data = value.empty() ? "" : value.data();
    if(sqlite3_bind_text(statement, index, data, static_cast<int>(value.size()),
                         SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("sqlite text bind failed");
}

inline std::string column_text(sqlite3_stmt* statement, int index) {
    const auto* value = sqlite3_column_text(statement, index);
    return value ? reinterpret_cast<const char*>(value) : std::string();
}

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_(database) {
        exec(database_, "BEGIN IMMEDIATE");
    }
    ~Transaction() {
        if(!committed_) sqlite3_exec(database_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    void commit() {
        exec(database_, "COMMIT");
        committed_ = true;
    }

private:
    sqlite3* database_;
    bool committed_{false};
};

}  // namespace agent_framework::internal::sqlite
