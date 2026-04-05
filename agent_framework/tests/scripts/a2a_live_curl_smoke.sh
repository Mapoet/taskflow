#!/usr/bin/env bash
# Tier C helper: Well-Known + JSON-RPC SendMessage + GetTask (requires curl, jq).
# Usage:
#   BASE_URL=http://127.0.0.1:8080 ./a2a_live_curl_smoke.sh
# Optional: TOKEN=secret for Bearer auth (must match server AGENT_SERVER_AUTH_TOKEN).
set -euo pipefail

BASE_URL="${BASE_URL:-http://127.0.0.1:8080}"
BASE_URL="${BASE_URL%/}"

HDR=()
if [[ -n "${TOKEN:-}" ]]; then
  HDR=(-H "Authorization: Bearer ${TOKEN}")
fi

echo "GET ${BASE_URL}/.well-known/agent-card.json"
curl -sS -f "${HDR[@]}" "${BASE_URL}/.well-known/agent-card.json" | tee /tmp/a2a_card.json >/dev/null
RPC_URL=$(jq -r '.url' /tmp/a2a_card.json)
echo "Card url (JSON-RPC): ${RPC_URL}"

BODY=$(jq -nc \
  --argjson id 1 \
  '{jsonrpc:"2.0",method:"SendMessage",id:$id,params:{message:{messageId:"curl-m",role:"ROLE_USER",parts:[{text:"curl smoke",mediaType:"text/plain"}]}}}')

echo "POST SendMessage -> ${RPC_URL}"
RESP=$(curl -sS -f "${HDR[@]}" -H 'Content-Type: application/json' -d "${BODY}" "${RPC_URL}")
echo "${RESP}" | jq .
TASK_ID=$(echo "${RESP}" | jq -r '.result.task.id // empty')
if [[ -z "${TASK_ID}" ]]; then
  echo "missing task id" >&2
  exit 1
fi

sleep 0.2
GET_BODY=$(jq -nc --arg id "${TASK_ID}" '{jsonrpc:"2.0",method:"GetTask",id:2,params:{id:$id}}')
echo "POST GetTask"
curl -sS -f "${HDR[@]}" -H 'Content-Type: application/json' -d "${GET_BODY}" "${RPC_URL}" | jq .

echo "a2a_live_curl_smoke.sh: ok"
