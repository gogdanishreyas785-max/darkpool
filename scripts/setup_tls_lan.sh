#!/usr/bin/env bash
# scripts/setup_tls_lan.sh
# Generates TLS 1.3 certificates, pins SHA-256 fingerprint, and configures secure directories.

set -euo pipefail

TARGET_IP="${1:-}"

# Auto-detect private LAN IP if not provided
if [[ -z "${TARGET_IP}" ]]; then
  # Try detecting primary interface IP
  DETECTED_IP=$(ip -4 addr show scope global 2>/dev/null | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | head -n1 || true)
  if [[ -n "${DETECTED_IP}" ]]; then
    TARGET_IP="${DETECTED_IP}"
  else
    TARGET_IP="127.0.0.1"
  fi
fi

DARKPOOL_DIR="${HOME}/.darkpool"
CERTS_DIR="${DARKPOOL_DIR}/certs"
TOKENS_DIR="${DARKPOOL_DIR}/tokens"
CONFIG_FILE="${DARKPOOL_DIR}/config"

# Enforce strict 0700 permissions on directory roots
mkdir -p "${CERTS_DIR}" "${TOKENS_DIR}"
chmod 0700 "${DARKPOOL_DIR}" "${CERTS_DIR}" "${TOKENS_DIR}"

KEY_FILE="${CERTS_DIR}/server.key"
CRT_FILE="${CERTS_DIR}/server.crt"

echo "[SETUP] Generating TLS 1.3 certificate for IP: ${TARGET_IP}"

# Generate certificate with exact IP in CN and SAN extension
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "${KEY_FILE}" \
  -out "${CRT_FILE}" \
  -days 30 \
  -subj "/CN=${TARGET_IP}" \
  -addext "subjectAltName=IP:${TARGET_IP}" 2>/dev/null

# Strict 0600 on private key
chmod 0600 "${KEY_FILE}"
chmod 0644 "${CRT_FILE}"

# Extract 64-character lowercase SHA-256 fingerprint
FINGERPRINT=$(openssl x509 -noout -fingerprint -sha256 -in "${CRT_FILE}" | tr -d ':' | awk -F= '{print tolower($2)}')

# Save persistent configuration for scripts
cat <<EOF > "${CONFIG_FILE}"
# DARKPOOL Environment Configuration
DARKPOOL_SERVER_HOST="${TARGET_IP}"
DARKPOOL_SERVER_PORT="4783"
DARKPOOL_SERVER_FINGERPRINT="${FINGERPRINT}"
DARKPOOL_CERT_PATH="${CRT_FILE}"
DARKPOOL_KEY_PATH="${KEY_FILE}"
DARKPOOL_TOKEN_DIR="${TOKENS_DIR}"
EOF
chmod 0600 "${CONFIG_FILE}"

echo "============================================================"
echo "[DARKPOOL SETUP COMPLETE]"
echo "Server LAN IP : ${TARGET_IP}"
echo "Server Port   : 4783"
echo "Certificate   : ${CRT_FILE}"
echo "Private Key   : ${KEY_FILE} (0600)"
echo "Fingerprint   : ${FINGERPRINT}"
echo "Config File   : ${CONFIG_FILE} (0600)"
echo "Token Store   : ${TOKENS_DIR} (0700)"
echo "============================================================"
echo "To distribute trust to player machines, share the fingerprint:"
echo "  export FINGERPRINT=\"${FINGERPRINT}\""
echo "============================================================"
