# DARKPOOL S0 decision register

Status note: the S1 follow-up approved the five-player baseline mechanics. A later explicit user decision approves variable-size matches and the role-scaling policy recorded in DEC-02/DEC-20. Other unresolved protocol, operations, scoring and audit items remain open.

## 1. Direct requirements from Draft 3

| ID | Requirement |
|---|---|
| REQ-01 | C++20 on Ubuntu; separate clients; one authoritative server owns official state. |
| REQ-02 | Five-player baseline: one Insider, one Investigator, three Employees. The moderator is a separate non-player by approved S1 decision. |
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

## 3. Deferred (Historical S0 Scope)

*(Historical context: At the time S0 was approved, TCP/TLS 1.3 was deferred. Private LAN TLS 1.3 has since been implemented and merged in Milestone S9 via PR #28. Public-Internet deployment, persistent accounts, and automated dynamic certificate lifecycle remain deferred).*

TCP/TLS 1.3/certificate lifecycle; real money, feeds, short selling, derivatives, complex auctions; GUI/mobile, public matchmaking, persistent accounts; multi-region/consensus/failover; unsupported advanced anti-cheat/host-root/side-channel claims; worker pool absent measured need; sandboxing absent approved testable requirement.

## Approval gate (Historical S0 status)

*(Historical context: At the time these initial S0 and S1 decisions were approved, capacity and performance targets were unmeasured design ceilings. The notes below record the historical status at that milestone. For current measured status following subsequent L4/L6 benchmark suites and S9 security merge, see [Post-PR #28 implementation status, capacity, and security follow-up](#post-pr-28-implementation-status-capacity-and-security-follow-up) below.)*

The original S0 proposals are not approvals by themselves. The explicit later user follow-ups approve DEC-02 and DEC-20 for variable-size matches. Other proposals remain governed by their individual status. At the time of this approval gate, capacity and performance remained unmeasured targets.

## S1 follow-up status (Historical S1 baseline)

Approved for S1 by the user's explicit follow-up: separate moderator boundary and stated powers; exactly five active roles; proposed economy, share reserve, event/health/price formulas, update order, clue, voting and victory precedence; supply-boundary fixture; and non-conserved player-credit model. The per-player cap is provisionally 1,000,000 credits (uint32_t) for S1, with overflow-causing trades rejected; this is an implementation detail directed by the S1 request. The engine receives an explicit role assignment and event impacts as inputs; S1 does not invent role shuffling or event randomness.

Still open after S1: protocol encoding/ACKs and duplicate policy; local authentication and production limits; chat/audit retention and secret audit access; reconnect/exit handling; and load-test profile/acceptance evidence. In the historical S1 baseline, the 64-player/four-match, latency and reliability targets remained unverified (subsequently measured in L4 and L6; see below). The later variable-size follow-up explicitly supersedes S1's deferred role policy and authorizes the formula in DEC-02.

## Variable-size follow-up status (Historical pre-L6 status)

Approved: 3–64 active players per match; moderator excluded; five remains the default; role counts follow DEC-02 using safe integer arithmetic. Multiple Insiders form a team: health zero gives an immediate Insider-team win; eliminating one Insider does not end the match while another remains; eliminating the last Insider with positive health gives Employees the win; surviving round three with any Insider active gives the Insider team the win. Every Investigator gets an independent once-per-match private binary role clue with same-target retry idempotency. These are approved game rules. In this historical milestone, four concurrent 64-player matches and a 256-player server remained unverified until end-to-end load and isolation testing (subsequently validated in L6; see below).

## Post-PR #28 implementation status, capacity, and security follow-up

Following the merge of PRs #24, #25, #26, and #28 (Milestone S9), the implementation and verification status has advanced beyond the initial unverified design ceilings. This section provides the authoritative current status:

1. **Initial capacity decisions versus measured evidence**:
   - At the time of the original S0 and S1 decisions, 64 players per match and four matches per process (REQ-07, DEC-20) were theoretical design ceilings and had not yet been measured.
   - Subsequent L4 load testing (`docs/load_test_64p_results.json`) supplied measured single-match capacity evidence (64 players, sequential p95 = 0.17 ms, concurrent burst p95 = 0.58 ms) and sustained reliability (10-minute smoke and 30-minute soak without resource leaks).
   - Subsequent L6 multi-match load testing (`docs/load_test_l6_results.json`) supplied measured four-match capacity evidence (4 concurrent matches × 64 players = 256 active players, concurrent burst p95 = 22.31 ms) and sustained reliability (10-minute smoke and 30-minute soak across 35,736 completed matches with zero transport or correctness failures).

2. **Moderator topology and connection limit reconciliation**:
   - The original S0 proposal (DEC-20) envisioned a single moderator connection across all matches (256 players + 1 moderator = 257 connections).
   - To support independent moderator sessions and match ownership across multiple matches, the L6 implementation in `src/server_main.cpp` updated the configuration to four independent authenticated moderator connections: `kMaxMatches = 4`, `kMaxModerators = 4`, `kMaxPlayerConnections = 256`, and `kMaxConnections = 260`.
   - Each match is independently owned and controlled by its dedicated moderator connection.

3. **Scope of verification and deployment boundaries**:
   - Historical statements that capacity and latency were "unverified" have been superseded **only** for the exact workloads, topologies, and environments measured and recorded in the repository artifacts (`docs/load_test_16p_results.json`, `docs/load_test_64p_results.json`, `docs/load_test_l1_results.json`, `docs/load_test_l3_results.json`, `docs/load_test_l5_results.json`, `docs/load_test_l6_results.json`, `docs/load_test_l7_results.json`, and `docs/load_test_l8_results.json`).
   - These measurements were performed on a single-host Linux environment over nonblocking sockets. They validate the specific recorded workloads, but do not prove universal capacity under all arbitrary hardware, operating system, network, or production deployment conditions.

4. **Private LAN TCP transport**:
   - The server supports non-loopback TCP binding on an assigned RFC 1918 private IPv4 address via `--bind-address` (PR #26).
   - This transport is unencrypted plaintext without TLS. Physical cross-device two-computer LAN acceptance for the 5-player baseline was completed on October 17, 2026 (see `docs/test-plan.md`). Multi-machine TLS 1.3 manual verification and public Internet deployment remain deferred.

5. **Transport security (TLS 1.3) status**:
   - Approved and implemented in the S9 engineering milestone (merged to `main` via PR #28).
   - Implements strict TLS 1.3 on a dedicated secure listener (default port 4783) using OpenSSL 3.x, supporting both local-CA verification and SHA-256 leaf fingerprint pinning.
   - Public-Internet deployment, persistent client accounts, dynamic certificate reload, and mutual TLS (mTLS) remain deferred. See detailed decisions below and specification in `docs/s9-transport-security-design.md`.

## S9 Approved Architectural Decisions (Transport Security & Identity)

The following explicit architectural decisions govern the S9 TLS 1.3 implementation:

| Decision ID | Summary | Approved Decision | Rationale & Scope | Approval |
|---|---|---|---|---|
| **AD-S9-01** | Listener Port Topology | Dedicated secure listener on port `4783` by default. Plaintext loopback TCP on port `4782` and UNIX sockets on `/tmp/darkpool.sock` remain operational. Dual-mode plaintext/TLS port is rejected. | Eliminates protocol-sniffing ambiguities on port 4782. Allows simultaneous plaintext development/testing and encrypted LAN gameplay. TLS listener speaks TLS from byte 1 and rejects plaintext safely. | **Approved** |
| **AD-S9-02** | Moderator & Client Authentication | Server-authenticated TLS 1.3 with existing `DARKPOOL_MODERATOR_TOKEN` transmitted inside the encrypted TLS application channel. Mutual TLS (mTLS) and individual player identity systems are deferred. | Protects moderator tokens and all player data against LAN sniffing without requiring complex client certificate provisioning for casual players. Anonymous player sessions preserved. | **Approved** |
| **AD-S9-03** | Client Certificate Verification Modes | Dual mode support: (1) Local-CA certificate verification (`--ca-cert <pem>`), and (2) Explicit SHA-256 leaf fingerprint pinning (`--server-fingerprint <64-hex>`). Both modes strictly enforce certificate validity and IP SAN matching. | Local-CA mode supports structured multi-client deployments; fingerprint pinning enables zero-install ad-hoc LAN matches. Both strictly fail closed. `--insecure` is prohibited. | **Approved** |
| **AD-S9-04** | Certificate Reload Strategy | Dynamic in-process certificate reload is deferred. Certificate/key rotation requires a controlled server restart and corresponding trust/pin update on clients. | Avoids signal-safety hazards and complex asynchronous state replacement in the single-threaded epoll reactor. Controlled restart is appropriate for private LAN deployments. | **Approved** |
| **AD-S9-05** | Strict Private Key Permission Policy | Server private key file permissions must be owner-only (`0600` or `0400`). Server initialization strictly fails closed if readable/writable by group or others (`st_mode & 0077 != 0`), emitting an actionable diagnostic. | Prevents local credential exposure on shared or multi-user systems. Insecure key permissions must not silently downgrade security. | **Approved** |
| **AD-S9-06** | Reactor TLS State Machine & TCP_NODELAY | Epoll reactor maintains explicit, separate bi-directional readiness interest flags (`tls_read_want_read`, `tls_read_want_write`, `tls_write_want_read`, `tls_write_want_write`) resuming the exact requested TLS operation. `TCP_NODELAY` is enabled on all TCP sockets to eliminate delayed-ACK floor. | Prevents CPU-spinning loops on write `WANT_READ` and avoids stall/starvation; eliminates the ~40ms delayed-ACK latency floor, dropping p95 latency below 1-5 ms. | **Approved** |
| **AD-S9-07** | Fingerprint Pinning Verification Model | Fingerprint pinning mode accepts both self-signed and CA-issued leaf certificates when the leaf SHA-256 matches the pin, enforcing validity period, IP SAN, and serverAuth purpose (`X509_PURPOSE_SSL_SERVER`), without requiring the CA root in the client trust store. | Allows ad-hoc LAN matches with either self-signed certificates or CA-issued certificates without client PKI distribution. Strict identity and validity checks remain fully enforced. | **Approved** |
| **AD-S9-08** | Mutually Exclusive Verification CLI Flags | Supplying both `--ca-cert` and `--server-fingerprint` to `darkpool_client` is rejected immediately with an explicit error diagnostic and exit code 2. | Eliminates ambiguous trust model selection and prevents order-dependent argument overrides. | **Approved** |

## Stage 4 Approved Architectural Decisions (Advanced Mechanics, Quantity Trading, Scouting)

The following explicit architectural decisions govern the Stage 4 gameplay enhancements:

| Decision ID | Summary | Approved Decision | Rationale & Scope | Approval |
|---|---|---|---|---|
| **AD-S4-01** | Centralized Market Model Architecture | Factor weights and pricing coefficients are centralized in `struct MarketModelCoefficients`. Baseline coefficients (`approved_baseline()`) maintain existing formula bounds and test compatibility by default. Proposed candidate coefficients (`candidate_proposal()`) are separated as candidate values awaiting explicit user review, documented in `docs/market-model-proposal.md`. | Eliminates magic numbers throughout the engine. Protects test suite reproducibility and ensures candidate formula weights are not treated as approved rules prematurely. | **Approved Architecture / Candidate Coefficients Pending Review** |
| **AD-S4-02** | Positive Integer Quantity Trading (All-or-Nothing) | Buy and sell commands support an explicit positive integer quantity ($q \ge 1$), defaulting to 1 for backward compatibility. Orders resolve against finite reserve in deterministic rotating seat priority. If remaining reserve $< q$, the order is rejected entirely (`RejectedInsufficientReserve`). Partial fills and limit order books are prohibited. | Maintains simplicity and determinism while enabling strategic position sizing without introducing complex order-book matching engines or partial-fill accounting. | **Approved** |
| **AD-S4-03** | Total Share Conservation Invariant | Total supply remains fixed at $20N$ shares across every resolution: $\sum \text{player.shares} + \text{reserve} = 20N$. Selling credits the player from the match pool at opening price $P$; buying debits the player and moves shares from reserve. Rejects any trade violating credit cap ($1,000,000$), share inventory, or cash sufficiency. | Guarantees finite economic conservation and prevents share creation or destruction bugs across variable player counts and multi-round games. | **Approved** |
| **AD-S4-04** | Research / Scouting Action and Audience Privacy | Active players may execute research at most once per match during `Phase::Discussion` at a cost of exactly 10 credits. Returns a coarse direction signal (`"Positive"`, `"Neutral"`, `"Negative"`). Idempotent retry returns cached signal at zero cost. Result is strictly private to requesting session; coordinator generates 0 broadcast notifications and leaves snapshots unmutated. | Provides valuable strategic signal during pre-trading discussion without leaking the exact hidden integer impact or revealing scouting activity to opponents. | **Approved** |
| **AD-S4-05** | Public Market Indicator Snapshots & Protocol Fields | Authoritative public snapshots and status messages include `sentiment` ($[-10, 10]$), `volatility` ($[0, 20]$), and `liquidity` ($[0, 100\%]$) as public market indicators. Private player balances, holdings, and research records remain strictly excluded from all public envelopes. | Enriches public gameplay information and transparency for all players while preserving zero information leakage of private roles, hints, and balances. | **Approved** |
| **AD-S4-06** | Settlement Order Provenance & Quantity Allocation | Rotating seat priority $((round - 1) \bmod N)$ is confirmed as a canonical approved S1 rule (DEC-04) dating from the project's inception. Under multi-quantity trading, this canonical priority is preserved with all-or-nothing allocation: if remaining reserve $< q$, the order is rejected entirely without partial mutation. | Reconciles settlement order provenance with pre-Stage 4 canonical rules. Avoids partial-fill ambiguities while conserving the finite $20N$ supply. | **Approved** |
| **AD-S4-07** | Mathematical Floor Division in Market Model | All integer division in candidate model formulas ($\lfloor \text{sentiment}/2 \rfloor$, $\lfloor \text{net\_demand}/2 \rfloor$, $\lfloor (|D|-1)/2 \rfloor$) strictly follows mathematical floor division via safe helper `Match::floor_div` rather than C++ signed integer truncation. Reconciles negative odd values (e.g. $\lfloor -1/2 \rfloor = -1$) and corrects Scenario 2 closing price to $P'=1$. | Fixes negative integer truncation discrepancies between published mathematical specifications and C++ code. Preserves exact deterministic arithmetic. | **Approved Architecture / Candidate Coefficients Pending Review** |
| **AD-S4-08** | Gross Volume Grounding for Volatility Indicator | Distinguish net demand $D = B - S$ from gross executed volume $G = B + S$. The public volatility indicator is defined as $\mathcal{V} = \text{clamp}(G + 2 \cdot |e|, 0, 20)$, ensuring that equal buy and sell volumes ($B = S > 0$) accurately reflect trading activity rather than falsely reporting zero activity. | Grounding volatility in gross volume $G$ ensures market activity is transparently reported in snapshots while directional price impact remains governed by net demand. | **Approved Architecture / Candidate Coefficients Pending Review** |
| **AD-S4-09** | Candidate Reserve Depletion Scarcity Surcharge (`+w_l`) vs Illiquidity Discount (`-w_l`) | When buying ($D > 0$) into a depleted dark pool reserve ($\mathcal{L} < 20\%$), the candidate formula applies a positive scarcity surcharge (`+w_l \cdot \text{ScarcitySurcharge}_{\mathcal{L}}$`), rather than a negative illiquidity discount (`-w_l`). Rationale: Depleted pools experience physical shortage squeeze and upward execution slippage (+2 price impact). Gameplay consequence: Forces traders to anticipate reserve depletion, as buying scarce shares becomes increasingly expensive. Baseline leaves $w_l = 0$. | Reconciles documented candidate equation with code and Scenario 7B ($P'=24$ vs $P'=22$ for high reserve). Retained as an unapproved candidate proposal pending explicit user review. | **Candidate Proposal Pending User Review** |
| **AD-S4-10** | Deterministic Volatility Magnification Factor (`volatility_weight` / $w_{\text{vol}}$) | Introduces centralized coefficient `volatility_weight` ($w_{\text{vol}}$, default 0 in `approved_baseline()`, 1 in `candidate_proposal()`). Market direction $\text{dir}_{\text{market}} = \text{sign}(D)$ if $D \ne 0$, else $\text{sign}(e)$ if $e \ne 0$, else 0. The volatility pricing term is $w_{\text{vol}} \cdot \text{dir}_{\text{market}} \cdot \lfloor \mathcal{V} / 4 \rfloor$. In neutral markets ($D=0, e=0$), $\text{dir}_{\text{market}} = 0$, strictly guaranteeing zero price bias. Higher gross volume under equal net demand increases price magnitude in the established market direction (e.g. Scenario 5: $G=3 \implies P'=23$ vs $G=7 \implies P'=24$). | Coherently incorporates calculated volatility $\mathcal{V}$ into candidate price determination without introducing directional bias in balanced markets. Kept separate from baseline. | **Candidate Proposal Pending User Review** |
| **AD-S4-11** | Safe Mathematical Floor Division Precondition & Overflow Protection | `Match::floor_div(a, b)` computes mathematical floor $\lfloor a / b \rfloor$ via 64-bit integer arithmetic (`std::int64_t`) and clamps results to `[INT32_MIN, INT32_MAX]`, preventing signed integer overflow for `INT32_MIN / -1`. Rejects invalid divisors ($b == 0$) by throwing `std::invalid_argument` rather than silently returning 0. | Eliminates undefined behavior across the entire 32-bit signed integer domain while strictly adhering to mathematical floor semantics for all valid market-model inputs and failing closed on invalid preconditions. | **Approved** |
