/**
 * @file memory_store.cpp
 * @brief Scoped memory backends with crash-consistent file generations.
 */

#include <agent/memory/memory.hpp>
#include <agent/skills/skill_supply_chain.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <sqlite3.h>
#include <stdexcept>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace agent_framework
{
    namespace
    {
        namespace fs = std::filesystem;

        constexpr int kMemorySchemaVersion = 2;

        bool valid_scope_id(const std::string &value)
        {
            if (value.empty() || value.size() > 128 || value == "." || value == "..")
                return false;
            return std::all_of(value.begin(), value.end(), [](unsigned char c)
                               { return std::isalnum(c) || c == '_' || c == '-' || c == '.'; });
        }

        void require_scope_id(const std::string &value, const char *field)
        {
            if (!valid_scope_id(value))
                throw std::invalid_argument(std::string("invalid memory ") + field);
        }

        std::string redact_text(std::string value)
        {
            const std::string marker = "Bearer ";
            for (std::size_t offset = 0; (offset = value.find(marker, offset)) != std::string::npos;)
            {
                const auto begin = offset + marker.size();
                auto end = value.find_first_of(" \t\r\n\"'", begin);
                if (end == std::string::npos)
                    end = value.size();
                value.replace(begin, end - begin, "***REDACTED***");
                offset = begin + 14;
            }
            return value;
        }

        bool sensitive_key(std::string key)
        {
            std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c)
                           { return static_cast<char>(std::tolower(c)); });
            return key.find("authorization") != std::string::npos ||
                   key.find("token") != std::string::npos ||
                   key.find("secret") != std::string::npos ||
                   key.find("password") != std::string::npos ||
                   key.find("api_key") != std::string::npos ||
                   key.find("apikey") != std::string::npos ||
                   key.find("credential") != std::string::npos;
        }

        json redact_json(json value)
        {
            if (value.is_object())
            {
                for (auto &[key, item] : value.items())
                {
                    if (sensitive_key(key))
                        item = "***REDACTED***";
                    else
                        item = redact_json(std::move(item));
                }
            }
            else if (value.is_array())
            {
                for (auto &item : value)
                    item = redact_json(std::move(item));
            }
            else if (value.is_string())
            {
                value = redact_text(value.get<std::string>());
            }
            return value;
        }

        json event_json(const Event &event)
        {
            return {{"v", kMemorySchemaVersion}, {"timestamp", event.timestamp}, {"node_name", event.node_name}, {"event_type", event.event_type}, {"data", redact_json(event.data)}};
        }

        Event parse_event(const json &value)
        {
            if (!value.is_object())
                throw std::runtime_error("invalid memory event");
            return {value.value("timestamp", std::time_t{}), value.value("node_name", ""),
                    value.value("event_type", ""), value.value("data", json::object())};
        }

        json summary_json(const MemorySummary &summary)
        {
            return {{"v", kMemorySchemaVersion}, {"session_id", summary.session_id}, {"summary", redact_text(summary.summary)}, {"keywords", summary.keywords}, {"embedding", summary.summary_embedding}, {"created_at", summary.created_at}, {"updated_at", summary.updated_at}};
        }

        MemorySummary parse_summary(const json &value)
        {
            return {value.value("session_id", ""), value.value("summary", ""),
                    value.value("keywords", std::vector<std::string>{}),
                    value.value("embedding", Embedding{}), value.value("created_at", std::time_t{}),
                    value.value("updated_at", std::time_t{})};
        }

        json message_json(const Message &message)
        {
            json value{{"v", kMemorySchemaVersion}, {"role", message.role}, {"content", redact_text(message.content)}, {"timestamp", message.timestamp}};
            if (message.tool_call_id)
                value["tool_call_id"] = *message.tool_call_id;
            if (message.tool_name)
                value["tool_name"] = *message.tool_name;
            if (message.tool_result)
                value["tool_result"] = redact_json(*message.tool_result);
            return value;
        }

        Message parse_message(const json &value)
        {
            Message message;
            message.role = value.value("role", "");
            message.content = value.value("content", "");
            if (value.contains("tool_call_id") && !value["tool_call_id"].is_null())
                message.tool_call_id = value["tool_call_id"].get<std::string>();
            if (value.contains("tool_name") && !value["tool_name"].is_null())
                message.tool_name = value["tool_name"].get<std::string>();
            if (value.contains("tool_result") && !value["tool_result"].is_null())
                message.tool_result = value["tool_result"];
            message.timestamp = value.value("timestamp", std::time_t{});
            return message;
        }

        void secure_directory(const fs::path &path)
        {
            fs::create_directories(path);
#if !defined(_WIN32)
            if (::chmod(path.c_str(), 0700) != 0)
                throw std::runtime_error("cannot secure memory directory");
#endif
        }

        void secure_file(const fs::path &path)
        {
#if !defined(_WIN32)
            if (::chmod(path.c_str(), 0600) != 0)
                throw std::runtime_error("cannot secure memory file");
#else
            (void)path;
#endif
        }

        bool durable_file(const fs::path &path)
        {
#if defined(_WIN32)
            (void)path;
            return true;
#else
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                return false;
            const bool ok = ::fsync(fd) == 0;
            ::close(fd);
            return ok;
#endif
        }

        bool durable_directory(const fs::path &path)
        {
#if defined(_WIN32)
            (void)path;
            return true;
#else
            const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            if (fd < 0)
                return false;
            const bool ok = ::fsync(fd) == 0;
            ::close(fd);
            return ok;
#endif
        }

        class ProcessFileLock
        {
        public:
            explicit ProcessFileLock(const fs::path &path)
            {
#if !defined(_WIN32)
                fd_ = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
                if (fd_ < 0 || ::flock(fd_, LOCK_EX) != 0)
                {
                    if (fd_ >= 0)
                        ::close(fd_);
                    throw std::runtime_error("cannot lock memory session");
                }
#else
                (void)path;
#endif
            }
            ~ProcessFileLock()
            {
#if !defined(_WIN32)
                if (fd_ >= 0)
                {
                    (void)::flock(fd_, LOCK_UN);
                    ::close(fd_);
                }
#endif
            }
            ProcessFileLock(const ProcessFileLock &) = delete;
            ProcessFileLock &operator=(const ProcessFileLock &) = delete;

        private:
            int fd_ = -1;
        };

        void ensure_no_symlink(const fs::path &path)
        {
            fs::path current;
            for (const auto &part : path)
            {
                current /= part;
                std::error_code error;
                const auto status = fs::symlink_status(current, error);
                if (!error && fs::is_symlink(status))
                    throw std::runtime_error("memory path contains symlink");
            }
        }

        void write_jsonl(const fs::path &path, const std::vector<json> &values)
        {
            std::ofstream output(path, std::ios::binary | std::ios::trunc);
            if (!output)
                throw std::runtime_error("cannot write memory generation");
            for (const auto &value : values)
                output << value.dump() << '\n';
            output.flush();
            if (!output)
                throw std::runtime_error("cannot flush memory generation");
            output.close();
            secure_file(path);
        }

        bool read_jsonl_strict(const fs::path &path, std::vector<json> &output)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return false;
            try
            {
                for (std::string line; std::getline(input, line);)
                {
                    if (line.empty())
                        return false;
                    output.push_back(json::parse(line));
                }
            }
            catch (...)
            {
                return false;
            }
            return input.eof();
        }

        std::optional<std::string> file_digest(const fs::path &path)
        {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                return std::nullopt;
            std::ostringstream bytes;
            bytes << input.rdbuf();
            if (!input.eof() && input.fail())
                return std::nullopt;
            return skill_sha256_bytes(bytes.str());
        }

        struct SessionGeneration
        {
            std::uint64_t revision = 0;
            std::vector<json> events;
            std::vector<json> messages;
            std::vector<json> summaries;
        };

        std::string generation_name(std::uint64_t revision)
        {
            std::ostringstream value;
            value << std::setw(20) << std::setfill('0') << revision;
            return value.str();
        }

        std::optional<SessionGeneration> read_generation(const fs::path &directory)
        {
            try
            {
                json manifest;
                std::ifstream manifest_input(directory / "manifest.json", std::ios::binary);
                manifest_input >> manifest;
                if (!manifest.is_object() || manifest.value("schema_version", 0) != kMemorySchemaVersion)
                    return std::nullopt;
                SessionGeneration data;
                data.revision = manifest.value("revision", std::uint64_t{0});
                if (data.revision == 0)
                    return std::nullopt;
                const std::array<std::pair<const char *, std::vector<json> *>, 3> files{{{"events.jsonl", &data.events}, {"messages.jsonl", &data.messages}, {"summaries.jsonl", &data.summaries}}};
                for (const auto &[name, values] : files)
                {
                    const auto digest = file_digest(directory / name);
                    if (!digest || *digest != manifest["digests"].value(name, "") ||
                        !read_jsonl_strict(directory / name, *values))
                        return std::nullopt;
                }
                if (data.events.size() != manifest["counts"].value("events", std::size_t(-1)) ||
                    data.messages.size() != manifest["counts"].value("messages", std::size_t(-1)) ||
                    data.summaries.size() != manifest["counts"].value("summaries", std::size_t(-1)))
                    return std::nullopt;
                return data;
            }
            catch (...)
            {
                return std::nullopt;
            }
        }

        std::vector<std::string> generation_candidates(const fs::path &session)
        {
            std::vector<std::string> candidates;
            std::ifstream current(session / "CURRENT");
            std::string selected;
            if (std::getline(current, selected) && valid_scope_id(selected))
                candidates.push_back(selected);
            const auto generations = session / "generations";
            std::error_code error;
            if (fs::exists(generations, error))
            {
                for (const auto &entry : fs::directory_iterator(generations))
                {
                    const auto name = entry.path().filename().string();
                    if (entry.is_directory() && !name.starts_with(".tmp-") && valid_scope_id(name))
                        candidates.push_back(name);
                }
            }
            std::sort(candidates.begin(), candidates.end(), std::greater<>());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            if (!selected.empty())
            {
                auto found = std::find(candidates.begin(), candidates.end(), selected);
                if (found != candidates.end())
                    std::rotate(candidates.begin(), found, found + 1);
            }
            return candidates;
        }

        SessionGeneration load_session(const fs::path &session)
        {
            for (const auto &candidate : generation_candidates(session))
            {
                if (auto data = read_generation(session / "generations" / candidate))
                    return *data;
                std::clog << "[memory] ignored invalid committed generation digest="
                          << skill_sha256_bytes(candidate).value_or("unavailable") << '\n';
            }
            return {};
        }

        void commit_session(const fs::path &session, SessionGeneration data)
        {
            ensure_no_symlink(session);
            secure_directory(session);
            ensure_no_symlink(session);
            const auto lock_path = session / "lock";
    ProcessFileLock lock(lock_path);
    secure_file(lock_path);

    // Quarantine a committed generation whose manifest/content no longer validates.
    // This happens under the process lock so another writer cannot race the move.
    std::string pointed_generation;
    { std::ifstream pointer(session / "CURRENT"); std::getline(pointer, pointed_generation); }
    if(valid_scope_id(pointed_generation)) {
        const auto pointed_path = session / "generations" / pointed_generation;
        if(fs::exists(pointed_path) && !read_generation(pointed_path)) {
            const auto quarantine = session / "quarantine";
            secure_directory(quarantine);
            auto target = quarantine / pointed_generation;
            if(fs::exists(target)) target += ".duplicate";
            fs::rename(pointed_path, target);
            (void)durable_directory(quarantine);
            std::clog << "[memory] quarantined invalid generation digest="
                      << skill_sha256_bytes(pointed_generation).value_or("unavailable") << '\n';
        }
    }

    // Reload after taking the cross-process lock so concurrent commits compose.
            const auto current = load_session(session);
            if (data.revision != current.revision + 1)
                throw std::runtime_error("memory revision conflict");
            const auto generations = session / "generations";
            secure_directory(generations);
            std::uint64_t largest_published_revision = current.revision;
            const auto inspect_revisions = [&](const fs::path &directory)
            {
                std::error_code error;
                if (!fs::exists(directory, error))
                    return;
                for (const auto &entry : fs::directory_iterator(directory))
                {
                    const auto candidate = entry.path().filename().string();
                    if (candidate.starts_with(".tmp-"))
                        continue;
                    try
                    {
                        largest_published_revision = std::max(
                            largest_published_revision,
                            static_cast<std::uint64_t>(std::stoull(candidate)));
                    }
                    catch (...)
                    {
                    }
                }
            };
            inspect_revisions(generations);
            inspect_revisions(session / "quarantine");
            data.revision = largest_published_revision + 1;
            const auto name = generation_name(data.revision);
#if defined(_WIN32)
            const auto process_id = 0;
#else
            const auto process_id = ::getpid();
#endif
            const auto temporary = generations /
                                   (".tmp-" + std::to_string(process_id) + "-" + name);
            const auto committed = generations / name;
            std::error_code cleanup_error;
            fs::remove_all(temporary, cleanup_error);
            secure_directory(temporary);

            write_jsonl(temporary / "events.jsonl", data.events);
            write_jsonl(temporary / "messages.jsonl", data.messages);
            write_jsonl(temporary / "summaries.jsonl", data.summaries);
            const auto event_digest = file_digest(temporary / "events.jsonl");
            const auto message_digest = file_digest(temporary / "messages.jsonl");
            const auto summary_digest = file_digest(temporary / "summaries.jsonl");
            if (!event_digest || !message_digest || !summary_digest)
                throw std::runtime_error("cannot digest memory generation");
            const json manifest{
                {"schema_version", kMemorySchemaVersion}, {"revision", data.revision}, {"counts", {{"events", data.events.size()}, {"messages", data.messages.size()}, {"summaries", data.summaries.size()}}}, {"digests", {{"events.jsonl", *event_digest}, {"messages.jsonl", *message_digest}, {"summaries.jsonl", *summary_digest}}}};
            {
                std::ofstream output(temporary / "manifest.json", std::ios::binary | std::ios::trunc);
                output << manifest.dump(2) << '\n';
                output.flush();
                if (!output)
                    throw std::runtime_error("cannot write memory manifest");
            }
            secure_file(temporary / "manifest.json");
            for (const auto &file : {"events.jsonl", "messages.jsonl", "summaries.jsonl", "manifest.json"})
                if (!durable_file(temporary / file))
                    throw std::runtime_error("memory fsync failed");
            if (!durable_directory(temporary))
                throw std::runtime_error("memory directory fsync failed");
            fs::rename(temporary, committed);
            if (!durable_directory(generations))
                throw std::runtime_error("memory generation publish failed");

            const auto current_tmp = session /
                                     ("CURRENT.tmp-" + std::to_string(process_id));
            {
                std::ofstream output(current_tmp, std::ios::binary | std::ios::trunc);
                output << name << '\n';
                output.flush();
                if (!output)
                    throw std::runtime_error("cannot write memory CURRENT");
            }
            secure_file(current_tmp);
            if (!durable_file(current_tmp))
                throw std::runtime_error("memory CURRENT fsync failed");
            fs::rename(current_tmp, session / "CURRENT");
            if (!durable_directory(session))
                throw std::runtime_error("memory commit fsync failed");

            // Retain current plus two rollback generations; abandoned temporaries are never visible.
            auto candidates = generation_candidates(session);
            std::set<std::string> keep;
            for (std::size_t index = 0; index < candidates.size() && index < 3; ++index)
                keep.insert(candidates[index]);
            for (const auto &entry : fs::directory_iterator(generations))
            {
                const auto entry_name = entry.path().filename().string();
                if (entry.is_directory() && (entry_name.starts_with(".tmp-") || !keep.contains(entry_name)))
                    fs::remove_all(entry.path(), cleanup_error);
            }
            (void)durable_directory(generations);
        }

        fs::path scoped_root(const std::string &root, const std::string &tenant,
                             const std::string &agent)
        {
            return fs::path(root) / tenant / agent;
        }

        fs::path session_root(const std::string &root, const std::string &tenant,
                              const std::string &agent, const std::string &session)
        {
            require_scope_id(session, "session id");
            return scoped_root(root, tenant, agent) / session;
        }

        void sqlite_require(int rc, sqlite3 *database, const char *operation)
        {
            if (rc != SQLITE_OK && rc != SQLITE_DONE && rc != SQLITE_ROW)
                throw std::runtime_error(std::string(operation) + ": " + sqlite3_errmsg(database));
        }
    } // namespace

    FileMemoryBackend::FileMemoryBackend(const std::string &data_dir, std::string tenant_id,
                                         std::string agent_id)
        : data_dir_(data_dir), tenant_id_(std::move(tenant_id)), agent_id_(std::move(agent_id))
    {
        if (data_dir_.empty())
            throw std::invalid_argument("memory data directory must not be empty");
        require_scope_id(tenant_id_, "tenant id");
        require_scope_id(agent_id_, "agent id");
        const auto root = scoped_root(data_dir_, tenant_id_, agent_id_);
        ensure_no_symlink(root);
        secure_directory(root);
        ensure_no_symlink(root);
    }

    void FileMemoryBackend::store_event(const Event &event)
    {
        const auto session = event.data.value("session_id", "default");
        std::lock_guard<std::mutex> guard(file_mutex_);
        const auto root = session_root(data_dir_, tenant_id_, agent_id_, session);
        auto data = load_session(root);
        data.events.push_back(event_json(event));
        ++data.revision;
        commit_session(root, std::move(data));
    }

    std::vector<Event> FileMemoryBackend::query_events(const std::string &session,
                                                       const std::string &node,
                                                       std::time_t begin, std::time_t end)
    {
        std::lock_guard<std::mutex> guard(file_mutex_);
        std::vector<Event> result;
        for (const auto &value : load_session(session_root(data_dir_, tenant_id_, agent_id_, session)).events)
        {
            const auto event = parse_event(value);
            if ((node.empty() || event.node_name == node) && (begin == 0 || event.timestamp >= begin) &&
                (end == 0 || event.timestamp <= end))
                result.push_back(event);
        }
        return result;
    }

    void FileMemoryBackend::store_message(const std::string &session, const Message &message)
    {
        require_scope_id(session, "session id");
        std::lock_guard<std::mutex> guard(file_mutex_);
        const auto root = session_root(data_dir_, tenant_id_, agent_id_, session);
        auto data = load_session(root);
        data.messages.push_back(message_json(message));
        ++data.revision;
        commit_session(root, std::move(data));
    }

    std::vector<Message> FileMemoryBackend::get_conversation_history(const std::string &session,
                                                                     int maximum)
    {
        std::lock_guard<std::mutex> guard(file_mutex_);
        std::vector<Message> result;
        for (const auto &value : load_session(session_root(data_dir_, tenant_id_, agent_id_, session)).messages)
            result.push_back(parse_message(value));
        if (maximum > 0 && result.size() > static_cast<std::size_t>(maximum))
            result.erase(result.begin(), result.end() - maximum);
        return result;
    }

    void FileMemoryBackend::store_memory_summary(const MemorySummary &summary)
    {
        require_scope_id(summary.session_id, "session id");
        std::lock_guard<std::mutex> guard(file_mutex_);
        const auto root = session_root(data_dir_, tenant_id_, agent_id_, summary.session_id);
        auto data = load_session(root);
        data.summaries.push_back(summary_json(summary));
        ++data.revision;
        commit_session(root, std::move(data));
    }

    std::vector<MemorySummary> FileMemoryBackend::query_memory_summaries(const std::string &query,
                                                                         int top)
    {
        std::lock_guard<std::mutex> guard(file_mutex_);
        std::vector<MemorySummary> result;
        const auto root = scoped_root(data_dir_, tenant_id_, agent_id_);
        for (const auto &entry : fs::directory_iterator(root))
        {
            if (!entry.is_directory() || !valid_scope_id(entry.path().filename().string()))
                continue;
            for (const auto &value : load_session(entry.path()).summaries)
            {
                auto summary = parse_summary(value);
                if (query.empty() || summary.summary.find(query) != std::string::npos)
                    result.push_back(std::move(summary));
            }
        }
        std::sort(result.begin(), result.end(), [](const auto &left, const auto &right)
                  { return left.updated_at > right.updated_at; });
        if (top > 0 && result.size() > static_cast<std::size_t>(top))
            result.resize(top);
        return result;
    }

    void FileMemoryBackend::cleanup_expired_data(std::time_t expiry)
    {
        std::lock_guard<std::mutex> guard(file_mutex_);
        const auto root = scoped_root(data_dir_, tenant_id_, agent_id_);
        for (const auto &entry : fs::directory_iterator(root))
        {
            if (!entry.is_directory() || !valid_scope_id(entry.path().filename().string()))
                continue;
            auto data = load_session(entry.path());
            const auto old_size = data.summaries.size();
            data.summaries.erase(std::remove_if(data.summaries.begin(), data.summaries.end(),
                                                [expiry](const json &value)
                                                { return value.value("updated_at", std::time_t{}) < expiry; }),
                                 data.summaries.end());
            if (data.summaries.size() != old_size)
            {
                ++data.revision;
                commit_session(entry.path(), std::move(data));
            }
        }
    }

    InMemoryBackend::InMemoryBackend() = default;
    void InMemoryBackend::store_event(const Event &event)
    {
        std::lock_guard<std::mutex> guard(data_mutex_);
        events_[event.data.value("session_id", "default")].push_back(event);
    }
    std::vector<Event> InMemoryBackend::query_events(const std::string &session, const std::string &node,
                                                     std::time_t begin, std::time_t end)
    {
        std::lock_guard<std::mutex> guard(data_mutex_);
        std::vector<Event> result;
        for (const auto &event : events_[session])
            if ((node.empty() || event.node_name == node) && (begin == 0 || event.timestamp >= begin) &&
                (end == 0 || event.timestamp <= end))
                result.push_back(event);
        return result;
    }
    void InMemoryBackend::store_message(const std::string &session, const Message &message)
    {
        require_scope_id(session, "session id");
        std::lock_guard<std::mutex> guard(data_mutex_);
        messages_[session].push_back(message);
    }
    std::vector<Message> InMemoryBackend::get_conversation_history(const std::string &session, int maximum)
    {
        std::lock_guard<std::mutex> guard(data_mutex_);
        auto result = messages_[session];
        if (maximum > 0 && result.size() > static_cast<std::size_t>(maximum))
            result.erase(result.begin(), result.end() - maximum);
        return result;
    }
    void InMemoryBackend::store_memory_summary(const MemorySummary &summary)
    {
        std::lock_guard<std::mutex> guard(data_mutex_);
        summaries_.push_back(summary);
    }
    std::vector<MemorySummary> InMemoryBackend::query_memory_summaries(const std::string &query, int top)
    {
        std::lock_guard<std::mutex> guard(data_mutex_);
        std::vector<MemorySummary> result;
        for (const auto &summary : summaries_)
            if (query.empty() || summary.summary.find(query) != std::string::npos)
            {
                result.push_back(summary);
                if (top > 0 && result.size() >= static_cast<std::size_t>(top))
                    break;
            }
        return result;
    }
    void InMemoryBackend::cleanup_expired_data(std::time_t expiry)
    {
        std::lock_guard<std::mutex> guard(data_mutex_);
        summaries_.erase(std::remove_if(summaries_.begin(), summaries_.end(),
                                        [expiry](const auto &summary)
                                        { return summary.updated_at < expiry; }),
                         summaries_.end());
    }

    SQLiteMemoryBackend::SQLiteMemoryBackend(const std::string &path) : db_path_(path), db_(nullptr)
    {
        sqlite3 *database = nullptr;
        if (sqlite3_open(path.c_str(), &database) != SQLITE_OK)
        {
            const std::string error = database ? sqlite3_errmsg(database) : "open failed";
            if (database)
                sqlite3_close(database);
            throw std::runtime_error("cannot open memory sqlite: " + error);
        }
        db_ = database;
        init_database();
    }
    SQLiteMemoryBackend::~SQLiteMemoryBackend()
    {
        if (db_)
            sqlite3_close(static_cast<sqlite3 *>(db_));
    }
    void SQLiteMemoryBackend::execute_sql(const std::string &sql, const std::vector<std::string> &)
    {
        char *error = nullptr;
        if (sqlite3_exec(static_cast<sqlite3 *>(db_), sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK)
        {
            const std::string message = error ? error : "sqlite error";
            sqlite3_free(error);
            throw std::runtime_error(message);
        }
    }
    void SQLiteMemoryBackend::init_database()
    {
        execute_sql("PRAGMA journal_mode=WAL; PRAGMA busy_timeout=5000; "
                    "CREATE TABLE IF NOT EXISTS memory_events(session TEXT,node TEXT,ts INTEGER,payload TEXT);"
                    "CREATE INDEX IF NOT EXISTS memory_events_lookup ON memory_events(session,node,ts);"
                    "CREATE TABLE IF NOT EXISTS memory_messages(session TEXT,ts INTEGER,payload TEXT);"
                    "CREATE INDEX IF NOT EXISTS memory_messages_lookup ON memory_messages(session,ts);"
                    "CREATE TABLE IF NOT EXISTS memory_summaries(session TEXT,summary TEXT,payload TEXT,updated INTEGER);"
                    "PRAGMA user_version=2;");
    }
    void SQLiteMemoryBackend::store_event(const Event &event)
    {
        std::lock_guard<std::mutex> guard(db_mutex_);
        auto *database = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *statement = nullptr;
        sqlite_require(sqlite3_prepare_v2(database, "INSERT INTO memory_events VALUES(?,?,?,?)", -1, &statement, nullptr), database, "prepare event");
        const auto payload = event_json(event).dump();
        const auto session = event.data.value("session_id", "default");
        sqlite3_bind_text(statement, 1, session.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, event.node_name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3, event.timestamp);
        sqlite3_bind_text(statement, 4, payload.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(statement);
        sqlite3_finalize(statement);
        sqlite_require(rc, database, "store event");
    }
    std::vector<Event> SQLiteMemoryBackend::query_events(const std::string &session, const std::string &node,
                                                         std::time_t begin, std::time_t end)
    {
        std::lock_guard<std::mutex> guard(db_mutex_);
        auto *database = static_cast<sqlite3 *>(db_);
        std::string sql = "SELECT payload FROM memory_events WHERE session=?";
        if (!node.empty())
            sql += " AND node=?";
        if (begin)
            sql += " AND ts>=?";
        if (end)
            sql += " AND ts<=?";
        sql += " ORDER BY ts";
        sqlite3_stmt *statement = nullptr;
        sqlite_require(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr), database, "prepare query events");
        int index = 1;
        sqlite3_bind_text(statement, index++, session.c_str(), -1, SQLITE_TRANSIENT);
        if (!node.empty())
            sqlite3_bind_text(statement, index++, node.c_str(), -1, SQLITE_TRANSIENT);
        if (begin)
            sqlite3_bind_int64(statement, index++, begin);
        if (end)
            sqlite3_bind_int64(statement, index++, end);
        std::vector<Event> result;
        for (int rc; (rc = sqlite3_step(statement)) == SQLITE_ROW;)
            result.push_back(parse_event(json::parse(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)))));
        sqlite3_finalize(statement);
        return result;
    }
    void SQLiteMemoryBackend::store_message(const std::string &session, const Message &message)
    {
        require_scope_id(session, "session id");
        std::lock_guard<std::mutex> guard(db_mutex_);
        auto *database = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *statement = nullptr;
        sqlite_require(sqlite3_prepare_v2(database, "INSERT INTO memory_messages VALUES(?,?,?)", -1, &statement, nullptr), database, "prepare message");
        const auto payload = message_json(message).dump();
        sqlite3_bind_text(statement, 1, session.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2, message.timestamp);
        sqlite3_bind_text(statement, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
        const int rc = sqlite3_step(statement);
        sqlite3_finalize(statement);
        sqlite_require(rc, database, "store message");
    }
    std::vector<Message> SQLiteMemoryBackend::get_conversation_history(const std::string &session, int maximum)
    {
        std::lock_guard<std::mutex> guard(db_mutex_);
        auto *database = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *statement = nullptr;
        sqlite_require(sqlite3_prepare_v2(database, "SELECT payload FROM memory_messages WHERE session=? ORDER BY ts DESC LIMIT ?", -1, &statement, nullptr), database, "prepare history");
        sqlite3_bind_text(statement, 1, session.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, maximum > 0 ? maximum : -1);
        std::vector<Message> result;
        while (sqlite3_step(statement) == SQLITE_ROW)
            result.push_back(parse_message(json::parse(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)))));
        sqlite3_finalize(statement);
        std::reverse(result.begin(), result.end());
        return result;
    }
    void SQLiteMemoryBackend::store_memory_summary(const MemorySummary &summary)
    {
        require_scope_id(summary.session_id, "session id");
        std::lock_guard<std::mutex> guard(db_mutex_);
        auto *database = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *statement = nullptr;
        sqlite_require(sqlite3_prepare_v2(database, "INSERT INTO memory_summaries VALUES(?,?,?,?)", -1, &statement, nullptr), database, "prepare summary");
        const auto payload = summary_json(summary).dump();
        sqlite3_bind_text(statement, 1, summary.session_id.c_str(), -1, SQLITE_TRANSIENT);
        const auto redacted = redact_text(summary.summary);
        sqlite3_bind_text(statement, 2, redacted.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, payload.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 4, summary.updated_at);
        const int rc = sqlite3_step(statement);
        sqlite3_finalize(statement);
        sqlite_require(rc, database, "store summary");
    }
    std::vector<MemorySummary> SQLiteMemoryBackend::query_memory_summaries(const std::string &query, int top)
    {
        std::lock_guard<std::mutex> guard(db_mutex_);
        auto *database = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *statement = nullptr;
        sqlite_require(sqlite3_prepare_v2(database, "SELECT payload FROM memory_summaries WHERE summary LIKE ? ORDER BY updated DESC LIMIT ?", -1, &statement, nullptr), database, "prepare summary query");
        const auto like = "%" + query + "%";
        sqlite3_bind_text(statement, 1, like.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, top > 0 ? top : -1);
        std::vector<MemorySummary> result;
        while (sqlite3_step(statement) == SQLITE_ROW)
            result.push_back(parse_summary(json::parse(reinterpret_cast<const char *>(sqlite3_column_text(statement, 0)))));
        sqlite3_finalize(statement);
        return result;
    }
    void SQLiteMemoryBackend::cleanup_expired_data(std::time_t expiry)
    {
        std::lock_guard<std::mutex> guard(db_mutex_);
        auto *database = static_cast<sqlite3 *>(db_);
        sqlite3_stmt *statement = nullptr;
        sqlite_require(sqlite3_prepare_v2(database, "DELETE FROM memory_summaries WHERE updated<?", -1, &statement, nullptr), database, "prepare cleanup");
        sqlite3_bind_int64(statement, 1, expiry);
        const int rc = sqlite3_step(statement);
        sqlite3_finalize(statement);
        sqlite_require(rc, database, "cleanup summaries");
    }

    MemoryStore::MemoryStore(std::unique_ptr<MemoryBackend> backend) : backend_(std::move(backend))
    {
        if (!backend_)
            throw std::invalid_argument("memory backend required");
    }
    void MemoryStore::store_event(const Event &event)
    {
        std::lock_guard<std::mutex> guard(backend_mutex_);
        backend_->store_event(event);
    }
    void MemoryStore::store_message(const std::string &session, const Message &message)
    {
        std::lock_guard<std::mutex> guard(backend_mutex_);
        backend_->store_message(session, message);
    }
    std::vector<Message> MemoryStore::get_conversation_history(const std::string &session, int maximum)
    {
        std::lock_guard<std::mutex> guard(backend_mutex_);
        return backend_->get_conversation_history(session, maximum);
    }
    std::vector<Event> MemoryStore::get_short_term_memory(const std::string &session)
    {
        std::lock_guard<std::mutex> guard(backend_mutex_);
        return backend_->query_events(session);
    }
    void MemoryStore::store_long_term_memory(const std::string &session, const MemorySummary &summary)
    {
        auto copy = summary;
        copy.session_id = session;
        std::lock_guard<std::mutex> guard(backend_mutex_);
        backend_->store_memory_summary(copy);
    }
    std::vector<MemorySummary> MemoryStore::query_long_term_memory(const std::string &query, int top)
    {
        std::lock_guard<std::mutex> guard(backend_mutex_);
        return backend_->query_memory_summaries(query, top);
    }
    void MemoryStore::switch_backend(std::unique_ptr<MemoryBackend> backend)
    {
        if (!backend)
            throw std::invalid_argument("memory backend required");
        std::lock_guard<std::mutex> guard(backend_mutex_);
        backend_ = std::move(backend);
    }

} // namespace agent_framework
