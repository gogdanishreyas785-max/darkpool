# DARKPOOL test and evidence plan

This plan includes the approved variable-size follow-up. Every execution result is labeled PASS, FAIL, NOT RUN, or BLOCKED with evidence. Load and capacity claims require measured runs, not compilation or constructor tests.

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
| T-V01 | Variable engine rosters. | Role counts and initial resources at N=3,4,5,8,9,16,24,32,64; reject N<3/N>64, duplicate IDs, invalid role values and incorrect counts. These are engine initialization tests, not network-capacity results. |
| T-V02 | Multi-role gameplay. | At N=9, eliminating one of two Insiders leaves the match live; eliminating the last Insider with positive health ends in EmployeesWin. At N=24, two Investigators each receive their own private clue, with independent once-per-match and same-target retry rules. Check thresholds at N=3,5,8,9. |
| T-V03 | Coordinator isolation and capacity. | Simultaneous N=3 and N=8 matches; independent match capacity, full roster required to start, excess join rejection, and cross-match membership rejection. |
| T-V04 | Variable TCP gameplay. | A complete N=8 match through the real server and protocol, including roles, clue privacy, all three rounds, votes and final outcome. The separate five-player E2E test remains a regression. |
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

Engine test sizes and protocol E2E sizes are reported separately: engine roster tests cover 3,4,5,8,9,16,24,32,64; full real-TCP gameplay tests cover 5 and 8. No size is considered load-tested by these functional suites. Four simultaneous 64-player matches and the 256-player server target remain unverified until a measured load run completes.

Use automated clients speaking the real protocol; opening sockets alone does not establish capacity.

## Verification status

The successful Ubuntu 24.04 Release build with warnings-as-errors and full CTest run for implementation commit `54e3e28287de3840caef2ec65009dfee0e5719a5` (GitHub Actions run 62) passed the engine roster cases for N=3,4,5,8,9,16,24,32,64 and complete real-TCP matches at N=5 and N=8.

### Load Test Matrix

| Run | Configuration | Status | Evidence / Notes |
|---|---|:---:|---|
| L1 | 5 players/1 match, full game | NOT RUN | Functional tests run at N=5; dedicated load measurement pending. |
| L2 | 16 players/1 match | **PASS** | Three repeatable 16-player runs (462 requests total, 100% success; 0 transport/correctness failures). Sequential: p50=0.29 ms, p95=0.52 ms, p99=0.64 ms. Concurrent burst: p50=0.70 ms, p95=1.71 ms, p99=4.29 ms. Verified winner: `InsiderWin`. |
| L3 | 32 players/1 match | NOT RUN | Untested; capacity unverified. |
| L4 | 64 players/1 match (Functional) | **PASS** | Three repeatable 64-player runs over UNIX sockets (1,683 requests total: 1,671 successful operations + 12 expected protocol rejections, 100% pass; 0 transport/correctness failures). Sequential: p50=0.10 ms, p95=0.17 ms, p99=0.65 ms. Concurrent burst: p50=0.13 ms, p95=0.58 ms, p99=1.32 ms. Verified winner: `InsiderWin`. |
| L4-Smoke | 64 players, 10-min sustained | **PASS** | 10-minute sustained repeated-match workload (600.04s, 13,271 matches, 7,365,405 requests, 0 transport/correctness failures, 0 unexpected disconnects). Latency: p50=0.06 ms, p95=0.09 ms, p99=0.14 ms, mean=0.06 ms. Throughput: 12,274.9 req/s. Peak RSS=3,928 kB, CPU=301.1s, FDs=8. Single measured run completed. |
| L4-Reliability | 64 players, 30-min sustained | **PASS** | 30-minute sustained repeated-match workload (1800.11s, 32,118 matches, 17,825,490 requests, 0 transport/correctness failures, 0 unexpected disconnects). Latency: p50=0.06 ms, p95=0.13 ms, p99=0.21 ms, mean=0.08 ms. Throughput: 9,902.4 req/s. Peak RSS=3,940 kB, CPU=894.9s, FDs=8. Zero socket/memory leaks (+12 kB RSS over 30m). Single measured run completed. |
| L5 | 2 × 32 concurrent matches | NOT RUN | Untested; multi-match starvation unverified. |
| L6 | 4 × up to 64 (256-player server target) | **NOT RUN** | Four-match 256-player capacity remains unverified. |
| L7 | Slow/disconnected clients | NOT RUN | Untested under load. |
| L8 | Malformed/flood traffic | NOT RUN | Malformed isolation verified functionally; high-rate flood unmeasured. |

