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

### 3.5 Nonblocking, asynchronous I/O with epoll

DARKPOOL uses **event-driven, nonblocking asynchronous I/O** based on Linux `epoll`. The server does not wait synchronously for each client operation to finish and does not create a dedicated thread for every player. Instead, it waits for readiness notifications and advances a connection when the relevant socket operation can make progress.

This is **readiness-driven asynchronous I/O**: `epoll` reports that a descriptor is ready for reading or writing; the runtime then performs the corresponding nonblocking socket or TLS operation. An operation may transfer only part of the pending data or report that it would block, in which case the runtime waits for the next relevant readiness notification. The event loop can therefore service many active and idle connections without blocking on one peer.

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

TLS adds an important detail: an OpenSSL read can need socket writability, and an OpenSSL write can need socket readability. The runtime resumes the operation according to the readiness direction requested by TLS, not merely according to whether the application intended to read or write. `include/darkpool/reactor_dispatch.hpp` provides a testable helper for these readiness decisions.

The server runtime also manages connection lifecycle, per-session request rate limits, bounded outgoing queues, and broadcast/multicast delivery.

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

The server launcher stores protected match tokens in an owner-only token directory, and the player launcher can synchronize those files through the SSH account saved during setup. This synchronization runs when the player launcher starts, so create the match first or relaunch the player client after token creation. See the [User Guide: Match tokens and access control](user-guide.md#8-match-tokens-and-access-control).

Public-Internet deployment, persistent player accounts, mutual TLS player identity, durable crash recovery, and dynamic in-process certificate reload are not currently supported. For setup instructions and troubleshooting, see the [User Guide](user-guide.md).

## 9. IPC and operating-system primitives in use

The following are the communication and operating-system primitives used by DARKPOOL's application code.

| Primitive | How DARKPOOL uses it |
|---|---|
| **Linux `epoll`** | The server monitors listeners, client sockets, TLS readiness, and the signal descriptor in one event loop. See `src/server_runtime.cpp`. |
| **Nonblocking socket I/O** | Socket and TLS operations progress when readiness events indicate they can proceed; partial transfers are handled without blocking the server on one connection. |
| **UNIX-domain stream sockets** | Provide local inter-process communication when the client and server run on the same Linux host. The stream carries length-prefixed JSON frames. |
| **TCP network sockets** | Connect clients to the server across a network. TCP provides a reliable, ordered byte stream, not encryption or application message boundaries. |
| **TLS 1.3 over TCP** | Protects remote-session bytes and validates the server's certificate identity through the configured CA or SHA-256 fingerprint method. |
| **Linux signals via `signalfd`** | `SIGINT` and `SIGTERM` are blocked and exposed as a file descriptor, then handled by the `epoll` loop. They control process shutdown rather than carrying game messages. |

### 9.1 How IPC actually works today

IPC is the exchange of data between separate processes on the **same host**. In DARKPOOL's local mode, the client process opens a UNIX-domain stream socket at the configured socket path (default `/tmp/darkpool.sock`) and the server listens on that path. The client serializes each command as JSON, adds the four-byte big-endian payload-length prefix, and writes the bytes to the socket. The server reads arbitrary chunks, uses `FrameDecoder` to reconstruct complete frames, and passes each request to the session/authentication and coordinator logic. Replies and allowed notifications travel back over the same full-duplex connection.

~~~text
Client process                                      Server process
--------------                                      --------------
CLI command
    |
JSON + 4-byte length prefix
    |
ClientConnection
    |
    +========= UNIX-domain stream socket ===========+
                                                    |
                                      ServerRuntime / epoll
                                                    |
                                           FrameDecoder
                                                    |
                                  session auth -> coordinator
                                                    |
                                              game engine
                                                    |
ClientConnection <==== framed replies/events =======+
    |
CLI displays result
~~~

The socket is the **inter-process channel**. `FrameDecoder`, `FrameStream`, and `OutputQueue` are ordinary in-process data structures: they buffer, assemble, and queue bytes but do not independently connect two processes. There is no IPC boundary between the coordinator and game engine in the current server; those are C++ components in the same process, communicating through direct function calls and in-process objects.

A UNIX-domain socket is a good fit here because it supports bidirectional request/reply and asynchronous notifications with stream semantics, and lets the operating system manage connection lifecycle. It is not interchangeable with a TCP socket on another host: a UNIX-domain socket path is local to one machine and is not routed across the LAN.

### 9.2 Network path: remote clients

For a remote client, the application uses a TCP socket to reach the server's address and port. On the TLS listener, OpenSSL negotiates TLS 1.3 before application frames are exchanged. The same length-prefixed JSON protocol is then carried inside the encrypted TLS byte stream.

~~~text
Remote client
    |
TCP connection to server IP:port
    |
TLS 1.3 handshake + server identity verification
    |
Encrypted byte stream containing framed JSON
    |
Server TLS/nonblocking I/O + epoll
    |
FrameDecoder -> session authentication -> coordinator -> engine
~~~

TCP makes bytes arrive reliably and in order while a connection remains viable, but it does not guarantee that one write equals one read, does not identify a game action, and does not encrypt traffic. Framing solves message boundaries; TLS solves in-transit confidentiality/integrity and server identity verification; the application protocol and coordinator solve message meaning and authorization. These are separate responsibilities.

### 9.3 Security model: do not confuse transport with authorization

DARKPOOL applies several distinct controls:

- **Binding and exposure:** the local socket is for same-host clients. The TCP binding code validates IPv4 bind addresses and rejects public/non-LAN addresses for the supported non-loopback mode. Use the TLS listener for remote LAN play; plaintext TCP is not suitable for untrusted networks.
- **TLS trust:** the TLS client uses the configured local CA or SHA-256 server-certificate fingerprint and checks server certificate validity and IP SAN. The client must use an address matching the certificate identity. Do not bypass certificate verification to make a connection work.
- **Session and role authorization:** successful TLS proves the server identity to the client; it does not prove that a connecting user is the moderator or has permission to perform an action.
- **Moderator authentication and match admission:** moderator credentials and protected-match join tokens serve different purposes. A join token grants match admission, not transport encryption or moderator privileges.
- **Request and resource limits:** the server limits frame payloads to 4 KiB, bounds queued output to 64 KiB per connection, applies per-session request rate limiting, and configures a connection limit. These limit memory pressure and request floods but are not substitutes for correct authentication or network-level firewall rules.
- **Private information:** the coordinator routes role-specific hints, clues, and research to the intended audience. TLS protects traffic in transit; audience filtering prevents the application from sending a private event to the wrong player in the first place.

Current limitations remain: the application does not provide durable crash recovery, and this is not a hardened public-Internet deployment design. Keep the service on trusted/private networks and follow the TLS setup instructions in the [User Guide](user-guide.md#3-option-b-multiple-computers-over-a-private-lan).

