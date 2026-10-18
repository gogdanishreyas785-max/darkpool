# DARKPOOL Architecture: Theory, Communication, and Runtime

This document explains the architecture implemented in the repository, including why each communication primitive exists and how they work together. It is not a proposed future layout. For installation, game rules, terminal commands, and troubleshooting, see the [User, Game, and Operator Guide](user-guide.md).

## 1. System overview

DARKPOOL is a Linux C++20 client/server game. Multiple player clients and a separate moderator client communicate with one authoritative server. The server owns role assignment, match membership, action validation, market and vote resolution, deadlines, and routing of public/private events.

The architecture separates five responsibilities:

1. **User interface:** converts human commands such as `vote 2` into requests and formats replies.
2. **Transport:** moves bytes between client and server using a UNIX-domain socket, TCP, or TLS over TCP.
3. **Message protocol:** identifies complete messages inside the byte stream using length-prefixed JSON frames.
4. **Coordination and authorization:** authenticates sessions, checks permissions and match membership, manages timers, and decides who may receive each notification.
5. **Game engine:** applies authoritative game rules and updates state.

This separation means the game engine does not need to know whether a command arrived through a local socket or an encrypted network connection. Conversely, TLS and socket code do not decide whether a vote is legal.

## 2. Layered architecture

~~~text
                  PLAYER / MODERATOR TERMINAL
                            |
                            v
                 src/cli.cpp: parse command
                            |
                            v
                    JSON request object
                            |
                            v
                Client runtime / ClientConnection
                            |
               +------------+-------------+
               |            |             |
               v            v             v
          UNIX socket   Plain TCP      TLS 1.3/TCP
               |            |             |
               +------------+-------------+
                            |
                            v
            Length prefix + JSON payload frame
                            |
                            v
        ServerRuntime: nonblocking I/O + epoll
                            |
                            v
      src/server_main.cpp: connection/session setup
                            |
                            v
         MatchCoordinator: permissions, match routing,
              timers, state-change orchestration
                            |
                            v
            src/game_engine.cpp: game rules/state
                            |
                            v
             response + event notifications
                            |
                 audience-aware routing
                            |
                            v
                 client runtime and CLI
~~~

Each layer has a specific contract. The client can ask for an action, but it cannot decide that the action succeeded. The server validates it and the game engine changes authoritative state only when the request is legal.

## 3. Communication primitives: theory and implementation

The CMake target **`darkpool_communications`** is defined in `CMakeLists.txt`. It is a project library, not a separate package to install. Its public headers are under `include/darkpool/`, with implementation in `src/`.

### 3.1 Sockets: the communication endpoint

A socket is an operating-system endpoint through which processes exchange bytes.

- **UNIX-domain stream socket** — connects processes on the same host through a local socket path. DARKPOOL's default local path is `/tmp/darkpool.sock`. It avoids IP routing and is the recommended option when server, moderator, and players all run on one Linux machine.
- **TCP socket** — connects endpoints using IP addresses and ports. TCP provides reliable, ordered delivery of bytes, but does not preserve application-message boundaries and does not encrypt the data.
- **TLS over TCP** — uses TCP for delivery and adds a cryptographic session above it. DARKPOOL uses TLS 1.3 through OpenSSL on port `4783` by default. Plaintext TCP uses loopback port `4782` by default and must not be treated as safe for an untrusted network.

Implemented in `include/darkpool/socket_primitives.hpp` and `src/socket_primitives.cpp`.

### 3.2 Why message framing is necessary

TCP and UNIX stream sockets expose a **stream of bytes**, not a sequence of JSON messages. If a client sends two JSON requests, the server is not guaranteed to receive them in two separate reads. Likewise, a single JSON request can arrive across several reads.

DARKPOOL therefore uses a length-prefixed frame:

~~~text
+------------------------------+----------------------------------+
| 4-byte unsigned big-endian N | N bytes of UTF-8 JSON payload    |
+------------------------------+----------------------------------+
            header                         body
~~~

The four-byte prefix stores the payload length and is not included in that length. The accepted payload range is 1–4096 bytes. For example, if the JSON body is 37 bytes long, its prefix is hexadecimal `00 00 00 25` (decimal 37), followed by the 37 JSON bytes.

This length prefix tells the receiver exactly how many bytes belong to one message. It is not a game-specific field; it is the boundary between messages in the transport stream.

Implemented in `include/darkpool/network.hpp` and `src/network.cpp`.

