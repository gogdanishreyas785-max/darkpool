# Requirements traceability

Source: DARKPOOL_Scalable_Design_and_Build_Plan_Draft_3.pdf, Draft 3. “Required” is directly stated by the PDF; the approved project decision is explicitly labeled separately. Implementation and test status must be reported from evidence for the relevant revision.

| ID | Kind | Requirement / source | Implication | Planned verification |
|---|---|---|---|---|
| R-G01 | Required | One Insider, Investigator, Employees; one company share/instrument and one decision/round. §§1–2. | Role/state model; detailed mechanics open. | Unit and visibility tests. |
| R-G02 | Required | Three rounds: event, public/authorized private discussion, BUY/HOLD/SELL, resolution, evidence/vote, final evaluation. §2.2. | Authoritative phase machine. | Full-match transcript. |
| R-G03 | Required | Confidential Insider event hint; one limited Investigator clue/match. §2.1. | Match/audience-scoped private events. | Visibility/use tests. |
| R-G04 | Required baseline; moderator boundary approved in S1 | Five active baseline: one Insider, one Investigator, three Employees. The moderator is a separate non-player. §§2–3. | Preserve five-player default while supporting approved variable sizes. | Five-player, moderator, and variable-size tests. |
| R-G05 | Required | Exclude real money, feeds, short selling, derivatives, auctions, GUI/mobile, matchmaking, persistent accounts, consensus, failover, unsupported anti-cheat. §2.3. | Keep initial release in scope. | Scope review. |
| R-G08 | Approved project decision (not a PDF requirement) | Match size 3–64 active players; moderator excluded; default five; role counts `ceil(N/8)` Insiders, `ceil(N/16)` Investigators, remainder Employees. | Server derives counts, shuffles its role list, and enforces capacity per match. Role-list entropy is an implementation detail, not proof of cryptographic secrecy. | Engine sizes 3,4,5,8,9,16,24,32,64; coordinator and real TCP tests. |
| R-N01 | Required | C++20/Ubuntu; separate clients; authoritative server owns/validates state. Cover, §§1,4. | Client requests only. | Ubuntu build and forged-state tests. |
| R-N02 | Required | Initial UNIX sockets; nonblocking epoll; no thread/client. §§4.1–4.2. | Event loop and transport adapter. Implemented with nonblocking UNIX domain stream sockets and TCP stream sockets (default loopback `127.0.0.1:4782` or assigned private RFC 1918 IPv4 via `--bind-address`) with epoll; LAN TCP is unencrypted plaintext. Remote TCP/TLS 1.3 remains deferred. | Multi-socket integration/thread review. |
| R-N03 | Required | Independent matches isolate players, roles, messages, clues, trades, votes, timers, logs. §§1,4.3. | Match-owned state and server membership. | Canary isolation tests. |
| R-N04 | Required | Bound inputs/queues/rates/matches/connections; slow clients cannot stall others. §§1,4.2,6. | Explicit caps/backpressure. | Flood, slow-reader, queue tests. |
| R-N05 | Required | Versioned length-prefixed envelope; reject invalid version/type, malformed/truncated/oversized frames and invalid text. §5.3. | Bounded parser separate from rules. | Negative suite/fuzzing. |
| R-N06 | Required | Validate frame, fields, session, membership, phase/role, rate, transition, audit, authorized response. §5.2. | Central request pipeline. | Rejection-boundary tests. |
| R-N07 | Required | Monotonic deadlines; no blocking event loop; optional workers return to owner loop. §4.2. | Single state owner. | Timer/event-loop metrics. |
| R-S01 | Required | Session identity, membership authorization, least privilege, socket permissions, safe cleanup. §§4–6. | Explicit local trust/reconnect. | Spoofing/permission/disconnect tests. |
| R-S02 | Required | Secret roles/clues excluded from player logs; no host/root or universal side-channel claim. §6. | Separate player/audit output. | Canary/log review. |
| R-C01 | Required target | Normal 8–16; three recorded 16-player runs. §§3,10,12. | Real protocol harness/report. | Three runs. |
| R-C02 | Required target | 64 players/match; four matches/process, up to 256 players plus moderators/test clients. §§3,9. | Implemented ceilings in server: 4 matches, 256 players, 4 moderators (260 total connections). Capacity measured on single-host Linux testbed: L4 verified 64-player single match (1,683 reqs); L6 verified 4 concurrent matches with 256 active players and 4 independent moderators (6,528 reqs in L6-Functional). Physical cross-device 2-computer LAN acceptance remains untested (NOT RUN). | Report actual max/environment/failures. Status: PASS for recorded single-host L4/L6 workloads; NOT RUN for physical LAN. |
| R-C03 | Proposed threshold in PDF | p95 ACK ≤100 ms at 64 under defined workload, excluding phase waits. §3. | Proposed latency threshold. Under measured single-match 64-player mixed64-v1 profile (L4), sequential p95 was 0.17 ms and concurrent burst p95 was 0.58 ms. Under 4-match 256-player profile (L6), pooled concurrent burst p95 was 22.31 ms and total p95 was 19.54 ms. Passed for the exact measured single-host profile; does not imply all arbitrary network/hardware conditions. | Three repeatable runs using test-plan profile. Status: PASS under defined L4/L6 test profiles. |
| R-C04 | Proposed objective in PDF | 30-minute steady load without crash/deadlock/unbounded memory growth. §3. | Measured 30-minute sustained workloads on persistent single-host server: L4-Reliability (64p, 32,118 matches, 17.8M reqs, 0 failures, RSS +12 kB) and L6-Reliability (4×64p, 35,736 matches, 19.4M reqs, 0 failures, post-cleanup RSS bounded within 4,144–4,248 kB envelope, FDs settled to 8). | 30-minute final test; L4 10-minute smoke and 30-minute soak completed. Status: PASS under recorded L4 and L6 sustained workloads. |
| R-C05 | Required reporting | Environment, clients/matches, latency, resources, correctness/reliability; statuses PASS/FAIL/NOT RUN/BLOCKED. §§3,9. | Timestamped reports/logs. | Test-plan evidence checklist. |
| R-D01 | Required future scope | Future TCP/TLS 1.3 and explicit identity; keep transport separate. §§4,6. | Later adapter only. | Certificate/plaintext tests if approved. |

