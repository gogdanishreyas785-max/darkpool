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
| L5 | 2 × 32 concurrent matches | **PASS** | Three repeatable 2-match 32-player runs over UNIX sockets with 2 independent moderators (1,758 requests total: 1,719 successful operations, 39 expected rejections, 100% pass; 0 transport/correctness failures). Concurrency overlap and multi-match anti-starvation verified across all runs. Sequential: p50=0.12 ms, p95=0.22 ms, p99=0.60 ms. Concurrent burst: p50=3.15 ms, p95=6.21 ms, p99=7.02 ms. Verified winner: `InsiderWin` for both matches. |
| L6-Functional | 4 × 64 (256 players / 4 matches) | **PASS** | Three repeatable 4-match runs over UNIX sockets with 4 independent moderators (6,528 requests total, 100% pass; 0 transport/correctness failures). Sequential: p50=0.14 ms, p95=0.33 ms, p99=1.05 ms. Concurrent burst: p50=14.39 ms, p95=22.31 ms, p99=29.32 ms. Verified winner: `InsiderWin`. |
| L6-Smoke | 4 × 64, 10-min sustained | **PASS** | 10-minute sustained repeated 4-match cycle workload (600.19s, 2,661 cycles, 10,644 matches, 5,790,336 requests: 5,758,404 successful operations, 31,932 expected rejections; 0 transport/correctness failures, 0 unexpected disconnects). Latency: p50=0.08 ms, p95=10.32 ms, p99=13.12 ms, mean=2.14 ms. Throughput: 9,647.4 req/s. Initial VmRSS=3,580 kB, End VmRSS=4,276 kB, Peak VmHWM=4,296 kB, CPU=308.8s, Settled FDs=8 (100% of 2,661 cycles settled within baseline + 2 tolerance). Single measured run completed. |
| L6-Reliability | 4 × 64, 30-min sustained | **PASS** | 30-minute sustained repeated 4-match cycle workload (1800.11s, 8,934 cycles, 35,736 matches, 19,440,384 requests: 19,333,176 successful operations, 107,208 expected rejections; 0 transport/correctness failures, 0 unexpected disconnects). Latency: p50=0.08 ms, p95=9.44 ms, p99=11.39 ms, mean=2.00 ms. Throughput: 10,799.5 req/s. Initial VmRSS=3,520 kB, End VmRSS=4,216 kB, Peak VmHWM=4,400 kB, CPU=973.5s, Settled FDs=8. Post-cleanup VmRSS series bounded in 4,144–4,248 kB envelope (mean 4,200 kB) across all 8,934 cycles. Single measured run completed. |
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

*Machine-readable report stored in: [`docs/load_test_16p_results.json`](load_test_16p_results.json)*

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

*Machine-readable report stored in: [`docs/load_test_64p_results.json`](load_test_64p_results.json)*

### Measured 2 × 32-Player Concurrent Match Results (L5)

- **Test Commands**:
  ```bash
  # Standalone 3-run 2-match 32-player load harness execution over UNIX sockets
  ./build/darkpool_load_tests_l5 ./build/darkpool_server --runs 3 --out docs/load_test_l5_results.json

  # Automated deterministic regression and anti-starvation selftest suite (10 test cases)
  ./build/darkpool_load_tests_l5 ./build/darkpool_server --selftest

  # Automated CTest suite execution
  ctest --test-dir build --output-on-failure -R '^darkpool_load_l5_'
  ```
- **Environment**: Linux 6.18.54.1-microsoft-standard-WSL2 (x86_64); CPU: 11th Gen Intel(R) Core(TM) i5-1155G7 @ 2.50GHz; GCC 11.4.0
- **Transport**: UNIX-Domain Socket (`SOCK_STREAM`) with non-blocking server epoll loop and bounded client timeouts.
- **Timing & ACK Boundary**:
  Timing is captured using `std::chrono::steady_clock` (monotonic clock). The measurement interval begins immediately before the client writes the request frame bytes to the socket and ends immediately when the complete correlated response frame is received and parsed. Intentional phase waits and intervals are excluded from latency measurements.
