# DARKPOOL — deterministic C++20 engine and TCP / UNIX-domain socket prototype

The C++20 engine and coordinator support matches configured for 3–64 active players. Five remains the default and the original baseline is one Insider, one Investigator, and three Employees. The moderator is a separate non-player.

The coordinator supports server-assigned membership, independent matches with different capacities, moderator-controlled event/phase transitions, and player trade, clue, vote, and hint commands. The Linux server prototype uses nonblocking stream sockets with epoll, supporting both local UNIX-domain stream sockets (`/tmp/darkpool.sock`) and localhost TCP (`127.0.0.1:4782`).

## Scope and implementation status

- **Transport and CLI:** The server implementation supports nonblocking UNIX-domain stream sockets (`/tmp/darkpool.sock`) and localhost TCP stream sockets (`127.0.0.1:4782`) monitored by epoll (`src/server_main.cpp`), with an interactive terminal CLI (`src/client_main.cpp`). UNIX sockets feature safe lifecycle management (stale socket file detection/recovery, non-socket file preservation, restrictive 0700 permissions, and signal-driven cleanup). Remote TCP/TLS 1.3 and production certificate lifecycle remain deferred future work.
- **Valid match sizes:** Valid match sizes are 3–64. Role counts are deterministic: `ceil(N/8)` Insiders, `ceil(N/16)` Investigators, and the remaining Employees. The server shuffles and assigns roles; clients cannot supply roles. Five remains the default.
- **Event injection and engine separation:** Business-event impacts are injected by the match's creating moderator. The game engine itself has no random source, clock, sockets, epoll, client, server, or persistence; match/session coordination is a separate server module.
- **Share conservation:** Shares are conserved between players and each match's finite reserve using the approved 20N total / 10N player-held / 10N reserve formula.
- **Victory and clue invariants:** A match ends with an Insider-team win if health reaches zero, or if at least one Insider survives through round three. Eliminating an Insider only gives Employees the win when no Insiders remain. Each Investigator has an independent once-per-match clue; legacy match-level clue fields mirror the first Investigator's clue.
- **Capacity limits and verification status:** The coordinator allows four concurrent matches. The server is configured for at most 256 player connections plus four independent authenticated moderator connections (260 total connections); each match is independently owned and controlled by its dedicated moderator connection. Four-match 256-player concurrent functional capacity is verified through reproducible benchmarks (`docs/load_test_l6_results.json`). Sustained long-duration multi-match reliability (10-minute smoke and 30-minute reliability soak workloads on a single persistent server) is evaluated as a distinct stage; see `docs/test-plan.md` for current measured status and evidence.
- **Credits:** Credits are non-conserved player counters. The provisional S1 cap is 1,000,000 credits per player (`uint32_t`); an order that would exceed it is rejected and consumes the round's trade-action slot under the approved action-slot rule. This is an implementation detail, not a new gameplay approval.
- **Canonical view privacy:** Canonical engine state is an internal test/replay view containing secret roles and clue/event information. It must not be sent to players.
- **Test coverage:** Engine roster tests cover N=3,4,5,8,9,16,24,32,64. Integration tests cover five-player and N=8 matches over TCP, as well as complete UNIX-domain socket lifecycle, authentication, five-player gameplay, and malformed-frame isolation. These functional tests do not establish load, latency, or server capacity; no load test is recorded for variable-size matches.

## Build and test on Ubuntu

Requirements: CMake 3.20 or later and a C++20 compiler.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Configure with warnings-as-errors using `-DDARKPOOL_WARNINGS_AS_ERRORS=ON`.

## Playable CLI Gameplay

Moderator connections require an authentication secret supplied via the `DARKPOOL_MODERATOR_TOKEN` environment variable on both the server and the moderator client. If the server does not have `DARKPOOL_MODERATOR_TOKEN` set, it fails closed and rejects all moderator connection attempts. Players connect without a token.

> **Security Scope Note**: The server binds to local UNIX domain sockets and `127.0.0.1` (loopback only). This mechanism does not implement TLS or production-ready transport authentication; authentication tokens travel over the local loopback or local socket connection.

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
# Terminal 0: Start the TCP coordinator server (default port: 4782)
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_server 4782

# Terminal 1: Start the Moderator client
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_client 127.0.0.1 4782 moderator

# Start 3-64 Player clients (no token required); use the same number as the created match
./build/darkpool_client 127.0.0.1 4782 player
```

The default port is `4782`. The client provides an interactive terminal CLI with command prompts, input validation, role guidance, and confidential information display. Raw JSON input (`{...}`) remains supported for scripted test harnesses.

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

## Engine API outline

Construct `Match` with 3–64 `{PlayerId, Role}` entries whose role counts match `role_counts(N)`, call `start()`, inject each event with `publish_event(impact)`, and advance phases through `open_trading()`, trade submissions, `resolve_market()`, optional `use_clue()` calls by each Investigator, `open_voting()`, vote submissions and `resolve_votes()`. Methods return explicit error codes for rejected commands. `Match::from_state()` accepts only a validated, share-conserving state and is intended for deterministic test fixtures.
