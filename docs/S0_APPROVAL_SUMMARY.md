# DARKPOOL S0 approval summary

**Status update:** The user's S1 follow-up authorized the five-player baseline mechanics. A later explicit decision approved variable-size matches and role scaling as recorded in DEC-02/DEC-20. The S0 approval requests below remain historical where superseded. Performance and scalability targets remain unmeasured.

## Mandatory specification requirements

These are requirements attributed to Draft 3 in `requirements.md`, not newly approved design defaults:

- C++20 on Ubuntu, built with CMake; separate clients and one authoritative server.
- Nonblocking, event-driven I/O using epoll; UNIX domain sockets for initial transport; no thread per client.
- Multiple independent matches with strict authorization and isolation.
- Versioned length-prefixed messages, validation, bounded resources/queues, rate limits and slow-client handling.
- Five-player baseline and three-round structure, with events, discussion, BUY/HOLD/SELL, market resolution, limited clue, vote and final evaluation.
- Capacity targets: 64 players per match and four concurrent matches (up to 256 players, plus moderator/test clients). These are unverified design targets.
- Draft 3 contains p95 acknowledgement and reliability objectives; workload and acknowledgement semantics need approval before they can be measured consistently.

## Prototype rules authorized for S1

1. **Moderator:** separate non-player with create, start, close and operational-status permissions. Secret audit access and chat/audit retention remain separate decisions.
2. **Baseline/default:** five active players: one Insider, one Investigator and three Employees. The moderator is a separate non-player. A later explicit decision adds configurable N=3..64 matches.
3. **Economy:** 100 fictional credits and 10 shares per player; for N seated active players, supply=20N, player-held=10N and finite reserve=10N. Initial health=5/10 and price=10. Trades settle at opening price; reserve BUY allocation precedes same-round SELL replenishment. No-reserve and insufficient-reserve BUYs reject. Recommended credit model treats balances as non-conserved counters: BUY debits and SELL credits from the match credit pool, without treasury sufficiency, subject to the provisional S1 implementation cap of 1,000,000 credits (uint32_t), selected under the user's S1 instruction. This resolves an unspecified SELL funding source; the cap is not an additional gameplay rule. These values are approved for the S1 prototype by the user's follow-up; they are not direct PDF requirements.
4. **Market formulas:** e∈[-2,2] integer, d=sign(B-S); H'=clamp(H+e+d,0,10), P'=clamp(P+2e+d,1,100). Freeze opening H/P/reserve; validate and allocate trades; settle at opening P; calculate both outputs from opening state and executed trades; check H=0 victory before clue/vote. Formulas are deterministic and bounded as written; the user's follow-up approves them for S1.
5. **Clue:** once-per-match Investigator target-role check, private binary Insider/not-Insider answer. This is the once-per-match clue approved for the S1 prototype, subject to balance review after playtesting.
6. **Voting and victory:** active eligible electorate, no self/eliminated targets, missing vote=abstain, abstentions stay in denominator; eliminate only with strictly more than half of eligible voters. Health zero gives immediate Insider win before clue/vote; identifying Insider by vote with health positive gives immediate Employee win; otherwise Insider wins at final evaluation after round 3. The user's follow-up approves this precedence and edge handling for S1.
7. **Variable-size roles (later approved):** for N=3..64, Insiders=`ceil(N/8)`, Investigators=`ceil(N/16)`, Employees are the remainder; the server assigns a shuffled role list with those exact counts. Five remains the default.
8. **Defaults:** protocol encoding/framing, local authentication, resource limits, duplicate handling, and ACK semantics remain proposed implementation defaults. Review `protocol.md` and DEC-09–16 before locking them.
9. **Capacity tests:** keep L4/64-client/one-match 10-minute run as a smoke test and 30-minute run as final reliability acceptance. mixed64-v1 and the p95 measurement boundary are proposed and require approval.

### Formula and supply consistency review

The formulas are integer deterministic with inclusive clamps, and opening-price settlement prevents same-round price feedback. In the five-player worked example, N=5, H=5, P=10, e=-1, two BUYs and one SELL yield reserve=49, player-held=51, H'=5 and P'=9; the 100-share total is conserved. The proposed credit model does not conserve a global credit total; player cash after the example is 490.

The finite-reserve “exhaustion” case is not reachable during ordinary prototype play under the recommended rules: initial reserve is 10N, but at most 3N BUYs can occur in three rounds, leaving at least 7N shares if no SELL occurs. Therefore the documented exhaustion case is a synthetic conserved-state boundary fixture (reserve=1, players hold 20N-1), not a gameplay scenario. This is an identified design/test limitation, not a silently changed rule.

## Deferred decisions and unresolved risks

- Balance evidence for matches larger than five under the approved role formula.
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
- Player exits, reconnects and match closure can affect role counts, votes and authorization; define and test policy before engine/network implementation. The provisional S1 balance cap and overflow rejection behavior are documented in game-rules.md.

## S1 scope and acceptance criteria

S1 delivers only the deterministic, in-process C++20 game engine/state machine; no sockets, epoll server, client, or network protocol implementation. The user's follow-up authorizes this scope and the S1 gameplay rules summarized above; no S2 work is included.

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

## Approval status and remaining decisions

The user has authorized S1 within the deliverables and acceptance criteria above. The following decisions remain open for later milestones:

- Balance evidence for the approved larger-match role policy.
- Production event text/table, random selection/seed policy and what event detail is exposed beyond the approved Insider hint.
- Protocol format/versioning, ACK semantics, duplicate handling, local authentication, reconnect policy and production resource limits.
- Secret audit access, chat/audit retention and disclosure.
- Exit, disconnect, shutdown, session cleanup and moderator-disconnect behavior.
- Scoring/final-summary disclosures.
- Exact mixed64-v1 workload approval, supported Ubuntu/compiler/kernel baseline, and load-test environment.

These open items do not block the in-process S1 engine and do not authorize S2. Capacity, latency, security and reliability targets remain unmeasured.
