# DARKPOOL User, Game, and Operator Guide

This is the canonical guide for installing, starting, and playing DARKPOOL. It describes the current implementation and does not present unfinished roadmap features as supported behavior. Commands are case-insensitive. Optional arguments are shown in brackets; type help in a moderator or player terminal to display its built-in command list.

- New players and moderators: start with Quick setup.
- Rules and strategy: read How the game works.
- Commands: see the moderator and player command references.
- Developers: see Communication library and code map, then the Architecture and Protocol references.

## 1. Quick setup

DARKPOOL currently targets Linux; Ubuntu/Debian is recommended. The server uses Linux facilities including epoll and UNIX-domain sockets. A full build requires CMake 3.20+, a C++20 compiler, and OpenSSL 3.x development libraries. Setup scripts install common packages using apt and may request sudo.

Choose one topology:

| Topology | Use it when | Setup |
|---|---|---|
| One computer | Server, moderator, and every player run on the same Linux machine. Recommended for first-time users. | Option A below |
| Private LAN with TLS 1.3 | Moderator or players run on other computers on a trusted private network. | Option B below |

Do not expose DARKPOOL to the public Internet. Port 4782 is legacy plaintext TCP; for networked multiplayer use TLS on port 4783.

### Get the repository

On each participating machine, clone the repository once:

~~~bash
git clone https://github.com/gogdanishreyas785-max/darkpool.git
cd darkpool
~~~

If you already have a checkout with local changes, inspect those changes before updating; do not blindly overwrite them.

## 2. Option A: one computer (recommended)

All terminals run on the same Linux machine and clients use a UNIX-domain socket. No IP address, TLS certificates, or SSH provisioning is required.

### A. Install dependencies and build

From the repository root:

~~~bash
bash scripts/setup_local.sh
~~~

This installs common Ubuntu/Debian packages, configures a Release build with warnings treated as errors, builds the binaries, runs the TLS integration test target, and prepares the local moderator credential. For a memory-constrained virtual machine:

~~~bash
DARKPOOL_BUILD_JOBS=1 bash scripts/setup_local.sh
~~~

### B. Start the server

Open Terminal 1 and keep it running:

~~~bash
bash scripts/run_local_server.sh
~~~

### C. Start the moderator and player clients

Open Terminal 2:

~~~bash
bash scripts/run_local_client.sh moderator
~~~

Open a separate terminal for each player seat. For a default five-player game, launch five player clients:

~~~bash
bash scripts/run_local_client.sh player
~~~

The moderator is not a player seat. A match can have 3–64 players.

### D. Create and join a match

In the moderator terminal:

~~~text
create 5
~~~

A protected match is created by default. The moderator terminal prints the match ID and a generated match join token. In each player terminal, join using that ID and token:

~~~text
join 1 <MATCH_JOIN_TOKEN>
~~~

