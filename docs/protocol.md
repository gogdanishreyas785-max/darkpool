# Proposed wire protocol

**Direct PDF requirements:** versioned length-prefixed envelope, request IDs, match IDs, payload, bounded input, validation, authorization, backpressure, initial UNIX sockets. **Proposed choices requiring approval:** encoding, field widths, ACK semantics and numeric limits.

**Transport status:**
- **Currently implemented:** Nonblocking UNIX domain stream sockets (`/tmp/darkpool.sock`), localhost TCP stream sockets (`127.0.0.1:4782`), explicit non-loopback plaintext TCP binding on an assigned RFC 1918 private IPv4 address (`--bind-address <IPv4>`), and dedicated TLS 1.3 secure TCP transport on port `4783` (AD-S9-01) supporting local CA and leaf fingerprint pinning via OpenSSL 3.x, all monitored by epoll in `src/server_main.cpp`, with an interactive terminal client CLI in `src/client_main.cpp`. The plaintext LAN TCP transport on port 4782 is unencrypted and restricted strictly to assigned private subnets; the secure TLS 1.3 transport on port 4783 enforces encrypted, authenticated communication.
- **Original architecture requirement:** Nonblocking UNIX domain stream sockets monitored by epoll (implemented).
- **Deferred future work:** Public-Internet deployment, public Web PKI automation, dynamic certificate reload, and mutual TLS (mTLS) for players. Transport contains no game rules.

**Proposed framing:** 4-byte unsigned big-endian frame length, then fixed header (version u16, message type u16, request ID u64, match ID 16 bytes, payload length u32), then UTF-8 JSON payload. Length counts header+payload; validate exact payload length before allocation. Zero match ID only for pre-match/control requests.

Proposed client types: HELLO, CREATE_MATCH, JOIN_MATCH, READY, CHAT, WHISPER, TRADE, REQUEST_CLUE, VOTE, LEAVE, PING. Requests never carry authoritative roles, balances, health, price, other votes or outcomes.

Proposed server types: WELCOME, ACK, MATCH_CREATED, JOINED, PHASE, EVENT, ROLE_PRIVATE, HINT_PRIVATE, CHAT, WHISPER, TRADE_RESULT, CLUE_PRIVATE, VOTE_RESULT, STATE_PUBLIC, MATCH_RESULT, ERROR, PONG. All events are audience-filtered.

**Proposed ACK semantics requiring approval:** Each well-framed correlated command receives exactly one ACK after validation/transition and response enqueue, or ERROR on rejection. ACK includes request ID, result code, and authoritative sequence where relevant. PING returns PONG. Exact retry replays the same outcome without a second mutation. Malformed framing may close without ACK. This defines the p95 boundary in test-plan.md.

Connection flow: accept and assign connection ID; authenticate peer; create/resume server session; assign player identity server-side; join by validated operation; route using server-side session-to-match mapping; validate frame/schema/session/membership/phase/role/rate; apply transition, audit and enqueue authorized output; cleanup on disconnect under approved grace policy.

The current coordinator `create_match` command accepts optional integer `player_count` in [3,64]; omission preserves the five-player default. The server derives the role counts using DEC-02, shuffles that role list, and assigns roles to server-owned seats. Client-supplied `roles` are rejected. `join_match` only fills an unoccupied seat in that match, and `start_match` requires all configured seats. The moderator is not a player seat.

**Capacity and connection limits:** The initial S0 proposal envisioned one shared moderator connection and up to 256 player connections (257 total connections) as unverified design ceilings. This was superseded by the merged L6 multi-match implementation, which configures four match records, up to 256 player connections, and four independent authenticated moderator connections (one per match, 260 total concurrent connections; `kMaxMatches = 4`, `kMaxModerators = 4`, `kMaxConnections = 260`). Furthermore, capacity for these workloads is no longer unmeasured: L4 measured single-match 64-player capacity, and L6 measured four-match 256-player concurrent capacity with four independent moderators across functional and sustained 10-minute smoke / 30-minute soak benchmarks (`docs/load_test_l6_results.json`). S9 integrates strict TLS 1.3 secure transport on port 4783 while preserving these limits. Physical cross-device two-computer LAN acceptance for a 5-player match was completed on October 17, 2026 (see `docs/test-plan.md`), while public-Internet deployment remains deferred.

Proposed local authentication requiring approval: Linux peer credentials, restrictive socket ownership/mode, server session IDs; same UID does not distinguish humans. No persistent accounts. Reconnect secret policy remains open.

