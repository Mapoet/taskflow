---
api-version: agent.taskflow/v1
kind: Skill
name: research-helper
version: 0.1.0
description: Reproducible evidence and implementation checklist for technical research
permissions:
  tools: []
  filesystem:
    read: [.]
resources:
  references:
    - id: checklist
      path: references/checklist.md
      media-type: text/markdown
---

# Research helper

Use the declared checklist to structure evidence, assumptions, implementation details, and reproducibility notes. Do not claim that an external source or command was checked unless the corresponding evidence is present.
