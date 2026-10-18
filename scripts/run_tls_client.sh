#!/usr/bin/env bash
# scripts/run_tls_client.sh
# Runs darkpool_client over TLS 1.3 as moderator or player with automatic token provisioning.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${HOME}/.darkpool/config"

if [[ -f "${CONFIG_FILE}" ]]; then
  # shellcheck source=/dev/null
  source "${CONFIG_FILE}"
fi

ROLE="${1:-player}"
SERVER_HOST="${2:-${DARKPOOL_SERVER_HOST:-127.0.0.1}}"
SERVER_PORT="${3:-${DARKPOOL_SERVER_PORT:-4783}}"
FINGERPRINT="${4:-${DARKPOOL_SERVER_FINGERPRINT:-}}"

# If fingerprint was not passed or in config, but connecting to localhost and cert exists, compute it
if [[ -z "${FINGERPRINT}" ]]; then
  LOCAL_CRT="${HOME}/.darkpool/certs/server.crt"
  if [[ -f "${LOCAL_CRT}" && ( "${SERVER_HOST}" == "127.0.0.1" || "${SERVER_HOST}" == "localhost" ) ]]; then
    FINGERPRINT=$(openssl x509 -noout -fingerprint -sha256 -in "${LOCAL_CRT}" | tr -d ':' | awk -F= '{print tolower($2)}')
  fi
fi

if [[ -z "${FINGERPRINT}" ]]; then
  echo "[ERROR] Server SHA-256 leaf certificate fingerprint is required." >&2
  echo "Provide as argument #4, in ~/.darkpool/config, or export DARKPOOL_SERVER_FINGERPRINT." >&2
  exit 1
fi

# Locate client binary
CLIENT_BIN=""
for candidate in \
  "${REPO_ROOT}/build-prod/darkpool_client" \
  "${REPO_ROOT}/build/darkpool_client" \
  "${REPO_ROOT}/build-debug/darkpool_client" \
  "${REPO_ROOT}/build-wsl/darkpool_client"; do
  if [[ -x "${candidate}" ]]; then
    CLIENT_BIN="${candidate}"
    break
  fi
done

if [[ -z "${CLIENT_BIN}" ]]; then
  echo "[ERROR] darkpool_client binary not found. Build the project first via cmake." >&2
  exit 1
fi

TOKEN_DIR="${DARKPOOL_TOKEN_DIR:-${HOME}/.darkpool/tokens}"
mkdir -p "${TOKEN_DIR}"
chmod 0700 "${TOKEN_DIR}"

if [[ "${ROLE}" == "moderator" ]]; then
  export DARKPOOL_MODERATOR_TOKEN="${DARKPOOL_MODERATOR_TOKEN:-admin-secret-token-2026}"
  echo "[CLIENT] Connecting to ${SERVER_HOST}:${SERVER_PORT} as MODERATOR..."
elif [[ "${ROLE}" == "player" ]]; then
  # If remote server and SSH provisioning host is configured, attempt secure sync
  SSH_USER="${DARKPOOL_SSH_USER:-$(whoami)}"
  SSH_HOST="${DARKPOOL_SSH_HOST:-}"
  
  if [[ -n "${SSH_HOST}" && "${SSH_HOST}" != "127.0.0.1" && "${SSH_HOST}" != "localhost" ]]; then
    echo "[PROVISION] Synchronizing match tokens from ${SSH_USER}@${SSH_HOST} via SSH/SCP..."
    scp -q -o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=3 \
      "${SSH_USER}@${SSH_HOST}:.darkpool/tokens/match_*.token" "${TOKEN_DIR}/" 2>/dev/null || true
  fi

  # Enable automatic admission resolution in client
  export DARKPOOL_AUTO_JOIN="1"
  export DARKPOOL_TOKEN_DIR="${TOKEN_DIR}"

  # If a specific match token exists, notify user
  LATEST_TOKEN_FILE=$(ls -t "${TOKEN_DIR}"/match_*.token 2>/dev/null | head -n1 || true)
  if [[ -n "${LATEST_TOKEN_FILE}" && -f "${LATEST_TOKEN_FILE}" ]]; then
    MATCH_NUM=$(basename "${LATEST_TOKEN_FILE}" | sed -E 's/match_([0-9]+)\.token/\1/')
    echo "[PROVISION] Auto-admission credential available for Match #${MATCH_NUM}."
    echo "[PROVISION] You may simply run 'join' or 'join ${MATCH_NUM}' inside the CLI."
  fi
  echo "[CLIENT] Connecting to ${SERVER_HOST}:${SERVER_PORT} as PLAYER..."
else
  echo "[ERROR] Unknown role: ${ROLE}. Supported roles: 'moderator', 'player'." >&2
  exit 1
fi

exec "${CLIENT_BIN}" \
  --tls \
  --server-fingerprint "${FINGERPRINT}" \
  "${SERVER_HOST}" \
  "${SERVER_PORT}" \
  "${ROLE}"
