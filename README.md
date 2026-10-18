# DARKPOOL — deterministic C++20 engine and TCP / UNIX-domain socket prototype

The C++20 engine and coordinator support matches configured for 3–64 active players. Five remains the default and the original baseline is one Insider, one Investigator, and three Employees. The moderator is a separate non-player.

The coordinator supports server-assigned membership, independent matches with different capacities, moderator-controlled event/phase transitions, and player trade, clue, vote, and hint commands. The Linux server prototype uses nonblocking stream sockets with epoll, supporting local UNIX-domain stream sockets (`/tmp/darkpool.sock`), default localhost plaintext TCP (`127.0.0.1:4782`), explicit non-loopback plaintext TCP binding on an assigned RFC 1918 private IPv4 address (`--bind-address`), and a dedicated TLS 1.3 secure listener on port `4783` (AD-S9-01) supporting local-CA and leaf fingerprint pinning. The plaintext LAN TCP transport on port 4782 remains unencrypted; the TLS 1.3 listener on port 4783 enforces encrypted, authenticated communication. Public-Internet deployment, persistent user accounts, and dynamic certificate reload remain deferred future work.

## Scope and implementation status

- **Transport and CLI:** The server implementation supports nonblocking UNIX-domain stream sockets (`/tmp/darkpool.sock`), plaintext TCP stream sockets defaulting to loopback (`127.0.0.1:4782`), optional explicit plaintext binding to an assigned RFC 1918 private IPv4 address via `--bind-address`, and a dedicated TLS 1.3 secure listener on port `4783` (`src/server_main.cpp`), accompanied by an interactive terminal CLI (`src/client_main.cpp`). UNIX sockets feature safe lifecycle management (stale socket file detection/recovery, non-socket file preservation, restrictive 0700 permissions, and signal-driven cleanup). The secure TLS 1.3 listener operates on dedicated port 4783 with OpenSSL 3.x, supporting both local-CA certificate verification (`--ca-cert`) and SHA-256 leaf fingerprint pinning (`--server-fingerprint`) with mandatory IP SAN validation and fail-closed semantics. The legacy LAN TCP path on port 4782 is unencrypted plaintext. Public-Internet deployment, dynamic certificate reload, and mutual TLS (mTLS) remain deferred future work.
- **Valid match sizes:** Valid match sizes are 3–64. Role counts are deterministic: `ceil(N/8)` Insiders, `ceil(N/16)` Investigators, and the remaining Employees. The server shuffles and assigns roles; clients cannot supply roles. Five remains the default.
- **Event injection and engine separation:** Business-event impacts are injected by the match's creating moderator. The game engine itself has no random source, clock, sockets, epoll, client, server, or persistence; match/session coordination is a separate server module.
- **Share conservation:** Shares are conserved between players and each match's finite reserve using the approved 20N total / 10N player-held / 10N reserve formula.
- **Victory and clue invariants:** A match ends with an Insider-team win if health reaches zero, or if at least one Insider survives through round three. Eliminating an Insider only gives Employees the win when no Insiders remain. Each Investigator has an independent once-per-match clue; legacy match-level clue fields mirror the first Investigator's clue.
- **Capacity limits and verification status:** The coordinator allows four concurrent matches. The server is configured for at most 256 player connections plus four independent authenticated moderator connections (260 total connections); each match is independently owned and controlled by its dedicated moderator connection. Four-match 256-player concurrent functional capacity is verified through reproducible benchmarks (`docs/load_test_l6_results.json`). Sustained long-duration multi-match reliability (10-minute smoke and 30-minute reliability soak workloads on a single persistent server) is evaluated as a distinct stage; see `docs/test-plan.md` for current measured status and evidence.
- **Credits:** Credits are non-conserved player counters. The provisional S1 cap is 1,000,000 credits per player (`uint32_t`); an order that would exceed it is rejected and consumes the round's trade-action slot under the approved action-slot rule. This is an implementation detail, not a new gameplay approval.
- **Canonical view privacy:** Canonical engine state is an internal test/replay view containing secret roles and clue/event information. It must not be sent to players.
- **Test coverage:** Engine roster and boundary tests cover N=2 (rejection), N=3,4,5,8,9,16,24,32,64, and N=65 (rejection). Integration tests cover three-player, five-player, and eight-player matches over TCP, boundary rejection, as well as complete UNIX-domain socket lifecycle, authentication, LAN interface binding, five-player gameplay, and malformed-frame isolation. Dedicated load benchmarks are recorded for L1 (5-player), L3 (32-player), L4 (64-player), L5 (2×32-player), L6 (4×64-player), L7 (slow-client queue bounds), and L8 (request flood rate limiting).