- **Workload, Isolation & Anti-Starvation Verification**:
  Real protocol clients driving 2 concurrent matches (Match 1 and Match 2) driven simultaneously by 2 independent authenticated moderators and 64 players (32 per match), for a total of 66 connected clients over UNIX sockets.
  - **Role Distribution (N=32 per match)**: Strictly asserted as exactly 4 Insiders, 2 Investigators, and 26 Employees per match (8 Insiders, 4 Investigators, 52 Employees total across both matches).
  - **Sibling Match Isolation Policies**: Verified that a moderator cannot issue commands to a sibling match (`connection is not a member of target match`), cross-match player votes/actions are strictly rejected (`connection is not a member of target match`), premature starts before 32 players join are rejected (`all configured players must join before start`), and excess 33rd player joins are rejected (`match is full`).
  - **Anti-Starvation & Concurrency Overlap**: Match 1 and Match 2 run simultaneously with full lifecycle overlap (overlap durations: 227 ms, 184 ms, 209 ms). Neither match starves, stalls, or blocks the progress of the other; both matches progress independently through all 3 rounds to completion with zero timeouts and zero unexpected disconnects (`starvation_verified: true`).
  - **Concurrent Burst Phase (Round 2)**: 64 simultaneous client threads (32 per match) submitting legal trades (`HOLD`) and votes (`null` - abstain) simultaneously.
  - **Guaranteed Outcome Verification**: In this deterministic workload, health remains > 0 and all players abstain, guaranteeing `phase == "Finished"` and `outcome == "InsiderWin"` for both concurrent matches.
  - **Socket & Resource Cleanup**: Verified that all 66 client connections close and server file descriptors settle to baseline open FDs (8 settled vs 8 baseline, bounded within `baseline + 2`).

| Run | Matches × Players | Total Clients | Duration | Overlap | Sequential Latency (p50 / p95 / p99) | Concurrent Latency (p50 / p95 / p99) | Total Latency (p50 / p95 / p99) | Failures | Settled FDs (Base) | Starvation Verified | Verified Winners |
|---|---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|
| Run 1 | 2 × 32 | 66 | 238 ms | 227 ms | 0.12 / 0.23 / 0.59 ms | 3.12 / 6.07 / 7.02 ms | 0.13 / 3.99 / 6.22 ms | 0 / 0 | 8 (8) | YES | `InsiderWin` (M1, M2) |
| Run 2 | 2 × 32 | 66 | 195 ms | 184 ms | 0.12 / 0.22 / 0.60 ms | 3.15 / 6.21 / 7.02 ms | 0.13 / 4.03 / 6.23 ms | 0 / 0 | 8 (8) | YES | `InsiderWin` (M1, M2) |
| Run 3 | 2 × 32 | 66 | 220 ms | 209 ms | 0.12 / 0.22 / 0.60 ms | 3.17 / 6.32 / 7.02 ms | 0.13 / 4.06 / 6.24 ms | 0 / 0 | 8 (8) | YES | `InsiderWin` (M1, M2) |
| **Aggregate** | **2 × 32** | **66** | **653 ms** | **620 ms total** | **0.12 / 0.22 / 0.60 ms** | **3.15 / 6.21 / 7.02 ms** | **0.13 / 4.03 / 6.23 ms** | **0 / 0 (100% Pass)** | **8 (8)** | **YES (100%)** | **`InsiderWin` (Both)** |

*Machine-readable report stored in: [`docs/load_test_l5_results.json`](load_test_l5_results.json)*

### L4 Sustained Workload Stages (10-Minute Smoke & 30-Minute Reliability)

- **10-Minute Repeated-Match Smoke Test (`--smoke-10m`)**: **PASS** (single measured run).
  - **Source Commit**: `154ae9147ee1f8606ff5c323b1b710ed3b421b14`
  - **Actual Duration**: 600.04s (600,038 ms; target: 600s, zero shortfall).
  - **Completed Matches**: 13,271 matches (13,271 planned resets, 0 unexpected disconnects).
  - **Request Volume**: 7,365,405 requests (100% successful; 0 transport failures, 0 correctness failures).
  - **Throughput**: 12,274.9 req/s.
  - **Round-Trip Latency**: p50 = 0.057 ms, p95 = 0.093 ms, p99 = 0.139 ms, mean = 0.062 ms, min = 0.008 ms, max = 24.965 ms (p95 meets provisional ≤100 ms guidance).
  - **Resources**: Peak RSS = 3,928 kB, Server CPU = 301,070.0 ms, Open FDs = 8.
  - **Report File**: [`docs/load_test_64p_smoke_10m_results.json`](load_test_64p_smoke_10m_results.json).

