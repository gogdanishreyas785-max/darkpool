# DARKPOOL Test and Verification Guide

This page describes how a developer can build the current checkout and run the automated test suite. It is not a player setup guide; use the [User, Game, and Operator Guide](user-guide.md) for installation, gameplay, commands, and troubleshooting.

## Standard verification

From the repository root on Ubuntu/Debian Linux:

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~

For a memory-constrained virtual machine, reduce build parallelism:

~~~bash
cmake --build build --parallel 1
~~~

A passing full CTest run is the default verification gate for a code change. Do not report a pass unless the commands actually completed successfully.

## What the automated suite covers

The CMake configuration defines tests for these areas:

- **Engine:** role-count boundaries, initial state, trading, reserve/supply invariants, clues, voting, victory precedence, and deterministic behavior.
- **Communications primitives:** JSON framing, fragmented/coalesced stream decoding, bounded output queues, socket lifecycle, and protocol errors.
- **Coordinator and CLI:** match creation/joining, admission-token enforcement, permission boundaries, phase transitions, command parsing, and result formatting.
- **Server integration:** UNIX-domain sockets, TCP sessions, authentication, gameplay flows, variable-size matches, notifications, timers/deadlines, late actions, and malformed input.
- **TLS integration:** OpenSSL TLS 1.3 handshakes, certificate trust and identity validation, transport I/O, and negative security cases.
- **Reactor dispatch:** readiness decisions for nonblocking socket and TLS operations.
- **Local POSIX MQ control:** actual server and CLI processes, `ping`/aggregate `status`/graceful `shutdown`, request correlation and duplicate handling, concurrent clients, queue limits/priority/FIFO, malformed requests and reply destinations, permissions, queue-full/empty behavior, cleanup, and continued gameplay socket responsiveness.
- **Local POSIX shared memory telemetry:** shared-memory initialization ordering, robust mutex synchronization, CRC32 snapshot integrity verification, observer recovery on owner death, `darkpool_watch` `--once` and `--follow` modes, observer mapping survival across publisher shutdown (IPC-4), pipe-coordinated concurrency tests verifying bounded lock acquisition and mutex preservation on lock timeouts, signal-based process termination (SIGTERM), and complete resource cleanup.
- **Asynchronous concurrency primitives (IPC-5):** `EventNotifier` nonblocking operations, counter accumulation and coalescing, maximum counter boundary (`UINT64_MAX - 1`) and `Saturated` overflow handling, multi-producer thread safety, `poll()` readiness notification, RAII descriptor lifecycle and move semantics; `BoundedMpmcQueue` empty/full boundaries, single-producer single-consumer FIFO ordering, wraparound over thousands of cycles, deterministic pending-head stalled-producer availability verification, concurrent multi-producer multi-consumer consistency (every item consumed exactly once without loss or duplication, bounded execution with monotonic deadlines), and safe destruction with queued elements; combined queue and eventfd notification integration ensuring queue contents are authoritative.
- **Load-harness regressions:** harness self-tests and short functional workloads included in CTest.

The canonical list of executable test targets is in CMakeLists.txt. To see the names available in a configured build:

~~~bash
ctest --test-dir build -N
~~~

To rerun a specific suite, use the exact registered name shown by CTest, for example:

~~~bash
ctest --test-dir build --output-on-failure -R '^darkpool_tls_integration_tests$'
~~~

The Linux-only local control-channel suite can be run independently with:

~~~bash
ctest --test-dir build --output-on-failure -R '^darkpool_posix_mq_tests$'
~~~

The Linux-only local shared memory telemetry suite can be run independently with:

~~~bash
ctest --test-dir build --output-on-failure -R '^darkpool_posix_shm_tests$'
~~~

The Linux-only asynchronous concurrency primitives suite can be run independently with:

~~~bash
ctest --test-dir build --output-on-failure -R '^darkpool_ipc_async_tests$'
~~~

### Microbenchmarks: Asynchronous IPC primitives (IPC-6)

The standalone executable `darkpool_ipc_benchmark` benchmarks `BoundedMpmcQueue` and `EventNotifier` against canonical baseline implementations (`BoundedMutexQueue` using a circular `std::vector` and nonblocking `pipe`). It is deliberately excluded from `CTest` so standard regression testing remains non-intrusive and fast.

To run the benchmark suite manually:

~~~bash
# Run all benchmark families with default workload sizes (human-readable table)
./build/darkpool_ipc_benchmark

# Run focused workloads with custom item counts and queue capacity
./build/darkpool_ipc_benchmark --benchmark=queue --items=200000 --capacity=1024
./build/darkpool_ipc_benchmark --benchmark=notify --items=100000