### 3.3 FrameDecoder: incremental stream parsing

`FrameDecoder` maintains buffered bytes between reads. Conceptually, it repeats these steps:

1. If fewer than four bytes are buffered, wait for more data.
2. Read the length prefix without consuming the body as a separate message.
3. Validate the declared length against the protocol's payload limits.
4. If the full payload has not arrived, retain the bytes and wait for another read.
5. When the complete payload is available, emit one JSON frame and continue in case more complete frames are already buffered.

Illustrative example: the sender transmits one frame containing 41 bytes of header-plus-body, but the operating system delivers it as 10 bytes, then 12 bytes, then 19 bytes. The decoder buffers those pieces and emits the JSON message only after all 41 bytes have arrived. If one read contains two full frames, it emits both rather than assuming the read contains just one message.

`FrameStream` manages decoded frames, while `OutputQueue` manages bytes waiting to be sent. The output queue is necessary because a nonblocking write can send only part of the pending data. The current queue is bounded at 64 KiB per connection to prevent an unresponsive peer from causing unlimited memory growth.

### 3.4 TLS 1.3: confidentiality and server identity

TLS is a security layer over TCP, not a replacement for framing or authorization. Its handshake establishes cryptographic session keys and validates the server identity according to the client's configured trust method. Once established, framed JSON bytes travel inside the encrypted TLS channel.

DARKPOOL's TLS implementation uses OpenSSL:

- `include/darkpool/tls_transport.hpp`
- `src/tls_transport.cpp`

The implementation configures TLS 1.3 and validates certificate properties, including validity and the server identity/IP Subject Alternative Name (SAN), with CA verification or the supported server-leaf fingerprint method. The client should connect using the IP address that matches the certificate identity. Disabling certificate verification is not a valid troubleshooting fix.

TLS does **not** decide whether a user is a moderator, whether a player has joined a particular match, or whether a vote is legal. Those checks belong to the application layer. DARKPOOL keeps the moderator authentication token separate from each match's join token.

### 3.5 Nonblocking I/O and epoll

A blocking server operation can stop a thread while it waits for a peer. DARKPOOL instead uses nonblocking I/O and Linux `epoll` to track many active connections in one event loop.

The event loop waits for readiness notifications, such as a socket becoming readable or writable, then advances the corresponding connection state. This is important when many players are connected but most are idle.

A simplified cycle is:

~~~text
epoll_wait()
    |
    v
Readiness event for connection C
    |
    +--> TLS handshake still pending? Advance handshake
    |
    +--> Incoming bytes available? Read and decode frames
    |
    +--> Complete request available? Dispatch to coordinator
    |
    +--> Output pending and socket can progress? Flush output queue
    |
    v
Return to epoll_wait()
~~~

TLS adds an important detail: an OpenSSL read can need socket writability, and an OpenSSL write can need socket readability. The runtime must resume the operation according to the readiness direction requested by TLS, not merely according to whether the application intended to read or write. `include/darkpool/reactor_dispatch.hpp` provides a testable helper for these readiness decisions.

The server runtime also manages connection lifecycle, per-session request rate limits, bounded outgoing queues, and broadcast/multicast delivery. It does not create a dedicated thread for every player.

Implemented in `include/darkpool/server_runtime.hpp` and `src/server_runtime.cpp`.

### 3.6 Client runtime

The client runtime provides the corresponding client-side transport behavior: connect using the selected transport, frame and send requests, receive and decode responses, and process asynchronous notifications. A notification can arrive independently of the direct reply to the command currently being typed, so clients must not assume that every incoming message is the reply to the most recent command.

Implemented in `include/darkpool/client_runtime.hpp` and `src/client_runtime.cpp`.

## 4. An illustrative request: `vote 2`

Suppose Player 1 enters `vote 2` in the terminal. The command travels through the following steps.

~~~text
Player types "vote 2"
       |
       v
src/cli.cpp parses command
       |
       v
JSON request (vote type, match ID, target player ID)
       |
       v
Client runtime creates a length-prefixed frame
       |
       v
UNIX socket / TCP / TLS connection
       |
       v
ServerRuntime receives bytes
       |
       v
FrameDecoder emits a complete JSON request
       |
       v
server_main.cpp identifies the authenticated session
       |
       v
MatchCoordinator checks match, phase, player status,
target, and voting permissions
       |
       v
Game engine applies the legal vote to authoritative state
       |
       v
