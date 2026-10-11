# DARKPOOL S0 approval summary

**Status: S0 review package for approval. No S1 implementation is authorized by this document.** The repository contains design proposals and planned tests only; nothing here demonstrates implemented behavior, measured capacity, or passed performance targets.

## Mandatory specification requirements

These are requirements attributed to Draft 3 in `requirements.md`, not newly approved design defaults:

- C++20 on Ubuntu, built with CMake; separate clients and one authoritative server.
- Nonblocking, event-driven I/O using epoll; UNIX domain sockets for initial transport; no thread per client.
- Multiple independent matches with strict authorization and isolation.
- Versioned length-prefixed messages, validation, bounded resources/queues, rate limits and slow-client handling.
- Five-player baseline and three-round structure, with events, discussion, BUY/HOLD/SELL, market resolution, limited clue, vote and final evaluation.
- Capacity targets: 64 players per match and four concurrent matches (up to 256 players, plus moderator/test clients). These are unverified design targets.
- Draft 3 contains p95 acknowledgement and reliability objectives; workload and acknowledgement semantics need approval before they can be measured consistently.

## Recommended prototype rules (all proposed decisions, approval required)

1. **Moderator:** separate non-player with create, start, close and operational-status permissions. Secret audit access and chat/audit retention remain separate decisions.
2. **First playable match:** exactly five active players: one Insider, one Investigator and three Employees. The baseline roles are directly specified; the moderator's separate status is proposed.
3. **Economy:** 100 fictional credits and 10 shares per player; for N seated active players, supply=20N, player-held=10N and finite reserve=10N. Initial health=5/10 and price=10. Trades settle at opening price; reserve BUY allocation precedes same-round SELL replenishment. No-reserve and insufficient-reserve BUYs reject. All numerical/economic details are proposals.
4. **Market formulas:** e∈[-2,2] integer, d=sign(B-S); H'=clamp(H+e+d,0,10), P'=clamp(P+2e+d,1,100). Freeze opening H/P/reserve; validate and allocate trades; settle at opening P; calculate both outputs from opening state and executed trades; check H=0 victory before clue/vote. Formulas are deterministic and bounded as written, but remain proposals pending approval.
5. **Clue:** once-per-match Investigator target-role check, private binary Insider/not-Insider answer. This is recommended for playtesting and remains approval-required.
6. **Voting and victory:** active eligible electorate, no self/eliminated targets, missing vote=abstain, abstentions stay in denominator; eliminate only with strictly more than half of eligible voters. Health zero gives immediate Insider win before clue/vote; identifying Insider by vote with health positive gives immediate Employee win; otherwise Insider wins at final evaluation after round 3. Proposed precedence and edge handling require approval.
7. **Larger matches:** role composition remains a separate configurable decision; no scaling policy is approved here.
8. **Defaults:** protocol encoding/framing, local authentication, resource limits, duplicate handling, and ACK semantics remain proposed implementation defaults. Review `protocol.md` and DEC-09–16 before locking them.
9. **Capacity tests:** keep L4/64-client/one-match 10-minute run as a smoke test and 30-minute run as final reliability acceptance. mixed64-v1 and the p95 measurement boundary are proposed and require approval.

### Formula and supply consistency review

The formulas are integer deterministic with inclusive clamps, and opening-price settlement prevents same-round price feedback. In the five-player worked example, N=5, H=5, P=10, e=-1, two BUYs and one SELL yield reserve=49, player-held=51, H'=5 and P'=9; the 100-share total and 500-credit total are conserved (including the reserve cash account).

The finite-reserve “exhaustion” case is not reachable during ordinary prototype play under the recommended rules: initial reserve is 10N, but at most 3N BUYs can occur in three rounds, leaving at least 7N shares if no SELL occurs. Therefore the documented exhaustion case is a synthetic conserved-state boundary fixture (reserve=1, players hold 20N-1), not a gameplay scenario. This is an identified design/test limitation, not a silently changed rule.

## Deferred decisions and unresolved risks

