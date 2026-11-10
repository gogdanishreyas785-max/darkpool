#!/usr/bin/env bash
set -Eeuo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "$0")" && pwd)"
REPO_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd)"
DATA_DIR="$HOME/.local/share/darkpool/tls-lan"
TOKEN_DIR="${DARKPOOL_TOKEN_DIR:-$HOME/.darkpool/tokens}"
CLIENT_BINARY="$REPO_ROOT/build/darkpool_client"
role=player
sync_only=0
sync_match_id=""
if [[ $# -gt 0 ]]; then role="$1"; fi
if [[ "$role" == -h || "$role" == --help || "$role" == help ]]; then
  printf 'Usage: bash scripts/run_tls_client.sh [player|moderator]\n'
  printf '       bash scripts/run_tls_client.sh player --sync-only MATCH_ID\n'
  exit 0
fi
if [[ "$#" -eq 3 && "$2" == --sync-only ]]; then
  sync_only=1
  sync_match_id="$3"
elif [[ "$#" -gt 1 ]]; then
  printf 'error: expected [player|moderator] or player --sync-only MATCH_ID\n' >&2
  exit 2
fi
[[ "$role" == player || "$role" == moderator ]] || { printf 'error: role must be player or moderator\n' >&2; exit 2; }
[[ "$sync_only" -eq 0 || "$role" == player ]] || { printf 'error: token sync is available for player role only\n' >&2; exit 2; }
if [[ "$sync_only" -eq 1 ]]; then
  [[ "$sync_match_id" =~ ^[1-9][0-9]*$ ]] || { printf 'error: MATCH_ID must be a positive integer\n' >&2; exit 2; }
fi
[[ -x "$CLIENT_BINARY" ]] || { printf 'error: run bash scripts/setup_tls_lan.sh client ... first\n' >&2; exit 1; }
[[ -s "$DATA_DIR/server-ip" && -s "$DATA_DIR/ca-cert.pem" ]] || { printf 'error: TLS client setup is incomplete\n' >&2; exit 1; }
install -d -m 700 "$TOKEN_DIR"
chmod 700 "$TOKEN_DIR"
export DARKPOOL_TOKEN_DIR="$TOKEN_DIR"
server_ip="$(<"$DATA_DIR/server-ip")"
if [[ "$role" == moderator ]]; then
  [[ "$sync_only" -eq 0 ]] || { printf 'error: token sync is available for player role only\n' >&2; exit 2; }
  [[ -s "$DATA_DIR/moderator.token" ]] || { printf 'error: rerun client setup with role moderator to retrieve the token over SSH\n' >&2; exit 1; }
  export DARKPOOL_MODERATOR_TOKEN="$(<"$DATA_DIR/moderator.token")"
  exec "$CLIENT_BINARY" --tls --ca-cert "$DATA_DIR/ca-cert.pem" "$server_ip" 4783 moderator
fi

export DARKPOOL_AUTO_JOIN=1
if [[ -s "$DATA_DIR/server-ssh-user" ]]; then
  server_ssh_user="$(<"$DATA_DIR/server-ssh-user")"
  printf '[PROVISION] Syncing match admission tokens from %s@%s over SSH/SCP...\n' "$server_ssh_user" "$server_ip"
  umask 077
  if scp -q -o ConnectTimeout=5 "$server_ssh_user@$server_ip:.darkpool/tokens/match_*" "$TOKEN_DIR/" 2>/dev/null; then
    chmod 600 "$TOKEN_DIR"/match_* 2>/dev/null || true
    printf '[PROVISION] Token synchronization completed.\n'
  else
    if [[ "$sync_only" -eq 1 ]]; then
      printf '[ERROR] Match token synchronization failed. Check SSH setup and that the protected match exists.\n' >&2
      exit 1
    fi
    printf '[INFO] No token files were synced. Launch this player after the moderator creates a protected match, or use the match token shared by the moderator.\n'
  fi
else
  if [[ "$sync_only" -eq 1 ]]; then
    printf '[ERROR] SSH token synchronization is not configured.\n' >&2
    exit 1
  fi
  printf '[INFO] SSH token synchronization is not configured; use the match token provided by the moderator.\n'
fi
if [[ "$sync_only" -eq 1 ]]; then
  [[ -s "$TOKEN_DIR/match_${sync_match_id}.token" ]] || {
    printf '[ERROR] The requested match token was not provisioned.\n' >&2
    exit 1
  }
  printf '[PROVISION] Match token synchronized for match %s.\n' "$sync_match_id"
  exit 0
fi
exec "$CLIENT_BINARY" --tls --ca-cert "$DATA_DIR/ca-cert.pem" "$server_ip" 4783 player