Process complete requests in receive order per connection; events carry per-match sequence, no total order across connections. Proposed duplicate policy requiring approval: request IDs per session, cache 256 outcomes/60s, exact retry replays, conflicting reuse rejected. ERROR has bounded stable code/request ID/safe detail and no secrets.

Proposed numeric limits requiring approval: ordinary frame 4 KiB (also suggested in PDF); chat/whisper 512 UTF-8 bytes; 20 req/s burst 40; output queue 64 KiB/client, high-water backpressure and disconnect persistent slow consumers; configurable connection/match/player caps. Validate lengths before allocation and bound work per readiness event.

**Enforced protocol resource limits:**
- **Maximum JSON frame payload:** 4 KiB (4,096 bytes). Inbound frames with declared length of 0 or exceeding 4,096 bytes are rejected immediately upon reading the 4-byte big-endian length prefix before allocating or accumulating payload data. Outbound frame serialization rejects payloads exceeding 4,096 bytes with an error.
- **Maximum pending output queue:** 64 KiB (65,536 bytes) per client connection. If a slow or stalled client's output queue exceeds 64 KiB, the server evicts and closes the offending connection cleanly while preserving all existing legitimate client sessions and match state.

Planned and executed tests: golden/fragmented/coalesced frames; invalid lengths/types/text/version; duplicate/retry; authorization/phase rejection; flood/slow reader; TLS 1.3 certificate validation, persistent write cursor semantics, and security-negative suites (T-S9-01..T-S9-25) verified under automated integration harness.

## Stage 2 and Stage 3/3.1 Event System, Timers, and Moderator Controls

### Push Notifications and Synchronized Market Snapshots
- Asynchronous notifications are broadcast to match participants as `{"type": "server_event", ...}` and `{"type": "market_snapshot", ...}`.
- Every state mutation delivers an audience-filtered push notification immediately without requiring players to poll with `status`.
- Response frames and asynchronous notification frames remain framed identically and separated; clients process notifications concurrently while idle or awaiting command responses.
- Privacy boundaries are strictly preserved: public events and snapshots NEVER reveal private roles, private cash/share balances, secret Insider event hints, or Investigator clue targets/results.

### Timed State Machine and Monotonic Deadlines
- The server owns all deadlines; deadlines are tracked using a monotonic clock (`std::chrono::steady_clock`) to ensure system time shifts do not alter remaining phase durations.
- Durations:
  - **Discussion**: 180 seconds.
  - **Trading**: 120 seconds.
  - **Clue**: 30 seconds.
  - **Transition interval**: 10 seconds.
  - **Voting**: 60 seconds.
