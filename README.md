# DARKPOOL — deterministic S1 engine and S2 TCP command prototype

S1 implements an in-process C++20 engine for the approved five-player baseline. It provides explicit state transitions, deterministic event input, market resolution, private clue results, voting and victory evaluation, plus a CMake unit-test target.

S2 adds a minimal TCP/JSON session coordinator for the existing S1 engine. It supports server-assigned player membership, isolated five-player matches, moderator-controlled event/phase transitions, and player trade, clue, vote, and hint commands. The Linux server uses nonblocking sockets with epoll and accepts up to five connections declaring `player` plus one declaring `moderator`. The server binds to `127.0.0.1` by default.

## Scope and implementation status

- The engine requires exactly five distinct player IDs with one Insider, one Investigator and three Employees. The role assignment is injected by the caller; S1 does not shuffle or choose roles because no role-assignment policy was approved.
- Business-event impacts are injected by the match's creating moderator. The game engine itself has no random source, clock, sockets, epoll, client, server, or persistence; match/session coordination is a separate server module.
- Shares are conserved between players and the finite match reserve. Five-player matches are implemented. `supply_plan(64)` verifies the documented 64-seat arithmetic only; larger-match role/gameplay rules are not implemented.
- Credits are non-conserved player counters. The provisional S1 cap is 1,000,000 credits per player (`uint32_t`); an order that would exceed it is rejected and consumes the round's trade-action slot under the approved action-slot rule. This is an implementation detail, not a new gameplay approval.
- Canonical engine state is an internal test/replay view containing secret roles and clue/event information. It must not be sent to players.
- The unchanged S1 engine and portable framing target build with MSVC 19.44.35228.0; the two available Windows CTest tests passed. On Ubuntu 24.04, GitHub Actions run `38188504530` built all targets with warnings-as-errors and passed the S1, framing, and live TCP integration tests. These checks do not establish load, latency, security, or scalability results.

## Build and test on Ubuntu

Requirements: CMake 3.20 or later and a C++20 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Enable warnings-as-errors with `-DDARKPOOL_WARNINGS_AS_ERRORS=ON` when configuring.

## TCP protocol prototype

On Linux, run the server and client in separate terminals:

```sh
./build/darkpool_server [port]
./build/darkpool_client 127.0.0.1 [port] player
```

Use `moderator` in place of `player` for the one moderator connection. The default port is `4782`. Enter one JSON value per input line; each value is echoed as JSON. The initial connection frame is `{"type":"connect","role":"player"}` or `{"type":"connect","role":"moderator"}` and is echoed like any other frame. Frames are a 4-byte unsigned big-endian JSON byte length followed by UTF-8 JSON, limited to 1 MiB. Malformed, zero-length, or oversized frames close the connection. The client accepts a hostname or IP address.

The TCP listener is loopback-only and has no secure authentication, TLS, reconnection, heartbeat, or persistence. The initial launcher is trusted to assign the self-declared `player` or `moderator` connection label; this is not moderator authentication and must not be used on an untrusted network. Five player slots plus one moderator slot are a transport smoke-test limit, not evidence of the 64-player/four-match capacity target. CMake fetches the pinned `nlohmann/json` 3.11.3 dependency when no package is installed. On non-Linux systems CMake builds the engine and framing tests; the epoll server/client and live server integration test require Linux.

Implemented command messages use the existing length-prefixed JSON framing. A moderator creates a match with an explicit five-role roster, players join and receive a server-assigned player ID and their own private role, and the creating moderator alone may start, inject an event, or advance/resolve phases. Players send `trade`, `request_clue`, `vote`, and `insider_hint`; their acting identity is derived from connection membership. `match_status` is a sanitized public view with aggregate market/vote outcomes; it omits roles, event impact, clue results, submitted orders, and vote targets. The server does not proactively broadcast results; players retrieve the latest public outcome through `match_status`.

## Engine API outline

Construct `Match` with five explicit `{PlayerId, Role}` entries, call `start()`, inject each event with `publish_event(impact)`, and advance phases through `open_trading()`, trade submissions, `resolve_market()`, optional `use_clue()`, `open_voting()`, vote submissions and `resolve_votes()`. Methods return explicit error codes for rejected commands. `Match::from_state()` accepts only a validated, share-conserving state and is intended for deterministic test fixtures.

