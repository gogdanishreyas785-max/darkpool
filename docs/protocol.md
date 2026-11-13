# DARKPOOL Implemented Wire Protocol

This document describes the current protocol implementation. Older draft text described a fixed header and version/request-ID fields that are not present in the current framing code. For user-facing commands and game rules, see the [User Guide](user-guide.md).

## Framing

The implementation in include/darkpool/network.hpp and src/network.cpp uses this TCP/UNIX/TLS stream framing:

~~~text
+-------------------------------+------------------------------+
| 4-byte unsigned big-endian N  | N bytes of UTF-8 JSON object |
+-------------------------------+------------------------------+
~~~

- The 4-byte length field is the JSON payload length; it does not include the prefix.
- Payload length must be in the range 1–4096 bytes (4 KiB maximum).
- Each JSON frame is an object with a string type field and command-specific fields.
- Frames are independent of socket read boundaries. A read may contain a partial prefix, partial payload, one complete frame, or multiple frames.
- FrameDecoder buffers incomplete bytes and emits complete frames; FrameStream queues decoded frames.
- OutputQueue handles partial writes on nonblocking sockets and bounds queued bytes at 64 KiB per connection.
- The current framing does not prepend the older proposed fixed header, version, request ID, or binary match-ID fields.

The same JSON framing is used over UNIX-domain sockets, plaintext TCP, and TLS 1.3 TCP. TLS changes transport protection, not application message framing.

## Connection and authorization model

1. The client establishes the selected transport. TLS clients verify the server certificate using a configured local CA or server-leaf fingerprint, including certificate validity and IP SAN checks.
2. The client identifies its session role as player or moderator during connection setup. Moderator sessions must provide the configured moderator authentication token; the server fails closed when that token is not configured.
3. The server assigns a session ID. For a protected match, the moderator creates a match with its own join token. Player admission requires the correct match ID and match token.
4. The server associates the connection/session with a match and assigns player identity/seat and private role. Client-supplied role/seat identity fields are rejected.
5. Commands are authorized against session role, match membership, current phase, player activity, and role-specific rules.
6. The coordinator applies state changes through the game engine and emits responses plus audience-filtered notifications. Private roles, hints, clue answers, research signals, and individual balances/holdings must not be sent through public snapshots.

TLS server trust, moderator authentication, and match admission are separate controls. The launch scripts store match tokens in an owner-only local token store. On a private LAN, player launchers can synchronize tokens from the server over the configured SSH/SCP connection; token files are not sent in public game notifications.

## Common client message types

The CLI constructs JSON requests. Common type values include:

| Operation | Message type | Important fields |
|---|---|---|
| Establish session | connect | client role; moderator authentication where required |
| Create match | create_match | optional player_count, optional join_token or open_lobby |
| Join match | join_match | match_id, optional join_token/token |
| Start match | start_match | match_id |
| Publish event | publish_event | match_id, impact from -2 to +2 |
| Trade | trade | match_id, action BUY/SELL/HOLD, quantity |
| Use clue | request_clue | match_id, target_id |
| Submit vote | vote | match_id, target_id or null for abstention |
| Status | match_status | match_id |
| Pause/resume/abort | pause, resume, abort | match_id; moderator-only |

The coordinator also supports moderator phase-control operations: open_trading, resolve_market, open_voting, and resolve_votes. See src/match_coordinator.cpp for validation and dispatch; see src/cli.cpp for terminal command mapping.

## Server response and notification types

Common responses include match_created, joined_match, trade_result, vote_result, market_result, vote_resolution, research_result, and error. Asynchronous notifications include server_event and market_snapshot; private results use dedicated private message types such as hint_private and clue_private.

The client runtime processes asynchronous notifications while awaiting ordinary command responses. They use the same frame format as all other messages. The coordinator chooses the audience: public state reaches match participants, while private results are routed only to authorized sessions.

## Timed progression

The coordinator uses server-owned monotonic deadlines and these default phase durations:

| Phase | Duration |
|---|---:|
| Discussion | 180 seconds |
| Trading | 120 seconds |
| Clue | 30 seconds |
| Transition interval before Voting | 10 seconds |
| Voting | 60 seconds |

