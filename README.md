# DARKPOOL — deterministic S1 engine and S2 Stage 1 TCP echo

S1 implements an in-process C++20 engine for the approved five-player baseline. It provides explicit state transitions, deterministic event input, market resolution, private clue results, voting and victory evaluation, plus a CMake unit-test target.

S2 Stage 1 adds a standalone, echo-only TCP/JSON transport prototype. It does not call the S1 engine or implement game commands. The Linux server uses nonblocking sockets with epoll and accepts up to five connections declaring `player` plus one declaring `moderator`; these labels only reserve connection slots and provide no authentication or moderator privileges. The server binds to `127.0.0.1` by default.

## Scope and implementation status

- The engine requires exactly five distinct player IDs with one Insider, one Investigator and three Employees. The role assignment is injected by the caller; S1 does not shuffle or choose roles because no role-assignment policy was approved.
- Business-event impacts are injected by the caller. The game engine itself has no random source, clock, sockets, epoll, client, server, persistence, or match-making; the echo transport is a separate library and executable pair.
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

## TCP echo prototype

On Linux, run the server and client in separate terminals:

```sh
./build/darkpool_server [port]
./build/darkpool_client 127.0.0.1 [port] player
```

Use `moderator` in place of `player` for the one moderator connection. The default port is `4782`. Enter one JSON value per input line; each value is echoed as JSON. The initial connection frame is `{"type":"connect","role":"player"}` or `{"type":"connect","role":"moderator"}` and is echoed like any other frame. Frames are a 4-byte unsigned big-endian JSON byte length followed by UTF-8 JSON, limited to 1 MiB. Malformed, zero-length, or oversized frames close the connection. The client accepts a hostname or IP address.

The TCP listener is loopback-only and has no authentication, TLS, reconnection, heartbeat, persistence, match state, or phase transitions. Five player slots plus one moderator slot are a transport smoke-test limit, not evidence of the 64-player/four-match capacity target. CMake fetches the pinned `nlohmann/json` 3.11.3 dependency when no package is installed. On non-Linux systems CMake builds the engine and framing tests; the epoll server/client and live server integration test require Linux.

## Engine API outline

Construct `Match` with five explicit `{PlayerId, Role}` entries, call `start()`, inject each event with `publish_event(impact)`, and advance phases through `open_trading()`, trade submissions, `resolve_market()`, optional `use_clue()`, `open_voting()`, vote submissions and `resolve_votes()`. Methods return explicit error codes for rejected commands. `Match::from_state()` accepts only a validated, share-conserving state and is intended for deterministic test fixtures.

