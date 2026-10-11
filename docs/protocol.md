# Proposed wire protocol

Draft 3 requires a versioned length-prefixed envelope and permits binary or newline-delimited JSON. The encoding and exact limits below are proposals; approval required before implementation.

## Transport and frame

Initial transport: nonblocking UNIX domain stream sockets on Ubuntu/Linux, monitored by epoll. Reads may be fragmented or coalesced; use bounded buffers. Transport exposes messages/queued writes and contains no game rules.

**Proposed framing — approval required:** 4-byte unsigned big-endian frame length, then fixed header: version u16, message type u16, request ID u64, match ID 16 bytes, payload length u32, then UTF-8 JSON payload. Length counts header plus payload; payload length must match remaining bytes. Zero match ID only for pre-match/control messages. Validate before allocation.

Later TCP adapter may use TLS 1.3 and explicit client identity without changing game rules. TCP is deferred.

## Message types (proposed names)

| Direction | Types | Purpose |
|---|---|---|
| Client → server | HELLO, CREATE_MATCH, JOIN_MATCH, READY, CHAT, WHISPER, TRADE, REQUEST_CLUE, VOTE, LEAVE, PING | Requests only. Never accept client authority over role, cash, shares, health, price, another player's vote, or outcome. |
| Server → client | WELCOME, MATCH_CREATED, JOINED, PHASE, EVENT, ROLE_PRIVATE, HINT_PRIVATE, CHAT, WHISPER, TRADE_RESULT, CLUE_PRIVATE, VOTE_RESULT, STATE_PUBLIC, MATCH_RESULT, ERROR, PONG | Audience-filtered replies/events. |
| Operator | MODERATOR_CREATE/START/CLOSE/STATUS | Separate authorization from player protocol. |

Payloads use typed allowlisted fields. Reject malformed required fields, invalid enums/IDs, unsupported versions and unknown required types. Never deserialize arbitrary object graphs.

## Connection, identity, authorization

1. Accept connection and assign server-side connection ID.
2. Authenticate transport peer; create/resume server-owned session under approved policy.
3. Assign player identity server-side; join via validated operation.
4. Route by server-side session-to-match mapping; match ID alone proves no membership.
5. Validate frame, schema/limits, session, membership, phase/role permission, rate; apply transition, append audit, enqueue authorized output.
6. On disconnect, mark disconnected, apply approved grace policy, clean resources, and retain no client-buffer pointers.

**Proposed local auth — approval required:** Check Linux peer credentials and restrictive socket ownership/mode; assign random server session IDs. Peer credentials identify an OS account, not distinct humans under one UID. No persistent accounts. Reconnect token lifetime and delivery remain decisions.

## Ordering, IDs, errors

Process complete requests in receive order per connection. Events carry per-match sequence numbers; no total order across connections is promised.

**Proposed duplicate policy — approval required:** Request IDs unique per session. Cache last 256 outcomes for 60 seconds; exact retry replays response without repeating mutation; reuse with different payload returns DUPLICATE_ID_CONFLICT. Reject stale phase/sequence actions.

ERROR includes bounded stable code, request ID, safe detail; never secret state. Codes: BAD_FRAME, UNSUPPORTED_VERSION, UNKNOWN_TYPE, INVALID_FIELD, UNAUTHENTICATED, NOT_MEMBER, NOT_PERMITTED, WRONG_PHASE, RATE_LIMITED, DUPLICATE_ID_CONFLICT, RESOURCE_LIMIT, INTERNAL. Malformed framing may close connection; invalid game action rejects without mutation.

## Proposed initial limits (approval required)

- Ordinary frame: 4 KiB (PDF proposal); larger events need explicit cap.
- Chat/whisper: 512 UTF-8 bytes.
- Request rate: 20/second sustained, burst 40; gameplay also limited by phase.
- Output queue: 64 KiB/connection; pause/reject nonessential traffic at high-water, disconnect persistent slow consumer at hard cap.
- Input buffer bounded to max frame plus framing; process bounded work per readiness event.
- Configurable caps for total connections, matches, players/match. Baseline is five active players; capacity target is 64/match and four matches (up to 256 players, plus moderators/test clients).

Rates and queue sizes beyond the PDF's proposed 4 KiB frame limit are new defaults. Record limit metrics without secret payloads.

## Validation and verification

Reject invalid version/type/schema, truncated/oversized/length-mismatch frame, invalid UTF-8, unauthorized membership/action, wrong phase, duplicate action and over-rate input. Validate before allocation. Use bounded queues and explicit backpressure. Never synchronously write to clients or block event loop on disk, DNS, or expensive work.

Version compatibility proposal: support one current major version and reject others with bounded error then close (approval required). Planned tests: golden encode/decode; fragmented/coalesced frames; invalid lengths/types/text; retry/duplicate behavior; auth/phase rejection; flood/slow-reader. Future TCP tests only if S9 approved.
