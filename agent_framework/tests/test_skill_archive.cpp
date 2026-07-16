#include <agent/skills/skill_archive.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace agent_framework;

namespace {

void write(const fs::path& path, const std::string& value) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << value;
    assert(output.good());
}

std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

} // namespace

int main() {
    const auto root = fs::temp_directory_path() /
        ("taskflow-skill-archive-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto source = root / "source";
    fs::create_directories(source);
    write(source / "SKILL.md", "---\nid: org.example.archive\nversion: 1.0.0\n---\nDemo\n");
    write(source / "scripts" / "run.sh", "#!/bin/sh\necho deterministic\n");
    fs::permissions(source / "scripts" / "run.sh", fs::perms::owner_all |
                    fs::perms::group_read | fs::perms::group_exec |
                    fs::perms::others_read | fs::perms::others_exec,
                    fs::perm_options::replace);

    const auto first = root / "first.tfskill";
    const auto second = root / "second.tfskill";
    auto built = build_skill_archive(source, first);
    assert(built.ok && built.entries.size() == 2);
    fs::last_write_time(source / "SKILL.md", fs::file_time_type::clock::now());
    fs::permissions(source / "SKILL.md", fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace);
    auto rebuilt = build_skill_archive(source, second);
    assert(rebuilt.ok && built.archive_digest == rebuilt.archive_digest);
    assert(read(first) == read(second));

    const auto extracted = root / "extracted";
    auto unpacked = extract_skill_archive(first, extracted);
    assert(unpacked.ok);
    assert(read(extracted / "scripts" / "run.sh") == "#!/bin/sh\necho deterministic\n");
    assert((fs::status(extracted / "scripts" / "run.sh").permissions() & fs::perms::owner_exec) !=
           fs::perms::none);

    write(source / "SKILL.md", "changed");
    const auto changed_path = root / "changed.tfskill";
    auto changed = build_skill_archive(source, changed_path);
    assert(changed.ok && changed.archive_digest != built.archive_digest);

    auto corrupt = read(first);
    corrupt[40] ^= 1;
    const auto corrupt_path = root / "corrupt.tfskill";
    write(corrupt_path, corrupt);
    assert(!inspect_skill_archive(corrupt_path).ok);

    auto compressed = read(first);
    compressed[8] = 8;
    const auto compressed_path = root / "compressed.tfskill";
    write(compressed_path, compressed);
    assert(!inspect_skill_archive(compressed_path).ok);

    const auto linked_source = root / "linked";
    fs::create_directories(linked_source);
    write(linked_source / "regular", "data");
    fs::create_symlink(linked_source / "regular", linked_source / "link");
    assert(!build_skill_archive(linked_source, root / "linked.tfskill").ok);

    const auto collision_source = root / "collision";
    fs::create_directories(collision_source);
    write(collision_source / "A", "one");
    write(collision_source / "a", "two");
    assert(!build_skill_archive(collision_source, root / "collision.tfskill").ok);

    SkillArchiveLimits tiny;
    tiny.max_entry_bytes = 2;
    assert(!build_skill_archive(source, root / "tiny.tfskill", {}, tiny).ok);

    fs::remove_all(root);
    std::cout << "skill archive tests passed\n";
}
