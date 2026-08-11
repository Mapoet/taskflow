#include <filesystem>
#include <fstream>
#include <iostream>

#include "agent/execution/artifact_executor.hpp"

namespace fs = std::filesystem;
using namespace agent_framework::execution;

#define REQUIRE(x) do { if(!(x)) { std::cerr << "requirement failed: " #x " at " << __LINE__ << '\n'; return 1; } } while(false)

int main() {
    const auto root = fs::temp_directory_path() / "taskflow-phase4-artifact-execution";
    fs::remove_all(root);
    fs::create_directories(root);
    WorkspaceArtifactExecutor executor(root);
    FilesystemArtifactOracle oracle(root);
    const std::vector<ArtifactRequirement> requirements{
        {"source", "src/main.cpp", true, {}}, {"documentation", "README.md", true, {}}};

    ArtifactAction denied{"denied", ArtifactActionKind::WriteText, "outside.txt", "x", "key-denied", false};
    REQUIRE(executor.execute("run-1", denied).error_code == "action_not_approved");
    ArtifactAction escape{"escape", ArtifactActionKind::WriteText, "../escape.txt", "x", "key-escape", true};
    REQUIRE(executor.execute("run-1", escape).error_code == "workspace_escape");
    const auto outside = root.parent_path() / "taskflow-phase4-outside";
    { std::ofstream file(outside); file << "protected"; }
    fs::create_symlink(outside, root / "linked.txt");
    ArtifactAction linked{"linked", ArtifactActionKind::WriteText, "linked.txt", "overwrite", "key-linked", true};
    REQUIRE(executor.execute("run-1", linked).error_code == "workspace_escape");
    std::ifstream protected_file(outside); std::string protected_text; protected_file >> protected_text;
    REQUIRE(protected_text == "protected");
    auto escaped_observation = oracle.verify(ArtifactManifest{}, {{"escape", "../escape.txt", true, {}}});
    REQUIRE(!escaped_observation[0].passed && escaped_observation[0].detail.find("escapes") != std::string::npos);
    fs::remove(root / "linked.txt"); fs::remove(outside);

    ArtifactAction initial{"initial", ArtifactActionKind::WriteText, "src/main.cpp",
                           "int main() { return 0; }\n", "key-initial", true};
    auto first = executor.execute("run-1", initial);
    REQUIRE(first.succeeded);
    auto replay = executor.execute("run-1", initial);
    REQUIRE(replay.succeeded && replay.replayed);
    REQUIRE(replay.effect_digest == first.effect_digest);

    auto initial_observations = oracle.verify(first.manifest, requirements);
    REQUIRE(initial_observations.size() == 2);
    REQUIRE(initial_observations[0].passed);
    REQUIRE(!initial_observations[1].passed);
    REQUIRE(initial_observations[1].finding_id == "artifact:documentation");

    ArtifactAction repair{"repair-documentation", ArtifactActionKind::WriteText, "README.md",
                          "# Complete artifact\n", "key-repair", true};
    auto repaired = executor.execute("run-1", repair, &first.manifest);
    REQUIRE(repaired.succeeded);
    REQUIRE(repaired.manifest.manifest_digest != first.manifest.manifest_digest);
    REQUIRE(!FilesystemArtifactOracle::reusable(initial_observations[0], repaired.manifest.manifest_digest));
    auto forced = oracle.verify(repaired.manifest, requirements);
    REQUIRE(forced[0].passed && forced[1].passed);
    REQUIRE(FilesystemArtifactOracle::reusable(forced[0], repaired.manifest.manifest_digest));

    { std::ofstream tamper(root / "README.md", std::ios::app); tamper << "tampered\n"; }
    auto tampered = oracle.verify(repaired.manifest, requirements);
    REQUIRE(!tampered[1].passed);
    REQUIRE(tampered[1].detail.find("manifest") != std::string::npos);

    REQUIRE(executor.rollback(repaired));
    REQUIRE(!fs::exists(root / "README.md"));
    fs::remove_all(root);
    std::cout << "phase4 artifact execution tests passed\n";
    return 0;
}
