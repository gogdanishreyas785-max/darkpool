# DARKPOOL Current Architecture

This document describes the implementation in the repository, not a proposed future layout. User-facing installation and game operation are in the [User Guide](user-guide.md).

## System overview

DARKPOOL is a Linux C++20 server/client application with a deterministic game engine, match coordinator, and communications library. The authoritative server owns role assignment, match membership, action validation, market/vote resolution, deadlines, and public/private event routing.

Supported transports:

- **UNIX-domain stream socket** (default /tmp/darkpool.sock) for processes on the same Linux machine.
- **Plaintext TCP** (default loopback port 4782; non-loopback binding, when enabled, is restricted to an assigned RFC 1918 private IPv4 address). This is not encrypted and must not be used on untrusted networks.
- **TLS 1.3 TCP** on the configured TLS listener (default port 4783), using OpenSSL 3.x and client verification by local CA or SHA-256 server-leaf fingerprint.

The Linux server uses nonblocking I/O and epoll. It does not create a thread per client. A single server event loop serializes coordinator state changes; bounded queues and per-connection rate limits protect the loop from unbounded output and request floods.

## Runtime request path

~~~text
Terminal command
    |
    v
src/cli.cpp (parse command -> JSON request)
    |
    v
ClientConnection
    |
    +-- UNIX-domain socket
    +-- TCP stream
    +-- TLS 1.3 over TCP
    |
    v
Frame / FrameDecoder / OutputQueue
    |
    v
ServerRuntime (epoll, connection lifecycle, limits)
    |
    v
src/server_main.cpp (session authentication and wiring)
    |
    v
MatchCoordinator (authorization, match routing, timers, notifications)
    |
    v
Match / game_engine.cpp (authoritative deterministic rules)
    |
    v
Response + audience-filtered event notifications
~~~

## CMake targets

| Target | Source modules | Responsibility |
|---|---|---|
| darkpool_engine | src/game_engine.cpp | Deterministic game rules and canonical state; no sockets or timer ownership. |
| darkpool_communications | src/network.cpp; Linux additionally includes socket/TLS and client/server runtime sources | Message framing, stream decoding, socket connection primitives, TLS 1.3, nonblocking I/O. |
| darkpool_coordinator | src/match_coordinator.cpp | Session/match routing, join-token enforcement, action authorization, phase timers, market/vote transitions, notifications. |
| darkpool_cli | src/cli.cpp | Human commands and response formatting. |
| darkpool_server | src/server_main.cpp | Linux server configuration, listeners, session authentication, and event-loop wiring. |
| darkpool_client | src/client_main.cpp | Terminal client, connection options, and interactive request/notification handling. |

## Communication library

The CMake target darkpool_communications is defined in CMakeLists.txt.

- include/darkpool/network.hpp / src/network.cpp: JSON payloads framed as a 4-byte big-endian length followed by JSON bytes. FrameDecoder handles fragmented/coalesced stream reads. Payload limit is 4 KiB; OutputQueue bounds pending output to 64 KiB.
- include/darkpool/socket_primitives.hpp / src/socket_primitives.cpp: TCP and UNIX-domain socket creation/connect, private-address validation, and safe UNIX socket path lifecycle.
- include/darkpool/tls_transport.hpp / src/tls_transport.cpp: TLS 1.3 contexts and nonblocking OpenSSL I/O; certificate validity, key usage, and IP SAN checks; CA verification or fingerprint pinning.
- include/darkpool/server_runtime.hpp / src/server_runtime.cpp: epoll-based nonblocking server I/O, handshake/read/write progression, connection lifecycle, rate limits, bounded output, multicast, and broadcast.
- include/darkpool/client_runtime.hpp / src/client_runtime.cpp: client transport adapters, frame send/receive, and asynchronous notification polling.
- include/darkpool/reactor_dispatch.hpp: helper for scheduling normal and resumed TLS operations according to epoll readiness.

The framing layer does not invent message types or game rules. The coordinator validates message semantics and the engine applies rules.

## Authoritative game state

- Matches contain 3–64 player seats, default five; the moderator is a separate connection.
- Role counts are derived from match capacity and shuffled on the server. Clients cannot supply roles or seat IDs.
- Up to four matches can coexist; configured limits allow up to 256 player connections and four moderator connections total.
- The moderator chooses the signed business-event impact for each round. Discussion, Trading, Clue, transition, and Voting timers then drive normal phase progression.
- Private role, hint, clue, and research information is audience-filtered. Canonical/replay state contains secrets and must not be sent to players.

## Security boundaries and limits

TLS authenticates the server to the client and protects bytes in transit; it does not replace application authorization. The moderator credential authenticates moderator sessions. Protected matches have separate match join tokens. Current setup scripts do not automatically distribute each new match token to all player clients; see the [User Guide](user-guide.md#8-match-tokens-and-access-control).

Public-Internet deployment, persistent player accounts, mutual TLS player identity, durable crash recovery, and dynamic in-process certificate reload are not currently supported. For deployment commands, see the [User Guide](user-guide.md#3-option-b-multiple-computers-over-a-private-lan).