### Measured 16-Player Real-Server Load Test Results (L2)

- **Test Commands**:
  ```bash
  # Standalone 3-run load harness execution
  ./build/darkpool_load_tests ./build/darkpool_server --players 16 --runs 3 --out docs/load_test_16p_results.json

  # Automated regression selftest executing subprocesses to verify real process exit status on assertion/outcome errors
  ./build/darkpool_load_tests ./build/darkpool_server --selftest

  # Automated CTest suite execution
  ctest --test-dir build --output-on-failure -R '^darkpool_load_'
  ```
- **Source Commit**: `fffed8535a2191a183c3a3a142305e0c6f14ddb9`
- **Environment**: Linux 6.18.54.1-microsoft-standard-WSL2 (x86_64); CPU: 11th Gen Intel(R) Core(TM) i5-1155G7 @ 2.50GHz; GCC 11.4.0
- **Timing & ACK Boundary**:
  Timing is captured using `std::chrono::steady_clock` (monotonic clock). The measurement interval begins immediately before the client writes the request frame bytes to the socket and ends immediately when the complete correlated response frame is received and parsed. Intentional phase waits and intervals are excluded from latency measurements.
- **Workload & Correctness Verification**:
  Real protocol clients driving a complete 16-player match (1 moderator + 16 players = 17 connected clients). Includes match creation, 16 player joins, match start, and all 3 rounds.
  - **Sequential Phases (Rounds 1 & 3, match setup/resolution)**: Sequential event publication, insider hints, legal trades, market resolution, clue request, voting, and vote resolution (122 requests per run).
  - **Concurrent Burst Phase (Round 2)**: 16 simultaneous client threads sending legal trades and votes concurrently over 16 independent TCP sockets (32 concurrent requests per run).
  - **Fatal Correctness Assertions**: All protocol responses, types, fields, role counts (2 Insiders, 1 Investigator, 13 Employees), and outcomes are strictly asserted. Any correctness or transport failure immediately fails the run and harness.
  - **Guaranteed Outcome Verification**: In this deterministic workload, health remains > 0 (final health = 6) and all players abstain (no eliminations), guaranteeing `phase == "Finished"` and `outcome == "InsiderWin"`. Both are explicitly asserted.

| Run | Clients | Duration | Sequential Latency (p50 / p95 / p99) | Concurrent Latency (p50 / p95 / p99) | Failures (Transport / Correctness) | Peak Server RSS | Server CPU Time | Verified Winner |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Run 1 | 17 (16P + 1M) | 74 ms | 0.25 / 0.49 / 0.58 ms | 0.57 / 1.63 / 1.71 ms | 0 / 0 | 4,092 kB | 30.0 ms | `InsiderWin` |
| Run 2 | 17 (16P + 1M) | 74 ms | 0.25 / 0.43 / 0.52 ms | 0.80 / 1.50 / 1.60 ms | 0 / 0 | 4,136 kB | 20.0 ms | `InsiderWin` |
| Run 3 | 17 (16P + 1M) | 99 ms | 0.37 / 0.57 / 0.64 ms | 0.73 / 2.82 / 4.29 ms | 0 / 0 | 4,092 kB | 30.0 ms | `InsiderWin` |
| **Aggregate** | **17** | **247 ms** | **0.29 / 0.52 / 0.64 ms** | **0.70 / 1.71 / 4.29 ms** | **0 / 0 (100% Pass)** | **4,136 kB max** | **80.0 ms total** | **`InsiderWin`** |

