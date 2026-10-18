#!/usr/bin/env bash
set -Eeuo pipefail

readonly DATA_DIR="$HOME/.local/share/darkpool/tls-lan"
readonly REPO_ROOT="$(cd -- "$(dirname -- "$0")/.." && pwd)"

usage() {
  cat <<'USAGE'
DARKPOOL TLS 1.3 LAN setup (Ubuntu/Debian)

Server machine:
  bash scripts/setup_tls_lan.sh server [SERVER_LAN_IPV4]

Each client machine:
  bash scripts/setup_tls_lan.sh client SERVER_LAN_IPV4 [SERVER_SSH_USER] [player|moderator]

The script installs dependencies, builds DARKPOOL, runs the TLS integration test,
and transfers trust material via authenticated SSH. Player setup does not fetch the
moderator token. Run from the root of a DARKPOOL checkout on each machine.
USAGE
}

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
info() { printf '\n==> %s\n' "$*"; }

as_root() {
  if (( EUID == 0 )); then "$@"; else sudo "$@"; fi
}

check_linux() {
  [[ "$(uname -s)" == Linux ]] || die "DARKPOOL TLS server/client setup requires Linux"
  command -v apt-get >/dev/null 2>&1 || die "this script supports Ubuntu/Debian systems with apt-get"
}

