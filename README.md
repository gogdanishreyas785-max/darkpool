# DARKPOOL

DARKPOOL is a C++20, terminal-based social-deduction and market game. One authoritative server manages roles, match state, trades, clues, votes, timers, and victory conditions. The moderator is a separate non-player operator.

**New to the project? Start with the [DARKPOOL User, Game, and Operator Guide](docs/user-guide.md).** It explains installation, single-machine and private-LAN TLS setup, rules, commands, token handling, troubleshooting, and the communications implementation.

## Quick links

- [User and operator guide](docs/user-guide.md) — canonical setup, game rules, commands, and troubleshooting.
- [Quickstart](docs/quickstart.md) — short first-run checklist.
- [Game rules](docs/game-rules.md) — detailed mechanics and market rules.
- [Architecture](docs/architecture.md) — current source/module architecture.
- [Protocol](docs/protocol.md) — implemented framing, commands, and phase notifications.
- [Threat model](docs/threat-model.md) — security assets, boundaries, and residual risks.
- [Test and evidence plan](docs/test-plan.md) — developer verification and performance evidence.

## Recommended setup paths

### One Linux computer

~~~bash
git clone https://github.com/gogdanishreyas785-max/darkpool.git
cd darkpool
bash scripts/setup_local.sh
~~~

Then open separate terminals for:

~~~bash
bash scripts/run_local_server.sh
bash scripts/run_local_client.sh moderator
bash scripts/run_local_client.sh player
~~~

Run the player launcher once per player seat. The moderator creates the match, and players can use `join` or `join <match_id>` without copying the token because the local launcher stores protected match tokens in the private token store.

### Multiple computers on a trusted private LAN

On the server computer, from its checkout:

~~~bash
bash scripts/setup_tls_lan.sh server
bash scripts/run_tls_server.sh
~~~

On every computer that runs a client, clone the repository and follow the provisioning and launcher steps in the [user guide](docs/user-guide.md). TLS game traffic uses port 4783. Public-Internet deployment is unsupported; port 4782 is plaintext TCP and must not be used on untrusted networks.

## Match-token provisioning and vote reporting

The launchers store protected-match tokens under `~/.darkpool/tokens` (directory mode `0700`, token files mode `0600`). For local play, use `join` or `join <match_id>` without copying the token. For LAN play, create the protected match first, then launch player clients so they can synchronize token files over the SSH connection configured during setup. Manual token entry remains available as a fallback.

Vote-resolution messages report aggregate ballot counts (submitted, explicitly abstained, missing, and eligible) without broadcasting individual ballot targets.

## Implementation at a glance

| CMake target | Purpose |
|---|---|
| `darkpool_engine` | Deterministic rules and match state. |
| `darkpool_communications` | JSON framing, stream decoding, socket primitives, TLS 1.3, client/server I/O runtime. |
| `darkpool_coordinator` | Session-to-match routing, authorization, token checks, timers, notifications. |
| `darkpool_cli` | Terminal command parsing and output formatting. |
| `darkpool_server`, `darkpool_client` | Linux server and interactive client executables. |

### Communications library: source files

The communications library is built as the CMake target `darkpool_communications`. Its key implementation files are linked below so you can open them directly in GitHub:

- [`include/darkpool/network.hpp`](include/darkpool/network.hpp) — public communication interfaces and message/stream abstractions.
- [`src/network.cpp`](src/network.cpp) — message framing, stream decoding, and output queue implementation.
- [`include/darkpool/socket_primitives.hpp`](include/darkpool/socket_primitives.hpp) — socket primitive declarations.
- [`src/socket_primitives.cpp`](src/socket_primitives.cpp) — socket primitive implementation.
- [`include/darkpool/tls_transport.hpp`](include/darkpool/tls_transport.hpp) — TLS transport interface.
- [`src/tls_transport.cpp`](src/tls_transport.cpp) — TLS transport implementation.
- [`include/darkpool/server_runtime.hpp`](include/darkpool/server_runtime.hpp) — server-side network runtime interface.
- [`include/darkpool/client_runtime.hpp`](include/darkpool/client_runtime.hpp) — client-side network runtime interface.

For the broader architecture and how these components work together, see the [communications section of the user guide](docs/user-guide.md#9-communication-library-and-code-map) and the [protocol documentation](docs/protocol.md).

## Build and tests

Setup scripts install common Ubuntu/Debian dependencies, build Release binaries with warnings-as-errors, and run a focused TLS integration test. To run the full configured CTest suite after setup:

~~~bash
ctest --test-dir build --output-on-failure
~~~

## Current limits and scope

- Match size: 3–64 active players; five by default.
- Roles are assigned and shuffled by the server. The moderator is not a player.
- Up to four concurrent matches and 256 player connections plus four moderator connections across the server.
- Phases after a published event advance on server-owned timers; selecting and publishing each round's event remains moderator-driven.
- Protected match tokens are stored in owner-only files under `~/.darkpool/tokens`. Local clients can join without copying the token; TLS LAN player launchers synchronize token files over the configured SSH account when a match already exists. See the user guide.
- Public-Internet hosting, persistent player accounts, mutual TLS for player identity, durable crash recovery, and dynamic in-process certificate reload are not currently supported.
