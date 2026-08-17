#pragma once

#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <limits>
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

inline void bind_int(sqlite3_stmt* statement, int index, int value) {
    if(sqlite3_bind_int(statement, index, value) != SQLITE_OK)
        throw std::runtime_error("sqlite int bind failed");
}

inline void bind_int64(sqlite3_stmt* statement, int index, std::int64_t value) {
    if(sqlite3_bind_int64(statement, index, static_cast<sqlite3_int64>(value)) != SQLITE_OK)
        throw std::runtime_error("sqlite int64 bind failed");
}

inline void bind_uint64(sqlite3_stmt* statement, int index, std::uint64_t value) {
    if(value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::overflow_error("sqlite uint64 value exceeds signed storage range");
    bind_int64(statement, index, static_cast<std::int64_t>(value));
}

inline void bind_double(sqlite3_stmt* statement, int index, double value) {
    if(sqlite3_bind_double(statement, index, value) != SQLITE_OK)
        throw std::runtime_error("sqlite double bind failed");
}

inline void bind_null(sqlite3_stmt* statement, int index) {
    if(sqlite3_bind_null(statement, index) != SQLITE_OK)
        throw std::runtime_error("sqlite null bind failed");
}

inline void bind_blob(sqlite3_stmt* statement, int index, const void* data, std::size_t size) {
    const auto* bytes = size == 0 ? "" : static_cast<const char*>(data);
    if(sqlite3_bind_blob(statement, index, bytes, static_cast<int>(size), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("sqlite blob bind failed");
}

inline int step(sqlite3_stmt* statement) noexcept { return sqlite3_step(statement); }
inline int column_int(sqlite3_stmt* statement, int index) noexcept {
    return sqlite3_column_int(statement, index);
}
inline std::int64_t column_int64(sqlite3_stmt* statement, int index) noexcept {
    return static_cast<std::int64_t>(sqlite3_column_int64(statement, index));
}
inline std::uint64_t column_uint64(sqlite3_stmt* statement, int index) {
    const auto value = column_int64(statement, index);
    if(value < 0) throw std::runtime_error("negative sqlite value cannot be read as uint64");
    return static_cast<std::uint64_t>(value);
}
inline double column_double(sqlite3_stmt* statement, int index) noexcept {
    return sqlite3_column_double(statement, index);
}
inline std::string column_blob(sqlite3_stmt* statement, int index) {
    const auto* data = static_cast<const char*>(sqlite3_column_blob(statement, index));
    const auto size = sqlite3_column_bytes(statement, index);
    return data && size > 0 ? std::string(data, static_cast<std::size_t>(size)) : std::string();
}
inline int changes(sqlite3* database) noexcept { return sqlite3_changes(database); }
inline std::int64_t last_insert_rowid(sqlite3* database) noexcept {
    return static_cast<std::int64_t>(sqlite3_last_insert_rowid(database));
}
inline void reset(sqlite3_stmt* statement) {
    if(sqlite3_reset(statement) != SQLITE_OK) throw std::runtime_error("sqlite statement reset failed");
}
inline void clear_bindings(sqlite3_stmt* statement) {
    if(sqlite3_clear_bindings(statement) != SQLITE_OK)
        throw std::runtime_error("sqlite clear bindings failed");
}

inline std::string column_text(sqlite3_stmt* statement, int index) {
    const auto* value = sqlite3_column_text(statement, index);
    return value ? reinterpret_cast<const char*>(value) : std::string();
}

inline bool table_has_column(sqlite3* database, std::string_view table,
                             std::string_view column) {
    const auto valid_identifier=[](std::string_view value) {
        return !value.empty() && value.size() <= 128 &&
            std::all_of(value.begin(),value.end(),[](unsigned char ch) {
                return std::isalnum(ch) || ch == '_';
            });
    };
    if(!valid_identifier(table)||!valid_identifier(column))
        throw std::invalid_argument("invalid sqlite identifier");
    const auto query_text=std::string("PRAGMA table_info(")+std::string(table)+")";
    Statement query(database,query_text.c_str());
    while(step(query.get())==SQLITE_ROW)
        if(column_text(query.get(),1)==column)return true;
    return false;
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
