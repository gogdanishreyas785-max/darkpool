#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$HOME/.local/share/darkpool/tls-lan"
SERVER_BINARY="$REPO_ROOT/build/darkpool_server"

[[ -x "$SERVER_BINARY" ]] || { printf 'error: run bash scripts/setup_tls_lan.sh server first\n' >&2; exit 1; }
[[ -s "$DATA_DIR/server-ip" && -s "$DATA_DIR/server-cert.pem" && -s "$DATA_DIR/server-key.pem" && -s "$DATA_DIR/moderator.token" ]] || {
  printf 'error: TLS setup is incomplete; run bash scripts/setup_tls_lan.sh server first\n' >&2; exit 1;
}
server_ip="$(<"$DATA_DIR/server-ip")"
export DARKPOOL_MODERATOR_TOKEN="$(<"$DATA_DIR/moderator.token")"
printf 'Starting DARKPOOL TLS 1.3 listener on %s:4783; press Ctrl+C to stop.\n' "$server_ip"
exec "$SERVER_BINARY" --tls-bind-address "$server_ip" --tls-port 4783 --tls-cert "$DATA_DIR/server-cert.pem" --tls-key "$DATA_DIR/server-key.pem"