## Project decisions not specified by the PDF (status is in DECISIONS.md)

| ID | Proposal | Verification |
|---|---|---|
| P-02 | 4-byte big-endian prefix, fixed header, UTF-8 JSON. | Golden/invalid-frame tests. |
| P-03 | Separate moderator; peer credentials + server sessions. | Permission/auth tests. |
| P-04 | 4 KiB frame, 512-byte chat, 20 req/s burst 40, 64 KiB queue. | Boundary/flood/slow-reader tests. |
| P-05 | One epoll state owner and monotonic timers. | Timer/load tests. |
| P-06 | 30-minute reliability acceptance; 10-minute L4 smoke. | T-R01/L4. |
| P-07 | Dynamic finite supply 20N total, 10N player allotment and 10N reserve, where N is filled active roster at match start, not configured maximum. | Supply conservation, 5/64 and shortage cases. |
| P-08 | Separate health/price formulas and update order; event impact -2..+2 and demand affects both values. | Fixed-event/action results, bounds and demand ties. |
| P-09 | Immediate victory precedence and strict-majority voting. | Deterministic edge-case tests. |
| P-10 | Targeted role-check clue. | Privacy, retry and use-limit tests. |
| P-12 | No individual score/leaderboard; team win only; own-balance summary. | Add scoring/victory tests only after approval. |
| P-11 | mixed64-v1 workload and ACK definition. | Repeatable load metrics. |
| P-13 | Reserve exhaustion is a synthetic conserved-state boundary test; ordinary three-round play cannot exhaust 10N reserve with at most one trade/player/round. | Prove share conservation and reject BUY when reserve is zero/insufficient. |

These choices are not PDF requirements. S1 follow-up approvals cover the economy, formulas, victory/voting, clue and synthetic reserve test; the later follow-up approves variable-size roles. Scoring remains open. Load objectives have been verified for the specific recorded L1, L2, L3, L4, L5, L6, L7, and L8 workloads on a single-host Linux testbed; physical cross-device LAN acceptance remains untested (NOT RUN), and transport security (TLS 1.3) remains deferred.