Coordinator routes a response and allowed notifications
       |
       v
Client runtime decodes messages; CLI displays the result
~~~

If the command is malformed, arrives in the wrong phase, targets an invalid player, or comes from an unauthorized session, the server rejects it. A client message is a **request**, not proof that the action succeeded.

The coordinator is also responsible for privacy. A public market snapshot may go to all match participants, while an Insider's hint or an Investigator's clue result must be sent only to the authorized player. Encryption protects data in transit; audience-aware routing prevents the server from intentionally disclosing private game information to other clients.

## 5. Source files and their responsibilities

| File / target | Responsibility |
|---|---|
| `include/darkpool/network.hpp`, `src/network.cpp` | JSON framing, incremental decoding, frame stream, output queue, message helpers. |
| `include/darkpool/socket_primitives.hpp`, `src/socket_primitives.cpp` | TCP and UNIX-domain socket creation/connect, bind-address checks, UNIX socket lifecycle. |
| `include/darkpool/tls_transport.hpp`, `src/tls_transport.cpp` | OpenSSL TLS 1.3 setup, nonblocking handshake/read/write, certificate and server-identity verification. |
| `include/darkpool/server_runtime.hpp`, `src/server_runtime.cpp` | epoll server loop, connection lifecycle, TLS progression, rate limiting, bounded output, broadcast/multicast. |
| `include/darkpool/client_runtime.hpp`, `src/client_runtime.cpp` | Client transports, framed send/receive, asynchronous notifications. |
| `include/darkpool/reactor_dispatch.hpp` | Readiness decisions for normal and resumed socket/TLS operations. |
| `src/cli.cpp`, `include/darkpool/cli.hpp` | Terminal command parsing and output formatting. |
| `src/client_main.cpp` | Client executable setup and transport selection. |
| `src/server_main.cpp` | Server setup, listeners, session authentication, runtime wiring. |
| `src/match_coordinator.cpp`, `include/darkpool/match_coordinator.hpp` | Match/session routing, join-token enforcement, permissions, timers, phase transitions, notifications. |
| `src/game_engine.cpp`, `include/darkpool/game_engine.hpp` | Authoritative deterministic game state and rules; no socket ownership. |
| `CMakeLists.txt` | Defines the engine, communications, coordinator, CLI, server/client, and test targets. |

## 6. CMake target boundaries

| Target | Main responsibility |
|---|---|
| `darkpool_engine` | Deterministic game rules and canonical state. |
| `darkpool_communications` | Message framing, sockets, TLS, and client/server I/O components. |
| `darkpool_coordinator` | Session and match routing, action authorization, timers, and notifications. |
| `darkpool_cli` | Terminal command parsing and formatting. |
| `darkpool_server` | Linux server startup, listeners, authentication, and event-loop wiring. |
| `darkpool_client` | Interactive terminal client. |

The separation lets the game engine be tested independently from real network I/O, while communication primitives can be tested for framing, partial reads/writes, TLS behavior, and socket lifecycle without needing a full game to be played.

## 7. Authoritative game state and timers

- Matches contain 3–64 player seats, default five; the moderator is a separate connection.
- Role counts are derived from match capacity and shuffled on the server. Clients cannot supply roles or seat IDs.
- Up to four matches can coexist; configured limits allow up to 256 player connections and four moderator connections total.
- The moderator chooses the numeric business-event impact for each round. The current implementation does not include an automatic narrative event deck.
- Discussion, Trading, Clue, transition, and Voting timers drive normal phase progression after an event is published.
- Private role, hint, clue, and research information is audience-filtered. Canonical/replay state contains secrets and must not be sent to players.

## 8. Security boundaries and limitations

Treat these as separate security controls:

1. **Transport security:** TLS verifies server identity and protects bytes in transit.
2. **Moderator authentication:** the moderator credential authorizes operator-level commands.
3. **Match admission:** a protected match's join token controls entry to that match.
4. **Action authorization:** the coordinator checks the session's permissions, match membership, game phase, and requested action.

The current setup scripts do not automatically distribute each new match's join token to every player client; the moderator must securely share it with intended players. See the [User Guide: Match tokens and access control](user-guide.md#8-match-tokens-and-access-control).

Public-Internet deployment, persistent player accounts, mutual TLS player identity, durable crash recovery, and dynamic in-process certificate reload are not currently supported. For setup instructions and troubleshooting, see the [User Guide](user-guide.md).