- **30-Minute Repeated-Match Reliability Test (`--reliability-30m`)**: **PASS** (single measured run).
  - **Source Commit**: `154ae9147ee1f8606ff5c323b1b710ed3b421b14`
  - **Actual Duration**: 1800.11s (1,800,110 ms; target: 1800s, zero shortfall).
  - **Completed Matches**: 32,118 matches (32,118 planned resets, 0 unexpected disconnects).
  - **Request Volume**: 17,825,490 requests (100% successful; 0 transport failures, 0 correctness failures).
  - **Throughput**: 9,902.4 req/s.
  - **Round-Trip Latency**: p50 = 0.064 ms, p95 = 0.131 ms, p99 = 0.212 ms, mean = 0.078 ms, min = 0.008 ms, max = 195.422 ms (p95 meets provisional ≤100 ms guidance).
  - **Resources**: Peak RSS = 3,940 kB, Server CPU = 894,870.0 ms, Open FDs = 8.
  - **Leak / Degradation Analysis**: Flat memory (+12 kB RSS increase from smoke to 30m reliability across >10.4M additional requests; exactly 8 open FDs maintained throughout) confirms no memory leaks, file-descriptor leaks, or event-loop degradation.
  - **Report File**: [`docs/load_test_64p_reliability_30m_results.json`](load_test_64p_reliability_30m_results.json).

- **L6 (Four-Match 256-Player Capacity & Sustained Reliability)**: **PASS** (functional concurrent capacity, 10-minute smoke soak, and 30-minute reliability soak verified; see dedicated L6 sections below).

> [!IMPORTANT]
> Single-match 64-player capacity (L4) and four-match 256-player concurrent capacity (L6) are verified across both functional benchmarks (3 repeatable runs) and sustained soak workloads (one 10-minute smoke run and one 30-minute reliability run each on a persistent server process). Multi-run soak repetitions (e.g. three 10m or three 30m runs) were not executed and are not claimed.

## L6: Four-Match Concurrent Capacity Test (256 Players, 4 Matches, 4 Independent Moderators)

- **Status**: **PASS** (reproducible functional capacity benchmark; sustained long-duration L6 smoke and reliability runs verified separately below).
- **Dedicated Machine-Readable Report**: [`docs/load_test_l6_results.json`](load_test_l6_results.json)
- **Source Commit**: `6f6d748b49d7a5337c4e67f270d8d6307fff0526` (clean checkout, `git_dirty: false`)
- **Build & Compiler**: Linux Release build (`-DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON`), GCC 11.4.0 (`Linux 6.18.54.1-microsoft-standard-WSL2 x86_64`, 11th Gen Intel(R) Core(TM) i5-1155G7 @ 2.50GHz).
- **Execution Command**: `./build/darkpool_load_tests_l6 ./build/darkpool_server --runs 3 --out docs/load_test_l6_results.json`
- **Automated CTest Targets**:
  - `darkpool_load_l6_functional`: Runs full 4-match 256-player concurrent functional workload (**PASS**).
  - `darkpool_load_l6_regression`: Runs regression self-tests covering 4 independent moderators, 64/64 full match capacity, negative cross-moderator unauthorized command rejection, negative cross-match player access rejection, conclusive 5th moderator rejection (prompt EOF / explicit error), sibling match isolation, assertion failure propagation, outcome mismatch propagation, semantic failure propagation, over-capacity connection acceptance failure propagation (exercised via controlled mock server), stalled capacity connection timeout failure propagation (exercised via controlled mock server), connection-establishment failure propagation, unreachable server binary failure handling, post-cleanup FD leak failure propagation, premature server exit during FD cleanup failure handling, dead server PID rejection, process exiting during settle polling classification, FD sampling failure while process remains alive classification, sustained status classification boundary validation, sustained injected-failure nonzero exit propagation, persistent-server short sustained cycle execution, exactly-once failure aggregation validation across all outcome variants, and unwritable output path failure handling (**PASS**; 19/19 sub-tests pass).
  - `darkpool_load_l6_sustained_short`: Fast automated sustained cycle integration test executing 5 cycles (20 matches) on a persistent server process with zero failures and settled descriptor verification (**PASS**).