# Export results to JSON
./build/darkpool_ipc_benchmark --json-out=docs/ipc_benchmark_results.json
~~~

The benchmark reports both successfully transferred items per second (`transferred_items_per_sec`) and total attempted queue operations per second (`total_ops_per_sec`), elapsed time, failed push/pop attempts (contention retries), read/write syscall counts where measurable, and average roundtrip latency (`avg_roundtrip_latency_ns`) along with estimated half-RTT (`estimated_half_rtt_latency_ns`). Every queue run explicitly validates that 100% of generated items are consumed without loss or duplication. Machine-readable JSON captures system and compiler metadata alongside workload metrics. Note that recorded benchmark runs reflect the specific execution environment (e.g. WSL2) and do not represent production server capacity or general production network guarantees.

It launches the real server and `darkpool_ctl`, as well as direct queue requests for malformed and boundary cases. It verifies orderly queue/socket cleanup and partial-startup cleanup. POSIX MQ descriptor polling through `epoll` is specific to the supported Linux runtime; the non-Linux build does not include this control target.

## Benchmark evidence

Historical load-test outputs are retained as machine-readable JSON under docs/ (files named load_test_*_results.json). Treat those as recorded evidence for their recorded source revision and environment, not as proof that a different checkout or machine has the same performance. A benchmark result does not replace the current build and regression suite.

Routine setup scripts run the focused TLS integration test. They do not run a long soak workload as part of first-time user setup.

## Adding a regression test

When fixing a bug:

1. Reproduce it with the smallest failing automated test possible.
2. Add regression coverage at the lowest layer that demonstrates the defect (engine, coordinator/protocol, or CLI), and add an integration case when the boundary between processes is involved.
3. Run the focused test while iterating.
4. Run the full CTest suite before reporting completion.
5. State which checks were executed, which failed, and any environment limitations. Do not replace an unrun test with a claim that it passed.

## Platform boundary

The deterministic game engine, communications library components that do not require Linux-specific APIs, coordinator, and CLI have separate CMake targets. The runnable server/client executables and their socket/TLS integration suite require Linux facilities including epoll, signalfd, and UNIX-domain sockets. The supported setup path is documented in the User Guide.

## LAN-TLS-64-MH-1: 64-player multi-computer TLS acceptance

This acceptance procedure validates a protected 64-seat match across separate Linux computers. It complements the loopback TLS and load tests; those tests do not establish multi-host capacity. Record physical execution as **NOT RUN** unless separate computers actually completed the procedure.

### Topology and prerequisites

- One Linux DARKPOOL server computer, reachable only on the trusted private LAN at TCP 4783; SSH/SCP on TCP 22 is used for existing token provisioning.
- One separate Linux player computer with the repository, `build/darkpool_client`, player TLS setup, and enough local process/file-descriptor capacity for 64 client processes.
- A moderator CLI session on a separate moderator computer (or a separate terminal on the server computer). It remains a non-player session and keeps the moderator credential on that host.
- All machines run the same recorded commit. The player host must not have `moderator.token` or `DARKPOOL_MODERATOR_TOKEN`.
- Default player CLI phase timeouts are retained; the operator should advance the match promptly after each prompt.

### Commands and expected results

On the server computer, configure and start the existing TLS listener:

~~~bash
bash scripts/setup_tls_lan.sh server SERVER_LAN_IPV4
bash scripts/run_tls_server.sh
~~~

On the moderator computer, configure the moderator role and run the normal moderator CLI:

~~~bash
bash scripts/setup_tls_lan.sh client SERVER_LAN_IPV4 SERVER_SSH_USER moderator
bash scripts/run_tls_client.sh moderator
~~~

Enter `create 64`. Record the non-secret match ID. Do not copy the moderator credential to the player host. On the player computer, configure only the player role and build the client if needed:

~~~bash
bash scripts/setup_tls_lan.sh client SERVER_LAN_IPV4 SERVER_SSH_USER player
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
python3 tools/tls_lan_64_acceptance.py --match-id MATCH_ID
~~~

The harness synchronizes the existing protected-match token once over the configured SSH/SCP path, then starts 64 instances of the actual `darkpool_client` with TLS 1.3 and CA verification. It asks the operator to enter each moderator command and press Enter after it is accepted. Follow these commands in order when prompted:

1. `start`
2. For each round (1 through 3): `event 0`, `trading`, `market`

The coordinator automatically enters Clue after `market`, starts a transition interval after the Clue timer, opens Voting, and resolves votes when the Voting timer expires. The harness therefore does not issue `voting` or `resolve`; those commands remain available for the normal moderator CLI but are not part of this timed acceptance procedure. The harness submits HOLD and abstain actions from all 64 player CLIs. In round 1 it sends `hint` from an Insider and a clue from an Investigator targeting an Employee. A per-connection `status` response is used only after the server's ordered response/notification processing has been established; the harness checks the complete notification sequence for the current phase and rejects stale or duplicate events. It does not record response contents. All votes abstain, so the expected final outcome is `InsiderWin` if the match reaches round 3 with positive health.

