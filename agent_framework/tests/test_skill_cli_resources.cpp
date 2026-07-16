#include <agent/skill_command.hpp>
#include <agent/skill_lifecycle.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>

namespace {
namespace fs = std::filesystem;
using namespace agent_framework;

void write_file(const fs::path& path, const std::string& content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << content;
}

std::string digest(const fs::path& path) {
    std::string error;
    const auto value = skill_sha256_file(path, &error);
    assert(value);
    return *value;
}
} // namespace

int main() {
    const fs::path base = fs::temp_directory_path() / "agent_skill_cli_resources_test";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path package = base / "skills/demo";
    write_file(package / "references/guide.txt", "Alpha alpha\nweather data\n");
    write_file(package / "models/model.onnx", "model-data");
    const auto reference_digest = digest(package / "references/guide.txt");
    const auto model_digest = digest(package / "models/model.onnx");
    std::ostringstream manifest;
    manifest << "---\napi-version: agent.taskflow/v1\nkind: Skill\nname: demo\n"
        "version: 1.0.0\ndescription: resource cli fixture\nresources:\n  references:\n"
        "    - id: guide\n      path: references/guide.txt\n      media-type: text/plain\n"
        "      read-mode: stream\n      sha256: " << reference_digest << "\n"
        "  models:\n    - id: forecast\n      path: models/model.onnx\n"
        "      media-type: application/onnx\n      read-mode: mmap\n      sha256: " << model_digest << "\n"
        "      size: 10\n      license: Apache-2.0\n      source: package://models/model.onnx\n"
        "      runtime: onnxruntime\n      requirements:\n        devices: [cpu]\n"
        "        precisions: [fp32]\n        min-memory-bytes: 1\n---\ndemo\n";
    write_file(package / "SKILL.md", manifest.str());

    auto registry = std::make_shared<SkillRegistry>(base / "skills");
    registry->scan_or_reload();
    assert(registry->get("demo"));
    SkillCommandService service(registry, base / "cache");
    const auto page = service.reference_page("demo", "guide", 0, 8);
    assert(page.ok() && page.data.at("nextOffset") == 8U);
    assert(page.to_json().at("apiVersion") == "agent.taskflow/skillctl-output/v1");
    const auto search = service.reference_search("demo", "guide", "ALPHA", 4);
    assert(search.ok() && search.data.at("hits").size() == 1U);
    assert(service.reference_page("missing", "guide", 0, 8).exit == SkillCliExit::NotFound);
    assert(service.reference_page("demo", "missing", 0, 8).exit == SkillCliExit::OperationFailed);

    SkillModelHostCapabilities host;
    host.runtimes = {"onnxruntime"};
    host.devices = {"cpu"};
    host.precisions = {"fp32"};
    host.available_memory_bytes = 1024;
    host.max_readonly_bytes = 1024;
    host.mmap_supported = true;
    const auto model = service.model_check("demo", "forecast", host);
    assert(model.ok() && model.data.at("compatible"));
    host.devices = {"tpu"};
    const auto incompatible = service.model_check("demo", "forecast", host);
    assert(incompatible.exit == SkillCliExit::DependencyUnavailable);

    auto cache = std::make_shared<SkillResourceCache>(base / "cache");
    SkillResourceHandle cache_handle;
    cache_handle.path = package / "models/model.onnx";
    cache_handle.size = 10;
    cache_handle.view_size = 10;
    cache_handle.resource_digest = model_digest;
    cache_handle.package_digest = "package";
    cache_handle.descriptor.id = "forecast";
    cache_handle.descriptor.media_type = "application/onnx";
    auto cached = cache->acquire(cache_handle);
    assert(cached.ok);
    assert(service.cache_status().data.at("objects") == 1U);
    assert(service.cache_pin(model_digest, true).ok());
    assert(service.cache_pin(model_digest, false).ok());
    cached.lease.reset();
    assert(service.cache_gc().ok());
    write_file(cached.object->path, "broken!!!x");
    assert(service.cache_verify().ok());
    assert(service.cache_status().data.at("objects") == 0U);
    assert(service.cache_pin(std::string(64, '0'), true).exit == SkillCliExit::OperationFailed);

    fs::remove_all(base, ec);
    std::cout << "test_skill_cli_resources: ok\n";
    return 0;
}