### Concurrency and Topology Contract
- **Concurrent Matches**: 4 independent matches hosted simultaneously on one server process with measured overlapping lifetimes.
- **Client Count**:
  - 256 active player clients (64 players per match across 4 matches).
  - 4 independent authenticated moderator connections (1 dedicated moderator per match).
  - **Total Connected Clients**: 260 concurrent connections.
- **Production Server Constraint**:
  - `src/server_main.cpp` enforces `constexpr std::size_t kMaxMatches = 4;`, `constexpr std::size_t kMaxModerators = kMaxMatches;` (4), and `constexpr std::size_t kMaxConnections = 256 + 4 = 260;`.
  - Enforces match ownership: each match is controlled exclusively by its own authenticated moderator session. A moderator attempting to issue commands on another moderator's match receives an unauthorized error.
  - Conclusive capacity enforcement:
    - Attempting a 5th moderator connection beyond 4 is deterministically rejected at connection handshake via prompt EOF (`recv == 0` from server `close()`) or explicit rejection frame; connection timeouts (`EAGAIN`/`EWOULDBLOCK`) and stalled sockets fail the test (standalone capacity assertion, not timed application request metric).
    - Attempting a 65th player in a match is deterministically rejected at the coordinator level, with explicit connect verification before join verification (tracked as a timed request expected rejection).
    - Attempting a 261st connection beyond 260 is deterministically rejected via prompt EOF or error frame; sockets remaining open without closure or response fail the test (standalone capacity assertion, not timed application request metric).
- **Concurrency & Overlap Evidence**: Per-match lifecycle timestamps (`lifecycle_start_ms` and `lifecycle_end_ms`) are recorded. Overlap is derived mathematically (`max(start) < min(end)`); all 4 matches overlap across all runs (e.g., 495 ms, 616 ms, and 415 ms measured overlap).

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
  - Expected negative rejections in timed requests: 36 requests across 3 runs (12 per run):
    - 4 employee insider hint rejections (1 per match in Round 1).
    - 4 employee clue rejections (1 per match in Round 1).
    - 2 cross-moderator unauthorized command rejections (Moderator 0 attempting to command Match 1, and Moderator 1 attempting to command Match 0).
    - 1 cross-match unauthorized player vote rejection (Player in Match 0 attempting to vote in Match 1).
    - 1 excess 65th player join rejection ("match is full" on Match 0).
    *(Note: The separate 5th-moderator and 261st-connection capacity checks verify socket-level handshake rejection/closure and are tracked as connection capacity assertions rather than timed application request metrics).*
- **Failures & Errors**:
  - Transport failures: 0
  - Correctness failures: 0
  - Unexpected disconnects: 0 (instrumented from socket events)
  - Timeouts: 0 (instrumented from socket events)
- **Elapsed Run Duration & Throughput**:
  - Run 1: 587 ms (3,707.0 req/s, 495 ms measured overlap)
  - Run 2: 682 ms (3,190.6 req/s, 616 ms measured overlap)
  - Run 3: 535 ms (4,067.3 req/s, 415 ms measured overlap)
  - Mean Run Duration: 601.3 ms | Mean Throughput: 3,655.0 req/s.
- **Latency Percentiles (3-Run Pooled Across 6,528 Requests)**:
  - **Sequential Phase (Rounds 1 & 3)**: p50 = 0.137 ms, p95 = 0.325 ms, p99 = 1.047 ms, mean = 0.185 ms.
  - **Concurrent Burst Phase (Round 2, 256 Simultaneous Client Threads)**: p50 = 14.391 ms, p95 = 22.307 ms, p99 = 29.324 ms, mean = 14.123 ms.
  - **Total Overall**: p50 = 0.159 ms, p95 = 19.543 ms, p99 = 22.565 ms, mean = 3.464 ms.
- **Resource Measurements & Settle Dynamics**:
  - Baseline Open FDs: 9 (sampled at zero client load).
  - Peak Open FDs: 268 (sampled with all 260 client connections active: 4 moderators + 256 players).
  - Immediate Post-Close Open FDs: 76 – 171 (sampled immediately following concurrent client close while epoll unregisters pending disconnects).
  - Settled Post-Cleanup Open FDs: 8 (settled within 10 ms across all runs; acceptance criterion is `post_cleanup_fds <= baseline + 2` tolerance, i.e. <= 11 descriptors; the observed counts are distinct: baseline of 9 descriptors and settled count of 8 descriptors; `post_cleanup_settled: true`).
  - Peak Server RSS: 4,236 kB (~4.2 MB across server daemon lifetime).
  - Total Server CPU Time: 700.0 ms across 3 runs.