is_private_ipv4() {
  python3 - "$1" <<'PY'
import ipaddress, sys
try:
    address = ipaddress.ip_address(sys.argv[1])
except ValueError:
    raise SystemExit(1)
networks = [ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")]
raise SystemExit(0 if isinstance(address, ipaddress.IPv4Address) and any(address in n for n in networks) else 1)
PY
}

validate_private_ipv4() {
  is_private_ipv4 "$1" || die "use an RFC 1918 private IPv4 address (10/8, 172.16/12, or 192.168/16)"
}

install_dependencies() {
  local role="$1"
  info "Installing build and TLS dependencies"
  as_root apt-get update
  if [[ "$role" == server ]]; then
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake git openssl libssl-dev iproute2 python3 openssh-server openssh-client
  else
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y build-essential cmake git openssl libssl-dev iproute2 python3 openssh-client
  fi
}

build_and_test() {
  info "Configuring Release build with warnings-as-errors"
  cmake -S "$REPO_ROOT" -B "$REPO_ROOT/build" -DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON
  info "Building DARKPOOL"
  local jobs
  jobs="$(printenv DARKPOOL_BUILD_JOBS 2>/dev/null || printf '2')"
  [[ "$jobs" =~ ^[1-9][0-9]*$ ]] || die "DARKPOOL_BUILD_JOBS must be a positive integer"
  cmake --build "$REPO_ROOT/build" --parallel "$jobs"
  info "Running TLS integration tests"
  ctest --test-dir "$REPO_ROOT/build" --output-on-failure -R '^darkpool_tls_integration_tests$'
}

ensure_data_dir() {
  install -d -m 700 "$DATA_DIR"
  chmod 700 "$DATA_DIR"
}

pick_server_ip() {
  local supplied=""
  if [[ $# -gt 0 ]]; then supplied="$1"; fi
  if [[ -n "$supplied" ]]; then printf '%s\n' "$supplied"; return; fi

  local detected
  detected="$(python3 - <<'PY'
import ipaddress, subprocess
lines = subprocess.check_output(["ip", "-o", "-f", "inet", "addr", "show", "scope", "global"], text=True).splitlines()
found = []
networks = [ipaddress.ip_network(n) for n in ("10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16")]
for line in lines:
    candidate = line.split()[3].split("/")[0]
    address = ipaddress.ip_address(candidate)
    if isinstance(address, ipaddress.IPv4Address) and any(address in n for n in networks):
        found.append(candidate)
print("\n".join(dict.fromkeys(found)))
PY
)"
  local count
  count="$(printf '%s\n' "$detected" | grep -c . || true)"
  if [[ "$count" == 1 ]]; then printf '%s\n' "$detected"; return; fi
  if [[ "$count" == 0 ]]; then die "could not find a private IPv4 address; inspect ip -4 addr and pass the server LAN IP explicitly"; fi
  printf 'Multiple private LAN addresses found:\n%s\n' "$detected" >&2
  local chosen
  read -r -p 'Enter the address that client machines can reach: ' chosen
  printf '%s\n' "$chosen"
}

check_ip_assigned() {
  ip -o -f inet addr show scope global | awk '{split($4,a,"/"); print a[1]}' | grep -Fxq -- "$1" || die "the chosen server IP is not assigned to this host"
}

start_ssh_server() {
  info "Enabling SSH for secure client bootstrap"
  if [[ -d /run/systemd/system ]] && command -v systemctl >/dev/null 2>&1; then
    as_root systemctl enable --now ssh
  elif command -v service >/dev/null 2>&1; then
    as_root service ssh start
  else
    die "could not start OpenSSH; start sshd before provisioning client machines"
  fi
}

configure_ufw_if_active() {
  local ip="$1"
  if ! command -v ufw >/dev/null 2>&1 || ! as_root ufw status 2>/dev/null | grep -q '^Status: active'; then
    printf 'UFW is not active; no UFW rules were changed.\n'
    return
  fi
  local host_cidr subnet
  host_cidr="$(ip -o -f inet addr show scope global | awk -v ip="$ip" '{split($4,a,"/"); if(a[1]==ip){print $4;exit}}')"
  [[ -n "$host_cidr" ]] || die "could not determine the LAN subnet for $ip"
  subnet="$(python3 -c 'import ipaddress,sys; print(ipaddress.ip_interface(sys.argv[1]).network)' "$host_cidr")"
  info "Restricting SSH and TLS ingress to $subnet in UFW"
  as_root ufw allow from "$subnet" to any port 22 proto tcp
  as_root ufw allow from "$subnet" to any port 4783 proto tcp
}

create_tls_material() {
  local ip="$1"
  local ca_key="$DATA_DIR/ca-key.pem" ca_cert="$DATA_DIR/ca-cert.pem"
  local server_key="$DATA_DIR/server-key.pem" server_cert="$DATA_DIR/server-cert.pem"
  umask 077

  if [[ ! -e "$ca_key" && ! -e "$ca_cert" ]]; then
    info "Creating the private LAN CA (its private key remains on this server)"
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out "$ca_key"
    openssl req -x509 -new -key "$ca_key" -sha256 -days 3650 -subj '/CN=DARKPOOL Private LAN CA' \
      -addext 'basicConstraints=critical,CA:TRUE' -addext 'keyUsage=critical,keyCertSign,cRLSign' -out "$ca_cert"
  elif [[ ! -s "$ca_key" || ! -s "$ca_cert" ]]; then
    die "an incomplete CA exists in $DATA_DIR; recover it instead of overwriting the trust root"
  fi
  chmod 600 "$ca_key"
  chmod 644 "$ca_cert"

  local reuse=false
  if [[ -s "$server_key" && -s "$server_cert" ]] && openssl verify -CAfile "$ca_cert" -verify_ip "$ip" "$server_cert" >/dev/null 2>&1; then
    local cert_pub key_pub
    cert_pub="$(openssl x509 -in "$server_cert" -pubkey -noout | openssl pkey -pubin -outform DER 2>/dev/null | sha256sum | awk '{print $1}')"
    key_pub="$(openssl pkey -in "$server_key" -pubout -outform DER 2>/dev/null | sha256sum | awk '{print $1}')"
    [[ -n "$cert_pub" && "$cert_pub" == "$key_pub" ]] && reuse=true
  fi
  if [[ "$reuse" != true ]]; then
    info "Issuing a TLS server certificate for $ip"
    local work
    work="$(mktemp -d "$DATA_DIR/.cert-work.XXXXXX")"
    openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out "$server_key"
    openssl req -new -key "$server_key" -subj "/CN=$ip" -out "$work/server.csr"
    cat > "$work/server.ext" <<EXT
[v3_server]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:$ip
subjectKeyIdentifier=hash
authorityKeyIdentifier=keyid,issuer
EXT
    openssl x509 -req -in "$work/server.csr" -CA "$ca_cert" -CAkey "$ca_key" -CAcreateserial \
      -out "$server_cert" -days 397 -sha256 -extfile "$work/server.ext" -extensions v3_server
    rm -rf -- "$work"
  fi
  openssl verify -CAfile "$ca_cert" -verify_ip "$ip" "$server_cert" >/dev/null || die "server certificate verification failed"
  chmod 600 "$server_key" "$ca_key"
  chmod 644 "$server_cert" "$ca_cert"
}

setup_server() {
  check_linux
  install_dependencies server
  local server_ip
  server_ip="$(pick_server_ip "$@")"
  validate_private_ipv4 "$server_ip"
  check_ip_assigned "$server_ip"
  ensure_data_dir
  start_ssh_server
  create_tls_material "$server_ip"
  if [[ ! -s "$DATA_DIR/moderator.token" ]]; then openssl rand -hex 32 > "$DATA_DIR/moderator.token"; fi
  chmod 600 "$DATA_DIR/moderator.token"
  printf '%s\n' "$server_ip" > "$DATA_DIR/server-ip"
  chmod 600 "$DATA_DIR/server-ip"
  configure_ufw_if_active "$server_ip"
  build_and_test
  cat <<NEXT

Server setup complete: $server_ip:4783 (TLS 1.3)
Start the server with: bash scripts/run_tls_server.sh

On each client machine, run one of these (replace SERVER_SSH_USER):
  bash scripts/setup_tls_lan.sh client $server_ip SERVER_SSH_USER player
  bash scripts/setup_tls_lan.sh client $server_ip SERVER_SSH_USER moderator
NEXT
}

setup_client() {
  [[ $# -ge 1 && $# -le 3 ]] || { usage; die "client requires SERVER_LAN_IPV4 [SERVER_SSH_USER] [player|moderator]"; }
  local server_ip="$1" ssh_user role
  ssh_user="$(id -un)"
  if [[ $# -ge 2 && -n "$2" ]]; then ssh_user="$2"; fi
  role=player
  if [[ $# -ge 3 ]]; then role="$3"; fi
  validate_private_ipv4 "$server_ip"
  [[ "$ssh_user" =~ ^[a-zA-Z_][a-zA-Z0-9_.-]*\$?$ ]] || die "invalid SSH username"
  [[ "$role" == player || "$role" == moderator ]] || die "role must be player or moderator"
  check_linux
  install_dependencies client
  ensure_data_dir
  local target="$ssh_user@$server_ip"

  info "Retrieving the public CA certificate over SSH from $target"
  printf 'On first SSH connection, verify the host-key fingerprint before accepting it.\n'
  ssh "$target" 'test -r "$HOME/.local/share/darkpool/tls-lan/ca-cert.pem" && test -r "$HOME/.local/share/darkpool/tls-lan/server-ip"' || die "SSH login or server TLS setup failed"
  local configured_ip
  configured_ip="$(ssh "$target" 'cat "$HOME/.local/share/darkpool/tls-lan/server-ip"')"
  [[ "$configured_ip" == "$server_ip" ]] || die "server TLS setup uses $configured_ip; pass that address instead"
  umask 077
  ssh "$target" 'cat "$HOME/.local/share/darkpool/tls-lan/ca-cert.pem"' > "$DATA_DIR/ca-cert.pem" || die "CA certificate transfer failed"
  openssl x509 -in "$DATA_DIR/ca-cert.pem" -noout >/dev/null || die "downloaded CA certificate is invalid"
  chmod 600 "$DATA_DIR/ca-cert.pem"
  if [[ "$role" == moderator ]]; then
    ssh "$target" 'cat "$HOME/.local/share/darkpool/tls-lan/moderator.token"' > "$DATA_DIR/moderator.token" || die "moderator token transfer failed"
    [[ -s "$DATA_DIR/moderator.token" ]] || die "downloaded moderator token is empty"
    chmod 600 "$DATA_DIR/moderator.token"
  else
    rm -f -- "$DATA_DIR/moderator.token"
  fi
  printf '%s\n' "$server_ip" > "$DATA_DIR/server-ip"
  chmod 600 "$DATA_DIR/server-ip"
  build_and_test
  printf '\nClient setup complete (%s). Launch with: bash scripts/run_tls_client.sh %s\n' "$role" "$role"
  if [[ "$role" == player ]]; then printf 'Player setup does not retrieve the moderator token.\n'; fi
}

main() {
  [[ $# -ge 1 ]] || { usage; exit 2; }
  local mode="$1"
  shift
  case "$mode" in
    server) [[ $# -le 1 ]] || { usage; die "server accepts at most one IP argument"; }; setup_server "$@" ;;
    client) setup_client "$@" ;;
    -h|--help|help) usage ;;
    *) usage; die "mode must be server or client" ;;
  esac
}

main "$@"