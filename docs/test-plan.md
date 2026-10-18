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
- **Load-harness regressions:** harness self-tests and short functional workloads included in CTest.

The canonical list of executable test targets is in CMakeLists.txt. To see the names available in a configured build:

~~~bash
ctest --test-dir build -N
~~~

To rerun a specific suite, use the exact registered name shown by CTest, for example:

~~~bash
ctest --test-dir build --output-on-failure -R '^darkpool_tls_integration_tests$'
~~~

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

