# Requirements traceability

Source: DARKPOOL_Scalable_Design_and_Build_Plan_Draft_3.pdf, Draft 3. “Required” means stated by the PDF; “proposed” means a suggested design, not a source requirement. Nothing here is implemented or passed.

| ID | Kind | Requirement / source | Implementation implication | Planned verification |
|---|---|---|---|---|
| R-G01 | Required | One Insider, one Investigator, Employees; one company share/instrument and one decision per round. PDF §§1–2. | Model roles and one instrument; exact mechanics need approval. | Rules unit and role-visibility tests. |
| R-G02 | Required | Three rounds: event, public discussion/authorized private messages, one BUY/HOLD/SELL per player, market resolution, evidence discussion/vote, final evaluation. §2.2. | Server-owned phase machine and deadlines. | Full-match integration transcript; transition tests. |
| R-G03 | Required | Insider gets one confidential event hint; Investigator may request one limited clue per match. §§2.1, Appendix A. | Match- and audience-scoped private events. | Role/clue visibility and use-limit tests. |
| R-G04 | Required | Initial development baseline: five active players (one Insider, one Investigator, three Employees) plus moderator; moderator status unresolved. §§2–3. | Baseline fixture; do not hard-code five in architecture. | Five-player full match and moderator lifecycle test. |
| R-G05 | Required | Exclude real money, external feeds/integrations, short selling, derivatives, complex auctions, GUI/mobile, matchmaking, persistent accounts, distributed consensus, automatic failover, unsupported anti-cheat claims. §2.3. | Keep initial release within scope. | Scope review. |
| R-N01 | Required | C++20 on Ubuntu; separate clients; authoritative server owns and validates all official state. Cover, §§1,4. | Client sends requests only; server applies state transitions. | Build on documented Ubuntu toolchain; forged-state tests. |
| R-N02 | Required | Initial UNIX domain sockets; nonblocking event-driven Linux I/O with epoll; no thread per client. §§4.1–4.2. | Event loop + transport adapter separate from rules. | Multi-socket integration; inspect runtime thread model. |
| R-N03 | Required | Independent matches in one process; isolate players, roles, messages, clues, trades, votes, timers, logs. §§1,4.3. | Match-owned state and server-side membership routing. | Canary isolation tests. |
| R-N04 | Required | Bound frame size, rates, queues, active matches, total connections; slow consumers cannot stall unrelated clients. §§1,4.2,6. | Explicit configurable limits and backpressure policy. | Flood, slow-reader, queue and responsiveness tests. |
| R-N05 | Required | Versioned length-prefixed envelope; reject invalid versions, malformed/truncated/oversized frames, unknown required types, invalid UTF-8 where text is expected. §5.3. | Bounded parser separate from game logic. | Negative protocol suite and parser fuzzing. |
| R-N06 | Required | Validation pipeline: frame, type/fields/limits, authenticated session, membership, phase/role, rate limit, transition, audit, authorized response. §5.2. | Central request path; no client proof of identity. | Rejection-boundary and audit tests. |
| R-N07 | Required | Monotonic deadlines; no blocking event-loop I/O, disk, DNS, or expensive work. Optional workers return results to owner loop and cannot mutate matches. §4.2. | Bounded work per readiness event; single state owner. | Timer/event-loop metrics and slow-client test. |
| R-S01 | Required | Bind identity to session; authorize membership/actions; least privilege, restrictive socket permissions, safe cleanup. §§4–6. | Explicit local trust and reconnect policy. | Spoofing, permissions, disconnect tests. |
| R-S02 | Required | Secret roles/clues excluded from player logs; do not claim host/root or universal timing-side-channel resistance. §6. | Separate player output and moderator/audit channels. | Secret-canary search and log access review. |
| R-C01 | Required target | Normal 8–16 players; 16-player session requires three recorded runs. §§3,10,12. | Real protocol client harness and per-run reports. | Three runs with latency/resource/correctness metrics. |
| R-C02 | Required target | 64 players in one match; four concurrent matches in one process, up to 64 each where resources permit (256 players plus moderators/test clients). §§3,9–10. | End-to-end capacity, not socket count alone. | Attempt target and report actual maximum, environment and bottleneck. |
| R-C03 | Proposed PDF threshold | p95 command acknowledgment ≤100 ms at 64 clients under defined workload; exclude deliberate phase wait. §3. | Define workload and measurement boundary. | Three repeatable runs with send-to-ack percentiles. |
| R-C04 | Proposed PDF objective | 30-minute steady-load run without crash, deadlock, or unbounded memory growth. §3. | Record RSS, CPU, queues, process status. | Recommend 30-minute acceptance; reconcile with 10-minute L4 (DEC-13). |
| R-C05 | Required reporting | Record hardware/software, clients/matches, accepted/rejected connections, reconnects, latency percentiles, CPU/RSS/FDs/queues/event-loop delay, correctness and reliability; label PASS/FAIL/NOT RUN/BLOCKED. §§3,9. | Timestamped reproducible report and logs. | Review report fields, command, environment and evidence. |
| R-D01 | Required future scope | TCP/TLS 1.3 and explicit client identity are optional later; keep transport separate. §§4,6,8. | Later adapter; no initial remote transport. | Certificate and plaintext rejection tests only if S9 approved. |

## Proposed design decisions, not PDF requirements

| ID | Proposal | Implication | Verification |
|---|---|---|---|
| P-01 | Keep one Insider and one Investigator at larger sizes; others Employees. | Extends five-player composition; balance needs approval. | Roster/visibility tests. |
| P-02 | 4-byte big-endian length, fixed version/type/request/match/payload-length header, UTF-8 JSON payload. | Deterministic framing and debuggable payload; encoding requires approval. | Golden and invalid-frame tests. |
| P-03 | Five active players plus separate moderator; local admission uses Linux peer credentials and server session IDs. | Same Unix UID does not prove distinct humans; approve trust boundary. | Peer-credential and permission tests. |
| P-04 | Proposed bounds: 4 KiB ordinary frame, 512-byte chat/whisper, 20 requests/s with burst 40, 64 KiB output queue, persistent-overflow disconnect. | Concrete starting values; tune from evidence only. | Boundary, flood, slow-reader tests. |
| P-05 | One state-owning epoll loop; timerfd or monotonic deadline heap. | Simple ownership; worker pool only on measured need. | Thread inventory, timer and load tests. |
| P-06 | 30-minute final reliability run; keep 10-minute L4 as smoke test. | Resolves duration mismatch; approval required. | Final stability report. |

See DECISIONS.md for approval gates. Proposed values are not approved by being documented.
