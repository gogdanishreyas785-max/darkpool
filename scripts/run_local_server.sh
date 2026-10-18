#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$HOME/.local/share/darkpool/tls-lan"
TOKEN_DIR="$HOME/.darkpool/tokens"
SERVER_BINARY="$REPO_ROOT/build/darkpool_server"

[[ -x "$SERVER_BINARY" ]] || { printf 'error: run bash scripts/setup_local.sh first\n' >&2; exit 1; }
[[ -s "$DATA_DIR/moderator.token" ]] || { printf 'error: run bash scripts/setup_local.sh first\n' >&2; exit 1; }
export DARKPOOL_MODERATOR_TOKEN="$(<"$DATA_DIR/moderator.token")"
install -d -m 700 "$TOKEN_DIR"
chmod 700 "$TOKEN_DIR"
export DARKPOOL_PROVISION_TOKENS=1
export DARKPOOL_TOKEN_DIR="$TOKEN_DIR"
printf 'Starting DARKPOOL on this machine using /tmp/darkpool.sock.\n'
printf 'Match tokens are stored securely under %s; players can use join without copying a token.\n' "$TOKEN_DIR"
printf 'Open another terminal for the moderator and one terminal per player.\n'
exec "$SERVER_BINARY"
