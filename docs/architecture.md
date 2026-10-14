# Proposed C++20 architecture

## System design

One authoritative server process owns official game state. The current prototype implementation connects client processes through nonblocking localhost TCP stream sockets (`127.0.0.1:4782`) monitored by epoll (`src/server_main.cpp`), with an interactive client CLI (`src/client_main.cpp`). The original architecture specification in Draft 3 required UNIX domain stream sockets; a dedicated UNIX domain transport adapter, remote TCP/TLS 1.3, and remote certificate lifecycle remain deferred future work. The server authenticates sessions, validates actions, routes requests to independent matches, applies transitions, and emits authorized events. Use a modular daemon, not service-per-match.

Flow: client processes → localhost TCP transport (implemented prototype) / UNIX transport (original specification) → nonblocking framing/epoll connection manager → session/auth manager → match router → independent Match state machines → bounded output queues and clients.

One event loop owns mutations initially; no thread per client. Optional workers only for measured CPU-heavy tasks, returning results to the owner loop. Workers never mutate matches. Use monotonic deadlines via timerfd or a deadline heap.

## Components

- **Transport adapter:** Prototype implemented using nonblocking localhost TCP sockets (`127.0.0.1`) with epoll; UNIX-domain stream sockets (original requirement) and remote TCP/TLS 1.3 adapter remain deferred. No game rules in transport.
- **Connection manager:** connection IDs, framing, lifecycle, bounded input/output queues, rates, backpressure, disconnects.
- **Protocol:** versioned envelope, schema/length validation, request correlation, error codes.
- **Session manager:** server-assigned identity, authentication, permissions, reconnect/cleanup.
- **Match manager:** create/find/close, enforce caps, associate sessions and matches.
- **Game engine / Match:** roles, phases/deadlines, events, trades, clue, votes, company state, win rules; all authoritative.
- **Event/audit log:** important transitions and security events; separate from player output and redact by audience.
- **Timer/metrics/config:** monotonic deadlines, limits and bounded observability.

Each Match owns membership, roles, phases, deadlines, company state, trades, clue, chat routing, votes and logs. A request needs authenticated membership and legal phase/role. Match state never retains client-buffer pointers. No slow client, match fan-out, log write, or expensive computation may block the loop.

## Isolation and targets

The approved per-match player range is 3–64, with five as the default; the moderator is outside that count. The coordinator permits at most four matches. The TCP process has explicit design caps of 256 player connections, one moderator connection, and 257 total connections. One moderator can own the four matches. These limits describe configured resource ceilings only; they do not establish that 64-player matches or four simultaneous matches meet performance or reliability targets.

Match IDs route requests but server-side session membership authorizes them. A match timeout or message surge must not stall another match. Bound frames, queues, rates, connections, matches, players and per-event work; safe disconnect/reconnect behavior follows approved policy.

The product configuration maximum is 64 active players per match and four concurrent matches (up to 256 players plus moderators). This is design support only. p95 acknowledgement ≤100 ms and 30-minute steady reliability are proposed acceptance thresholds, not achieved results; the 64-player/four-match capacity remains explicitly unverified until measured under a real load test.

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

## Future transport

Keep game and session interfaces transport-neutral. TCP/TLS 1.3, certificate/client identity lifecycle and deployment are later work; adding a transport must not rewrite game rules. Single-process initial design does not provide automatic failover.