- Warnings:
  - Timed phases broadcast countdown warnings at **30 seconds remaining** and **10 seconds remaining** via `{"type": "server_event", "event": "phase_warning", "seconds_remaining": 30|10}`. Warnings fire at most once per timed phase.
  - At Clue entry, the 30-second warning is broadcast immediately alongside `phase_started` (sharing the mutation's `state_version`) because a 30-second Clue phase begins with exactly 30 seconds remaining.
- Expiration behavior:
  - **Discussion expiration**: automatically transitions to `Trading` (120s timer).
  - **Trading expiration**: trading closes; unsubmitted player orders default to `HOLD`; authoritative market resolution executes; company health, stock price, and share reserves update; transitions to `Clue` (30s timer) (or `Finished` if health reaches 0).
  - **Clue expiration**: automatically starts the distinct 10-second transition interval to `Voting`.
  - **Transition interval expiration**: automatically opens `Voting` (60s timer).
  - **Voting expiration**: votes resolve; unsubmitted player votes default to abstention; strict majority threshold applied; candidate eliminated or tie declared; transitions to `AwaitingEvent` (next round) or `Finished` (on victory condition).

### Stage 3.2 Clue Semantics and Transition Interval Distinction
- **Clue Phase (30 seconds)**: Canonical game rules (`docs/game-rules.md`, Step 5) establish `Clue` as an independent game phase following trade resolution where active Investigators may submit one private clue per match (`request_clue`) with a binary Insider/not-Insider answer returned privately (`clue_private`). Under approved Stage 3.2 rules, Clue has a 30-second default monotonic duration.
- **Entry Warning Semantics**: Because the Clue phase starts with exactly 30 seconds remaining, the server emits `phase_started` and an immediate `phase_warning` (`seconds_remaining: 30`) upon entering Clue. At ~10 seconds remaining, a `phase_warning` (`seconds_remaining: 10`) is emitted once.
- **Decoupled Architecture & Automatic Progression**: `Clue` and `Transition interval` are maintained as strictly separate concepts:
  - `Clue` is the legal investigator inquiry window (30s). Votes are not accepted and voting is not resolved during Clue.
  - When the Clue deadline expires, the server automatically starts the distinct **Transition Interval** (`transition_interval` event reporting 10 seconds remaining before `Voting`).
  - Once in transition, new clue requests are rejected (`clue phase has ended`), and voting has not yet begun.
  - When the 10-second transition interval expires, the coordinator automatically opens `Voting` with a 60-second deadline.
  - In delayed tick processing scenarios where a single tick observes both the 10s Clue warning threshold and Clue expiry, the 10-second warning is guaranteed to be ordered before the transition interval announcement.
  - Direct transition APIs (`open_voting`) and the optional `with_transition` flag remain available for backward compatibility and test control without causing duplicate transitions.
- **Moderator-Driven Event Selection**: Business event selection in `AwaitingEvent` (`publish_event`) remains moderator-driven pending canonical approval of an automated scenario deck.

### Hybrid Moderator Controls
- **Authorization**: `pause`, `resume`, and `abort` commands are restricted strictly to the authenticated moderator who created the match. Player requests receive permission errors.
- **`pause`**: Freezes the match timer, computes remaining monotonic duration (`paused_remaining`), sets `is_paused: true`, broadcasts `match_paused` event and updated snapshot with remaining seconds. Phase progression (manual or automatic) is blocked while paused. Players can submit in-flight commands (e.g. trades, votes, status checks) while paused.
- **`resume`**: Restores deadline from saved remaining duration (`deadline = now + paused_remaining`), sets `is_paused: false`, broadcasts `match_resumed` event and updated snapshot with remaining seconds. Timers resume counting down from the exact saved duration without resetting or unexpectedly triggering expired deadlines.
- **`abort`**: Permanently terminates the match to `Finished` with outcome `"Aborted"`, stops all timers and transitions, broadcasts `match_aborted` and terminal snapshot, and rejects all subsequent commands for that match.

### State Versioning Policy
- `state_version` is a strictly monotonically increasing counter per match.
- State mutations (`start_match`, `publish_event`, `open_trading`, `resolve_market`, `open_voting`, `resolve_votes`, `pause`, `resume`, `abort`) increment `state_version` by 1.
- Multiple notifications generated as part of a single state mutation (e.g. `vote_resolved`, `round_started`, and `market_snapshot`) legitimately share that mutation's `state_version`.
- Non-mutating events (such as `phase_warning`) retain the current phase's `state_version`.
- Clients discard strictly older updates (`version < latest_state_version_`) to prevent out-of-order display, while processing equal-version events (`version == latest_state_version_`) sequentially.

### Transport Coverage and Verification
Asynchronous push notification delivery and moderator controls have been verified across all supported transports:
1. **Plaintext TCP**: Verified in `darkpool_notifications_e2e_tests` (`test_notifications_and_market_snapshots`).
2. **TLS 1.3 Secure Transport**: Verified with OpenSSL certificate and fingerprint verification in `darkpool_tls_integration_tests` (`test_scenario_26_tls_push_notifications_and_moderator_controls`).
3. **UNIX Domain Sockets**: Verified with nonblocking stream sockets in `darkpool_unix_socket_tests` (`test_unix_socket_notifications_and_moderator_controls`).

## Stage 4 Wire Protocol Extensions

### 1. Quantity Trading Command
- **Inbound Command**:
  ```json
  {
    "type": "trade",
    "match_id": 1,
    "action": "BUY",
    "quantity": 3
  }
  ```
  - `action`: `"BUY"`, `"SELL"`, or `"HOLD"` (case-sensitive on wire; CLI normalizes to upper).
  - `quantity`: Positive integer ($\ge 1$). Optional; if omitted, defaults to 1 for backward compatibility.
  - Zero, negative, non-integer, or overflowing quantities are rejected with an error before mutating state.
- **Outbound Response**:
  ```json
  {
    "type": "trade_result",
    "ok": true,
    "code": "Accepted",
    "quantity": 3
  }
  ```

### 2. Research / Scouting Action Command
- **Inbound Command**:
  ```json
  {
    "type": "research",
    "match_id": 1
  }
  ```
  (Alias `"type": "scout"` is accepted interchangeably).
- **Outbound Response (Strictly Private to Requesting Client)**:
  ```json
  {
    "type": "research_result",
    "ok": true,
    "match_id": 1,
    "direction": "Positive",
    "already_used": false,
    "credits_remaining": 90
  }
  ```
  - `direction`: Coarse classification (`"Positive"`, `"Neutral"`, or `"Negative"`). Does not leak numeric impact.
  - `already_used`: `false` on initial use (10 credits debited); `true` on repeated use (0 credits debited).
  - Calling outside `Discussion` phase or with insufficient credits returns `operation_result` with error code `WrongPhase` or `InsufficientCredits`.

### 3. Updated Public Market Snapshot and Status Schema
The authoritative `market_snapshot` and `match_status` envelopes include defined public market indicators:
```json
{
  "type": "market_snapshot",
  "match_id": 1,
  "state_version": 20,
  "round": 2,
  "phase": "Discussion",
  "health": 7,
  "price": 14,
  "sentiment": 3,
  "volatility": 5,
  "liquidity": 45,
  "total_supply": 100,
  "reserve": 45,
  "shares_held_by_players": 55,
  "outcome": "None",
  "last_market": {
    "executed_buys": 4,
    "executed_sells": 2,
    "closing_health": 7,
    "closing_price": 14,
    "closing_reserve": 45,
    "sentiment": 3,
    "volatility": 5,
    "liquidity": 45
  }
}
```
- `sentiment`: Signed integer $[-10, 10]$ tracking cumulative market buying/selling sentiment.
- `volatility`: Unsigned integer $[0, 20]$ reflecting trade volume and event tension.
- `liquidity`: Unsigned integer $[0, 100]$ percentage representing reserve share depth relative to total supply.
- Private individual holdings, credits, roles, clue targets, and research reports are strictly omitted from public snapshots.

### 4. Authoritative Vote Resolution and Secret-Ballot Reporting Schema
The authoritative `vote_resolved` (push event) and `vote_resolution` (command response) envelopes separate submitted participation counts from candidate elimination thresholds, preserving secret voting while preventing deceptive zero-vote reports:
```json
{
  "type": "server_event",
  "event": "vote_resolved",
  "match_id": 1,
  "state_version": 25,
  "round": 1,
  "eliminated_player": null,
  "votes_received": 0,
  "votes_cast": 1,
  "votes_submitted": 1,
  "votes_abstained": 0,
  "votes_missing": 3,
  "eligible_voters": 4,
  "outcome": "None"
}
```
- `eligible_voters`: Total number of active, non-eliminated players eligible when voting opened.
- `votes_submitted` / `votes_cast`: Total number of ballots received before deadline, including explicit abstentions.
- `votes_abstained`: Number of submitted ballots with explicit abstention (`target_id: null`).
- `votes_missing`: Number of eligible players who submitted no ballot prior to deadline (`eligible_voters - votes_cast`).
- `votes_received`: Number of votes received by the eliminated candidate (only populated when strict majority threshold > E/2 is met). When no candidate is eliminated, `votes_received` is reported as 0 or omitted from candidate display to avoid confusing the user.
- **Privacy Guarantee**: Individual voter IDs and ballot targets are never broadcast, preserving secret voting.

### 5. Match Discovery and Admission Protocol
To support variable-capacity lobbies, multiple concurrent matches, and automated admission without manual token copying:
- **List Matches (`list_matches`)**:
  - Inbound: `{"type": "list_matches"}`
  - Outbound:
    ```json
    {
      "type": "matches_list",
      "matches": [
        {
          "match_id": 1,
          "phase": "Lobby",
          "player_count": 4,
          "joined_players": 1,
          "open_lobby": false
        }
      ]
    }
    ```
- **Authenticated Token Enrollment (`get_match_token`)**:
  - Inbound: `{"type": "get_match_token", "match_id": 1, "enrollment_token": "<server-enrollment-secret>"}`
  - Outbound (Authorized):
    ```json
    {
      "type": "match_token_result",
      "ok": true,
      "match_id": 1,
      "join_token": "a1b2c3d4e5f60718293a4b5c6d7e8f90"
    }
    ```
  - Outbound (Unauthorized / Missing Authorization):
    ```json
    {
      "type": "error",
      "error": "invalid enrollment authorization"
    }
    ```
  - Pre-shared enrollment secret is configured on the server via `DARKPOOL_ENROLLMENT_TOKEN` or `DARKPOOL_PLAYER_SECRET`. Fails closed if not configured.
