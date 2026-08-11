#pragma once

#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <libpq-fe.h>

namespace agent_framework::internal::postgres {

class Result {
public:
    explicit Result(PGresult* value = nullptr) : value_(value) {}
    ~Result() { if(value_) PQclear(value_); }
    Result(const Result&) = delete;
    Result& operator=(const Result&) = delete;
    Result(Result&& other) noexcept : value_(other.value_) { other.value_ = nullptr; }
    Result& operator=(Result&& other) noexcept {
        if(this == &other) return *this;
        if(value_) PQclear(value_);
        value_ = other.value_;
        other.value_ = nullptr;
        return *this;
    }
    PGresult* get() const noexcept { return value_; }
    int rows() const noexcept { return value_ ? PQntuples(value_) : 0; }
    std::string_view value(int row, int column) const {
        if(!value_ || PQgetisnull(value_, row, column)) return {};
        return {PQgetvalue(value_, row, column),
                static_cast<std::size_t>(PQgetlength(value_, row, column))};
    }
private:
    PGresult* value_;
};

class Connection {
public:
    explicit Connection(std::string_view conninfo) {
        value_ = PQconnectdb(std::string(conninfo).c_str());
        if(!value_ || PQstatus(value_) != CONNECTION_OK) {
            const auto message = value_ ? PQerrorMessage(value_) : "libpq allocation failed";
            if(value_) PQfinish(value_);
            value_ = nullptr;
            throw std::runtime_error(message);
        }
    }
    ~Connection() { if(value_) PQfinish(value_); }
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    PGconn* get() {
        if(!value_) throw std::runtime_error("PostgreSQL connection is unavailable");
        if(PQstatus(value_) != CONNECTION_OK) {
            PQreset(value_);
            if(PQstatus(value_) != CONNECTION_OK)
                throw std::runtime_error(PQerrorMessage(value_));
        }
        return value_;
    }
private:
    PGconn* value_{nullptr};
};

inline Result checked(PGconn* connection, PGresult* raw,
                      std::initializer_list<ExecStatusType> accepted) {
    Result result(raw);
    const auto status = raw ? PQresultStatus(raw) : PGRES_FATAL_ERROR;
    for(const auto expected : accepted) if(status == expected) return result;
    const auto detail = raw ? PQresultErrorMessage(raw) : PQerrorMessage(connection);
    throw std::runtime_error(detail && *detail ? detail : "PostgreSQL operation failed");
}

inline Result exec(PGconn* connection, std::string_view sql) {
    return checked(connection, PQexec(connection, std::string(sql).c_str()),
                   {PGRES_COMMAND_OK, PGRES_TUPLES_OK});
}

inline Result exec_params(PGconn* connection, std::string_view sql,
                          const std::vector<std::string>& parameters) {
    std::vector<const char*> values;
    values.reserve(parameters.size());
    for(const auto& parameter : parameters) values.push_back(parameter.c_str());
    return checked(connection,
        PQexecParams(connection, std::string(sql).c_str(),
                     static_cast<int>(values.size()), nullptr, values.data(),
                     nullptr, nullptr, 0),
        {PGRES_COMMAND_OK, PGRES_TUPLES_OK});
}

inline std::uint64_t uint64(std::string_view value) {
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if(parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid PostgreSQL uint64 value");
    return result;
}

inline std::int64_t int64(std::string_view value) {
    std::int64_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if(parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw std::runtime_error("invalid PostgreSQL int64 value");
    return result;
}

class Transaction {
public:
    explicit Transaction(PGconn* connection) : connection_(connection) {
        exec(connection_, "BEGIN");
    }
    ~Transaction() { if(!committed_) PQclear(PQexec(connection_, "ROLLBACK")); }
    void commit() { exec(connection_, "COMMIT"); committed_ = true; }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
private:
    PGconn* connection_;
    bool committed_{false};
};

}  // namespace agent_framework::internal::postgres
