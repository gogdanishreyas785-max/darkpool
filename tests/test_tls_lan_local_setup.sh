#!/usr/bin/env bash
set -Eeuo pipefail

readonly REPO_ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
readonly SERVER_IP="192.168.56.101"
readonly TEMP_ROOT="$(mktemp -d)"
readonly TEST_HOME="$TEMP_ROOT/home"
readonly MOCK_BIN="$TEMP_ROOT/bin"
readonly DATA_DIR="$TEST_HOME/.local/share/darkpool/tls-lan"
readonly SSH_CALL_LOG="$TEMP_ROOT/ssh-called"

cleanup() {
  rm -rf -- "$TEMP_ROOT"
}
trap cleanup EXIT

mkdir -p "$TEST_HOME" "$MOCK_BIN" "$DATA_DIR"

cat > "$MOCK_BIN/sudo" <<'MOCK'
#!/usr/bin/env bash
exec "$@"
MOCK

cat > "$MOCK_BIN/apt-get" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK

cat > "$MOCK_BIN/ip" <<'MOCK'
#!/usr/bin/env bash
printf '2: enp0s8 inet 192.168.56.101/24 scope global enp0s8\\n'
MOCK

cat > "$MOCK_BIN/ssh" <<'MOCK'
#!/usr/bin/env bash
printf 'ssh was unexpectedly invoked\\n' >> "$SSH_CALL_LOG"
exit 99
MOCK

cat > "$MOCK_BIN/cmake" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK

cat > "$MOCK_BIN/ctest" <<'MOCK'
#!/usr/bin/env bash
exit 0
MOCK

chmod +x "$MOCK_BIN/"*

openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$DATA_DIR/ca-key.pem" \
  -out "$DATA_DIR/ca-cert.pem" \
  -days 1 -subj "/CN=DARKPOOL local setup test" >/dev/null 2>&1
printf '%s\\n' "$SERVER_IP" > "$DATA_DIR/server-ip"
printf 'test-moderator-token\\n' > "$DATA_DIR/moderator.token"
printf '%s\\n' "$(id -un)" > "$DATA_DIR/server-ssh-user"
chmod 600 "$DATA_DIR/ca-key.pem" "$DATA_DIR/moderator.token" "$DATA_DIR/server-ssh-user"
chmod 644 "$DATA_DIR/ca-cert.pem"

cert_before="$(sha256sum "$DATA_DIR/ca-cert.pem" | awk '{print $1}')"
token_before="$(sha256sum "$DATA_DIR/moderator.token" | awk '{print $1}')"

run_setup() {
  local role="$1"
  HOME="$TEST_HOME" \
    PATH="$MOCK_BIN:$PATH" \
    SSH_CALL_LOG="$SSH_CALL_LOG" \
    bash "$REPO_ROOT/scripts/setup_tls_lan.sh" client "$SERVER_IP" "$(id -un)" "$role"
}

run_setup player
[[ "$(sha256sum "$DATA_DIR/ca-cert.pem" | awk '{print $1}')" == "$cert_before" ]]
[[ "$(sha256sum "$DATA_DIR/moderator.token" | awk '{print $1}')" == "$token_before" ]]
[[ ! -e "$DATA_DIR/server-ssh-user" ]]
[[ ! -e "$SSH_CALL_LOG" ]]

run_setup moderator
[[ "$(sha256sum "$DATA_DIR/ca-cert.pem" | awk '{print $1}')" == "$cert_before" ]]
[[ "$(sha256sum "$DATA_DIR/moderator.token" | awk '{print $1}')" == "$token_before" ]]
[[ ! -e "$SSH_CALL_LOG" ]]

printf 'Same-host TLS client setup regression test passed.\\n'
