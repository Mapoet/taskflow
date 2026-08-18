#!/usr/bin/env bash
set -Eeuo pipefail

BINARY="${1:?provider_live_task_semantics binary is required}"
CORPUS="${2:?task semantics corpus is required}"
REPORT="${3:?output report path is required}"

if [[ -z "${AGENT_LLM_PROVIDER:-}" || -z "${AGENT_LLM_MODEL:-}" ]] ||
   [[ -z "${OPENAI_API_KEY:-}" && -z "${ANTHROPIC_API_KEY:-}" ]]; then
  printf 'ProviderLive NotCertified: configure provider, model and credential in the shell\n' >&2
  exit 78
fi
if [[ -z "${AGENT_MCP_ENDPOINT:-}" && -z "${AGENT_TEST_CURSOR_MCP_JSON:-}" ]]; then
  printf 'ProviderLive NotCertified: configure AGENT_MCP_ENDPOINT or AGENT_TEST_CURSOR_MCP_JSON\n' >&2
  exit 78
fi

"${BINARY}" "${CORPUS}" "${REPORT}"
printf 'ProviderLive semantic report: %s\n' "${REPORT}"
