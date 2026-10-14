# Proposed wire protocol

**Direct PDF requirements:** versioned length-prefixed envelope, request IDs, match IDs, payload, bounded input, validation, authorization, backpressure, initial UNIX sockets. **Proposed choices requiring approval:** encoding, field widths, ACK semantics and numeric limits.

**Transport status:**
- **Currently implemented:** Nonblocking UNIX domain stream sockets (`/tmp/darkpool.sock`) and localhost TCP stream sockets (`127.0.0.1:4782`) monitored by epoll in `src/server_main.cpp`, with an interactive terminal client CLI in `src/client_main.cpp`.
- **Original architecture requirement:** Nonblocking UNIX domain stream sockets monitored by epoll (implemented).
- **Deferred future work:** Production TCP/TLS 1.3 remote transport and certificate lifecycle. Transport contains no game rules.

**Proposed framing:** 4-byte unsigned big-endian frame length, then fixed header (version u16, message type u16, request ID u64, match ID 16 bytes, payload length u32), then UTF-8 JSON payload. Length counts header+payload; validate exact payload length before allocation. Zero match ID only for pre-match/control requests.

Proposed client types: HELLO, CREATE_MATCH, JOIN_MATCH, READY, CHAT, WHISPER, TRADE, REQUEST_CLUE, VOTE, LEAVE, PING. Requests never carry authoritative roles, balances, health, price, other votes or outcomes.

Proposed server types: WELCOME, ACK, MATCH_CREATED, JOINED, PHASE, EVENT, ROLE_PRIVATE, HINT_PRIVATE, CHAT, WHISPER, TRADE_RESULT, CLUE_PRIVATE, VOTE_RESULT, STATE_PUBLIC, MATCH_RESULT, ERROR, PONG. All events are audience-filtered.

**Proposed ACK semantics requiring approval:** Each well-framed correlated command receives exactly one ACK after validation/transition and response enqueue, or ERROR on rejection. ACK includes request ID, result code, and authoritative sequence where relevant. PING returns PONG. Exact retry replays the same outcome without a second mutation. Malformed framing may close without ACK. This defines the p95 boundary in test-plan.md.

Connection flow: accept and assign connection ID; authenticate peer; create/resume server session; assign player identity server-side; join by validated operation; route using server-side session-to-match mapping; validate frame/schema/session/membership/phase/role/rate; apply transition, audit and enqueue authorized output; cleanup on disconnect under approved grace policy.

The current coordinator `create_match` command accepts optional integer `player_count` in [3,64]; omission preserves the five-player default. The server derives the role counts using DEC-02, shuffles that role list, and assigns roles to server-owned seats. Client-supplied `roles` are rejected. `join_match` only fills an unoccupied seat in that match, and `start_match` requires all configured seats. The moderator is not a player seat. Four match records and 256 player plus one moderator connection (257 total) are configured limits, not measured capacity.

Proposed local authentication requiring approval: Linux peer credentials, restrictive socket ownership/mode, server session IDs; same UID does not distinguish humans. No persistent accounts. Reconnect secret policy remains open.

Process complete requests in receive order per connection; events carry per-match sequence, no total order across connections. Proposed duplicate policy requiring approval: request IDs per session, cache 256 outcomes/60s, exact retry replays, conflicting reuse rejected. ERROR has bounded stable code/request ID/safe detail and no secrets.

Proposed numeric limits requiring approval: ordinary frame 4 KiB (also suggested in PDF); chat/whisper 512 UTF-8 bytes; 20 req/s burst 40; output queue 64 KiB/client, high-water backpressure and disconnect persistent slow consumers; configurable connection/match/player caps. Validate lengths before allocation and bound work per readiness event.

**Enforced protocol resource limits:**
- **Maximum JSON frame payload:** 4 KiB (4,096 bytes). Inbound frames with declared length of 0 or exceeding 4,096 bytes are rejected immediately upon reading the 4-byte big-endian length prefix before allocating or accumulating payload data. Outbound frame serialization rejects payloads exceeding 4,096 bytes with an error.
- **Maximum pending output queue:** 64 KiB (65,536 bytes) per client connection. If a slow or stalled client's output queue exceeds 64 KiB, the server evicts and closes the offending connection cleanly while preserving all existing legitimate client sessions and match state.

Planned tests: golden/fragmented/coalesced frames; invalid lengths/types/text/version; duplicate/retry; authorization/phase rejection; flood/slow reader. TCP cert/plaintext tests only if later S9 is approved.