Expected checkpoints:

- TLS connections succeed with the configured CA and the server identity check; no plaintext fallback is attempted.
- 64 distinct player IDs join the requested protected match. The current documented 64-player role policy yields 8 Insiders, 4 Investigators, and 52 Employees.
- The moderator remains outside those 64 player IDs.
- Each public phase/market/vote result reaches the player sessions. Hint and clue private markers reach only their authorized recipient.
- All three rounds resolve and every player CLI receives `InsiderWin`.
- The harness exits every player process in its `finally` cleanup, including on admission, command, or timeout failure. A failure names the stage and affected session without printing client output, tokens, roles, or clue contents.
- After the harness reports player cleanup, the operator stops the server orderly (`Ctrl+C`, or `sudo systemctl stop darkpool` when installed as a systemd service), verifies the TCP listener is closed, and separately verifies that the server process/service terminated (`systemctl is-active darkpool.service` must be inactive, or the foreground server PID must no longer exist). The harness does not stop the remote server itself. Player-process cleanup, listener closure, and server-process termination are separate checks.

### Failure-path checks and evidence record

The runner has bounded connection, admission, phase, and action waits. Before a physical acceptance, verify failure cleanup in a controlled test match. After the protected match token is synchronized, the following diagnostic overrides can be used; they do not change TLS verification or production server settings:

~~~bash
# Unreachable private-LAN test address; token sync still uses the configured server.
python3 tools/tls_lan_64_acceptance.py --match-id MATCH_ID --server-address 192.0.2.1 --timeout 5

MATCH_ID=123
# Reproducible untrusted CA and invalid-admission-credential checks.
diagnostic_dir="$(mktemp -d)"
trap 'rm -rf -- "$diagnostic_dir"' EXIT
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout "$diagnostic_dir/untrusted-ca.key" \
  -out "$diagnostic_dir/untrusted-ca.pem" \
  -days 1 -subj '/CN=DARKPOOL diagnostic untrusted CA' >/dev/null 2>&1
chmod 600 "$diagnostic_dir/untrusted-ca.key" "$diagnostic_dir/untrusted-ca.pem"
python3 tools/tls_lan_64_acceptance.py --match-id "$MATCH_ID" \
  --ca-cert "$diagnostic_dir/untrusted-ca.pem" --timeout 5

bad_token_dir="$diagnostic_dir/darkpool-bad-token"
install -d -m 700 "$bad_token_dir"
printf 'invalid-test-token\n' > "$bad_token_dir/match_${MATCH_ID}.token"
chmod 600 "$bad_token_dir/match_${MATCH_ID}.token"
python3 tools/tls_lan_64_acceptance.py --match-id "$MATCH_ID" \
  --token-dir "$bad_token_dir" --skip-token-sync --timeout 5
~~~

An unreachable endpoint and untrusted CA must fail during TLS connection within the timeout; the invalid token file is explicitly passed through `--token-dir` and must fail with a session-specific admission rejection. Each attempt must stop any client processes it created, and the `trap` removes all diagnostic files. A missing SSH token sync must fail before any client starts. To check a client process that exits during admission or a later phase, terminate one player CLI during that stage in a disposable run; the harness must name that session and clean up the rest. Do not alter the production CA or expose a live token for these checks. Record each attempted case; unrun cases remain **NOT RUN**.

Record results using this template:

~~~text
Test ID: LAN-TLS-64-MH-1
Status: NOT RUN | PASS | FAIL | BLOCKED
Server computer / OS:
Player computer / OS:
Moderator computer / OS:
Repository commit:
TLS server identity / CA verification: NOT RUN | PASS | FAIL
Connected and admitted player sessions: ____ / 64
Role counts (Insider / Investigator / Employee): ____ / ____ / ____
Moderator separate from player seats: NOT RUN | PASS | FAIL
Public phase/market/vote notifications: NOT RUN | PASS | FAIL
Private hint/clue authorization: NOT RUN | PASS | FAIL
Three-round outcome: NOT RUN | PASS | FAIL (expected InsiderWin)
Client cleanup / server orderly shutdown: NOT RUN | PASS | FAIL
Failure-path checks and diagnostics:
Evidence location (exclude tokens, private roles/clues, and credentials):
~~~

Historical loopback load results and five-player LAN records remain separate. Do not use a same-host 64-player result as evidence for this multi-computer acceptance.
