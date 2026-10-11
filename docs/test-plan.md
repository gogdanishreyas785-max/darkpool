# DARKPOOL test and evidence plan

Planned tests only; none are reported as run. Every result must be labeled PASS, FAIL, NOT RUN, or BLOCKED, with evidence.

## Functional and unit tests

| ID | Test | Acceptance evidence |
|---|---|---|
| T-F01 | Five clients complete all three rounds. | Correct sequence and automated transcript/demo. |
| T-F02 | Server-side roles and visibility. | Baseline roster correct; unauthorized clients never receive role/hint; moderator view follows approved policy. |
| T-F03 | Public chat and authorized whispers. | Correct recipients; private message not broadcast; transcript. |
| T-F04 | Trade, clue, vote permissions and phase. | Legal action once; illegal/late/duplicate action no state mutation. |
| T-U01 | State-machine invariants. | Deterministic tests for approved roles, economy, event effects, clues, votes, deadlines and victory. |
| T-U02 | Boundary/tie behavior. | Ties/no votes, absent players, health/price bounds, insufficient cash/shares, empty reserve, repeated actions follow approved rules. |

## Integration, isolation and security

| ID | Test | Acceptance evidence |
|---|---|---|
| T-I01 | UNIX socket lifecycle, fragmented/coalesced frames. | Multiple client processes; disconnect safe. |
| T-I02 | Two concurrent matches. | Independent outcomes and timers. |
| T-I03 | Four-match isolation. | Unique role/chat/clue/trade/vote/timer/log canaries never escape owning match. |
| T-I04 | Authentication and membership. | Forged identity/nonmember rejected, state unchanged. |
| T-I05 | Disconnect/reconnect. | Approved grace policy, cleanup; no identity theft. |
| T-P01 | Invalid versions/types/fields, truncated/oversized/invalid UTF-8 frames. | Bounded rejection/close; no crash/state corruption. |
| T-P02 | Retries, duplicate IDs, replay. | Approved idempotency behavior and exactly one transition. |
| T-S01 | Flood/rate limit. | Abusive traffic rejected; normal clients and matches responsive; record thresholds. |
| T-S02 | Slow reader/backpressure. | Queue remains within cap or connection dropped; other clients responsive; record RSS/queue. |
| T-S03 | Secret leakage. | Role/clue canaries absent from unauthorized output and logs. |
| T-R01 | Steady-load reliability. | Recommended 30 minutes without crash/deadlock/unbounded growth; record RSS, CPU, queues, exit and latency. |
| T-R02 | Timer/event-loop behavior. | Monotonic phase deadlines; record event-loop delay under load. |

## Load matrix

Use automated clients that follow the actual protocol and a realistic mixture of joins, chat, trades, votes and acknowledgements. Opening sockets alone is not a capacity test.

| Run | Configuration | Evidence |
|---|---|---|
| L1 | 5 players/1 match, full game | Correctness, baseline CPU/RSS, protocol errors. |
| L2 | 8 and 16/1 match | Three 16-player runs; p50/p95, CPU/RSS, responsiveness. |
| L3 | 32/1 match | Sustained messaging/phases; queue depth, rejects, latency. |
| L4 | 64/1 match | At least 10-minute smoke run; no crash, bounded queues/memory; report CPU/latency. |
| L5 | 2 × 32 | Concurrent actions, no leakage/starvation. |
| L6 | 4 × up to 64 | Attempt 256 players plus moderators/test clients where hardware permits; report actual maximum and limits/failures. |
| L7 | Slow/disconnected clients | Bounded queue, cleanup, other-match responsiveness. |
| L8 | Malformed/flood traffic | Safe rejection; no crash, growth, corruption. |

Proposed p95 command send-to-ack ≤100 ms at 64 connected clients under an explicitly defined mixed workload, excluding deliberate phase waits. Significant runs repeated three times where practical. Four full matches are a stretch target; never claim without actual evidence.

Record CPU model/cores, RAM, Ubuntu, compiler/build mode, kernel, exact command/config/workload/duration, active clients/matches, accepted/rejected connections, reconnects, latency p50/p95/p99 where meaningful, RSS/CPU/FDs/queue depth/event-loop delay, correctness failures, crashes/timeouts. Save timestamped CSV or JSON plus logs.

## Reporting

Each row includes status, timestamp, source revision, environment, command, measurements, failures and artifact paths. A compilation or socket-open test does not demonstrate capacity. Distinguish target from observation. Report measured limit/bottleneck if a target misses; do not silently lower thresholds. TCP/TLS tests are deferred until S9 is approved.
