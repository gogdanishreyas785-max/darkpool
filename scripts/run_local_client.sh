#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$HOME/.local/share/darkpool/tls-lan"
CLIENT_BINARY="$REPO_ROOT/build/darkpool_client"
SOCKET_PATH="${DARKPOOL_SOCKET_PATH:-/tmp/darkpool.sock}"
role=player
if [[ $# -gt 0 ]]; then role="$1"; fi
if [[ "$role" == -h || "$role" == --help || "$role" == help ]]; then
  printf 'Usage: bash scripts/run_local_client.sh [player|moderator]\n'
  exit 0
fi
[[ "$#" -le 1 ]] || { printf 'error: specify at most one role\n' >&2; exit 2; }
[[ "$role" == player || "$role" == moderator ]] || { printf 'error: role must be player or moderator\n' >&2; exit 2; }
[[ -x "$CLIENT_BINARY" ]] || { printf 'error: run bash scripts/setup_local.sh first\n' >&2; exit 1; }
if [[ "$role" == moderator ]]; then
  [[ -s "$DATA_DIR/moderator.token" ]] || { printf 'error: run bash scripts/setup_local.sh first\n' >&2; exit 1; }
  export DARKPOOL_MODERATOR_TOKEN="$(<"$DATA_DIR/moderator.token")"
else
  unset DARKPOOL_MODERATOR_TOKEN || true
fi
exec "$CLIENT_BINARY" "$SOCKET_PATH" "$role"
