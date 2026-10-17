# S9 Transport Security Design and Implementation Specification (TLS 1.3)

**Status:** APPROVED S9 ARCHITECTURE AND IMPLEMENTATION SPECIFICATION
**Author:** Senior Networking & Security Architecture
**Target Milestone:** S9 (Secure Transport & Identity Layer)
**Applies to:** `src/server_main.cpp`, `src/client_main.cpp`, `include/darkpool/tls_transport.hpp`, `src/tls_transport.cpp`, `include/darkpool/network.hpp`
**Prerequisites:** S1 Deterministic Engine, S2 Match Coordinator, S3 Frame Decoding & Resource Limits, L6 Multi-Match Capacity, LAN TCP Binding (PR #26), Documentation Reconciliation (PR #27)

---

## 1. Executive Summary

The DARKPOOL server supports local UNIX-domain stream sockets (`/tmp/darkpool.sock`), loopback TCP (`127.0.0.1:4782`), and explicit private LAN TCP binding restricted to RFC 1918 private IPv4 addresses (`--bind-address`). While UNIX-domain sockets leverage filesystem DAC permissions (0700) and loopback traffic remains restricted to the local OS kernel, the LAN TCP path on port 4782 transmits all traffic as unencrypted plaintext.

On a physical or switched local network, plaintext TCP exposes sensitive application data to passive eavesdropping and active packet modification:
1. The moderator authentication token (`DARKPOOL_MODERATOR_TOKEN`) is transmitted in plaintext during connection handshake.
2. Player private roles (`Insider`, `Investigator`, `Employee`) and private investigation clues are transmitted in plaintext.
3. Player trade orders, hints, and elimination votes can be snooped or tampered with by any device sharing the local broadcast domain or Wi-Fi network.

This specification defines the **strict TLS 1.3 transport security architecture** implemented in DARKPOOL's nonblocking, epoll-driven C++20 server (`darkpool_server`) and client CLI (`darkpool_client`), backed by a dedicated transport module (`include/darkpool/tls_transport.hpp` and `src/tls_transport.cpp`). This design satisfies transport security requirements while strictly preserving:
- The deterministic, zero-dependency C++20 authoritative game engine.
- Existing match coordination, isolation rules, and audience-scoped message filtering.
- Existing framing boundaries (4 KiB maximum frame payload), output queue limits (64 KiB client buffer eviction), and token-bucket rate limiting (20 req/s, burst 40).
- The low-friction local developer workflow (UNIX domain stream sockets and loopback TCP on port 4782).
- The 260 total connection ceiling, 4-match limit, and per-connection protections.

> [!NOTE]
> S9 implementation decisions AD-S9-01 through AD-S9-04 are formally approved. TLS 1.3 is active on a dedicated secure listener (default port 4783) and strictly fails closed. Public Internet deployment, persistent user accounts, dynamic certificate reloading, and mutual TLS (mTLS) remain deferred.

---

## 2. Goals and Non-Goals

### Goals
- **Strict Transport Confidentiality & Integrity:** Ensure all network communication over the TLS endpoint (moderator authentication, player roles, private clues, trades, votes, and events) is cryptographically protected against passive interception and active in-flight tampering.
- **Strict Server Authentication:** Require client verification of server certificates, including Subject Alternative Names (SANs) for hostnames and IP addresses, with fail-closed semantics on any untrusted or mismatched certificate.
- **Dual Verification Modes:** Support both a dedicated local CA trust root and explicit SHA-256 leaf-certificate fingerprint pinning.
- **Epoll & Nonblocking Architecture Alignment:** Integrate TLS handshakes, record encryption/decryption, and I/O readiness into the single-threaded epoll reactor without blocking the event loop or introducing thread-per-client complexity.
- **Resource Boundary & Backpressure Preservation:** Maintain strict 4 KiB frame payload limits, 64 KiB client output buffer limits, and per-connection rate limiting inside the decrypted stream.
- **Strict TLS 1.3:** Enforce TLS 1.3 as the minimum and only allowed protocol version on secure endpoints, eliminating legacy cipher suite negotiation and downgrade attacks.
- **No Plaintext Fallback:** Explicitly reject plaintext connections on ports or endpoints configured for TLS. Reject TLS 1.2 or earlier handshakes.
- **Dedicated Port Topology:** Secure transport operates on dedicated port 4783 by default (AD-S9-01), avoiding dual-mode plaintext/TLS ambiguity.

### Non-Goals
- **Public ACME Automation for RFC 1918 Private LANs:** Public certificate authorities (e.g., Let's Encrypt via ACME) do not issue publicly trusted certificates for RFC 1918 private IPv4 subnets (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`). Automated ACME lifecycle is a non-goal for private LAN deployments.
- **Persistent Client Accounts or PKI Enrollment:** This milestone does not implement persistent databases, user passwords, OAuth2/OIDC, or global PKI enrollment for casual players.
- **Game Engine Mutation:** No changes to `darkpool::Match`, game rules, victory conditions, share conservation formulas, or role ratios.
- **Replacement of Application-Level Authorization:** TLS provides transport security; it does not replace server-side match membership checks, role verification, or coordinator state machine guards.
- **Dynamic In-Process Certificate Reloading:** Deferred per AD-S9-04; rotation requires a controlled server restart.
- **Mutual TLS (mTLS) for Players:** Deferred per AD-S9-02; players use server-authenticated TLS with anonymous sessions.

---

## 3. Scope and Transport Boundaries

### 3.1 Transport Topology

DARKPOOL recognizes four distinct operational transport environments:

| Transport Mode | Interface / Address | Default Port | Encryption / Security | Target Environment | Status |
|---|---|---|---|---|---|
| **Local UNIX-Domain** | `/tmp/darkpool.sock` | N/A | Filesystem DAC (mode 0700, stale socket check) | Single-host IPC, local testing | Implemented |
| **Localhost TCP** | `127.0.0.1` (loopback) | `4782` | Kernel loopback isolation; unencrypted | Single-host developer debugging | Implemented |
| **Private LAN TCP** | RFC 1918 IPv4 (`10.x`, `172.16-31.x`, `192.168.x`) | `4782` | Plaintext; restricted by IP parsing and host firewall | Dedicated multi-machine testing on trusted isolated LAN | Implemented (PR #26) |
| **Secure Private LAN TCP** | RFC 1918 IPv4 or loopback | `4783` | **TLS 1.3** with local CA or pinned leaf fingerprint | Multi-machine gameplay over LAN or Wi-Fi; secure testbed | **Implemented (S9)** |
| **Remote Internet TCP** | Public IPv4/IPv6 / FQDN | TBD | **TLS 1.3** with CA-signed certificate, FQDN SAN, firewalling | Public internet play, hosted servers | **Deferred Future Work** |

### 3.2 Transport Invariants

1. **UNIX Socket and Loopback Independence:** The local UNIX-domain socket path and localhost plaintext TCP listener remain functional without requiring TLS libraries or certificate configuration, preserving zero-overhead local development and continuous integration tests.
2. **Explicit Secure Configuration:** The server never silently downgrades a secure listener to plaintext. Configuring a TLS listener (`--tls-cert` and `--tls-key`) requires valid certificates and keys; context initialization failures fail closed immediately at server startup.
3. **Decoupled Engine Pipeline:** The protocol pipeline maintains clean architectural separation:
   $$\text{Physical Socket} \longrightarrow \text{TLS Record Layer} \longrightarrow \text{FrameDecoder (4 KiB limit)} \longrightarrow \text{RateLimiter} \longrightarrow \text{MatchCoordinator} \longrightarrow \text{GameEngine}$$
   The game engine never interacts with socket descriptors or TLS session contexts.
4. **Dedicated Secure Port (AD-S9-01):** The secure listener binds to port 4783 by default. The plaintext listener (port 4782) and secure listener (port 4783) operate on separate sockets and separate epoll events.
5. **Combined Connection Limit:** Handshake-incomplete TLS connections, established TLS connections, plaintext TCP connections, and UNIX socket connections all count against the unified `kMaxConnections = 260` ceiling.

---

## 4. Server Identity and Certificate Management

### 4.1 Approved Trust Models (AD-S9-03)

DARKPOOL implements two mutually exclusive, strictly validated client verification modes:

```
Mode 1: Dedicated Local Root CA
  +--------------------+        Signs        +----------------------+
  | Local DARKPOOL CA  | ------------------> | Server Certificate   |
  | (Root Cert PEM)    |                     | (SAN: IP:192.168.1.50)
  +--------------------+                     +----------------------+
            |                                           |
    Distributed via                                 Presented to
    --ca-cert <path>                                client in TLS
            v                                           v
  +--------------------+        Verifies     +----------------------+
  | Client Trust Store | ==================> | Client Connection    |
  +--------------------+                     +----------------------+

Mode 2: Leaf Certificate SHA-256 Fingerprint Pinning
  +----------------------+      Presented    +----------------------+
  | Server Certificate   | ----------------> | Client Connection    |
  | (Self-signed / Leaf) |                   |                      |
  +----------------------+                   +----------------------+
            |                                           |
     SHA-256 Digest                                 SHA-256 Digest
     Out-of-band pin                                Computed from leaf
            v                                           v
  +--------------------+       Matches?      +----------------------+
  | Expected 64-hex    | ==================> | Pinned Verification  |
  +--------------------+                     +----------------------+
```

#### Verification Specifications

1. **Local CA Mode (`--tls --ca-cert <ca-cert.pem>`):**
   - Loads the specified PEM-encoded CA certificate into an isolated `X509_STORE`.
   - Enables strict peer verification (`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`).
   - Sets IP SAN verification (`X509_VERIFY_PARAM_set1_ip_asc` / `X509_check_ip_asc`) using the destination IP address. Common Name (CN) fallback is rejected.
   - Enforces certificate validity period (`notBefore` and `notAfter` against current time).
   - Rejects untrusted roots, expired certificates, self-signed certificates without root trust, and mismatched IP SANs.

2. **Fingerprint-Pinning Mode (`--tls --server-fingerprint <HEX>`):**
   - Strictly validates that the provided fingerprint is exactly 64 hexadecimal characters (case-insensitive SHA-256 digest).
   - Retrieves the leaf certificate presented by the server during the handshake.
   - Calculates the SHA-256 digest of the DER-encoded leaf certificate (`X509_digest(cert, EVP_sha256(), ...)`).
   - Compares digests using constant-time evaluation; fails closed if they differ.
   - Validates that the certificate is currently within its validity period (`notBefore` <= now <= `notAfter`). OpenSSL time comparison results (`X509_cmp_current_time`) returning `0` (indicating unparseable ASN.1 timestamps or comparison errors) are explicitly rejected as invalid.
   - Verifies that the certificate IP SAN contains the destination IP address.
   - Verifies that the certificate contains appropriate server-authentication usage (`X509_check_purpose(cert, X509_PURPOSE_SSL_SERVER, 0)`).
   - Accepts both self-signed leaves and CA-issued leaves when the leaf fingerprint matches the pinned hash, without requiring the issuing CA root to be locally installed in the client's trust store.
   - Does not require a local CA file. Does not use `--insecure` or bypass certificate validity checks.

3. **Residual Revocation Limitations:**
   - In closed or offline RFC 1918 private LANs, Online Certificate Status Protocol (OCSP) and Certificate Revocation Lists (CRLs) are generally unavailable.
   - Revocation in Local CA mode is accomplished by updating or rotating the local CA root.
   - Revocation in Fingerprint mode is instantaneous upon distributing an updated fingerprint string.

### 4.2 Certificate Validation and Strict SAN Requirements

To prevent impersonation and man-in-the-middle (MITM) attacks:
1. **Mandatory Peer Verification:** Under no circumstances is an `--insecure` or "skip certificate validation" flag supported in the official client binary for secure endpoints.
2. **IP-Address SAN Matching:** For connections to an IP address (e.g. `192.168.1.50`), the client strictly verifies that the server certificate contains a Subject Alternative Name extension with an `iPAddress` entry matching the connected IP address.
3. **Fail-Closed Semantics:** Any certificate validation error—including expired certificates, untrusted CA roots, IP mismatches, or malformed extensions—immediately aborts the TLS handshake and terminates the connection with an explicit diagnostic message to stderr.
4. **Credential Isolation:** Test certificates and test private keys used in automated tests are generated ephemerally in memory or temporary directories at test time. No private keys or production certificates are committed to Git.
5. **Strict Private Key Permissions (AD-S9-05):** Server private key files on disk must be owner-only (`0600` or `0400`). Server initialization strictly fails closed if the file is readable or writable by group or others (`st_mode & 0077 != 0`), outputting an actionable remediation diagnostic (`chmod 0600 or 0400 <path>`).

### 4.3 Expiry and Rotation Design (AD-S9-04)

- **Restart-Based Rotation:** In S9, dynamic in-process certificate reload (e.g. via `SIGHUP`) is explicitly deferred per AD-S9-04.
- Rotating a server certificate or private key requires a graceful server restart:
  1. Stop the server process (SIGINT / SIGTERM).
  2. Replace certificate and private key files on disk with appropriate permissions (0600 for private key).
  3. Start the server with the updated configuration.
  4. In Fingerprint mode, distribute the updated 64-hex SHA-256 fingerprint to clients. In CA mode, if the CA root remains unchanged, existing clients continue connecting without configuration changes.

---

## 5. Client Identity and Moderator Authentication (AD-S9-02)

### 5.1 Approved Baseline Architecture

Per AD-S9-02, the initial S9 implementation uses **Server-Authenticated TLS 1.3** and transmits the existing `DARKPOOL_MODERATOR_TOKEN` inside the encrypted application stream:

```
Client ------------------- [TLS 1.3 Handshake: Server Verified] -----------------> Server
Client -- (Encrypted TLS Record) {"type": "connect", "role": "moderator", ...} --> Server
```

- **Moderator Authentication:**
  - The moderator client verifies the server certificate (via CA or pinned fingerprint).
  - Upon establishing the secure TLS 1.3 channel, the client transmits the `DARKPOOL_MODERATOR_TOKEN` inside the encrypted JSON `connect` payload.
  - The server decrypts the payload, extracts the token, and performs constant-time string comparison against the server's environment variable.
  - If `DARKPOOL_MODERATOR_TOKEN` is unset or invalid, the connection is rejected immediately.
  - The token is never transmitted over plaintext when connecting via TLS.
- **Player Anonymous Sessions:**
  - Players connect over TLS without credentials; the server allocates an anonymous session and assigns an internal player seat.
  - Individual cryptographic player identities, user accounts, and persistent identities are deferred.
- **Deferred mTLS:**
  - Mutual TLS (mTLS) with client certificates is deferred to a future operational hardening milestone.

---

## 6. TLS and Epoll Nonblocking Architecture

### 6.1 Epoll Reactor Integration

Integrating TLS into a nonblocking single-threaded event loop requires accommodating the OpenSSL state machine:

```
+-----------------------------------------------------------------------------------+
|                            DARKPOOL Epoll Reactor Loop                            |
+-----------------------------------------------------------------------------------+
        |                                                                   ^
   EPOLLIN event                                                       EPOLLOUT event
        v                                                                   |
+--------------------------+                                       +------------------+
| SSL_read()               |                                       | SSL_write()      |
| Decrypts TLS records     |                                       | Encrypts frames  |
+--------------------------+                                       +------------------+
        |                                                                   ^
        |-- SSL_ERROR_WANT_READ  --> Monitor EPOLLIN on client fd ----------|
        |-- SSL_ERROR_WANT_WRITE -> Monitor EPOLLOUT on client fd (buffer full or TLS handshake)
        |-- Decrypted plaintext bytes available
        v
+--------------------------+
| FrameDecoder (<= 4 KiB)  |
| Parses JSON frames       |
+--------------------------+
        v
+--------------------------+
| TokenBucketRateLimiter   | (20 req/s, burst 40)
+--------------------------+
        v
+--------------------------+
| MatchCoordinator         |
+--------------------------+
```

1. **Nonlinear Readiness (`WANT_READ` / `WANT_WRITE`) and Cross-Direction State Machine (AD-S9-06):**
   - **No TLS 1.3 Renegotiation:** Note that TLS 1.3 (RFC 8446 §Appendix D) completely eliminated session renegotiation.
   - However, `SSL_ERROR_WANT_READ` and `SSL_ERROR_WANT_WRITE` can still occur during handshake progression, post-handshake messages (e.g. `NewSessionTicket`, `KeyUpdate`), or socket send/receive buffer constraints.
   - The epoll reactor maintains explicit, separate bi-directional state for each connection: `tls_read_want_read`, `tls_read_want_write`, `tls_write_want_read`, and `tls_write_want_write`.
   - When `SSL_read()` returns `SSL_ERROR_WANT_WRITE`, the reactor monitors `EPOLLOUT` and resumes the read operation when write readiness fires.
   - When `SSL_write()` returns `SSL_ERROR_WANT_READ`, the reactor monitors `EPOLLIN` and resumes that pending write when read readiness fires; it strictly avoids leaving permanent writable interest enabled, preventing CPU-spinning retry loops.
   - `TCP_NODELAY` is enabled on all client and server stream sockets to eliminate Linux delayed-ACK timer latency floors (~40ms), dropping command-ack p95 below 1-5 ms.
2. **Write Buffer Preservation and Partial Writes:**
   - In OpenSSL, when an asynchronous write returns `SSL_ERROR_WANT_WRITE`, repeating `SSL_write()` normally requires supplying the exact same buffer pointer and length unless `SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` is enabled.
   - The S9 implementation enables `SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER` on the server and client `SSL_CTX`.
   - The outbound `OutputQueue` maintains unencrypted response bytes; data is consumed only to the extent encrypted bytes are accepted by `SSL_write()`.
3. **Disabling TLS Early Data / 0-RTT:**
   - Early data (0-RTT) in TLS 1.3 is susceptible to replay attacks by network adversaries.
   - S9 explicitly disables 0-RTT via `SSL_CTX_set_max_early_data(ctx, 0)`.
4. **Nonblocking Handshake State Machine:**
   - Accepted sockets on the TLS listener are nonblocking (`SOCK_NONBLOCK | SOCK_CLOEXEC`).
   - Sockets begin in `HandshakeState::InProgress`. The reactor drives `SSL_accept()` on each readiness event until completion, transitioning to `HandshakeState::Established`.
5. **Handshake Timeout and Eviction:**
   - Sockets in `HandshakeState::InProgress` are governed by `kHandshakeTimeout` (5 seconds).
   - If a client opens a TCP connection to the TLS listener but stalls without completing the handshake within 5 seconds, the server evicts the connection, frees the `SSL*` session, closes the socket descriptor, and reclaims all resources.
6. **Plaintext Rejection on Secure Listener:**
   - A client sending plaintext HTTP or raw bytes to port 4783 fails the TLS record header check (`SSL_R_WRONG_VERSION_NUMBER` / `SSL_R_UNKNOWN_PROTOCOL`).
   - The server cleanly tears down the offending connection without crashing, hanging, or falling back to plaintext.

---

## 7. Configuration and CLI Specification

### 7.1 Server CLI Configuration

The server supports configuring the dedicated TLS listener alongside or independent of existing listeners:

```sh
# Full secure configuration on private LAN
./build/darkpool_server \
  --tls-bind-address 192.168.1.50 \
  --tls-port 4783 \
  --tls-cert /path/to/server-cert.pem \
  --tls-key /path/to/server-key.pem

# Localhost secure development configuration
./build/darkpool_server \
  --tls-bind-address 127.0.0.1 \
  --tls-port 4783 \
  --tls-cert /path/to/server-cert.pem \
  --tls-key /path/to/server-key.pem
```

#### CLI Flags:
- `--tls-bind-address <IPv4>`: Explicit RFC 1918 private IPv4 address (or loopback `127.0.0.1`) assigned to a local network interface. Wildcard `0.0.0.0` is prohibited.
- `--tls-port <port>`: Secure listener TCP port (defaults to `4783`).
- `--tls-cert <path>`: Path to PEM-encoded certificate chain file.
- `--tls-key <path>`: Path to PEM-encoded private key file.
- Both `--tls-cert` and `--tls-key` must be provided together.
- Private key file permissions are strictly checked; keys accessible by group or others (`st_mode & 0077 != 0`) fail closed on startup with exit code 1.

### 7.2 Client CLI Configuration

The CLI client provides additive secure flags while preserving all existing plaintext workflows:

```sh
# Mode 1: Local CA Trust Root
./build/darkpool_client --tls --ca-cert ca-cert.pem 192.168.1.50 4783 moderator
./build/darkpool_client --tls --ca-cert ca-cert.pem 192.168.1.50 4783 player

# Mode 2: SHA-256 Leaf Fingerprint Pinning
./build/darkpool_client --tls --server-fingerprint <64-HEX-DIGEST> 192.168.1.50 4783 moderator
./build/darkpool_client --tls --server-fingerprint <64-HEX-DIGEST> 192.168.1.50 4783 player
```

> [!IMPORTANT]
> `--ca-cert` and `--server-fingerprint` are strictly mutually exclusive. Providing both flags is rejected immediately with an explicit diagnostic message and exit code 2.

---

## 8. Approved Architectural Decisions (S9 Decision Register)

| Decision ID | Summary | Approved Selection | Rationale |
|---|---|---|---|
| **AD-S9-01** | Listener Port Topology | **Dedicated Port 4783** | Eliminates dual-mode plaintext/TLS ambiguity on port 4782; allows simultaneous plaintext loopback/UNIX socket debugging and encrypted LAN testing. |
| **AD-S9-02** | Moderator & Client Identity | **Server-Auth TLS + App Token inside TLS** | Encrypts `DARKPOOL_MODERATOR_TOKEN` and all gameplay traffic without requiring complex client certificate distribution (mTLS) for casual players. |
| **AD-S9-03** | Client Certificate Trust Models | **Both Local CA and Leaf Fingerprint Pinning** | Local CA supports structured multi-device deployments; fingerprint pinning enables zero-install ad-hoc LAN matches. Both strictly verify validity and IP SAN. |
| **AD-S9-04** | Certificate Reload Strategy | **Restart-Based Rotation (Dynamic Reload Deferred)** | Avoids signal-safety and dynamic state replacement complexity in the epoll reactor. Controlled restart provides clean, deterministic rotation. |
| **AD-S9-05** | Strict Key File Permissions | **Owner-Only 0600/0400 Fail-Closed** | Prevents credential exposure on multi-user systems; overly permissive keys abort startup with actionable instructions. |
| **AD-S9-06** | Reactor TLS State Machine & TCP_NODELAY | **Independent Bi-Directional Interest & Nagle Disable** | Prevents busy-spin on write `WANT_READ` and drops command-ack p95 below 1-5 ms by eliminating delayed-ACK timer floors. |
| **AD-S9-07** | Fingerprint Pinning Verification Model | **Accepts CA-Signed & Self-Signed Leaves** | Accepts any valid leaf certificate matching the SHA-256 pin, validity period, IP SAN, and serverAuth purpose without requiring CA root in trust store. |
| **AD-S9-08** | Mutually Exclusive CLI Flags | **Explicit Error & Exit 2 on Dual Flags** | Eliminates ambiguous trust model selection and prevents order-dependent argument overrides. |

---

## 9. Verification Matrix

The S9 automated verification suite covers 25 comprehensive integration scenarios registered with CTest:

1. **T-S9-01:** Valid TLS 1.3 handshake with trusted local CA and matching IP SAN.
2. **T-S9-02:** Valid TLS 1.3 handshake in fingerprint-pinning mode with self-signed leaf.
3. **T-S9-03:** Rejection of an untrusted CA root (fails closed).
4. **T-S9-04:** Rejection of an incorrect fingerprint digest (fails closed).
5. **T-S9-05:** Rejection of a mismatched IP SAN (fails closed).
6. **T-S9-06:** Rejection of an expired certificate (`notAfter` in the past).
7. **T-S9-07:** Rejection of TLS 1.2 or earlier handshakes (protocol version alert).
8. **T-S9-08:** Rejection of plaintext traffic sent to the TLS listener (clean teardown without fallback).
9. **T-S9-09:** Handshake stall timeout (slowloris connection evicted at 5.0s; baseline FDs settle cleanly).
10. **T-S9-10:** Fragmented TLS records and multiple coalesced application frames reassembled cleanly.
11. **T-S9-11:** Slow-reader output queue bounding (64 KiB client buffer limit enforced over TLS, affirmative server-side queue eviction verified, concurrent sibling match progression verified).
12. **T-S9-12:** Request flood rate limiting over TLS (20 req/s, burst 40 enforced; verified zero state mutation on authoritative state; sibling match isolated).
13. **T-S9-13:** Secret redaction in server and client diagnostics.
14. **T-S9-14:** Complete 5-player end-to-end match gameplay through terminal victory over secure transport.
15. **T-S9-15:** Multiple concurrent TLS clients and match isolation with cross-match authorization probes.
16. **T-S9-16:** Disconnect, shutdown, and resource cleanup (monitored server PID file descriptors settle back to baseline 6).
17. **T-S9-17:** Valid TLS 1.3 in fingerprint mode with CA-signed leaf matching pinned SHA-256 without CA root in trust store.
18. **T-S9-18:** Rejection of not-yet-valid certificate (`notBefore` in the future).
19. **T-S9-19:** Rejection of invalid server certificate usage / EKU (missing `serverAuth`).
20. **T-S9-20:** Certificate rotation causing old fingerprint pin to fail until explicitly updated.
21. **T-S9-21:** Strict private key permissions (0600 succeeds, 0644 fails closed with actionable error).
22. **T-S9-22:** Mutually exclusive verification CLI flags (`--ca-cert` and `--server-fingerprint` rejected).
23. **T-S9-23:** Truncated / unexpected TLS termination (EOF without close_notify alert treated as error; reactor remains responsive).
24. **T-S9-24:** Reactor state machine cross-direction readiness and TLS 1.3 KeyUpdate.
25. **T-S9-25:** Persistent frame-write cursor retry and timeout resilience (incomplete frame resumes from saved cursor offset; timeouts and backpressure preserve pending state without starting a new frame or corrupting the stream; typed outcomes distinguish timeout from peer closure).

---

## 10. Status and Acceptance Gate

- [x] Formal review and written approval of S9 specification (`docs/s9-transport-security-design.md`).
- [x] Explicit consensus on Decisions AD-S9-01 through AD-S9-08 recorded in `docs/DECISIONS.md`.
- [x] OpenSSL 3.x integrated into CMake build system.
- [x] Implementation merged to main via PR #28 (`feature/s9-tls13-transport`).
- [x] Nonblocking TLS state machine integrated into epoll reactor with TCP_NODELAY.
- [x] Client CLI updated with CA and fingerprint validation modes and mutual exclusivity.
- [x] Full automated test matrix (25 scenarios) executed and passing via CTest.
- [x] Corrected benchmark harness executed with descriptor leak fix and Plaintext TCP baseline on port 4782.
- [ ] Physical two-computer cross-network LAN acceptance test: **NOT RUN** (remains deferred; requires two physical machines).
