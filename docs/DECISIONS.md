# DARKPOOL S0 decision register

Documenting a proposal does not approve it. Approval-required decisions must be confirmed or changed before S1 implements them.

## 1. Direct requirements from Draft 3

| ID | Requirement |
|---|---|
| REQ-01 | C++20 on Ubuntu; separate clients; one authoritative server owns official state. |
| REQ-02 | Five-player baseline: one Insider, one Investigator, three Employees; moderator status unresolved. |
| REQ-03 | Three rounds with events, public/authorized private chat, BUY/HOLD/SELL, resolution, clue, vote, final evaluation. |
| REQ-04 | UNIX sockets initially; nonblocking epoll; no thread per client. |
| REQ-05 | Independent matches isolate members, roles, messages, clues, trades, votes, timers and logs. |
| REQ-06 | Bounded resources, validation, rate limits, cleanup and slow-client protection. |
| REQ-07 | Target 64 players/match and four matches/process, up to 256 players plus moderators/test clients; unverified. |
| REQ-08 | Proposed p95 command acknowledgement ≤100 ms at 64 clients under a defined workload. |
| REQ-09 | Proposed 30-minute steady load without crash, deadlock or unbounded memory growth. |
| REQ-10 | Future TCP/TLS 1.3 is a later milestone. |
| REQ-11 | Exclude real money, feeds, complex trading, GUI/mobile, matchmaking, persistent accounts, consensus and failover from initial scope. |

## 2. Proposed decisions requiring approval

| ID | Recommendation | Rationale / impact | Approval |
|---|---|---|---|
| DEC-01 | Moderator is separate non-player; create/start/close/status; no secret visibility in player output. | Consistent with five active players plus moderator; audit access impacts secrecy. | Yes |
| DEC-02 | At all sizes, one Insider and one Investigator; remainder Employees. | Preserves baseline simplicity; balance at 64 may weaken. Scaling proposal, not requirement. | Yes |
| DEC-03 | 100 fictional credits and 10 shares/player; for N filled active seats at match start, total supply 20N and reserve 10N. Health 5/10, price 10; bounds health 0–10, price 1–100. | Finite supply works at 5 seats (total100/reserve50) and 64 (total1280/reserve640). | Yes |
| DEC-04 | Trades one share at opening price; buys use finite opening reserve, sells add after allocation; rotate seat priority on shortage. | Conserves supply and makes shortage deterministic. | Yes |
| DEC-05 | Event impact e∈{-2,-1,0,+1,+2}; H'=clamp(H+e+sign(B-S),0,10); P'=clamp(P+2e+sign(B-S),1,100). Event and demand affect both values with separate formulas. Tests inject fixed events. | Separate deterministic formulas and reproducibility. | Yes |
| DEC-06 | Health zero at market resolution: immediate Insider win before clue/vote. With health positive, a vote eliminating Insider causes immediate Employee win. If round-three vote ends without identification, Insider wins. | Resolves timing and precedence. | Yes |
| DEC-07 | Active voters, no self/eliminated targets; missing/explicit abstain; eliminate only on strictly more than E/2 votes with E eligible-at-open; tie/no majority none. | Deterministic edge cases. | Yes |
| DEC-08 | Replace trade-action clue with private once-per-match target-role check by Investigator. | Trade clue may be weak/redundant; role check is meaningful but powerful. | Yes |
| DEC-09 | Local peer credentials + server session IDs; no persistent accounts; same UID is not distinct human. | Explicit local trust boundary. | Yes |
| DEC-10 | 4-byte big-endian frame length, fixed header, UTF-8 JSON payload; one current major version. | Encoding proposal; PDF leaves format open. | Yes |
| DEC-11 | 4 KiB frame, 512-byte chat, 20 req/s burst 40, 64 KiB output queue; persistent overflow disconnect. | Concrete initial limits; only 4 KiB is suggested in PDF. | Yes |
| DEC-12 | Request IDs per session; cache 256 outcomes/60 seconds; exact retry replays; conflicting reuse rejects. | Defines duplicate behavior. | Yes |
| DEC-13 | 30-minute steady-load test governs final reliability; 10-minute L4 is smoke only. | Resolves PDF §3/§9 discrepancy. | Yes |
| DEC-14 | One state-owning epoll loop; timerfd or deadline heap; workers only after measured CPU need. | Avoids races/premature threading. | Yes |
| DEC-15 | mixed64-v1 workload: 64 clients in one match; 30s discussion, 10s trade, 5s clue, 10s vote per round; one chat/client/10s discussion with deterministic 0–2s jitter (80% public, 20% authorized whisper); one trade/vote per eligible player per phase, one clue per match; repeat matches after completion. ACK p95 from last request byte sent to full correlated ACK received, monotonic client clock; excludes phase wait and broadcast delivery. | Reproducible gameplay-like load profile; details absent from PDF. | Yes |
| DEC-16 | Each well-framed command receives one correlated ACK after validation/transition and response enqueue, or ERROR on rejection; PING gets PONG. | Makes command acknowledgement measurable. | Yes |
| DEC-17 | Team win only; cash/shares do not create individual points or tie-break; private summary shows only own balances. | PDF gives no scoring rule; avoids an unrequested leaderboard. | Yes |

Full definitions are in game-rules.md, test-plan.md and protocol.md. DEC-02 through DEC-08 are gameplay defaults; DEC-10 through DEC-12 and DEC-16 are protocol choices. None are approved.

### Other approvals still open

Moderator powers/secret audit access; chat and audit retention; reconnect grace/moderator disconnect behavior; connection/match/player caps including test clients; confirm all economy, market, vote, clue, victory and scoring values; confirm protocol framing, authentication, duplicate policy and limits; approve mixed64-v1 and ACK semantics; confirm acceptance Ubuntu/compiler/kernel; assess larger-match role balance.

## 3. Deferred

TCP/TLS 1.3/certificate lifecycle; real money, feeds, short selling, derivatives, complex auctions; GUI/mobile, public matchmaking, persistent accounts; multi-region/consensus/failover; unsupported advanced anti-cheat/host-root/side-channel claims; worker pool absent measured need; sandboxing absent approved testable requirement.

## Approval gate

S0 only records proposals. No proposed gameplay or protocol behavior is approved until you confirm it. Capacity and performance remain unmeasured targets.
