#!/usr/bin/env bash
set -Eeuo pipefail

REPO_ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"
DATA_DIR="$HOME/.local/share/darkpool/tls-lan"

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
as_root() { if (( EUID == 0 )); then "$@"; else sudo "$@"; fi; }

if [[ "${1:-}" == -h || "${1:-}" == --help || "${1:-}" == help ]]; then
  cat <<'USAGE'
Single-machine DARKPOOL setup (Ubuntu/Debian)
  bash scripts/setup_local.sh

Then start separate terminals on the same machine:
  bash scripts/run_local_server.sh
  bash scripts/run_local_client.sh moderator
  bash scripts/run_local_client.sh player   # repeat for each player seat

This mode uses the local UNIX-domain socket; no LAN IP or SSH setup is needed.
USAGE
  exit 0
fi
[[ $# -eq 0 ]] || die "setup_local.sh takes no positional arguments"
[[ "$(uname -s)" == Linux ]] || die "this setup requires Linux"
command -v apt-get >/dev/null 2>&1 || die "this script supports Ubuntu/Debian systems with apt-get"

printf '\n==> Installing local build dependencies\n'
as_root apt-get update
as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake git openssl libssl-dev iproute2 python3

printf '\n==> Configuring Release build\n'
cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON
jobs="$(printenv DARKPOOL_BUILD_JOBS 2>/dev/null || printf '2')"
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "DARKPOOL_BUILD_JOBS must be a positive integer"
printf '\n==> Building DARKPOOL (parallel jobs: %s)\n' "$jobs"
cmake --build "$REPO_ROOT/build" --parallel "$jobs"

printf '\n==> Running TLS transport integration tests\n'
ctest --test-dir "$REPO_ROOT/build" --output-on-failure -R '^darkpool_tls_integration_tests$'

install -d -m 700 "$DATA_DIR"
chmod 700 "$DATA_DIR"
if [[ ! -s "$DATA_DIR/moderator.token" ]]; then
  umask 077
  openssl rand -hex 32 > "$DATA_DIR/moderator.token"
fi
chmod 600 "$DATA_DIR/moderator.token"

cat <<'DONE'

Single-machine setup complete.

Open separate terminals on this same machine:
  bash scripts/run_local_server.sh
  bash scripts/run_local_client.sh moderator
  bash scripts/run_local_client.sh player

Repeat the player command for every player. One match supports 3–64 active players.
DONE