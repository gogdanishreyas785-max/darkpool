#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$HOME/.local/share/darkpool/tls-lan"
CLIENT_BINARY="$REPO_ROOT/build/darkpool_client"
role=player
if [[ $# -gt 0 ]]; then role="$1"; fi
if [[ "$role" == -h || "$role" == --help || "$role" == help ]]; then
  printf 'Usage: bash scripts/run_tls_client.sh [player|moderator]\n'
  exit 0
fi
[[ "$role" == player || "$role" == moderator ]] || { printf 'error: role must be player or moderator\n' >&2; exit 2; }
[[ -x "$CLIENT_BINARY" ]] || { printf 'error: run bash scripts/setup_tls_lan.sh client ... first\n' >&2; exit 1; }
[[ -s "$DATA_DIR/server-ip" && -s "$DATA_DIR/ca-cert.pem" ]] || { printf 'error: TLS client setup is incomplete\n' >&2; exit 1; }
server_ip="$(<"$DATA_DIR/server-ip")"
if [[ "$role" == moderator ]]; then
  [[ -s "$DATA_DIR/moderator.token" ]] || { printf 'error: rerun client setup with role moderator to retrieve the token over SSH\n' >&2; exit 1; }
  export DARKPOOL_MODERATOR_TOKEN="$(<"$DATA_DIR/moderator.token")"
  exec "$CLIENT_BINARY" --tls --ca-cert "$DATA_DIR/ca-cert.pem" "$server_ip" 4783 moderator
fi
exec "$CLIENT_BINARY" --tls --ca-cert "$DATA_DIR/ca-cert.pem" "$server_ip" 4783 player
