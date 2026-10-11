# DARKPOOL test and evidence plan

Planned tests only. Every result is labeled PASS, FAIL, NOT RUN, or BLOCKED with evidence. Test approved rules, not unapproved defaults.

## Functional, unit and integration

| ID | Test | Acceptance evidence |
|---|---|---|
| T-F01 | Five active clients complete all three rounds. | Correct sequence and transcript/demo. |
| T-F02 | Server-side roles and visibility. | Baseline roster; no unauthorized role/hint; moderator view follows approved policy. |
| T-F03 | Public chat and authorized private messages. | Correct recipients; private message not broadcast. |
| T-F04 | Trade/clue/vote phase and permission. | Legal actions once; rejected actions do not mutate balances, share supply, health or price; action-slot behavior follows the proposed rule. |
| T-U01 | Economy/formulas and worked example. | Reproduce the five-player example in game-rules.md exactly: N=5, e=-1, B=2, S=1 gives reserve49, player shares51, H=5, P=9, and conserved share supply (player credits total 490 under the proposed non-conserved credit model). |
| T-U02 | Supply conservation and boundaries. | For N=5 and N=64 verify total=20N, player shares=10N, reserve=10N; after every BUY/SELL assert player holdings + reserve =20N; player credits change by exactly the opening price (credits are not globally conserved under proposed DEC-19). Exercise the approved balance-cap boundary once fixed. Empty configured seats receive no shares. Test P=1/100 and H=0/10 clamping for all e and demand-sign values. |
| T-U03A | Finite-reserve exhaustion boundary. | Use the documented conserved synthetic fixture (reserve1, player-held99 at N=5); two valid BUYs with seat0 priority accept one, reject one, reach reserve0 without violating supply. Then verify another BUY is rejected without mutation. Document that the state is unreachable in ordinary three-round play (initial reserve10N; at most 3N BUYs). |
| T-U03B | Invalid trades. | Insufficient-cash BUY, zero-share SELL, duplicate action, inactive/eliminated trader, and wrong-phase trade each reject with no balance/supply/market mutation; assert the proposed action-slot rule (invalid submission consumes slot) consistently. Same-round SELL proceeds cannot fund BUY allocation. |
| T-U03C | Deterministic replay. | Two runs with identical initial state, fixed event sequence, complete ordered request stream (including invalid requests), votes, and seat order produce byte-identical canonical state/event outputs. Repeat with different socket-arrival order for a fixed accepted action set to verify batch settlement is seat-order-based, not arrival-order-based. |
| T-U04 | Voting/victory. | Abstentions, missing votes, ties, strict threshold, eliminated voters/targets, zero health, Insider elimination, final survival, and precedence. |
| T-U05 | Scoring/final summary. | After approval, verify team-only scoring and that a player's summary reveals only that player's balances and permitted result. |
| T-I01 | UNIX lifecycle and fragmented/coalesced frames. | Multiple clients; disconnect safe. |
| T-I02 | Concurrent matches. | Independent state/timers; canaries never leak. |
| T-I03 | Auth/membership/reconnect. | Forged identity/nonmember rejected; approved cleanup policy. |

## Protocol, security and reliability

- Invalid version/type/fields, truncated/oversized/invalid UTF-8 frames: bounded rejection, no crash/corruption.
- Duplicate/retried/conflicting request IDs: approved semantics; at most one mutation.
- Flood/rate limit: abusive requests rejected, normal matches responsive, thresholds recorded.
- Slow reader: queue remains bounded or disconnect; record RSS/queue and other-client latency.
- Secret canaries: role/hint/clue absent from unauthorized output/logs.
- Final reliability: 30 minutes without crash/deadlock/unbounded growth; record RSS, CPU, queues, process status and latency.
- Timer/event-loop: monotonic deadlines and event-loop delay under load.

## Load matrix

Use automated clients speaking the real protocol; opening sockets alone does not establish capacity.

| Run | Configuration | Evidence |
|---|---|---|
| L1 | 5 players/1 match, full game | Correctness, CPU/RSS, protocol errors. |
| L2 | 8 and 16/1 match | Three 16-player runs; p50/p95 and resources. |
| L3 | 32/1 match | Queue depth, rejected requests, latency. |
| L4 | 64/1 match | 10-minute smoke test; bounded resources, CPU/latency. |
| L5 | 2 × 32 | Isolation/starvation results. |
| L6 | 4 × up to 64 | Attempt 256 plus moderators/test clients where hardware permits; report actual maximum/failure. |
| L7 | Slow/disconnected clients | Bounded queue, cleanup, responsiveness. |
| L8 | Malformed/flood traffic | Safe rejection and no corruption. |

## Proposed mixed64-v1 p95 workload — approval required

Draft 3 proposes p95 command acknowledgement ≤100 ms at 64 clients but does not define workload/ACK boundary. This profile is a proposed test decision:

- 64 simulated active clients in one match over local UNIX sockets. Use for the 10-minute L4 smoke and 30-minute final reliability run; repeat significant acceptance runs three times.
- Test phase lengths per round: discussion 30 seconds, trading 10 seconds, clue 5 seconds, voting 10 seconds. Repeat complete matches with same connected clients after a match ends. These accelerated timings are test settings, not game rules.
- During discussion, each client sends one chat every 10 seconds with deterministic seeded jitter 0–2 seconds; 80% public, 20% authorized whispers to rotating same-match recipients.
- In each trade phase, every eligible player sends exactly one legal BUY/HOLD/SELL, distributed deterministically over the first second. In each vote phase, every eligible player sends one vote/abstention over the first second. Investigator requests one clue per match. Use real join/readiness and phase protocol.
- ACK p95 starts when client has written the complete request and ends when it receives the complete correlated ACK/ERROR, using a monotonic clock. Includes local IPC, event-loop queueing, validation, transition and response enqueue. Excludes intentional phase wait and broadcast delivery; measure broadcast separately.
- Proposed threshold remains p95 ≤100 ms. Record the same metrics during four-match stretch runs separately.

Phase lengths, workload mix/jitter, ACK semantics, and repeat count are proposals requiring approval. Workload details are not PDF requirements.

## Metrics and reporting

Record hardware, Ubuntu/compiler/build/kernel, exact command/config/profile/duration, clients/matches, accepted/rejected connections, reconnects, p50/p95/p99 where meaningful, RSS/CPU/file descriptors/queue depth/event-loop delay, correctness failures, timeouts/crashes. Save timestamped CSV/JSON and logs with source revision and status. Never claim capacity from compilation or open sockets; report measured limits/bottlenecks. TCP/TLS tests wait for S9 approval.