- Role scaling for matches larger than five: role ratios, minimum roles and balance evidence.
- Event text/impact table, random selection/seed policy, and whether the numerical impact is hidden from players other than the Insider.
- Exact phase deadlines, clue window/order if a market-health win happens, and whether clue retries are idempotent.
- How eliminated players participate in chat and receive public results.
- Moderator's precise status scope, authority, and whether they can inspect secret state/audit.
- Individual scoring/final-summary fields and what is disclosed.
- Event-log/player-chat retention, access, integrity, deletion and privacy.
- Local authentication across same-UID users, reconnect tokens/grace, moderator disconnect behavior.
- Production connection/match/player limits, filesystem/socket ownership and mode, stale-socket cleanup, shutdown behavior, and resource exhaustion policy.
- Protocol defaults and backward compatibility/version policy.
- Supported Ubuntu release, compiler/kernel baseline and reproducible load-test hardware/environment.
- A single-process server has no crash recovery/failover in the initial scope; process loss ends active matches absent later persistence.

### Principal technical risks

- Single event-loop stalls or synchronous logging/fan-out can delay every match; enforce bounded work and measure loop delay.
- Same-UID peer credentials do not distinguish separate humans; socket ACLs and any token/session policy need a threat-model decision.
- Secret role, clue, hint, vote or whisper leakage through logs/metrics/errors remains a high-impact risk.
- 256 clients plus moderators/test clients, bounded output fan-out and four independent matches are unmeasured.
- p95 ≤100 ms is not meaningful without a fixed workload and exact ACK start/end events; mixed64-v1 documents a candidate workload.
- Share reserve shortage is only a synthetic boundary case with the proposed 10N reserve and three-round action cap.
- Player exits, reconnects and match closure can affect role counts, votes and authorization; define and test policy before engine/network implementation.

## Precise S1 proposal and acceptance gate

S1 should deliver only the deterministic, in-process C++20 game engine/state machine; no sockets, epoll server, client, or network protocol implementation. Begin only after explicit user approval of gameplay decisions below.

**S1 deliverables**

- CMake-configured Ubuntu C++20 library and test executable.
- Domain types for match/player/roles, company state, rounds/phases, orders, clue, votes, outcomes and validated transitions.
- Injected deterministic event sequence; no hidden wall-clock or random dependency in rules.
- Pure or explicitly state-transitioning APIs for start, trade submission/resolution, clue, voting and victory.
- Canonical state/event representation for deterministic replay tests.
- Unit tests for approved game rules and decision precedence.

**S1 acceptance criteria**

- Configure/build with the approved Ubuntu compiler; warnings policy and exact supported versions recorded.
- Five-player baseline starts with exactly one Insider, one Investigator, three Employees and correct proposed initial resources, if approved.
- Three-round transition completes with valid phase/action gating; early victory stops further phases exactly as approved.
- Supply invariant holds after every settlement for N=5 and synthetic N=64 fixtures; finite-reserve shortage and rejected trades never mutate invalid balances.
- Formula vectors, inclusive health/price bounds, demand ties, boundary values and the worked examples match exactly.
- Re-running identical initial state, event sequence, complete action/vote input stream and seat order yields identical canonical outputs.
- All approved voting/victory edge cases pass, including abstention, missing vote, strict threshold, ties, zero-health precedence, Insider elimination and final-round survival.
- No network, epoll, socket, persistence, or performance claims are part of S1 acceptance.

S1 acceptance is functional and deterministic; the 64×4 capacity, p95 and 30-minute reliability targets belong to later integrated load testing and remain unverified.

## Explicit approval requested before S1

Please approve, reject or amend these grouped proposals:

1. Moderator boundary and powers; separate audit access/retention policy.
2. Five-player playable roster and economy/supply/trading defaults.
3. Event impact model, formulas, update order and win precedence.
4. Voting edge rules and once-per-match target-role clue.
5. Larger-match role policy (or defer it, as recommended).
6. Protocol format, authentication, limits, duplicate policy and ACK definition.
7. mixed64-v1 workload and ACK latency boundary.
8. S1 scope and acceptance criteria above.

Until those are confirmed, this package is a review proposal only and S1 remains on hold.
