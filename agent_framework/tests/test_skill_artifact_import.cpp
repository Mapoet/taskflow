#include <agent/skill_artifact_import.hpp>
#include <agent/skill_lifecycle.hpp>
#include <agent/task_state_machine.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

SkillArtifactReader reader(std::string bytes, TaskControl* cancel_after = nullptr) {
    return [bytes = std::move(bytes), cancel_after](const SkillArtifactChunkConsumer& sink,
                                                     nlohmann::json*) {
        const std::size_t middle = bytes.size() / 2;
        if(!sink(std::string_view(bytes).substr(0, middle))) return false;
        if(cancel_after) cancel_after->request_cancel();
        return sink(std::string_view(bytes).substr(middle));
    };
}

std::string digest_for(const fs::path& base, const std::string& bytes) {
    const fs::path path = base / "digest.tmp";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << bytes;
    output.close();
    std::string error;
    const auto digest = skill_sha256_file(path, &error);
    assert(digest);
    fs::remove(path);
    return *digest;
}

SkillArtifactSource source_for(const fs::path& base, const std::string& bytes) {
    return {"fixture", bytes.size(), digest_for(base, bytes), reader(bytes)};
}

SkillArchiveEntry entry(std::string path, std::string bytes) {
    SkillArchiveEntry value;
    value.path = std::move(path);
    value.type = SkillArchiveEntryType::Regular;
    value.compressed_size = bytes.size();
    value.expanded_size = bytes.size();
    value.reader = reader(std::move(bytes));
    return value;
}

bool code_is(const nlohmann::json& error, const char* code) {
    return error.value("code", "") == code;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_artifact_import_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    fs::create_directories(base);
    SkillCacheLimits cache_limits;
    cache_limits.max_object_bytes = 64;
    cache_limits.max_total_bytes = 1024;
    auto cache = std::make_shared<SkillResourceCache>(base / "cache", cache_limits);
    SkillArtifactLimits limits{16, 16, 8, 3, 3.0};
    SkillArtifactImporter importer(cache, base / "staging", limits);

    const auto valid = importer.import_archive(
        source_for(base, "archive"), {entry("dir/a.txt", "abc"), entry("b.txt", "def")});
    assert(valid.ok && valid.objects.size() == 2U && valid.leases.size() == 2U);
    assert(fs::is_regular_file(valid.objects[0].path));

    auto oversized_source = source_for(base, std::string(17, 'x'));
    assert(code_is(importer.import_archive(oversized_source, {}).error,
                   "skill_artifact_download_limit"));
    auto bad_digest = source_for(base, "archive");
    bad_digest.expected_sha256 = std::string(64, '0');
    assert(code_is(importer.import_archive(bad_digest, {}).error,
                   "skill_artifact_digest_mismatch"));

    for(const auto& path : {"/absolute", "../escape", "a/../../escape", "C:\\escape"}) {
        assert(code_is(importer.import_archive(source_for(base, "archive"),
                                               {entry(path, "x")}).error,
                       "skill_artifact_path_invalid"));
    }
    assert(code_is(importer.import_archive(source_for(base, "archive"),
        {entry("a", "x"), entry("a", "y")}).error, "skill_artifact_duplicate_path"));

    auto link = entry("link", "x");
    link.type = SkillArchiveEntryType::Symlink;
    assert(code_is(importer.import_archive(source_for(base, "archive"), {link}).error,
                   "skill_artifact_entry_type_forbidden"));
    auto special = entry("device", "x");
    special.type = SkillArchiveEntryType::Special;
    assert(code_is(importer.import_archive(source_for(base, "archive"), {special}).error,
                   "skill_artifact_entry_type_forbidden"));

    assert(code_is(importer.import_archive(source_for(base, "archive"),
        {entry("1", "x"), entry("2", "x"), entry("3", "x"), entry("4", "x")}).error,
        "skill_artifact_entry_count_limit"));
    assert(code_is(importer.import_archive(source_for(base, "archive"),
        {entry("large", std::string(9, 'x'))}).error, "skill_artifact_entry_size_limit"));
    assert(code_is(importer.import_archive(source_for(base, "archive"),
        {entry("a", std::string(8, 'x')), entry("b", std::string(8, 'x')),
         entry("c", "x")}).error, "skill_artifact_expanded_limit"));
    assert(code_is(importer.import_archive(source_for(base, "x"),
        {entry("expanded", "1234")}).error, "skill_artifact_expansion_ratio"));

    TaskControl cancelled;
    auto cancelled_source = source_for(base, "archive");
    cancelled_source.reader = reader("archive", &cancelled);
    const auto cancelled_result = importer.import_archive(cancelled_source,
                                                           {entry("a", "abc")}, &cancelled);
    assert(code_is(cancelled_result.error, "skill_artifact_cancelled"));
    assert(!fs::exists(base / "staging") ||
           fs::directory_iterator(base / "staging") == fs::directory_iterator{});

    fs::remove_all(base, ec);
    std::cout << "test_skill_artifact_import: ok\n";
    return 0;
}