Replace the placeholder with the exact token printed for that match. Every player must join before the moderator starts the match. See [Match tokens and access control](#8-match-tokens-and-access-control) for the difference between a match join token and the moderator authentication token.

To create an open lobby without a match join token, use:

~~~text
create 5 open
~~~

An open lobby removes token-based admission for that match. Use it only when all connections to the server are trusted.

When every configured player has joined, enter in the moderator terminal:

~~~text
start
event -1
~~~

The event impact may be any integer from -2 through +2. This starts the first round's Discussion phase. Read [How the game works](#4-how-the-game-works) for the rest of the round sequence.

## 3. Option B: multiple computers over a private LAN

Use this mode when server, moderator, and/or players run on different computers. Use trusted private IPv4 networking and TLS 1.3. Each machine needs a repository checkout because the setup script builds the client or server locally.

### A. Set up the server computer

On the server machine, from the cloned repository:

~~~bash
bash scripts/setup_tls_lan.sh server
~~~

If the machine has more than one private IPv4 address, the script asks you to choose one. You can specify it explicitly:

~~~bash
bash scripts/setup_tls_lan.sh server 192.168.1.50
~~~

Replace the example address with the server's actual private address. Setup installs dependencies, builds the project, runs the TLS integration-test target, creates or reuses a private CA and IP-address certificate, prepares the moderator credential, and enables SSH for client bootstrap. The CA private key remains on the server.

Start the server and leave it running:

~~~bash
bash scripts/run_tls_server.sh
~~~

### B. Set up each client computer

Each client computer must be able to SSH to the server using a trusted Linux account. Verify the SSH host-key fingerprint the first time you connect; do not blindly accept an unexpected key.

On each player computer:

~~~bash
bash scripts/setup_tls_lan.sh client 192.168.1.50 SERVER_SSH_USER player
~~~

On the moderator computer:

~~~bash
bash scripts/setup_tls_lan.sh client 192.168.1.50 SERVER_SSH_USER moderator
~~~

Replace the server address and SSH username. If the local Linux username is also the remote SSH username, player setup can be shortened to:

~~~bash
bash scripts/setup_tls_lan.sh client 192.168.1.50
~~~

To configure a moderator using the local username, pass moderator as the second argument.

The setup script retrieves the public CA certificate over SSH. Moderator setup also retrieves the moderator authentication credential; player setup does not. Restrict SSH access to trusted users.

### C. Start clients and join the match

On the moderator computer:

~~~bash
bash scripts/run_tls_client.sh moderator
~~~

On every player computer:

~~~bash
bash scripts/run_tls_client.sh player
~~~

Create the match in the moderator terminal, then have each player join the displayed match ID using the match's join token. **The current scripts automate TLS trust and moderator setup, but do not automatically distribute a newly created match's join token to every player.** The moderator must securely share that match-specific token with intended players until automated match enrollment is implemented. Do not confuse it with the moderator authentication token; players must never receive the moderator credential.

There is no fixed number of participating computers. The server limits are four concurrent matches, 3–64 players per match, up to 256 player connections, and up to four moderator connections total.

### Network and security notes

- TLS game traffic uses TCP port 4783. SSH bootstrap uses TCP port 22.
- If UFW was already active during server setup, the script attempts to add rules scoped to the selected private subnet. Check other firewalls separately.
- The client must connect to the same IP address that appears in the server certificate's IP Subject Alternative Name (SAN). Use the server's private LAN IP, not 127.0.0.1, from another computer.
- Port 4782 is plaintext TCP and must not be used across an untrusted network.
- Public-Internet deployment, player accounts, mutual TLS, and dynamic in-process certificate reload are outside the current supported scope.

## 4. How the game works

DARKPOOL is a three-round social-deduction and market game. An **Insider** secretly undermines the company; an **Investigator** can privately investigate a suspect; **Employees** work to preserve company health and identify the Insider. The moderator is a separate non-player operator and cannot trade or vote.

### Roles and match size

Valid match sizes are 3–64 active players. Five is the default. The server determines and shuffles roles; clients cannot choose their own roles.

| Active players | Insiders | Investigators | Employees |
|---:|---:|---:|---:|
| 3 | 1 | 1 | 1 |
| 4 | 1 | 1 | 2 |
| 5 | 1 | 1 | 3 |
| 8 | 1 | 1 | 6 |
| 9 | 2 | 1 | 6 |
| 16 | 2 | 1 | 13 |
| 32 | 4 | 2 | 26 |
| 64 | 8 | 4 | 52 |

For capacity N, the role counts are ceil(N/8) Insiders, ceil(N/16) Investigators, and the remaining players as Employees. Player IDs are assigned by the server and are used by clue and vote commands.

### The three-round timeline

The server owns phase timers. The moderator chooses and publishes each round's business-event impact; after that, later phases advance automatically when their timers expire.

| Stage | Default duration | What happens |
|---|---:|---|
| Awaiting Event | Moderator action | Moderator publishes the round's event impact from -2 through +2. |
| Discussion | 180 seconds | Players discuss. The Insider can inspect the exact event impact using hint; any player can use research/scout once per match for 10 credits. |
| Trading | 120 seconds | Each active player submits one BUY, SELL, or HOLD order. Missing orders become HOLD. |
| Market resolution | On Trading expiry | The server resolves orders, updates health, price, and indicators, then enters Clue unless the match has already ended. |
| Clue | 30 seconds | Each active Investigator may use their one private clue per match. |
| Transition | 10 seconds | Automatic interval between Clue and Voting. |
| Voting | 60 seconds | Each eligible active player votes for another active player or abstains. Missing votes count as abstentions. The server resolves votes at the deadline. |
| Next round / Finished | Automatic | If no victory condition has ended the match, it advances to event selection for the next round. |

These are the default durations in the current coordinator, not player-selected settings. A moderator can also use manual phase commands below, but should not mix manual overrides with the normal timed sequence unless intending to operate phase progression manually.

### Business events, health, price, and shares

The moderator currently enters a numeric impact from -2, -1, 0, +1, or +2 with the event command. The current implementation does not include a public narrative event description or an automatic scenario deck. The impact is stored for market resolution and can be retrieved privately by the Insider using hint; it is not separately randomized on each request.

At match start, each player has 100 fictional credits and 10 shares. Company health begins at 5 (bounded 0–10), stock price at 10 (bounded 1–100), and total share supply is 20N: 10N player-held shares and 10N in reserve.

- **buy [quantity]**: buy shares at the round's opening price. Quantity defaults to 1 and must be positive. Orders are all-or-nothing; cash and reserve inventory are checked.
- **sell [quantity]**: sell shares at the round's opening price. The player must own the shares and stay below the credit cap.
- **hold**: do not trade this round.
- Each active player gets one trade action per round. Missing orders become HOLD.
- Share supply is conserved. Buys draw from the finite reserve; sells return shares to it. Sells do not expand the reserve available for same-round buy allocation.
- Company health responds to event impact and the direction of executed net trading demand. Stock price is calculated by the implemented deterministic market model using event impact, demand, health, sentiment, volatility, and liquidity, and is clamped to 1–100. Market indicators are shown on the public snapshot.

Full details are in [Game rules](game-rules.md) and [Market model proposal](market-model-proposal.md).

### Special actions

**Insider — hint**

The Insider can privately see the exact current event impact during the active round. Other players must not see or share this private information.

**Any active player — research or scout**

Costs 10 credits, is available during Discussion, and can be used once per player per match. It returns a private coarse signal: Positive, Neutral, or Negative. It does not reveal the exact impact.

**Investigator — clue <player_id>**

During Clue, the Investigator can target one other active player and privately learn whether that target is an Insider. Each Investigator has one clue per match. The result is private.

### Voting rules

Enter **vote 2** to vote to eliminate Player 2. It does not mean three votes or cast multiple votes. Use **vote abstain** or **vote 0** to abstain.

- One vote per eligible active player per round; no self-votes.
- A target must be another active player.
- An elimination requires **strictly more than half** of the players eligible when voting opened. With four eligible voters, at least three votes for the same target are required.
- If no candidate reaches that threshold, no one is eliminated.
- Explicit abstentions and missing votes do not support a candidate; a missing vote is treated as an abstention at the deadline.
- Votes are secret. The server reports resolution, not individual ballots.

The current CLI's “Votes Received” field represents votes received by an eliminated candidate; if nobody is eliminated, it can display zero even if players submitted votes. That field is not a reliable count of all ballots submitted.

### Victory conditions

- If company health reaches zero during market resolution, the Insider team wins immediately.
- If voting eliminates an Insider and no active Insiders remain while company health is positive, the Employees/Investigator team wins.
- Eliminating an Employee does not immediately end the match.
- If round three's vote resolves and at least one Insider remains active while health is above zero, the Insider team wins.
- A match ends as soon as an immediate victory condition is met.

### Public versus private information

Public information includes current phase and round, company health, stock price, market indicators, reserve, aggregate market results, and match outcomes. Private information includes each player's role, the Insider's hint, the Investigator's clue result, research signal, and individual balances/holdings. A public status snapshot does not expose all internal state.

## 5. Normal timed gameplay: moderator checklist

For a protected match, the standard operator sequence is:

1. Enter **create 5** (or another capacity from 3 to 64).
2. Share the displayed match ID and match join token privately with intended players.
3. Wait until all configured players have successfully joined.
4. Enter **start**.
5. Enter **event <impact>**, for example **event -1**, to begin Round 1.
6. The server advances Discussion → Trading → market resolution → Clue → Transition → Voting automatically.
7. After voting resolves, check **status**. If the match continues, publish the next round with **event <impact>**.
8. Repeat until a victory condition ends the match.

If using manual commands such as trading, market, voting, or resolve, you are operating phase transitions yourself. The timer-based path above is the recommended normal flow.

## 6. Moderator command reference

Enter **help** in a moderator terminal.

| Command | Meaning |
|---|---|
| **create [player_count]** | Create a protected match. Defaults to five seats; valid range 3–64. A random match join token is generated by default. |
| **create <player_count> <join_token>** | Create a protected match with a caller-supplied token. The count is required for this form; keep the token private. |
| **create [player_count] open** | Create a match without match-token admission. Use only on a trusted server. |
| **match <id>** | Select the active match context for subsequent commands. |
| **start** | Start after every configured player seat has joined. |
| **event <impact> [match_id]** | Publish event impact (-2 through +2) and start Discussion. |
| **trading [match_id]** | Manually open Trading. Normally Discussion expiry opens it automatically. |
| **market [match_id]** | Manually resolve orders. Normally Trading expiry resolves them automatically. |
| **voting [match_id]** | Manually open Voting. Normally Clue and the transition interval lead to Voting. |
| **resolve [match_id]** | Manually resolve votes. Normally Voting expiry resolves them automatically. |
| **status [match_id]** | Display public status and the latest public market snapshot. |
| **pause [match_id]** | Pause the match timer. |
| **resume [match_id]** | Resume a paused match timer. |
| **abort [match_id]** | Permanently abort the match. |
| **help** | Display moderator commands. |
| **quit / exit** | Disconnect this client. |

Command aliases accepted by the CLI include **open_trading** / **open-trading** for trading, **resolve_market** / **resolve-market** for market, **open_voting** / **open-voting** for voting, and **resolve_votes** / **resolve-votes** for resolve. Commands are case-insensitive. A moderator can control only matches assigned to that moderator session. The moderator is not a player and does not count toward the match capacity.

## 7. Player command reference

Enter **help** in a player terminal.

| Command | Meaning / restrictions |
|---|---|
| **join <match_id> <token>** | Join a protected match using its exact ID and token. For a token supplied through a supported environment variable, or for an open lobby, use **join [match_id]**. |
| **status [match_id]** | View the public board and current phase. |
| **hint** (also **intel** or **insider_hint**) | Insider only. Privately inspect current event impact. |
| **research / scout** | During Discussion: spend 10 credits for a private coarse direction signal; one use per player per match. |
| **trade BUY [quantity]** | Submit a buy order; quantity defaults to one share. |
| **trade SELL [quantity]** | Submit a sell order; quantity defaults to one share. |
| **trade HOLD** | Submit a hold/no-trade action. |
| **buy [quantity]** | Shortcut for trade BUY. |
| **sell [quantity]** | Shortcut for trade SELL. |
| **hold** | Shortcut for trade HOLD. |
| **clue / investigate <target_id>** | Investigator only, during Clue. Secretly investigate an active target; one clue per match. |
| **vote <target_id>** | During Voting, cast a vote to eliminate that player ID. |
| **vote abstain** or **vote 0** | Submit an explicit abstention. |
| **help** | Display player commands. |
| **quit / exit** | Disconnect this client. |

Quantities must be positive integers. You cannot choose your role, vote for yourself, vote for an inactive player, or target an inactive player with a clue. Typing event, trade, clue, or vote without its required argument opens an interactive prompt. Raw JSON lines beginning with an opening brace are passed directly to the server for advanced debugging; normal players should use the documented commands.

## 8. Match tokens and access control

Three different things should not be confused:

1. **TLS trust material** (the public CA certificate): lets a TLS client verify that it is connecting to the configured DARKPOOL server.
2. **Moderator authentication token** (DARKPOOL_MODERATOR_TOKEN): authenticates an operator session with moderator privileges. The LAN setup transfers it only during moderator setup; player machines must not receive it.
3. **Match join token**: a secret generated when a protected match is created. It authorizes joining that specific match while its lobby is open.

The player CLI accepts the match token directly as **join <match_id> <token>**. It also recognizes DARKPOOL_PLAYER_TOKEN and DARKPOOL_JOIN_TOKEN in the client process environment. The moderator-side create command recognizes DARKPOOL_MATCH_JOIN_TOKEN and DARKPOOL_PLAYER_TOKEN for a custom token, but normally generates a secure random token automatically.

The setup scripts **do not currently transfer a new match's join token to all player clients automatically**. The moderator must share the match ID and token through a private trusted channel. Keep it limited to intended players. Use an open lobby only if you intentionally want to disable match-token admission.

## 9. Communication library and code map

The communication library is the CMake target **darkpool_communications**, defined in CMakeLists.txt. It is not a separately downloaded package. Public headers are under include/darkpool; implementation is under src.

### Communication primitives

| File / target | Responsibility |
|---|---|
| include/darkpool/network.hpp + src/network.cpp | JSON message frames, incremental stream decoding, frame queue, output queue, message helpers. A frame is a 4-byte big-endian payload length followed by a JSON payload; maximum payload is 4 KiB. |
| include/darkpool/socket_primitives.hpp + src/socket_primitives.cpp | Create/connect TCP and UNIX-domain sockets, validate allowed bind addresses, and manage UNIX socket lifecycle. |
| include/darkpool/tls_transport.hpp + src/tls_transport.cpp | OpenSSL TLS 1.3 contexts and nonblocking handshake/read/write helpers; verifies certificate validity and IP SAN using CA trust or server fingerprint pinning. Linux build only. |
| include/darkpool/server_runtime.hpp + src/server_runtime.cpp | Server event loop, Linux epoll readiness, connection/session lifecycle, rate limiting, bounded per-connection output, broadcast/multicast, and TLS integration. |
| include/darkpool/client_runtime.hpp + src/client_runtime.cpp | Client connections over UNIX sockets, TCP, or TLS; sends/receives framed messages and handles asynchronous server notifications. |
| include/darkpool/reactor_dispatch.hpp | Testable decision helper choosing which pending TLS/socket operations can resume for an epoll readiness event. |

### How a message travels

1. src/cli.cpp parses a terminal command into a JSON request such as vote or join_match.
2. ClientConnection in the communications library frames and sends the JSON request over the selected transport.
3. ServerRuntime accepts connections, processes framed messages with nonblocking I/O, applies connection/rate limits, and invokes the registered message handler.
4. src/server_main.cpp authenticates the connection and routes the request to MatchCoordinator.
5. src/match_coordinator.cpp checks session permissions, match membership, token, phase, and action authorization, then invokes the game engine.
6. src/game_engine.cpp applies authoritative game rules. The coordinator sends a response and routes private/public notifications to the correct audience.
7. The client decodes the response and src/cli.cpp formats it for the terminal.

Framing matters because TCP and TLS are byte streams, not message-oriented transports: a read may contain part of one frame, one frame, or several frames. FrameDecoder buffers incomplete data and emits complete JSON frames. OutputQueue handles partial nonblocking writes and bounds queued output to 64 KiB per connection.

TLS protects network traffic and verifies server identity; it does not replace match-level authorization. Match and moderator tokens have different purposes. Plaintext TCP on port 4782 is unsuitable for untrusted networks.

### Other important source files

| File / target | Responsibility |
|---|---|
| src/game_engine.cpp, include/darkpool/game_engine.hpp | Deterministic state, roles, events, trading, clues, votes, and victory rules. No sockets or wall-clock timer ownership. |
| src/match_coordinator.cpp, include/darkpool/match_coordinator.hpp | Match/session routing, join-token enforcement, phase/timer progression, authorization, and audience-filtered notifications. |
| src/cli.cpp, include/darkpool/cli.hpp | Human command parsing and output formatting. |
| src/server_main.cpp | Server configuration, startup, listeners, moderator authentication, and runtime wiring. |
| src/client_main.cpp | Executable argument parsing and client startup for UNIX, TCP, or TLS connections. |
| CMakeLists.txt | Builds engine, communications, coordinator, CLI, server/client executables, and tests. |

## 10. Build and test manually

The setup scripts are recommended for first-time use. For a manual Release build on Ubuntu/Debian:

~~~bash
sudo apt-get update
sudo apt-get install -y build-essential cmake git openssl libssl-dev python3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~

The server/client executable targets require Linux because they use epoll, signalfd, and UNIX-domain sockets. CTest includes unit, coordinator, CLI, socket, gameplay integration, timeout, authentication, phase-timer, TLS integration, and other regression tests. Setup scripts run a focused TLS integration target; that is not a physical multi-computer LAN test.

For deeper implementation and design details, see Architecture, Protocol, Threat model, and Test/evidence plan. The test/evidence plan is a developer verification record, not a required player setup procedure.

## 11. Troubleshooting

| Symptom | Likely cause and action |
|---|---|
| cmake or compiler missing | Run the appropriate setup script, or install packages listed in Build and test manually. |
| Permission denied running a script | Invoke it with bash scripts/name.sh; executable-bit settings are not required for that form. |
| Server says it requires Linux | The network server requires Linux. Use an Ubuntu/Debian VM or WSL2 with the Linux toolchain and required networking features. |
| Connection refused | Start the server first, confirm it is still running, then check address and port. Local clients should use the local launcher; remote clients need the server's reachable private IP. |
| TLS certificate or identity mismatch | The client address may not match the IP SAN or the CA may be wrong. Rerun setup with the actual server IP, then rerun client setup. Do not disable certificate verification. |
| SSH bootstrap fails | Confirm SSH is running, the remote username is correct, the account can log in, TCP 22 is allowed from the private subnet, and the SSH host key is expected. |
| Invalid or missing match join token | For a protected match, use the token printed when that match was created: join <match_id> <token>. TLS setup does not distribute the match token to players automatically. Open lobbies do not require it. |
| Match is not joinable | A match can only be joined while it remains in Lobby. The moderator must create a new match if the game has already started. |
| Start fails or still waiting for players | Every configured seat must be joined. The moderator does not count as a player; verify each player's MATCH JOINED response and use status. |
| Command rejected for wrong phase | Use status to inspect the current phase. Submit trades during Trading, clues during Clue, and votes during Voting. |
| Votes Received is zero when nobody was eliminated | That field reports votes received by an eliminated candidate, not all submitted ballots. A no-elimination result can display zero even if some votes were accepted. |
| Address already in use | A server may already be listening on the port/socket. Stop the prior process or use an unused configured port. Do not delete a socket path until you confirm no server is using it. |
| Setup/build exhausts VM memory | Run setup with DARKPOOL_BUILD_JOBS=1 to build with one job. |

For an uncovered issue, capture the exact command, full error text, transport mode, and relevant server-side output. Never post live moderator or match tokens publicly.
