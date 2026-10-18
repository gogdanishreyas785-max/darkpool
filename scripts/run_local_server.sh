#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$HOME/.local/share/darkpool/tls-lan"
SERVER_BINARY="$REPO_ROOT/build/darkpool_server"

[[ -x "$SERVER_BINARY" ]] || { printf 'error: run bash scripts/setup_local.sh first\n' >&2; exit 1; }
[[ -s "$DATA_DIR/moderator.token" ]] || { printf 'error: run bash scripts/setup_local.sh first\n' >&2; exit 1; }
export DARKPOOL_MODERATOR_TOKEN="$(<"$DATA_DIR/moderator.token")"
printf 'Starting DARKPOOL on this machine using /tmp/darkpool.sock.\n'
printf 'Open another terminal for the moderator and one terminal per player.\n'
exec "$SERVER_BINARY"
