# DARKPOOL S0 decision register

Status note: the S1 follow-up approved the five-player baseline mechanics. A later explicit user decision approves variable-size matches and the role-scaling policy recorded in DEC-02/DEC-20. Other unresolved protocol, operations, scoring and audit items remain open.

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

## 2. Decisions initially proposed in S0 (current approval status shown)

| ID | Recommendation | Rationale / impact | Approval |
|---|---|---|---|
| DEC-01 | Moderator is separate non-player; create/start/close/status; no secret visibility in player output. | Consistent with five active players plus moderator; audit access impacts secrecy. | Yes |
| DEC-02 | For N=3..64, Insiders=`ceil(N/8)`, Investigators=`ceil(N/16)`, Employees=`N-Insiders-Investigators`; integer ceiling formulas, no floating point. N=5 retains 1/1/3. | Deterministic scaling selected by explicit user follow-up; moderator is outside N. | Approved by variable-size follow-up |
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
| DEC-18 | Reserve-exhaustion test uses a synthetic, supply-conserving state; ordinary play from 10N reserve cannot exhaust inventory in three rounds at one action/player/round. | Preserves proposed economy while testing finite-reserve rejection. | Yes |
| DEC-19 | Credits are per-player counters, not globally conserved; SELL credits the player without a reserve-cash test, subject to an implementation-safe balance cap. | PDF does not define the source of SELL proceeds; avoids an unstated finite treasury while making balance overflow behavior explicit. | Yes |
| DEC-20 | Match size is explicit, 3–64 active players; omitted size defaults to five. Server derives role counts and shuffles its server-owned role list. Four matches may be configured per process; planned hard caps are 256 player connections and one moderator connection. | Preserves legacy five-player workflows while defining finite product/resource bounds. The moderator can own all four matches. These caps are design limits, not measured capacity. | Approved by variable-size follow-up |

### S1 formula and reserve consistency review

The S1-approved formulas are mathematically deterministic and bounded when inputs are integers in their declared ranges. Both use opening H/P, the same executed-trade net demand sign, and inclusive clamps. Settlement at opening P precedes updating H/P; the health-zero victory check follows both calculations and precedes clue/vote. This avoids a price update feeding back into the same round's trade settlement.

The finite-reserve model conserves shares: player holdings + reserve = 20N before and after each settlement. Credits are deliberately not described as conserved by Draft 3; the approved S1 counter model treats SELL proceeds as issued by the match credit pool, with the provisional 1,000,000-credit cap to prevent overflow. However, natural reserve exhaustion is unreachable from the proposed initial condition within three rounds: at most 3N BUYs can execute, versus 10N reserve, leaving at least 7N even with zero SELLs. Test reserve shortage/exhaustion only through a synthetic state that preserves total supply. See worked examples in game-rules.md and explicit cases in test-plan.md.

Full definitions are in game-rules.md, test-plan.md and protocol.md. The S1 follow-up status below supersedes the original S0 approval-request column for the S1 scope.

### Remaining approvals for later milestones

Protocol format/versioning and ACK/duplicate semantics; local authentication and production limits; secret audit access and chat/audit retention; reconnect/exit/shutdown handling; scoring/final disclosure; and exact load-test workload/environment remain separate decisions where not already approved.

## 3. Deferred

TCP/TLS 1.3/certificate lifecycle; real money, feeds, short selling, derivatives, complex auctions; GUI/mobile, public matchmaking, persistent accounts; multi-region/consensus/failover; unsupported advanced anti-cheat/host-root/side-channel claims; worker pool absent measured need; sandboxing absent approved testable requirement.

## Approval gate

The original S0 proposals are not approvals by themselves. The explicit later user follow-ups approve DEC-02 and DEC-20 for variable-size matches. Other proposals remain governed by their individual status. Capacity and performance remain unmeasured targets.


## S1 follow-up status

Approved for S1 by the user's explicit follow-up: separate moderator boundary and stated powers; exactly five active roles; proposed economy, share reserve, event/health/price formulas, update order, clue, voting and victory precedence; supply-boundary fixture; and non-conserved player-credit model. The per-player cap is provisionally 1,000,000 credits (uint32_t) for S1, with overflow-causing trades rejected; this is an implementation detail directed by the S1 request. The engine receives an explicit role assignment and event impacts as inputs; S1 does not invent role shuffling or event randomness.

Still open after S1: protocol encoding/ACKs and duplicate policy; local authentication and production limits; chat/audit retention and secret audit access; reconnect/exit handling; and load-test profile/acceptance evidence. The 64-player/four-match, latency and reliability targets remain unverified. The later variable-size follow-up explicitly supersedes S1's deferred role policy and authorizes the formula in DEC-02.

## Variable-size follow-up status

Approved: 3–64 active players per match; moderator excluded; five remains the default; role counts follow DEC-02 using safe integer arithmetic. Multiple Insiders form a team: health zero gives an immediate Insider-team win; eliminating one Insider does not end the match while another remains; eliminating the last Insider with positive health gives Employees the win; surviving round three with any Insider active gives the Insider team the win. Every Investigator gets an independent once-per-match private binary role clue with same-target retry idempotency. These are approved game rules. Four concurrent 64-player matches and a 256-player server remain unverified until end-to-end load and isolation testing.
