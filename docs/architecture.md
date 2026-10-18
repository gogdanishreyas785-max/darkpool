# Proposed C++20 architecture

## System design

One authoritative server process owns official game state. The implementation connects local and trusted LAN client processes through nonblocking UNIX domain stream sockets (`/tmp/darkpool.sock`), configurable plaintext TCP stream sockets (defaulting to localhost `127.0.0.1:4782`, or explicitly bound to an assigned RFC 1918 private IPv4 address via `--bind-address`), and a dedicated TLS 1.3 secure listener on port `4783` (supporting local-CA and fingerprint-pinning verification via OpenSSL 3.x), all monitored by epoll (`src/server_main.cpp`), with an interactive client CLI (`src/client_main.cpp`). Remote public-Internet deployment, dynamic certificate reload, and production client PKI remain deferred future work. The server authenticates sessions, validates actions, routes requests to independent matches, applies transitions, and emits authorized events. Use a modular daemon, not service-per-match.

Flow: client processes → UNIX domain / localhost TCP / private LAN TCP / TLS 1.3 stream transport → nonblocking framing/epoll connection manager → session/auth manager → match router → independent Match state machines → bounded output queues and clients.

One event loop owns mutations initially; no thread per client. Optional workers only for measured CPU-heavy tasks, returning results to the owner loop. Workers never mutate matches. Use monotonic deadlines via timerfd or a deadline heap.

## Components

- **Transport adapter:** Implemented using nonblocking UNIX domain stream sockets (`/tmp/darkpool.sock`), configurable plaintext TCP stream sockets (default: `127.0.0.1:4782`; optional `--bind-address <IPv4>` restricted to local assigned RFC 1918 private addresses), and a dedicated TLS 1.3 secure listener on port `4783` (AD-S9-01) with OpenSSL 3.x in `include/darkpool/tls_transport.hpp` and `src/tls_transport.cpp`; remote public-internet deployment remains deferred. No game rules in transport.
- **Connection manager:** connection IDs, framing, lifecycle, bounded input/output queues, rates, backpressure, disconnects.
- **Protocol:** versioned envelope, schema/length validation, request correlation, error codes.
- **Session manager:** server-assigned identity, authentication, permissions, reconnect/cleanup.
- **Match manager:** create/find/close, enforce caps, associate sessions and matches.
- **Game engine / Match:** roles, phases/deadlines, events, trades, clue, votes, company state, win rules; all authoritative.
- **Event/audit log:** important transitions and security events; separate from player output and redact by audience.
- **Timer/metrics/config:** monotonic deadlines, limits and bounded observability.

Each Match owns membership, roles, phases, deadlines, company state, trades, clue, chat routing, votes and logs. A request needs authenticated membership and legal phase/role. Match state never retains client-buffer pointers. No slow client, match fan-out, log write, or expensive computation may block the loop.

## Isolation and targets

The approved per-match player range is 3–64, with five as the default; the moderator is outside that count. The server coordinator permits at most four matches (`kMaxMatches = 4`). The server configuration in `src/server_main.cpp` enforces explicit capacity ceilings: up to 256 player connections (`kMaxPlayerConnections = 256`) and four independent authenticated moderator connections (`kMaxModerators = 4`), allowing up to 260 total concurrent connections (`kMaxConnections = 260`). Each active match is independently owned and controlled by its dedicated moderator connection. Initial S0 proposals envisioned 1 moderator and 257 connections; this was superseded in the L6 multi-match implementation to enable independent moderator sessions per match.

Match IDs route requests but server-side session membership authorizes them. A match timeout or message surge must not stall another match. Bound frames, queues, rates, connections, matches, players and per-event work; safe disconnect/reconnect behavior follows approved policy.

The product configuration maximum is 64 active players per match and four concurrent matches (up to 256 active players plus four moderators, 260 total connections). Capacity and reliability for this architecture have been established under reproducible single-host Linux benchmark suites: L4 verified 64-player capacity and 30-minute reliability, and L6 verified four-match 256-player concurrent capacity and 30-minute reliability soak workloads (see `docs/test-plan.md`). Physical cross-device two-computer LAN acceptance for the 5-player baseline was completed on October 17, 2026 (see `docs/test-plan.md`), while remote public-Internet deployment remains deferred (private LAN TLS 1.3 on port 4783 is implemented in S9).

## Proposed repository layout

    darkpool/
      CMakeLists.txt
      README.md
      docs/ (requirements, game-rules, protocol, architecture, threat-model, test-plan, DECISIONS)
      include/darkpool/ (protocol.hpp, transport.hpp, session.hpp, match.hpp, match_manager.hpp, audit_log.hpp)
      include/darkpool/game/ (rules.hpp, state.hpp, action.hpp)
      src/server_main.cpp
      src/client_main.cpp
      src/transport/ (transport_unix.cpp, connection_manager.cpp)
      src/protocol/protocol.cpp
      src/session/session_manager.cpp
      src/match/ (match.cpp, match_manager.cpp)
      src/game/ (rules.cpp, state.cpp)
      src/security/audit_log.cpp
      tests/unit/
      tests/integration/
      tests/isolation/
      tests/security/
      tests/load/
      scripts/run_local_demo.sh
      scripts/run_load_test.sh

Create modules as implementation requires, not as empty placeholder code. CMake selects C++20 and documents supported Ubuntu compiler/kernel.

## Future transport & deferred roadmap

Keep game and session interfaces transport-neutral. The following items remain deferred future milestones:
- Remote internet play over public FQDNs / public Web PKI (private LAN TLS 1.3 on port 4783 is implemented in S9)
- Production client persistent authentication and credential lifecycle (mTLS deferred per AD-S9-02)
- Dynamic in-process TLS certificate reloading (deferred per AD-S9-04; restart rotation implemented)
- Operational monitoring, Prometheus metrics, and systemd supervision
- Durable crash-recovery and transaction persistence
- Release packaging and archive generation (deferred until explicitly requested)
- Physical two-computer cross-network LAN acceptance test (completed for 5-player plaintext baseline on October 17, 2026; multi-machine TLS 1.3 and large-scale physical LAN load testing remain future work)

Single-process initial design does not provide automatic failover. Adding new transports must preserve core game engine rules and determinism.
