#include <agent/agent_template/session.hpp>

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace agent_framework;
using namespace agent_framework::agent_template;
namespace fs = std::filesystem;
void write(const fs::path &p, const std::string &s)
{
  fs::create_directories(p.parent_path());
  std::ofstream f(p);
  f << s;
}
contracts::ContractMetadata meta()
{
  contracts::ContractMetadata m;
  m.identity.tenant_id = "tenant";
  m.identity.task_id = "task";
  return m;
}
int main()
{
  const auto root = fs::temp_directory_path() / "agent_template_session";
  std::error_code ec;
  fs::remove_all(root, ec);
  write(root / "search/SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: literature-search
version: 1.0.0
description: literature retrieval worker
tags: [retrieval, literature-search]
trigger-keywords: [literature, search]
permissions:
  tools: [web_search]
resources:
  - id: search
    kind: tool
    path: web_search
    license: MIT
---
Search literature.)");
  write(root / "verify/SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: citation-verifier
version: 1.0.0
description: verify citations
tags: [verification]
trigger-keywords: [citation]
permissions:
  tools: [web_fetch]
---
Verify.)");
  SkillRegistry registry(root);
  registry.scan_or_reload();
  assert(registry.valid());
  auto snapshot = registry.snapshot();
  SkillCandidateRetriever retriever;
  CandidateQuery query;
  query.task = "search literature and verify citation";
  query.required_capabilities = {"literature-search"};
  auto candidates = retriever.retrieve(snapshot, query);
  assert(candidates.size() == 1 && candidates[0].skill_id == "literature-search");
  auto again = retriever.retrieve(snapshot, query);
  assert(again[0].skill_id == candidates[0].skill_id && again[0].score == candidates[0].score);
  SkillCollaborationPlan plan;
  plan.metadata = meta();
  plan.plan_id = "plan";
  SkillPlanNode node;
  node.node_id = "search";
  node.resolved_skill_id = "literature-search";
  node.resolved_skill_version = "1.0.0";
  node.resolved_skill_digest = snapshot.get("literature-search")->package_digest;
  plan.nodes = {node};
  PermissionEnvelope parent;
  parent.tools = {"web_search", "web_fetch"};
  SessionBuildRequest request{meta(), "session", plan, snapshot, parent, "sha256:model", "sha256:prompt", "deploy"};
  ActiveSkillSessionBuilder builder;
  auto built = builder.build(request);
  if (!built.session)
    for (const auto &i : built.issues)
      std::cerr << i.code << ":" << i.message << "\n";
  assert(built.session);
  assert(built.session->skills.size() == 1);
  assert(built.session->skills[0].effective_permissions.tools == std::vector<std::string>{"web_search"});
  // Reload publishes a new generation, while the active session remains pinned to the old one.
  write(root / "search/SKILL.md", R"(---
api-version: agent.taskflow/v1
kind: Skill
name: literature-search
version: 2.0.0
description: changed
permissions: {tools: [web_search]}
---
Changed.)");
  registry.scan_or_reload();
  assert(registry.snapshot().generation() > snapshot.generation());
  assert(built.session->skills[0].version == "1.0.0");
  auto mismatch = request;
  mismatch.plan.nodes[0].resolved_skill_version = "9.0.0";
  assert(!builder.build(mismatch).session);
  PermissionEnvelope escalation;
  escalation.tools = {"web_search", "danger"};
  std::string reason;
  assert(!permission_is_subset(escalation, parent, &reason));
  fs::remove_all(root, ec);
  std::cout << "test_agent_template_session: ok\n";
}
