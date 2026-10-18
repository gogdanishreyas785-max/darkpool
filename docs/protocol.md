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

TLS server trust, moderator authentication, and match admission are separate controls. Current setup scripts automate TLS trust and moderator setup, but do not automatically distribute each match's join token to all player clients.

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