## Build and test on Ubuntu

Requirements: CMake 3.20 or later and a C++20 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Configure with warnings-as-errors using `-DDARKPOOL_WARNINGS_AS_ERRORS=ON`.

## Quickstart

For a complete, verified step-by-step walkthrough—from prerequisite installation, certificate generation, starting the server, connecting the moderator and 5 players, playing through all 3 rounds, to terminal victory resolution—refer to the [Quickstart Guide](docs/quickstart.md).

## Playable CLI Gameplay

Moderator connections require an authentication secret supplied via the `DARKPOOL_MODERATOR_TOKEN` environment variable on both the server and the moderator client. If the server does not have `DARKPOOL_MODERATOR_TOKEN` set, it fails closed and rejects all moderator connection attempts. Players connect without a token.

> **Security & Transport Architecture Note**: DARKPOOL provides three distinct transport endpoints:
> 1. **Local UNIX-domain stream socket**: `/tmp/darkpool.sock` for high-performance single-host IPC.
> 2. **Plaintext TCP stream socket**: defaults to loopback (`127.0.0.1:4782`), with optional explicit non-loopback binding to an assigned RFC 1918 private IPv4 address (`--bind-address`). **Warning**: Port 4782 is unencrypted plaintext; moderator tokens and gameplay messages travel across the network in plaintext. It must never be exposed to public or untrusted networks.
> 3. **Secure TLS 1.3 TCP stream socket**: dedicated listener on port `4783` (AD-S9-01) backed by OpenSSL 3.x, enforcing TLS 1.3 as the minimum and only allowed protocol version, with zero silent fallback to plaintext.
>
> Supported TLS client verification modes:
> - **Local CA verification** (`--ca-cert <path>`): validates server certificate chain against a trusted local CA.
> - **SHA-256 leaf fingerprint pinning** (`--server-fingerprint <64-hex>`): validates the SHA-256 digest of the server leaf certificate directly.
>
> Both verification modes strictly enforce certificate validity periods and IP Subject Alternative Name (SAN) matching against the target address.
>
> **Deferred Scope**: Public Internet deployment is unsupported. Mutual TLS (mTLS) for players, persistent player accounts, and dynamic in-process certificate reload remain deferred future milestones.

On Linux, run the server and clients in separate terminals:

### Option A: Local UNIX-domain stream socket (Recommended)

```sh
# Terminal 0: Start the server (binds /tmp/darkpool.sock and port 4782 by default)
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_server

# Terminal 1: Start the Moderator client
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_client /tmp/darkpool.sock moderator

# Start 3-64 Player clients (no token required); use the same number as the created match
./build/darkpool_client /tmp/darkpool.sock player
```

### Option B: Localhost TCP stream socket

```sh
# Terminal 0: Start the TCP coordinator server (default port: 4782, binds 127.0.0.1)
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_server 4782

# Terminal 1: Start the Moderator client
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_client 127.0.0.1 4782 moderator

# Start 3-64 Player clients (no token required); use the same number as the created match
./build/darkpool_client 127.0.0.1 4782 player
```

### Option C: Trusted Private LAN TCP stream socket (Multi-machine play)

For multiplayer over a trusted private local network (RFC 1918 IPv4), configure the server to bind to an assigned private IPv4 address using `--bind-address <IPv4-address>` (or `--bind-address=<IPv4-address>` / `-b <IPv4-address>`):

1. **Discover the Server's Assigned LAN Address**:
   On the server host, run:
   ```sh
   ip -4 addr show
   ```
   Identify your active private IPv4 address on the primary interface (e.g. `192.168.1.50`, `10.0.0.25`, or `172.16.0.10`).

   > **Note on Localhost vs Remote IP**: On player machines, `127.0.0.1` points back to that player's own local computer, **not** the game server. Players connecting from other machines across the LAN must specify the server's actual assigned private LAN IP address.

