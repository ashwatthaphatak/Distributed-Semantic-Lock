#!/usr/bin/env bash
# Author: Ayush Gala
set -euo pipefail

# ---------------------------------------------------------------------------
# agent_request.sh — Simulate an AI agent acquiring a semantic lock via DSCC.
#
# Converts a plain-text payload into an embedding (via Ollama) and sends a
# gRPC AcquireGuard request to the distributed semantic lock cluster, just
# like a real AI agent would in production.
#
# Prerequisites:
#   - The Docker Compose stack must be running (docker compose up)
#   - grpcurl must be installed  (brew install grpcurl)
#   - jq must be installed       (brew install jq)
#   - curl must be installed     (usually pre-installed)
# ---------------------------------------------------------------------------

PROTO_DIR="$(cd "$(dirname "$0")/../.." && pwd)/proto"
PROTO_FILE="dscc.proto"

EMBEDDING_HOST="${EMBEDDING_HOST:-127.0.0.1}"
EMBEDDING_PORT="${EMBEDDING_PORT:-7997}"
EMBEDDING_MODEL="${EMBEDDING_MODEL:-all-minilm:latest}"

DSCC_HOST="${DSCC_HOST:-127.0.0.1}"
DSCC_PORT="${DSCC_PORT:-50050}"

AGENT_ID="${AGENT_ID:-agent-$(date +%s)-$$}"

usage() {
    cat <<EOF
Usage: $(basename "$0") <read|write> "<payload text>"

Arguments:
  read|write       The operation directive — 'write' for mutations, 'read' for queries.
  payload text     The natural-language description of the work the agent wants to do.

Environment overrides:
  EMBEDDING_HOST   Ollama host             (default: 127.0.0.1)
  EMBEDDING_PORT   Ollama port             (default: 7997)
  EMBEDDING_MODEL  Embedding model name    (default: all-minilm:latest)
  DSCC_HOST        Lock service host       (default: 127.0.0.1)
  DSCC_PORT        Lock service gRPC port  (default: 50050, the proxy)
  AGENT_ID         Custom agent identifier (default: agent-<epoch>-<pid>)

Examples:
  $(basename "$0") write "Review the massing concept and prioritize passive cooling"
  $(basename "$0") read  "Check current daylight access metrics for the west facade"
  DSCC_PORT=50051 $(basename "$0") write "Update structural load calculations"
EOF
    exit 1
}

# ---- Argument validation ---------------------------------------------------

if [[ $# -lt 2 ]]; then
    usage
fi

DIRECTIVE="$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')"
PAYLOAD="$2"

case "$DIRECTIVE" in
    read)  OP_TYPE="OPERATION_TYPE_READ"  ;;
    write) OP_TYPE="OPERATION_TYPE_WRITE" ;;
    *)
        echo "Error: directive must be 'read' or 'write', got '$1'" >&2
        exit 1
        ;;
esac

for cmd in grpcurl jq curl; do
    if ! command -v "$cmd" &>/dev/null; then
        echo "Error: '$cmd' is required but not found. Install it first." >&2
        exit 1
    fi
done

# ---- Step 1: Generate embedding via Ollama ---------------------------------

echo "==> Agent ID:   $AGENT_ID"
echo "==> Directive:  $DIRECTIVE"
echo "==> Payload:    $PAYLOAD"
echo ""
echo "--- Step 1: Requesting embedding from Ollama ($EMBEDDING_HOST:$EMBEDDING_PORT) ---"

ESCAPED_PAYLOAD=$(printf '%s' "$PAYLOAD" | jq -Rs .)

ESCAPED_MODEL=$(printf '%s' "$EMBEDDING_MODEL" | jq -Rs .)

EMBED_RESPONSE=$(curl -sf \
    "http://${EMBEDDING_HOST}:${EMBEDDING_PORT}/v1/embeddings" \
    -H "Content-Type: application/json" \
    -d "{\"model\":${ESCAPED_MODEL},\"input\":${ESCAPED_PAYLOAD}}" \
)

if [[ -z "$EMBED_RESPONSE" ]]; then
    echo "Error: Empty response from embedding service. Is Ollama running?" >&2
    exit 1
fi

EMBEDDING_JSON=$(echo "$EMBED_RESPONSE" | jq -c '[.data[0].embedding[]]')
EMBED_DIM=$(echo "$EMBEDDING_JSON" | jq 'length')
echo "    Embedding received (${EMBED_DIM} dimensions)"

# ---- Step 2: Build and send the AcquireGuard gRPC request ------------------

TIMESTAMP_MS=$(( $(date +%s) * 1000 ))

echo ""
echo "--- Step 2: Sending AcquireGuard to dscc ($DSCC_HOST:$DSCC_PORT) ---"
echo "    operation_type: $OP_TYPE"

GRPC_RESPONSE=$(grpcurl -plaintext \
    -import-path "$PROTO_DIR" \
    -proto "$PROTO_FILE" \
    -d "$(jq -nc \
        --arg aid "$AGENT_ID" \
        --argjson emb "$EMBEDDING_JSON" \
        --arg ptxt "$PAYLOAD" \
        --arg src "agent_request.sh" \
        --argjson ts "$TIMESTAMP_MS" \
        --arg op "$OP_TYPE" \
        '{
            agent_id: $aid,
            embedding: $emb,
            payload_text: $ptxt,
            source_file: $src,
            timestamp_unix_ms: ($ts | tostring),
            operation_type: $op
        }'
    )" \
    "${DSCC_HOST}:${DSCC_PORT}" \
    dscc.LockService/AcquireGuard \
    2>&1) || true

echo ""
echo "--- Response ---"
echo "$GRPC_RESPONSE" | jq . 2>/dev/null || echo "$GRPC_RESPONSE"

GRANTED=$(echo "$GRPC_RESPONSE" | jq -r '.granted // empty' 2>/dev/null)
if [[ "$GRANTED" == "true" ]]; then
    echo ""
    echo "==> Lock GRANTED"
    WAIT_MS=$(echo "$GRPC_RESPONSE" | jq -r '.lockWaitMs // "0"')
    echo "    Wait time: ${WAIT_MS}ms"
else
    echo ""
    MSG=$(echo "$GRPC_RESPONSE" | jq -r '.message // empty' 2>/dev/null)
    REDIRECT=$(echo "$GRPC_RESPONSE" | jq -r '.leaderRedirect // empty' 2>/dev/null)
    if [[ "$MSG" == "NOT_LEADER" && -n "$REDIRECT" ]]; then
        echo "==> NOT the leader. Retry against: $REDIRECT"
    else
        echo "==> Lock NOT granted. ${MSG:+Message: $MSG}"
    fi
fi
