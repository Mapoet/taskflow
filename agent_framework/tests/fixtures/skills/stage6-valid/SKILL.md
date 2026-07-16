---
api-version: agent.taskflow/v1
kind: Skill
name: stage6-valid
version: 1.0.0
description: Stage 6 isolated runner fixture
permissions:
  tools: [base_value, run_skill_script, run_skill_cli, skill::stage6-valid::value]
  filesystem:
    read: [.]
  secrets: [PRIVATE_TOKEN]
resources:
  references:
    - id: data
      path: references/data.json
      media-type: application/json
  tools:
    - id: value
      path: tools/value.json
  workflows:
    - id: main
      path: workflows/main.json
  scripts:
    - id: normalize
      path: scripts/normalize.sh
      executable: true
    - id: wait
      path: scripts/wait.sh
      executable: true
  cli:
    - id: echo
      path: cli/echo.sh
      executable: true
  tests:
    - id: resource-test
      path: tests/resource.json
    - id: tool-test
      path: tests/tool.json
    - id: workflow-test
      path: tests/workflow.json
    - id: script-test
      path: tests/script.json
    - id: cli-test
      path: tests/cli.json
    - id: digest-negative
      path: tests/digest-negative.json
    - id: output-negative
      path: tests/output-negative.json
    - id: timeout-negative
      path: tests/timeout-negative.json
    - id: cancel-negative
      path: tests/cancel-negative.json
    - id: undeclared-negative
      path: tests/undeclared-negative.json
    - id: secret-negative
      path: tests/secret-negative.json
    - id: parallel-alpha
      path: tests/parallel-alpha.json
    - id: parallel-beta
      path: tests/parallel-beta.json
    - id: parallel-mixed-failure
      path: tests/parallel-mixed-failure.json
---
Stage 6 test package.
