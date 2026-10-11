# DARKPOOL S0 decision register

Documenting a proposal does not approve it. Confirm or revise approval-required defaults before S1 implements rules/protocol behavior.

## 1. Direct requirements from Draft 3

| ID | Requirement |
|---|---|
| REQ-01 | C++20 on Ubuntu; separate clients; one authoritative server owns official state. |
| REQ-02 | Initial five active players: one Insider, Investigator, three Employees; moderator status unresolved. |
| REQ-03 | Three rounds with events, public/authorized private chat, BUY/HOLD/SELL, resolution, clue, vote, final evaluation. |
| REQ-04 | UNIX sockets initially; nonblocking epoll; no thread per client. |
| REQ-05 | Independent matches isolate membership, roles, messages, clues, trades, votes, timers and logs. |
| REQ-06 | Bounded resources, validation, rate limits, cleanup and slow-client protection. |
| REQ-07 | Target 64 players/match and four matches/process (up to 256 players plus moderators/test clients); unmeasured. |
| REQ-08 | Proposed p95 acknowledgment ≤100 ms at 64 clients under defined workload. |
| REQ-09 | Proposed 30-minute steady load without crash/deadlock/unbounded memory. |
| REQ-10 | Future TCP/TLS 1.3 is a later milestone. |
| REQ-11 | First release excludes real money, external feeds, complex trading, GUI/mobile, matchmaking, persistent accounts, consensus and failover. |

## 2. Recommended defaults requiring approval

| ID | Recommendation | Rationale and impact | Approval before implementation |
|---|---|---|---|
| DEC-01 | Moderator is separate non-player; create/start/close/status; no secret visibility in player output. | Consistent with five active clients plus moderator; audit access affects secrecy. | Yes |
| DEC-02 | One Insider/Investigator at any size; remainder Employees. | Extends baseline composition; balance impact. | Yes |
| DEC-03 | Fictional economy: 100 credits, 10 shares/player, total 640, health 5/10, price 10; match-local reserve counterparty. | Simple bounded state; no such values/mechanism in PDF. | Yes |
| DEC-04 | Seeded fixed event table; public text and hidden impact -1/0/+1; Insider gets true impact; apply to health/price. | Makes hint and replay meaningful; event semantics unspecified. | Yes |
| DEC-05 | One-share BUY/SELL at opening price; insufficient funds/shares/reserve rejected; missing action HOLD; price formula in game-rules.md. | Deterministic simple market; no order book. | Yes |
| DEC-06 | Once-per-match clue after trade resolution before vote privately reveals one random other player's action. | Concrete testable clue; content/timing absent from PDF. | Yes |
| DEC-07 | Active players vote for another, no self; abstention at deadline; eliminate only with >50% non-abstaining votes; tie/no majority none; eliminated inactive, match continues. | Defines eligibility, tie and consequences absent in PDF. | Yes |
| DEC-08 | Employees win if Insider eliminated and health >0; otherwise Insider wins. | Exclusive simple condition; actual win/scoring absent. | Yes |
| DEC-09 | UNIX peer credentials + server session IDs; no persistent accounts; reconnect secret. Same UID is not distinct human. | Explicit local trust boundary. | Yes |
| DEC-10 | 4-byte big-endian length + fixed header + UTF-8 JSON payload, one current major version. | Deterministic and debuggable; PDF leaves encoding open. | Yes |
| DEC-11 | 4 KiB frame, 512-byte chat, 20 req/s burst 40, 64 KiB output queue; persistent overflow disconnect. | Concrete initial bounds; PDF proposes only 4 KiB frame. | Yes |
| DEC-12 | Request IDs unique per session; cache 256 outcomes/60 seconds; retry replays; conflicting reuse rejects. | Safe duplicate behavior, which PDF requires to define. | Yes |
| DEC-13 | 30-minute run governs final reliability; retain L4's 10 minutes as smoke run. | Reconciles §3 vs §9 duration discrepancy; stronger test. | Yes |
| DEC-14 | One state-owning epoll loop; timerfd or deadline heap; workers only after measured CPU need. | Avoid races/premature threads. | Yes |

Other open approvals: chat/audit retention and moderator access; reconnect grace and moderator disconnect behavior; configured caps including test clients; queue/rate thresholds; clue availability windows; zero-health/early-elimination behavior; workload definition for 100 ms; whether repeat runs apply to every target; Ubuntu/compiler/kernel acceptance environment.

## 3. Deferred

- TCP/TLS 1.3, certificate and client identity lifecycle (optional S9).
- Real money, feeds/integrations, short selling, derivatives, complex auctions.
- GUI/mobile clients, public matchmaking, persistent accounts.
- Multi-region, distributed consensus, automatic failover.
- Unsupported advanced anti-cheat, compromised-host/root protection, universal side-channel defense.
- Worker pool unless profiling demonstrates need; workers cannot mutate match state.
- seccomp/systemd/Landlock/cover traffic absent a testable approved requirement.

## Approval gate

S0 records recommendations only. Approval is required for DEC-01 through DEC-14 and the additional open items before the relevant behavior is implemented. Capacity and performance remain design targets until repeatable reports support claims.