### Measurement Boundaries and Limitations
1. **Measurement Boundary**: Request latency is measured via monotonic clock (`std::chrono::steady_clock`) from frame write initiation until complete correlated response frame deserialization and validation. Excludes harness coordination pauses.
2. **Concurrency Scope**: Tests 256 simultaneous player clients across 4 matches with burst concurrency in Round 2 and 4 independent moderator connections.
3. **FD Settle Dynamics**: Immediate post-close FD count captures the transient state before the server epoll loop drains pending connection closes (epoll batch size 16); bounded polling confirms all 260 client sockets are closed and the process settles at 8 open descriptors within 10 ms, fully satisfying the baseline + 2 tolerance threshold (baseline = 9 descriptors, settled = 8 descriptors; note these are distinct counts, not equal counts).
4. **Sustained / Soak Testing**: Functional capacity benchmark is complemented by single measured runs for L6 sustained workloads: 10-minute smoke test (2,661 cycles, 10,644 matches, 0 failures) and 30-minute reliability test (8,934 cycles, 35,736 matches, 0 failures) on a persistent server process. Multi-run soak repetitions (e.g., three 30m runs) were not executed.

### L6 Sustained Workload Stages (10-Minute Smoke & 30-Minute Reliability)

- **10-Minute Repeated-Cycle Smoke Test (`--smoke-10m`)**: **PASS** (single measured run on persistent server).
  - **Source Commit**: `a443d0a349a6ca8436fa9ce3751ae1ac3f7803ec` (`git_dirty: false`)
  - **Actual Duration**: 600.19s (600,194 ms; target: 600s, zero shortfall).
  - **Completed Cycles & Matches**: 2,661 cycles (10,644 matches across 4 concurrent matches per cycle; 0 unexpected disconnects, 0 match limit retries).
  - **Request Volume**: 5,790,336 requests (5,758,404 successful operations, 31,932 expected negative protocol rejections; 0 transport failures, 0 correctness failures).
  - **Throughput**: 9,647.4 req/s (denominator: actual elapsed duration in seconds).
  - **Round-Trip Latency**: Sequential p50 = 0.07 ms, p95 = 0.14 ms, p99 = 0.24 ms; Concurrent Burst p50 = 8.68 ms, p95 = 12.80 ms, p99 = 16.61 ms; Total p50 = 0.08 ms, p95 = 10.32 ms, p99 = 13.12 ms, mean = 2.14 ms (p95 meets provisional ≤100 ms guidance).
  - **Resources**: Initial Current RSS (`VmRSS`) = 3,580 kB, End Current RSS (`VmRSS`) = 4,276 kB, Peak Process High-Water Mark (`VmHWM`) = 4,296 kB, Server CPU = 308,810.0 ms, Open FDs = 8 settled (100% of 2,661 cycles settled within `baseline + 2` tolerance).
  - **Telemetry & Evidence**: Full cycle-level records are preserved in [`docs/evidence/load_test_l6_smoke_10m_cycle_evidence.jsonl`](evidence/load_test_l6_smoke_10m_cycle_evidence.jsonl) (2,661 records, SHA-256: `cd5adfe41bddee922215b1ff1072b221a692c9c671af5f9580b1d35f5c1aafb3`). Per-cycle `VmRSS` series: mean 4,263.7 kB; median 4,264 kB; range 4,212–4,296 kB; explicit FD cleanup records verified across all 2,661 cycles.
  - **Summary Report**: [`docs/load_test_l6_smoke_10m_results.json`](load_test_l6_smoke_10m_results.json).