2. **Configure Firewall**:
   Restrict incoming TCP traffic on port 4782 to your local subnet (substituting your actual subnet, e.g. `192.168.1.0/24`):
   ```sh
   sudo ufw allow from 192.168.1.0/24 to any port 4782 proto tcp
   # Or using iptables:
   # sudo iptables -A INPUT -p tcp -s 192.168.1.0/24 --dport 4782 -j ACCEPT
   ```

3. **Launch Server on Private Interface**:
   ```sh
   # On Server Host (substitute 192.168.1.50 with your actual assigned LAN address):
   export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
   ./build/darkpool_server --bind-address 192.168.1.50 4782
   ```

4. **Launch Clients Across the LAN**:
   ```sh
   # On Moderator Machine (same host or another LAN machine):
   export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
   ./build/darkpool_client 192.168.1.50 4782 moderator

   # On Player Machines:
   ./build/darkpool_client 192.168.1.50 4782 player
   ```

> **Security Warning (Plaintext LAN Transport)**: The plaintext LAN TCP transport on port 4782 is unencrypted without TLS. The moderator token and all gameplay messages traverse the local network in plaintext. Non-loopback binding is restricted strictly to RFC 1918 private subnets (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`) and must never be exposed to public or untrusted networks. For encrypted multi-machine play, use Option D below.

### Option D: Secure Private LAN TCP stream socket (TLS 1.3 on dedicated port 4783)

For encrypted, tamper-resistant multiplayer over a private local network, launch the server with dedicated TLS listener configuration on port `4783` (AD-S9-01):

1. **Launch Server with TLS Listener**:
   ```sh
   # On Server Host (substitute 192.168.1.50 with your actual assigned LAN address):
   export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
   ./build/darkpool_server \
     --tls-bind-address 192.168.1.50 \
     --tls-port 4783 \
     --tls-cert /path/to/server-cert.pem \
     --tls-key /path/to/server-key.pem
   ```

2. **Connect Clients using Local CA Mode (Mode 1)**:
   ```sh
   # On Moderator Machine:
   export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
   ./build/darkpool_client --tls --ca-cert /path/to/ca-cert.pem 192.168.1.50 4783 moderator

   # On Player Machines:
   ./build/darkpool_client --tls --ca-cert /path/to/ca-cert.pem 192.168.1.50 4783 player
   ```

3. **Connect Clients using Leaf Fingerprint Pinning Mode (Mode 2)**:
   ```sh
   # Obtain SHA-256 fingerprint:
   # openssl x509 -noout -fingerprint -sha256 -in server-cert.pem | tr -d ':' | awk -F= '{print tolower($2)}'

   # On Moderator Machine:
   export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
   ./build/darkpool_client --tls --server-fingerprint <64-hex-sha256> 192.168.1.50 4783 moderator

   # On Player Machines:
   ./build/darkpool_client --tls --server-fingerprint <64-hex-sha256> 192.168.1.50 4783 player
   ```


### LAN Troubleshooting Guide

- **Connection refused / Timed out**:
  - Verify that the server process is running and actively listening: `ss -tulpn | grep 4782`.
  - Check firewall status on the server host: `sudo ufw status`.
  - Check whether Wi-Fi router has "AP isolation" or "guest network isolation" enabled, which blocks direct peer-to-peer communication between devices on Wi-Fi.
  - If a VPN (Tailscale, WireGuard, OpenVPN, corporate VPN) is active, confirm both machines are routing traffic over the intended physical LAN subnet rather than split tunnels.
- **Bind failure / Error exit 1 on startup**:
  - `bind_address is not assigned to any local network interface`: The address specified with `--bind-address` must be actively assigned to an interface on the server machine. Check `ip -4 addr show`.
  - `Address already in use`: Another process or previous server instance is already bound to port 4782. Terminate it with `killall darkpool_server` or select a different port.
- **Invalid address rejection (exit 2)**:
  - The server strictly rejects public IPs, wildcard `0.0.0.0`, multicast `224.0.0.0/4`, loopback aliases, or malformed strings. Only valid local RFC 1918 private IPv4 addresses are accepted for non-loopback binding.

### Moderator CLI Commands

- `create [player_count]`: Create a match for 3–64 players. Omitted count defaults to five. Roles are assigned by the server using the approved scaling formula.
- `match <id>`: Switch active match ID context (default: 1).
- `start`: Start the match once all configured players have joined.
- `event <impact>`: Publish round business event impact (-2 to 2). If omitted, prompts for the impact. Transitions phase to Discussion.
- `trading`: Open the Trading phase for player orders.
- `market`: Resolve market orders and display executed orders, price changes, and reserve health. Transitions phase to Clue.
- `voting`: Open the Voting phase for player elimination votes.
- `resolve`: Resolve votes, announce eliminations, and check victory conditions. If game continues, advances round to AwaitingEvent.
- `status`: Display public match board, health, price, phase, round, and aggregate market/vote resolutions.
- `help`: Display available moderator commands.
- `quit` / `exit`: Disconnect from the match server.

### Player CLI Commands

- `join [match_id]`: Join match (default: 1). The server assigns a player ID and returns the player's secret role (`Insider`, `Investigator`, or `Employee`).
- `status`: Display public match board, company health, stock price, and round phase.
- `hint`: (*Insider only*) Secretly inspect upcoming event impact during Discussion.
- `trade [action]`: Submit order (`BUY`, `SELL`, or `HOLD`) during Trading. Also supports direct shortcuts `buy`, `sell`, and `hold`. If omitted, prompts for the action.
- `clue [target_id]`: (*Investigator only*) Secretly investigate an active player ID (1-64) during Clue phase. Each Investigator has one independent clue per match. If omitted, prompts for the target.
- `vote [target_id|abstain]`: Cast elimination vote (player ID 1-64) or abstain (`0` or `abstain`) during Voting. If omitted, prompts for the target.
- `help`: Display available player commands and role notes.
- `quit` / `exit`: Disconnect from the match server.

### Confidentiality & Information Display

- **Private Information**: Private roles (`joined_match`), Insider hints (`hint_private`), and Investigator investigation results (`clue_private`) are delivered only to the acting connection and displayed privately on their terminal.
- **Public Information**: Phase transitions, company health, stock price, aggregate market execution summaries, elimination announcements, and game-over winner declarations are public and displayed to all participants.

### How to Run a Complete Game

1. **Launch Server**: Run `DARKPOOL_MODERATOR_TOKEN="secret" ./build/darkpool_server 4782` in Terminal 0.
2. **Launch Moderator**: Run `DARKPOOL_MODERATOR_TOKEN="secret" ./build/darkpool_client 127.0.0.1 4782 moderator` in Terminal 1.
3. **Launch Players**: Launch one player client for each configured seat (3–64).
4. **Create Match**: In Moderator terminal, type `create` for five players or `create 8` for eight.
5. **Join Players**: In each player terminal, type `join 1`. Each player receives their private role and confidential objectives.
6. **Start Match**: In Moderator terminal, type `start`. (Phase: `AwaitingEvent`).
7. **Publish Event**: In Moderator terminal, type `event 0` (or impact between -2 and 2). (Phase: `Discussion`).
8. **Insider Hint**: An Insider types `hint` to view the event impact secretly.
9. **Open Trading**: Moderator types `trading`.
10. **Submit Trades**: Each player submits their order: `buy`, `sell`, or `hold`.
11. **Resolve Market**: Moderator types `market`. The closing health, price, and market outcome are displayed. (Phase: `Clue`).
12. **Investigate Clue**: Each Investigator may type `clue <target_id>` to secretly check whether the target is an Insider.
13. **Open Voting**: Moderator types `voting`.
14. **Cast Votes**: Each player casts an elimination vote, e.g. `vote 1` or `vote abstain`.
15. **Resolve Votes**: Moderator types `resolve`. Eliminations and victory outcomes are announced.
16. **Next Rounds**: If neither side has won, repeat steps 7–15 for Rounds 2 and 3. The final winner is announced when the Insider is eliminated (`EmployeesWin`), the company collapses (`InsiderWin`), or Round 3 concludes (`InsiderWin`).

## Two-Computer Manual Acceptance (Trusted Private LAN)

**Status:** `COMPLETED (October 17, 2026)`
- **Tested Application Revision:** `5479dad55341e36d94fb7a7b29847993a12cc037` (tested on October 17, 2026; distinct from subsequent documentation changes)
- **Automated CTest Suite:** 32/32 tests passed (0 failures, 84.83 seconds runtime) on Computer A. Note: The automated test log reported for this run (`evidence/lan-acceptance.txt`) was generated on the testing host during acceptance and is not present in this repository checkout.
- **Manual LAN Session:** Operator reported successful completion of the full 21-step interactive two-computer LAN acceptance workflow on a trusted RFC 1918 private IPv4 subnet (5 players + 1 moderator across Computer A and Computer B, completing all three rounds to a final outcome).
- **Detailed Acceptance Record:** See the [LAN Acceptance Test section in `docs/test-plan.md`](docs/test-plan.md#lan-acceptance-test) for complete test breakdown, scope, and evidence limitations.

| Step | Verification Item | Expected Result | Acceptance Status |
|---|---|---|:---:|
| 1 | Discover Server IP | `ip -4 addr show` displays active RFC 1918 address (e.g. `192.168.1.50`). | PASS |
| 2 | Start Server | `./build/darkpool_server --bind-address <LAN-IP> 4782` starts and prints bound address & security warning. | PASS |
| 3 | Firewall Rule | Port 4782 restricted to subnet (`sudo ufw allow from <subnet> to any port 4782 proto tcp`). | PASS |
| 4 | Connect Moderator | From Computer B: `./build/darkpool_client <LAN-IP> 4782 moderator` authenticates with token. | PASS (Operator-reported) |
| 5 | Connect Players | Connect 5 players across Computer A and Computer B using `darkpool_client <LAN-IP> 4782 player`. | PASS (Operator-reported) |
| 6 | Match Creation | Moderator runs `create 5`; receives `match_created` with 5 seats (1 Insider, 1 Investigator, 3 Employees). | PASS (Operator-reported) |
| 7 | Player Joins & Role Privacy | Each player runs `join 1`; receives assigned private role. Confirm roles are never leaked to other terminals. | PASS (Operator-reported) |
| 8 | Match Start & Round 1 Event | Moderator runs `start`, then `event 0`. Phase transitions to `Discussion` on all client terminals. | PASS (Operator-reported) |
| 9 | Insider Hint Privacy | Insider runs `hint`; receives event impact 0. Confirm non-insiders cannot view or receive the hint. | PASS (Operator-reported) |
| 10 | Trading Phase & Execution | Moderator opens trading; players submit `hold`; market resolves with conserved shares and updated price. | PASS (Operator-reported) |
| 11 | Clue Investigation Privacy | Investigator runs `clue <target_id>`; receives private clue. Confirm result is private to investigator only. | PASS (Operator-reported) |
| 12 | Voting Phase & Elimination | Moderator opens voting; players cast votes; vote resolution handles eliminations/abstentions accurately. | PASS (Operator-reported) |
| 13 | Rounds 2 & 3 Progression | Complete rounds 2 and 3 through all phase transitions; verify deterministic outcome. | PASS (Operator-reported) |
| 14 | Post-Game Disconnect | Disconnect all clients; verify server cleans up connection state cleanly and firewall rule is removed. | PASS (Operator-reported) |

## Deferred Future Work (Backlog)

The following items are explicitly tracked as separate future milestones and are **not** implemented in this phase:

1. **Public Internet Deployment**: Wide-area public routing, DNS FQDN SAN validation, public Web PKI, and internet-facing edge firewalling (private LAN TLS 1.3 on port 4783 is implemented in S9).
2. **Mutual TLS (mTLS) & Persistent Player Identities**: Cryptographic client certificates, persistent player accounts, and credential databases (session-scoped anonymous player identities and in-channel moderator tokens are currently used).
3. **Dynamic In-Process Certificate Reload & Automated Lifecycle**: Live certificate reload without restarting the server process and automated ACME/Let's Encrypt renewal (certificate rotation is currently restart-based).
4. **Public Deployment & Operational Monitoring**: Prometheus metrics export, Grafana dashboards, health-check endpoints, and paging/alerting.
5. **Service Packaging & Supervision**: Systemd unit files, process supervision, auto-restart policies, rollback procedures, and hardening profiles.
6. **Durable Match Persistence**: State journaling and crash-recovery to resume active matches across process restarts.
7. **Distribution Bundling**: Standalone release archives and packaging workflows (deferred until explicitly scheduled).

## Engine API outline

Construct `Match` with 3–64 `{PlayerId, Role}` entries whose role counts match `role_counts(N)`, call `start()`, inject each event with `publish_event(impact)`, and advance phases through `open_trading()`, trade submissions, `resolve_market()`, optional `use_clue()` calls by each Investigator, `open_voting()`, vote submissions and `resolve_votes()`. Methods return explicit error codes for rejected commands. `Match::from_state()` accepts only a validated, share-conserving state and is intended for deterministic test fixtures.