Discussion expiry opens Trading. Trading expiry closes orders, treats missing orders as HOLD, resolves market state, and enters Clue unless the match finishes. Clue expiry starts the transition interval. Transition expiry opens Voting. Voting expiry treats missing votes as abstentions, resolves the vote, and either finishes the match or advances to next-round event selection.

Business-event selection remains moderator-driven. The current game does not use an approved server-selected production event deck.

## Limits and transport notes

- Maximum JSON payload: 4096 bytes.
- Maximum queued output: 64 KiB per client.
- Per-session request limiter: 20 requests/second with a burst of 40 in the Linux server runtime.
- At most four concurrent matches; 3–64 player seats per match; up to 256 player and four moderator connections in total.
- UNIX-domain socket: default path /tmp/darkpool.sock.
- Plaintext TCP: default loopback endpoint 127.0.0.1:4782. It provides no encryption.
- TLS 1.3 TCP: default secure port 4783; certificate chain/fingerprint, validity, server purpose, and IP SAN are verified.
- Public-Internet hosting, persistent user identities, mutual TLS for players, and durable state persistence are not implemented.

## Relevant implementation files

- include/darkpool/network.hpp and src/network.cpp: JSON framing, incremental decoder, stream and output queues.
- include/darkpool/socket_primitives.hpp and src/socket_primitives.cpp: socket creation and validation.
- include/darkpool/tls_transport.hpp and src/tls_transport.cpp: OpenSSL TLS configuration and I/O.
- include/darkpool/server_runtime.hpp and src/server_runtime.cpp: epoll server loop.
- include/darkpool/client_runtime.hpp and src/client_runtime.cpp: client transport runtime.
- include/darkpool/match_coordinator.hpp and src/match_coordinator.cpp: command routing, admission, and phase coordination.
- include/darkpool/game_engine.hpp and src/game_engine.cpp: authoritative game rules.


## Vote resolution and secret-ballot reporting

The `vote_resolved` server event and `vote_resolution` command response report aggregate participation separately from the elimination decision. Fields include:

- `eligible_voters`: active players eligible when voting opened.
- `votes_cast` and `votes_submitted`: accepted ballots, including explicit abstentions.
- `votes_abstained`: ballots submitted with `target_id: null`.
- `votes_missing`: eligible voters who submitted no ballot before resolution.
- `eliminated_player`: the player eliminated by the strict-majority rule, or `null`.
- `votes_received`: the eliminated player's tally; it is zero when nobody is eliminated.

The public resolution does not identify individual voters or broadcast their ballot targets. The engine's internal canonical event trace can contain vote-submission details and must not be exposed to players or included in public logs.

Example resolution event:

~~~json
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
~~~

## Match discovery and token enrollment

### List matches

Players can request the available match list without knowing the match ID:

~~~json
{"type":"list_matches"}
~~~

The response has type `matches_list` and an array of match entries. Each entry includes `match_id`, `phase`, `player_count`, `joined_players`, `open_lobby`, and `is_aborted`. This listing does not return any admission token.

### Request a protected match token

A player session can request a token for a joinable protected match using:

~~~json
{
  "type": "get_match_token",
  "match_id": 1,
  "enrollment_token": "<pre-shared-enrollment-secret>"
}
~~~

The server only honors this request for a player session while the match is in its Lobby phase. The server must have `DARKPOOL_ENROLLMENT_TOKEN` or `DARKPOOL_PLAYER_SECRET` configured, and the client must present the matching secret. The response contains `match_token_result` with `join_token`; invalid or missing authorization is rejected. The open-lobby response indicates that no token is needed.

This in-band enrollment is optional and is not configured automatically by the standard TLS setup scripts. Treat the enrollment secret as a credential that grants access to protected-match join tokens. The default launcher workflow instead uses SSH/SCP token synchronization after a match has been created.

## Server-Worker Internal IPC Wire Protocol

In addition to client TCP/TLS JSON framing, the server can delegate non-authoritative market modeling to a child worker process (`darkpool_worker`) over POSIX shared memory queues (`darkpool::ipc::ProcessSharedQueue`) synchronized by Linux `eventfd` (`EventNotifier`).

### Memory Layout & Alignment

The request and response queues use fixed-size 256-byte message slots (`darkpool::ipc::QueueMessageSlot`) with zero dynamic memory allocation. All structures use 8-byte alignment `#pragma pack(push, 8)`.

