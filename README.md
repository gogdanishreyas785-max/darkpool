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

## Playable CLI Gameplay

Moderator connections require an authentication secret supplied via the `DARKPOOL_MODERATOR_TOKEN` environment variable on both the server and the moderator client. If the server does not have `DARKPOOL_MODERATOR_TOKEN` set, it fails closed and rejects all moderator connection attempts. Players connect without a token.

> **Security Scope Note**: The server binds to `127.0.0.1` (loopback only). This mechanism does not implement TLS or production-ready transport authentication; the authentication token travels over the local loopback TCP connection.

On Linux, run the server and clients in separate terminals:

```sh
# Terminal 0: Start the TCP coordinator server (default port: 4782)
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_server [port]

# Terminal 1: Start the Moderator client
export DARKPOOL_MODERATOR_TOKEN="secret-moderator-token"
./build/darkpool_client 127.0.0.1 [port] moderator

# Terminals 2-6: Start five Player clients (no token required)
./build/darkpool_client 127.0.0.1 [port] player
```

The default port is `4782`. The client provides an interactive terminal CLI with command prompts, input validation, role guidance, and confidential information display. Raw JSON input (`{...}`) remains supported for scripted test harnesses.

### Moderator CLI Commands

- `create [r1 r2 r3 r4 r5]`: Create a match with a 5-player roster (defaults to standard roster: 1 Insider, 1 Investigator, 3 Employees). Sets the active match ID.
- `match <id>`: Switch active match ID context (default: 1).
- `start`: Start the match once all five players have joined.
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
- `clue [target_id]`: (*Investigator only*) Secretly investigate suspect player ID (1-5) during Clue phase. If omitted, prompts for the suspect ID.
- `vote [target_id|abstain]`: Cast elimination vote (player ID 1-5) or abstain (`0` or `abstain`) during Voting. If omitted, prompts for the target.
- `help`: Display available player commands and role notes.
- `quit` / `exit`: Disconnect from the match server.

### Confidentiality & Information Display

- **Private Information**: Private roles (`joined_match`), Insider hints (`hint_private`), and Investigator investigation results (`clue_private`) are delivered only to the acting connection and displayed privately on their terminal.
- **Public Information**: Phase transitions, company health, stock price, aggregate market execution summaries, elimination announcements, and game-over winner declarations are public and displayed to all participants.

### How to Run a Complete Game

1. **Launch Server**: Run `DARKPOOL_MODERATOR_TOKEN="secret" ./build/darkpool_server 4782` in Terminal 0.
2. **Launch Moderator**: Run `DARKPOOL_MODERATOR_TOKEN="secret" ./build/darkpool_client 127.0.0.1 4782 moderator` in Terminal 1.
3. **Launch Players**: In Terminals 2 to 6, launch `./build/darkpool_client 127.0.0.1 4782 player`.
4. **Create Match**: In Moderator terminal, type `create`.
5. **Join Players**: In each of the 5 Player terminals, type `join 1`. Each player receives their private role and confidential objectives.
6. **Start Match**: In Moderator terminal, type `start`. (Phase: `AwaitingEvent`).
7. **Publish Event**: In Moderator terminal, type `event 0` (or impact between -2 and 2). (Phase: `Discussion`).
8. **Insider Hint**: The Insider (Player 1) types `hint` to view the event impact secretly. Players discuss in the room/chat.
9. **Open Trading**: Moderator types `trading`.
10. **Submit Trades**: Each player submits their order: `buy`, `sell`, or `hold`.
11. **Resolve Market**: Moderator types `market`. The closing health, price, and market outcome are displayed. (Phase: `Clue`).
12. **Investigate Clue**: The Investigator (Player 2) types `clue <target_id>` (e.g. `clue 1` or `clue 3`) to secretly check if the target is the Insider.
13. **Open Voting**: Moderator types `voting`.
14. **Cast Votes**: Each player casts an elimination vote, e.g. `vote 1` or `vote abstain`.
15. **Resolve Votes**: Moderator types `resolve`. Eliminations and victory outcomes are announced.
16. **Next Rounds**: If neither side has won, repeat steps 7–15 for Rounds 2 and 3. The final winner is announced when the Insider is eliminated (`EmployeesWin`), the company collapses (`InsiderWin`), or Round 3 concludes (`InsiderWin`).

## Engine API outline

Construct `Match` with five explicit `{PlayerId, Role}` entries, call `start()`, inject each event with `publish_event(impact)`, and advance phases through `open_trading()`, trade submissions, `resolve_market()`, optional `use_clue()`, `open_voting()`, vote submissions and `resolve_votes()`. Methods return explicit error codes for rejected commands. `Match::from_state()` accepts only a validated, share-conserving state and is intended for deterministic test fixtures.
