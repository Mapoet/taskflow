#include "agent/memory_v2/project_instruction_provider.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "agent/contracts/contract.hpp"

namespace agent_framework::memory_v2 {
namespace {

bool within(const std::filesystem::path& parent, const std::filesystem::path& child) {
    auto p = parent.begin();
    auto c = child.begin();
    for(; p != parent.end(); ++p, ++c)
        if(c == child.end() || *p != *c) return false;
    return true;
}

std::string read_bounded(const std::filesystem::path& path, std::size_t limit) {
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if(error) throw std::runtime_error("cannot stat instruction: " + path.string());
    if(size > limit) throw std::runtime_error("instruction exceeds size limit: " + path.string());
    std::ifstream input(path, std::ios::binary);
    if(!input) throw std::runtime_error("cannot open instruction: " + path.string());
    std::ostringstream content;
    content << input.rdbuf();
    if(!input.good() && !input.eof())
        throw std::runtime_error("cannot read instruction: " + path.string());
    return content.str();
}

std::uint64_t generation_hash(std::uint64_t generation, std::string_view value) {
    constexpr std::uint64_t prime = 1099511628211ULL;
    if(generation == 0) generation = 1469598103934665603ULL;
    for(unsigned char ch : value) generation = (generation ^ ch) * prime;
    return generation;
}

MemoryRecord instruction_record(const ProjectInstructionProviderConfig& config,
                                const MemoryQuery& query,
                                const std::filesystem::path& path,
                                std::string content, bool global) {
    const auto content_digest = contracts::embedded_digest(content).value_or("");
    const auto id_digest = contracts::embedded_digest(
        nlohmann::json{{"provider", config.provider_id}, {"path", path.generic_string()}}).value_or("");
    MemoryRecord record;
    record.metadata.identity.tenant_id = config.tenant_id;
    record.metadata.identity.principal_id = query.principal_id;
    record.metadata.identity.project_id = config.project_id;
    record.metadata.identity.memory_id = id_digest;
    record.record_id = "instruction:" + id_digest;
    record.scope.tenant_id = config.tenant_id;
    record.scope.project_id = global ? "" : config.project_id;
    record.scope.workspace_id = global ? "" : config.workspace_id;
    record.scope.path_scope = global ? "" : path.parent_path().generic_string();
    record.scope.level = global ? MemoryLevel::System : MemoryLevel::Project;
    record.kind = MemoryKind::Instruction;
    record.authority = Authority::Verified;
    record.status = MemoryStatus::Verified;
    record.source_kind = global ? "global_instruction" : "project_instruction";
    record.source_locator = path.generic_string();
    record.source_digest = content_digest;
    record.content_type = "text/markdown";
    record.content = {{"text", std::move(content)}, {"path", path.generic_string()},
                      {"override", path.filename() == config.override_name}};
    record.trust_class = "versioned_instruction";
    record.purpose = "agent_instruction";
    return record;
}

}  // namespace

ProjectInstructionProvider::ProjectInstructionProvider(ProjectInstructionProviderConfig config)
    : config_(std::move(config)) {
    if(config_.provider_id.empty() || config_.tenant_id.empty() || config_.project_id.empty() ||
       config_.workspace_root.empty() || config_.working_directory.empty())
        throw std::invalid_argument("project instruction provider configuration is incomplete");
}

std::string ProjectInstructionProvider::id() const { return config_.provider_id; }

ProviderResult ProjectInstructionProvider::fetch(const MemoryQuery& query) {
    ProviderResult result;
    result.provider_id = config_.provider_id;
    if(query.subject.tenant_id != config_.tenant_id ||
       (!query.subject.project_id.empty() && query.subject.project_id != config_.project_id))
        return result;
    try {
        std::error_code error;
        const auto root = std::filesystem::weakly_canonical(config_.workspace_root, error);
        if(error) throw std::runtime_error("cannot resolve workspace root");
        const auto cwd = std::filesystem::weakly_canonical(config_.working_directory, error);
        if(error || !within(root, cwd))
            throw std::runtime_error("working directory escapes workspace root");

        std::vector<std::pair<std::filesystem::path, bool>> files;
        if(config_.global_instruction && std::filesystem::is_regular_file(*config_.global_instruction)) {
            const auto global = std::filesystem::weakly_canonical(*config_.global_instruction, error);
            if(error) throw std::runtime_error("cannot resolve global instruction");
            files.emplace_back(global, true);
        }
        auto directory = root;
        while(true) {
            const auto override_path = directory / config_.override_name;
            const auto normal_path = directory / config_.instruction_name;
            const auto selected = std::filesystem::is_regular_file(override_path) ? override_path : normal_path;
            if(std::filesystem::is_regular_file(selected)) {
                const auto canonical = std::filesystem::weakly_canonical(selected, error);
                if(error || !within(root, canonical))
                    throw std::runtime_error("instruction path escapes workspace root");
                files.emplace_back(canonical, false);
            }
            if(directory == cwd) break;
            auto relative = cwd.lexically_relative(directory);
            if(relative.empty() || *relative.begin() == "..")
                throw std::runtime_error("invalid root-to-cwd instruction traversal");
            directory /= *relative.begin();
        }

        for(const auto& [path, global] : files) {
            auto content = read_bounded(path, config_.max_document_bytes);
            result.generation = generation_hash(result.generation, path.generic_string());
            result.generation = generation_hash(result.generation, content);
            auto record = instruction_record(config_, query, path, std::move(content), global);
            if(memory_visible_to(record, query)) result.records.push_back(std::move(record));
        }
    } catch(const std::exception& exception) {
        result.records.clear();
        result.error = exception.what();
    }
    return result;
}

}  // namespace agent_framework::memory_v2