#### 1. Worker Request Message (`WorkerRequestMessage`)
Transferred from `darkpool_server` to `darkpool_worker`:
- `request_id`: uint64 sequence / correlation identifier generated monotonically by the server.
- `client_session_id`: uint64 client network session identifier.
- `opcode`: int32 operation code (`1 = Ping`, `2 = EvaluateMarketModel`, `3 = Shutdown`, `4 = Ready`).
- `price`: int32 baseline asset price.
- `health`: int32 baseline market health.
- `event_impact`: int32 scheduled business event impact (-100 to +100).
- `demand`: int32 buy volume / demand pressure (-10000 to +10000).
- `volume`: int32 total transaction volume (non-negative).
- `sentiment`: int32 market sentiment index (-1000 to +1000).
- `total_supply`: uint32 circulating supply.
- `reserve`: uint32 treasury reserve units.
- `flags`: uint32 control bits.

#### 2. Worker Response Message (`WorkerResponseMessage`)
Transferred from `darkpool_worker` to `darkpool_server`:
- `request_id`: uint64 matching the request sequence correlation ID.
- `client_session_id`: uint64 client network session identifier.
- `opcode`: int32 operation code matching the request.
- `status`: int32 outcome status (`0 = Success`, `1 = InvalidPayload`, `2 = QueueFull`, `3 = TimedOut`, `4 = WorkerFaulted`, `5 = WorkerExited`).
- `projected_price`: int32 calculated model price after simulation (clamped between 1 and 100).
- `projected_health`: int32 calculated market health (clamped between 0 and 10).
- `price_delta`: int32 change in asset price.
- `baseline_price`: int32 original input price.
- `flags`: uint32 status bits.
- `worker_pid`: uint32 worker PID producing the response.

### Client-Facing JSON Framing (`eval_model`)

Clients can submit an asynchronous simulation query. The optional `request_id` may be a JSON string or number; the server uses a separate internal 64-bit correlation ID and echoes the original client value with its original JSON type in success and failure responses.

```json
{
  "type": "eval_model",
  "request_id": "req-client-42",
  "price": 100,
  "health": 50,
  "event_impact": 1,
  "demand": 10,
  "volume": 20,
  "sentiment": 5,
  "total_supply": 1000,
  "reserve": 500
}
```

Input fields are strictly validated on the server before queuing:
- `price`: Integer in `[1, 100000]`
- `health`: Integer in `[0, 100]`
- `event_impact`: Integer in `[-100, 100]`
- `demand`: Integer in `[-10000, 10000]`
- `volume`: Non-negative integer in `[0, 1000000]`
- `sentiment`: Integer in `[-1000, 1000]`

If any input is outside its valid range, the server immediately returns:
```json
{
  "type": "eval_model_response",
  "request_id": "req-client-42",
  "status": "failed",
  "error": "invalid price: must be integer between 1 and 100000"
}
```

When the worker returns the calculation via the process-shared queue, `ServerRuntime` receives an epoll notification on the server eventfd and responds asynchronously:
```json
{
  "type": "eval_model_response",
  "request_id": "req-client-42",
  "status": "success",
  "projected_price": 102,
  "projected_health": 7,
  "price_delta": 2,
  "baseline_price": 103,
  "worker_pid": 12345
}
```
If the worker process is absent, terminated, or the queue is saturated, the server returns an error response (`status: "failed"`, `error: "worker unavailable"`) without delaying or affecting authoritative match coordination. All calculations use widened safe 64-bit integer arithmetic to prevent signed integer overflow.



### Player Seat Reconnection

Every successful `join_match` response includes a private, per-seat `resume_token`. Keep it secret. If the client disconnects, the server reserves that seat for five minutes. Reconnect by sending `join_match` with the same `match_id` and `resume_token`; the server restores the original player ID, role, and seat, including during an active match. A successful reconnect rotates the token, invalidating the previous value.

The CLI prints the token after joining. Set `DARKPOOL_PLAYER_RESUME_TOKEN` to that value before starting a replacement client, then run `join <match_id>`. A reconnect token is sufficient to reclaim an existing seat and does not require the match-wide admission token. Once the five-minute reservation expires, the seat may be assigned to a new player and the old token is invalid.
