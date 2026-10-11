# DARKPOOL S1 — deterministic game engine

S1 implements an in-process C++20 engine for the approved five-player baseline. It provides explicit state transitions, deterministic event input, market resolution, private clue results, voting and victory evaluation, plus a CMake unit-test target.

## Scope and implementation status

- The engine requires exactly five distinct player IDs with one Insider, one Investigator and three Employees. The role assignment is injected by the caller; S1 does not shuffle or choose roles because no role-assignment policy was approved.
- Business-event impacts are injected by the caller. The engine has no random source, clock, sockets, epoll, client, server, persistence, or matchmaking.
- Shares are conserved between players and the finite match reserve. Five-player matches are implemented. `supply_plan(64)` verifies the documented 64-seat arithmetic only; larger-match role/gameplay rules are not implemented.
- Credits are non-conserved player counters. The provisional S1 cap is 1,000,000 credits per player (`uint32_t`); an order that would exceed it is rejected and consumes the round's trade-action slot under the approved action-slot rule. This is an implementation detail, not a new gameplay approval.
- Canonical engine state is an internal test/replay view containing secret roles and clue/event information. It must not be sent to players.
- CMake build and unit tests have passed with MSVC 19.44.35228.0 on Windows. An Ubuntu build has not yet been run. No load, latency, security, or scalability result is claimed.

## Build and test on Ubuntu

Requirements: CMake 3.20 or later and a C++20 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Enable warnings-as-errors with `-DDARKPOOL_WARNINGS_AS_ERRORS=ON` when configuring.

## Engine API outline

Construct `Match` with five explicit `{PlayerId, Role}` entries, call `start()`, inject each event with `publish_event(impact)`, and advance phases through `open_trading()`, trade submissions, `resolve_market()`, optional `use_clue()`, `open_voting()`, vote submissions and `resolve_votes()`. Methods return explicit error codes for rejected commands. `Match::from_state()` accepts only a validated, share-conserving state and is intended for deterministic test fixtures.