- **30-Minute Repeated-Cycle Reliability Test (`--reliability-30m`)**: **PASS** (single measured run on persistent server).
  - **Source Commit**: `a443d0a349a6ca8436fa9ce3751ae1ac3f7803ec` (`git_dirty: false`)
  - **Actual Duration**: 1800.11s (1,800,113 ms; target: 1800s, zero shortfall).
  - **Completed Cycles & Matches**: 8,934 cycles (35,736 matches across 4 concurrent matches per cycle; 0 unexpected disconnects, 0 match limit retries).
  - **Request Volume**: 19,440,384 requests (19,333,176 successful operations, 107,208 expected negative protocol rejections; 0 transport failures, 0 correctness failures).
  - **Throughput**: 10,799.5 req/s (denominator: actual elapsed duration in seconds).
  - **Round-Trip Latency**: Sequential p50 = 0.07 ms, p95 = 0.12 ms, p99 = 0.18 ms; Concurrent Burst p50 = 8.29 ms, p95 = 11.21 ms, p99 = 13.31 ms; Total p50 = 0.08 ms, p95 = 9.44 ms, p99 = 11.39 ms, mean = 2.00 ms (p95 meets provisional ≤100 ms guidance).
  - **Resources**: Initial Current RSS (`VmRSS`) = 3,520 kB, End Current RSS (`VmRSS`) = 4,216 kB, Peak Process High-Water Mark (`VmHWM`) = 4,400 kB, Server CPU = 973,450.0 ms, Open FDs = 8 settled (100% of 8,934 cycles settled within `baseline + 2` tolerance).
  - **Memory & Resource Telemetry Analysis**: Rather than inferring memory stability by subtracting high-water marks from separate runs, the harness captures the actual per-cycle post-cleanup `VmRSS` trajectory. Across all 8,934 cycles (>19.4M requests over 30 minutes on a persistent server), post-cleanup `VmRSS` remained bounded in a narrow 4,144–4,248 kB envelope (mean 4,199.9 kB, median 4,200.0 kB; the first 50 cycles averaged 4,193.1 kB and the final 50 cycles averaged 4,202.4 kB). The process peak high-water mark (`VmHWM`) reached 4,400 kB during peak 260-connection burst load. Settled file descriptors stabilized at 8 descriptors (within baseline 9 + 2 tolerance) across 100% of completed cycles with zero descriptor leakage. While no finite workload can prove the absence of every theoretically conceivable memory leak, these observed current-RSS trajectory and descriptor-settling series demonstrate bounded steady-state memory and resource behavior under sustained multi-match load.
  - **Telemetry & Evidence**: Full cycle-level records are preserved in [`docs/evidence/load_test_l6_reliability_30m_cycle_evidence.jsonl`](evidence/load_test_l6_reliability_30m_cycle_evidence.jsonl) (8,934 records, SHA-256: `5262f9b97d226e336d313534efa12477af01c331beaba2e294e26bcc8b4c8ffc`).
  - **Summary Report**: [`docs/load_test_l6_reliability_30m_results.json`](load_test_l6_reliability_30m_results.json).

*Note: The 10-minute and 30-minute sustained results are single measured benchmark runs on a persistent server process, not repeated multi-run statistical trials.*

### Evidence Layout and Automated Validation

To maintain repository readability and prevent bloated diffs while ensuring complete cycle-level auditability, sustained benchmark telemetry is organized into two tiers:
1. **Human-Readable Summary Reports**:
   - [`docs/load_test_l6_smoke_10m_results.json`](load_test_l6_smoke_10m_results.json)
   - [`docs/load_test_l6_reliability_30m_results.json`](load_test_l6_reliability_30m_results.json)
   Contain aggregate run metrics, timing percentiles, derived RSS summary statistics (sample count, min, max, mean, median, first/last 50-cycle means), source provenance, and SHA-256 checksums of the detailed evidence files.
2. **Complete Cycle-Level Evidence**:
   - [`docs/evidence/load_test_l6_smoke_10m_cycle_evidence.jsonl`](evidence/load_test_l6_smoke_10m_cycle_evidence.jsonl) (2,661 records)
   - [`docs/evidence/load_test_l6_reliability_30m_cycle_evidence.jsonl`](evidence/load_test_l6_reliability_30m_cycle_evidence.jsonl) (8,934 records)
   Stored as one complete JSON record per line, preserving every cycle's open FD counts (peak, immediate post-close, settled), settle times, post-cleanup `VmRSS` and `VmHWM` samples, per-cycle request reconciliation, and failure reasons.

#### Running the Evidence Validator

An independent validator tool is provided in [`tools/validate_l6_sustained_evidence.py`](../tools/validate_l6_sustained_evidence.py). It parses both summary reports and full cycle evidence files, failing with a non-zero exit code if checksums mismatch, record counts disagree, cycle sequences are non-sequential, required resource samples are missing, FD cleanup thresholds (`settled <= baseline + 2`) are violated, or request counts fail to reconcile:

```bash
# Run validator against repository evidence
python tools/validate_l6_sustained_evidence.py
```

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
