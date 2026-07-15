#include <agent/skill_doctor.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>

namespace {

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    assert(output.good());
    output << content;
}

} // namespace

int main() {
    using namespace agent_framework;
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / "agent_skill_doctor_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    const auto package = root / "doctor-fixture";
    write_file(package / "cli" / "helper", "#!/bin/sh\nexit 0\n");
    write_file(package / "models" / "weights.bin", "model");
    write_file(package / "mcp" / "server.json", "{not-json");
    write_file(package / "SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: doctor-fixture
version: 1.0.0
description: offline doctor fixture
permissions:
  filesystem:
    read:
      - data/
  secrets:
    - PRIVATE_TOKEN
resources:
  cli:
    - id: helper
      path: cli/helper
      runtime: missing-shell
      executable: true
  models:
    - id: weights
      path: models/weights.bin
      media-type: application/octet-stream
      read-mode: mmap
      sha256: 9372c470eeadd5ecd9c3c74c2b3cb633f8e2f2fad799250a0f70d652b6b825e4
      size: 5
      license: Apache-2.0
      source: package://models/weights.bin
      runtime: missing-model-runtime
      requirements:
        devices:
          - cpu
        precisions:
          - fp32
        min-memory-bytes: 1
  mcp:
    - id: server
      path: mcp/server.json
      sha256: f1dec6e9ee608550bd1c39ff2b90134059bac5d02e4e78f6410aed2fbd870bd0
---
doctor
)");

    auto registry = std::make_shared<SkillRegistry>(root);
    registry->scan_or_reload();
    assert(registry->get("doctor-fixture"));
    write_file(package / "mcp" / "server.json", "{changed");
    SkillDoctor doctor(registry);
    SkillDoctorOptions options;
    const auto report = doctor.inspect("doctor-fixture", options);
    assert(!report.ready);
    std::set<std::string> codes;
    for(const auto& diagnostic : report.diagnostics) codes.insert(diagnostic.code);
    assert(codes.contains("skill_doctor_runtime_missing"));
    assert(codes.contains("skill_doctor_cli_unavailable"));
    assert(codes.contains("skill_doctor_model_unavailable"));
    assert(codes.contains("skill_doctor_mcp_malformed"));
    assert(codes.contains("skill_doctor_digest_mismatch"));
    assert(codes.contains("skill_doctor_filesystem_grant_insufficient"));
    assert(report.to_json().dump().find("PRIVATE_TOKEN") == std::string::npos);

    fs::remove_all(root, ec);
    std::cout << "test_skill_doctor: ok\n";
    return 0;
}
