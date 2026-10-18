#!/usr/bin/env bash
# scripts/run_tls_server.sh
# Starts darkpool_server over TLS 1.3 with automated token provisioning enabled.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
CONFIG_FILE="${HOME}/.darkpool/config"

if [[ -f "${CONFIG_FILE}" ]]; then
  # shellcheck source=/dev/null
  source "${CONFIG_FILE}"
fi

# Locate server binary
SERVER_BIN=""
for candidate in \
  "${REPO_ROOT}/build-prod/darkpool_server" \
  "${REPO_ROOT}/build/darkpool_server" \
  "${REPO_ROOT}/build-debug/darkpool_server" \
  "${REPO_ROOT}/build-wsl/darkpool_server"; do
  if [[ -x "${candidate}" ]]; then
    SERVER_BIN="${candidate}"
    break
  fi
done

if [[ -z "${SERVER_BIN}" ]]; then
  echo "[ERROR] darkpool_server binary not found. Build the project first via cmake." >&2
  exit 1
fi

BIND_ADDR="${DARKPOOL_SERVER_BIND:-0.0.0.0}"
PORT="${DARKPOOL_SERVER_PORT:-4783}"
CERT="${DARKPOOL_CERT_PATH:-${HOME}/.darkpool/certs/server.crt}"
KEY="${DARKPOOL_KEY_PATH:-${HOME}/.darkpool/certs/server.key}"
MOD_TOKEN="${DARKPOOL_MODERATOR_TOKEN:-admin-secret-token-2026}"
TOKEN_DIR="${DARKPOOL_TOKEN_DIR:-${HOME}/.darkpool/tokens}"

if [[ ! -f "${CERT}" || ! -f "${KEY}" ]]; then
  echo "[NOTICE] TLS certificate/key not found. Running setup_tls_lan.sh first..."
  bash "${SCRIPT_DIR}/setup_tls_lan.sh"
  if [[ -f "${CONFIG_FILE}" ]]; then
    # shellcheck source=/dev/null
    source "${CONFIG_FILE}"
    CERT="${DARKPOOL_CERT_PATH}"
    KEY="${DARKPOOL_KEY_PATH}"
  fi
fi

# Ensure tokens directory exists with strict permissions
mkdir -p "${TOKEN_DIR}"
chmod 0700 "${TOKEN_DIR}"

export DARKPOOL_MODERATOR_TOKEN="${MOD_TOKEN}"
export DARKPOOL_PROVISION_TOKENS="1"
export DARKPOOL_TOKEN_DIR="${TOKEN_DIR}"

# Optional player enrollment secret for over-the-wire authenticated provisioning
if [[ -n "${DARKPOOL_PLAYER_SECRET:-}" ]]; then
  export DARKPOOL_ENROLLMENT_TOKEN="${DARKPOOL_PLAYER_SECRET}"
fi

echo "============================================================"
echo "[DARKPOOL SERVER STARTING]"
echo "Binary            : ${SERVER_BIN}"
echo "Bind Address      : ${BIND_ADDR}"
echo "TLS Port          : ${PORT}"
echo "Certificate       : ${CERT}"
echo "Private Key       : ${KEY}"
echo "Token Store       : ${TOKEN_DIR} (Auto-Provisioning Enabled)"
echo "Moderator Token   : [CONFIGURED]"
if [[ -n "${DARKPOOL_ENROLLMENT_TOKEN:-}" ]]; then
  echo "Enrollment Token  : [CONFIGURED]"
fi
echo "============================================================"

exec "${SERVER_BIN}" \
  --tls-bind-address "${BIND_ADDR}" \
  --tls-port "${PORT}" \
  --tls-cert "${CERT}" \
  --tls-key "${KEY}"