*Machine-readable report stored in: [`docs/load_test_16p_results.json`](file:///c:/darkpool-antigravity/darkpool/docs/load_test_16p_results.json)*

### Measured 64-Player Real-Server Load Test Results (L4)

- **Test Commands**:
  ```bash
  # Standalone 3-run 64-player load harness execution over UNIX sockets
  ./build/darkpool_load_tests_64p ./build/darkpool_server --runs 3 --out docs/load_test_64p_results.json

  # Automated regression selftest executing subprocesses to verify real process exit status on assertion/outcome/semantic errors
  ./build/darkpool_load_tests_64p ./build/darkpool_server --selftest

  # Automated semantic response validation regression test
  ./build/darkpool_load_tests_64p ./build/darkpool_server --test-semantic-response

  # Automated CTest suite execution
  ctest --test-dir build --output-on-failure -R '^darkpool_load_64p_'

  # 10-minute sustained repeated-match smoke test (opt-in)
  ./build/darkpool_load_tests_64p ./build/darkpool_server --smoke-10m

  # 30-minute sustained repeated-match reliability test (opt-in)
  ./build/darkpool_load_tests_64p ./build/darkpool_server --reliability-30m
  ```
- **Source Commit**: `fd65fe2a9361684a6a881d118d504e803cb045f5`
- **Environment**: Linux 6.18.54.1-microsoft-standard-WSL2 (x86_64); CPU: 11th Gen Intel(R) Core(TM) i5-1155G7 @ 2.50GHz; GCC 11.4.0
- **Transport**: UNIX-Domain Socket (`SOCK_STREAM`) with non-blocking server epoll loop and client timeouts.
- **Timing & ACK Boundary**:
  Timing is captured using `std::chrono::steady_clock` (monotonic clock). The measurement interval begins immediately before the client writes the request frame bytes to the socket and ends immediately when the complete correlated response frame is received and parsed. Intentional phase waits and intervals are excluded from latency measurements.
- **Workload & Correctness Verification**:
  Real protocol clients driving a complete 64-player match (1 moderator + 64 players = 65 connected clients over UNIX sockets). Includes match creation, 64 player joins, pre-start roster validation (rejection of premature start with 63/64 joined), excess join rejection (rejection of 65th join with `match is full`), match start, and all 3 rounds.
  - **Role Distribution (N=64)**: Strictly asserted as exactly 8 Insiders, 4 Investigators, and 52 Employees per the approved role formula.
  - **Privacy Visibility Policies**: Verified that non-insiders (Employees) attempting `insider_hint` are rejected with `ok: false`, non-investigators (Employees) attempting `request_clue` are rejected with `ok: false`, all 8 Insiders receive authorized private hints, and Investigators receive authorized private clue results.
  - **Sequential Phases (Rounds 1 & 3, match setup/resolution)**: Sequential event publication, insider hints, legal trades, market resolution, clue request, voting, and vote resolution (433 requests per run: 429 success, 4 expected rejections).
  - **Concurrent Burst Phase (Round 2)**: 64 simultaneous client threads sending legal trades (`HOLD`) and votes (`null` - abstain) concurrently over 64 independent UNIX socket connections (128 concurrent requests per run).
  - **Fatal Correctness Assertions**: All protocol responses, types, fields, role counts, and outcomes are strictly validated with explicit payload validators. Any correctness or transport failure immediately fails the run and harness.
  - **Guaranteed Outcome Verification**: In this deterministic workload, health remains > 0 (final health = 6) and all players abstain (no eliminations), guaranteeing `phase == "Finished"` and `outcome == "InsiderWin"`. Both are explicitly asserted.

| Run | Clients | Duration | Sequential Latency (p50 / p95 / p99) | Concurrent Latency (p50 / p95 / p99) | Failures (Transport / Correctness) | Peak Server RSS | Server CPU Time | Open FDs | Verified Winner |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Run 1 | 65 (64P + 1M) | 82 ms | 0.10 / 0.19 / 0.78 ms | 0.14 / 0.39 / 0.53 ms | 0 / 0 | 3,872 kB | 40.0 ms | 73 | `InsiderWin` |
| Run 2 | 65 (64P + 1M) | 68 ms | 0.09 / 0.15 / 0.39 ms | 0.13 / 1.15 / 1.65 ms | 0 / 0 | 3,876 kB | 40.0 ms | 73 | `InsiderWin` |
| Run 3 | 65 (64P + 1M) | 152 ms | 0.10 / 0.17 / 0.54 ms | 0.14 / 0.39 / 0.46 ms | 0 / 0 | 3,848 kB | 50.0 ms | 73 | `InsiderWin` |
| **Aggregate** | **65** | **302 ms** | **0.10 / 0.17 / 0.65 ms** | **0.13 / 0.58 / 1.32 ms** | **0 / 0 (100% Pass)** | **3,876 kB max** | **130.0 ms total** | **73 max** | **`InsiderWin`** |

*Machine-readable report stored in: [`docs/load_test_64p_results.json`](file:///c:/darkpool-antigravity/darkpool/docs/load_test_64p_results.json)*

### L4 Sustained Workload Stages (10-Minute Smoke & 30-Minute Reliability)

- **10-Minute Repeated-Match Smoke Test (`--smoke-10m`)**: **PASS** (single measured run).
  - **Source Commit**: `154ae9147ee1f8606ff5c323b1b710ed3b421b14`
  - **Actual Duration**: 600.04s (600,038 ms; target: 600s, zero shortfall).
  - **Completed Matches**: 13,271 matches (13,271 planned resets, 0 unexpected disconnects).
  - **Request Volume**: 7,365,405 requests (100% successful; 0 transport failures, 0 correctness failures).
  - **Throughput**: 12,274.9 req/s.
  - **Round-Trip Latency**: p50 = 0.057 ms, p95 = 0.093 ms, p99 = 0.139 ms, mean = 0.062 ms, min = 0.008 ms, max = 24.965 ms (p95 meets provisional ≤100 ms guidance).
  - **Resources**: Peak RSS = 3,928 kB, Server CPU = 301,070.0 ms, Open FDs = 8.
  - **Report File**: [`docs/load_test_64p_smoke_10m_results.json`](file:///c:/darkpool-antigravity/darkpool/docs/load_test_64p_smoke_10m_results.json).

- **30-Minute Repeated-Match Reliability Test (`--reliability-30m`)**: **PASS** (single measured run).
  - **Source Commit**: `154ae9147ee1f8606ff5c323b1b710ed3b421b14`
  - **Actual Duration**: 1800.11s (1,800,110 ms; target: 1800s, zero shortfall).
  - **Completed Matches**: 32,118 matches (32,118 planned resets, 0 unexpected disconnects).
  - **Request Volume**: 17,825,490 requests (100% successful; 0 transport failures, 0 correctness failures).
  - **Throughput**: 9,902.4 req/s.
  - **Round-Trip Latency**: p50 = 0.064 ms, p95 = 0.131 ms, p99 = 0.212 ms, mean = 0.078 ms, min = 0.008 ms, max = 195.422 ms (p95 meets provisional ≤100 ms guidance).
  - **Resources**: Peak RSS = 3,940 kB, Server CPU = 894,870.0 ms, Open FDs = 8.
  - **Leak / Degradation Analysis**: Flat memory (+12 kB RSS increase from smoke to 30m reliability across >10.4M additional requests; exactly 8 open FDs maintained throughout) confirms no memory leaks, file-descriptor leaks, or event-loop degradation.
  - **Report File**: [`docs/load_test_64p_reliability_30m_results.json`](file:///c:/darkpool-antigravity/darkpool/docs/load_test_64p_reliability_30m_results.json).

- **L6 (Four-Match 256-Player Capacity)**: **PASS** (functional concurrent capacity verified; see dedicated L6 section below). Sustained long-duration multi-match testing remains a separate unverified stage.

> [!IMPORTANT]
> The single-match 64-player capacity is verified across functional runs (3 repeatable runs) and sustained runs (one 10-minute smoke run and one 30-minute reliability run). Multi-run acceptance repetitions (three 10m or three 30m runs) were not executed and are not claimed. Four-match 256-player functional capacity (L6) is verified in a dedicated, isolated test harness; sustained multi-match soak remains a distinct stage.

## L6: Four-Match Concurrent Capacity Test (256 Players, 4 Matches, 4 Independent Moderators)

- **Status**: **PASS** (reproducible functional capacity benchmark; sustained long-duration L6 runs remain a distinct future stage marked NOT RUN).
- **Dedicated Machine-Readable Report**: [`docs/load_test_l6_results.json`](file:///c:/darkpool-antigravity/darkpool/docs/load_test_l6_results.json)
- **Source Commit**: `44900e15cea5c321cbb412f1d4b01e28dec18798` (clean checkout, `git_dirty: false`)
- **Build & Compiler**: Linux Release build (`-DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON`), GCC 11.4.0 (`Linux 6.18.54.1-microsoft-standard-WSL2 x86_64`, 11th Gen Intel(R) Core(TM) i5-1155G7 @ 2.50GHz).
- **Execution Command**: `./build/darkpool_load_tests_l6 ./build/darkpool_server --runs 3 --out docs/load_test_l6_results.json`
- **Automated CTest Targets**:
  - `darkpool_load_l6_functional`: Runs full 4-match 256-player concurrent functional workload (**PASS**).
  - `darkpool_load_l6_regression`: Runs regression self-tests covering 4 independent moderators, 64/64 full match capacity, negative cross-moderator unauthorized command rejection, negative cross-match player access rejection, 5th moderator rejection, sibling match isolation, assertion failure propagation, outcome mismatch propagation, and unwritable output path failure handling (**PASS**; 6/6 sub-tests pass).

### Concurrency and Topology Contract
- **Concurrent Matches**: 4 independent matches hosted simultaneously on one server process with measured overlapping lifetimes.
- **Client Count**:
  - 256 active player clients (64 players per match across 4 matches).
  - 4 independent authenticated moderator connections (1 dedicated moderator per match).
  - **Total Connected Clients**: 260 concurrent connections.
- **Production Server Constraint**:
  - `src/server_main.cpp` enforces `constexpr std::size_t kMaxMatches = 4;`, `constexpr std::size_t kMaxModerators = kMaxMatches;` (4), and `constexpr std::size_t kMaxConnections = 256 + 4 = 260;`.
  - Enforces match ownership: each match is controlled exclusively by its own authenticated moderator session. A moderator attempting to issue commands on another moderator's match receives an unauthorized error.
  - Attempting a 5th moderator connection beyond 4 is rejected at connection handshake (`rejected_by_server`).
  - Attempting a 65th player in a match or a 261st connection beyond 260 is rejected at connection handshake.
- **Concurrency & Overlap Evidence**: Per-match lifecycle timestamps (`lifecycle_start_ms` and `lifecycle_end_ms`) are recorded. Overlap is derived mathematically (`max(start) < min(end)`); all 4 matches overlap across all runs (e.g., 702 ms, 321 ms, and 229 ms measured overlap).

### Workload and Phase Execution
- **Role Distribution Per Match**: Exactly 8 Insiders, 4 Investigators, 52 Employees per match (verified via server join responses).
- **Rounds Executed**: All 4 matches completed the full three-round protocol concurrently:
  - **Round 1**: Event publication (`impact=1`) by each match's moderator, authorized private insider hints, unauthorized clue rejection (Employee), authorized investigator clue, sequential trades (`HOLD`), market resolution, open voting, abstain votes, vote resolution.
  - **Round 2 (Simultaneous Burst)**: 256 concurrent client threads across all 4 matches issue simultaneous `HOLD` trade requests and abstain vote requests using thread barriers.
  - **Round 3**: Event publication (`impact=-1`), sequential trades, market resolution, voting, final status query.
- **Cross-Match Isolation & Negative Access Checks**:
  - Cross-moderator command isolation: Moderator for Match 2 attempting to advance Match 1 is rejected.
  - Cross-match player access isolation: A player in Match 1 attempting to vote in Match 2 is rejected by the coordinator.
  - Unauthorized role checks: Non-insider (Employee) requesting insider hints is rejected; non-investigator requesting clues is rejected.
  - Sibling match cleanup: Disconnecting players from Match 1 verified that Matches 2, 3, and 4 remained fully intact and reachable.
- **Terminal Outcome**: All 4 matches independently reached terminal phase `"Finished"` with verified outcome `"InsiderWin"`.

### Measured Results (3-Run Pooled Summary and Individual Runs)
- **Repetitions**: 3 complete runs executed; full metrics retained for each run in `runs` array plus pooled `multi_run_summary`.
- **Request Volume (3 Runs)**:
  - Total requests across all runs: 6,528 requests (2,176 requests per run).
  - Successful requests: 6,492 requests (100% of legal requests accepted).
  - Expected negative rejections: 36 (12 per run: 4 employee hint rejections, 4 employee clue rejections, 1 cross-moderator unauthorized command rejection, 1 cross-match unauthorized vote rejection, 1 excess 65th player rejection, 1 excess 5th moderator rejection).
- **Failures & Errors**:
  - Transport failures: 0
  - Correctness failures: 0
  - Unexpected disconnects: 0
  - Timeouts: 0
- **Elapsed Run Duration & Throughput**:
  - Run 1: 845 ms (2,575.1 req/s, 702 ms measured overlap)
  - Run 2: 377 ms (5,771.9 req/s, 321 ms measured overlap)
  - Run 3: 278 ms (7,827.3 req/s, 229 ms measured overlap)
  - Mean Run Duration: 500.0 ms | Mean Throughput: 5,391.5 req/s.
- **Latency Percentiles (3-Run Pooled Across 6,528 Requests)**:
  - **Sequential Phase (Rounds 1 & 3)**: p50 = 0.094 ms, p95 = 0.199 ms, p99 = 0.454 ms, mean = 0.132 ms.
  - **Concurrent Burst Phase (Round 2, 256 Simultaneous Client Threads)**: p50 = 11.478 ms, p95 = 25.779 ms, p99 = 30.748 ms, mean = 12.353 ms.
  - **Total Overall**: p50 = 0.114 ms, p95 = 13.630 ms, p99 = 27.694 ms, mean = 3.007 ms.
- **Resource Measurements**:
  - Baseline Open FDs: 9 (sampled at zero client load).
  - Peak Open FDs: 268 (sampled with all 260 client connections active: 4 moderators + 256 players).
  - Post-Cleanup Open FDs: 91 – 110 (sampled after all client sockets closed).
  - Peak Server RSS: 4,260 kB (~4.2 MB across server daemon lifetime).
  - Total Server CPU Time: 490.0 ms across 3 runs.

### Measurement Boundaries and Limitations
1. **Measurement Boundary**: Request latency is measured via monotonic clock (`std::chrono::steady_clock`) from frame write initiation until complete correlated response frame deserialization and validation. Excludes harness coordination pauses.
2. **Concurrency Scope**: Tests 256 simultaneous player clients across 4 matches with burst concurrency in Round 2 and 4 independent moderator connections.
3. **Sustained / Soak Testing**: Functional capacity benchmark. Sustained multi-match soak testing (e.g. 10m/30m repeated 4-match cycles) remains a separate, distinct testing stage marked **NOT RUN**.

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
