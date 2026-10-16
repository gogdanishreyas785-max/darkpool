# S9 Transport Security Design Proposal (TLS 1.3)

**Status:** PROPOSAL FOR ARCHITECTURAL REVIEW — NOT YET APPROVED OR IMPLEMENTED  
**Author:** Senior Networking & Security Architecture Review  
**Target Milestone:** S9 (Secure Transport & Identity Layer)  
**Applies to:** `src/server_main.cpp`, `src/client_main.cpp`, `include/darkpool/network.hpp`  
**Prerequisites:** S1 Deterministic Engine, S2 Match Coordinator, S3 Frame Decoding & Resource Limits, L6 Multi-Match Capacity, LAN TCP Binding (PR #26)

---

## 1. Executive Summary

The DARKPOOL server prototype currently supports local UNIX-domain stream sockets (`/tmp/darkpool.sock`), loopback TCP (`127.0.0.1:4782`), and explicit private LAN TCP binding restricted to RFC 1918 private IPv4 addresses (`--bind-address`). While UNIX-domain sockets leverage filesystem DAC permissions (0700) and loopback traffic remains restricted to the local OS kernel, the LAN TCP path transmits all traffic as unencrypted plaintext.

On a physical or switched local network, plaintext TCP exposes sensitive application data to passive eavesdropping and active packet modification:
1. The moderator authentication token (`DARKPOOL_MODERATOR_TOKEN`) is transmitted in plaintext during connection handshake.
2. Player private roles (`Insider`, `Investigator`, `Employee`) and private investigation clues are transmitted in plaintext.
3. Player trade orders, hints, and elimination votes can be snooped or tampered with by any device sharing the local broadcast domain or Wi-Fi network.

This document proposes an architectural design for integrating **strict TLS 1.3 transport security** into DARKPOOL's nonblocking, epoll-driven C++20 server and client CLI. This design satisfies future transport security requirements while strictly preserving:
- The deterministic, zero-dependency C++20 authoritative game engine.
- Existing match coordination, isolation rules, and audience-scoped message filtering.
- Existing framing boundaries (4 KiB maximum frame payload), output queue limits (64 KiB client buffer eviction), and token-bucket rate limiting (20 req/s, burst 40).
- The low-friction local developer workflow (UNIX domain stream sockets and loopback TCP).

> [!IMPORTANT]
> This document is a **design proposal for engineering and security review**. It does not approve implementation, does not commit production credentials, and does not alter current engine or server behavior.

---

## 2. Goals and Non-Goals

### Goals
- **Strict Transport Confidentiality & Integrity:** Ensure all over-the-network communication (moderator authentication, player roles, private clues, trades, votes, and events) is cryptographically protected against passive interception and active in-flight tampering.
- **Mutual/Strict Server Authentication:** Require client verification of server certificates, including Subject Alternative Names (SANs) for hostnames and IP addresses, with fail-closed semantics on any untrusted or mismatched certificate.
- **Epoll & Nonblocking Architecture Alignment:** Integrate TLS handshakes, record encryption/decryption, and I/O readiness into the single-threaded epoll reactor without blocking the event loop or introducing thread-per-client complexity.
- **Resource Boundary & Backpressure Preservation:** Maintain strict 4 KiB frame payload limits, 64 KiB client output buffer limits, and per-connection rate limiting inside the decrypted stream.
- **Strict TLS 1.3:** Enforce TLS 1.3 as the minimum and only allowed protocol version on secure endpoints, eliminating legacy cipher suite negotiation and downgrade attacks.
- **No Plaintext Fallback:** Explicitly reject plaintext connections on ports or endpoints configured for TLS.

### Non-Goals
- **In-Scope TLS Implementation in this PR:** No C++ code changes, CMake modifications, or external dependencies are introduced in this document.
- **Persistent Client Accounts or PKI Enrollment:** This milestone does not design a persistent database, user passwords, OAuth2/OIDC, or global PKI enrollment for casual players.
- **Game Engine Mutation:** No changes to `darkpool::Match`, game rules, victory conditions, share conservation formulas, or role ratios.
- **Replacement of Application-Level Authorization:** TLS provides transport security; it does not replace server-side match membership checks, role verification, or coordinator state machine guards.
- **Public ACME Automation for RFC 1918 Private LANs:** Public certificate authorities (e.g., Let's Encrypt via ACME) do not issue publicly trusted certificates for RFC 1918 private IPv4 subnets (`10.0.0.0/8`, `172.16.0.0/12`, `192.168.0.0/16`). Automated ACME lifecycle is a non-goal for private LAN deployments.
- **Side-Channel Defense against Root Host Compromise:** Host root attackers with memory inspection access remain out of scope.

---

## 3. Scope and Transport Boundaries

### 3.1 Transport Topology

DARKPOOL recognizes three distinct operational transport environments:

| Transport Mode | Interface / Address | Encryption / Security | Target Environment | Status |
|---|---|---|---|---|
| **Local UNIX-Domain** | `/tmp/darkpool.sock` | Filesystem DAC (mode 0700, stale socket check) | Single-host inter-process communication, local testing | Implemented |
| **Localhost TCP** | `127.0.0.1` (loopback) | Kernel loopback isolation; unencrypted | Single-host developer debugging | Implemented |
| **Private LAN TCP** | RFC 1918 IPv4 (`10.x`, `172.16-31.x`, `192.168.x`) | Plaintext; restricted by IP parsing and host firewall | Dedicated multi-machine testing on trusted isolated LAN | Implemented (PR #26) |
| **Secure Private LAN TCP** | RFC 1918 IPv4 (`10.x`, `172.16-31.x`, `192.168.x`) | **TLS 1.3** with local CA or pinned self-signed certificate | Multi-machine gameplay over LAN or Wi-Fi | **Proposed (S9)** |
| **Remote Internet TCP** | Public IPv4/IPv6 / FQDN | **TLS 1.3** with CA-signed certificate, FQDN SAN, firewalling | Public internet play, hosted servers | **Deferred Future Work** |

### 3.2 Transport Invariants

1. **UNIX Socket Independence:** The local UNIX-domain socket path must remain functional without requiring TLS libraries or certificate configuration, preserving zero-overhead local development and fast continuous integration tests.
2. **Explicit Secure Configuration:** The server must not silently downgrade a secure listener to plaintext. If TLS configuration (certificate/private key) is specified or `--tls` is enabled, failure to load keys or initialize the cryptographic context must cause the server daemon to exit with an error.
3. **Decoupled Engine Pipeline:** The protocol pipeline must maintain clean separation:
   $$\text{Physical Socket} \longrightarrow \text{TLS Record Layer} \longrightarrow \text{FrameDecoder (4 KiB limit)} \longrightarrow \text{RateLimiter} \longrightarrow \text{MatchCoordinator} \longrightarrow \text{GameEngine}$$
   The game engine never interacts with socket descriptors or TLS session contexts.

---

## 4. Server Identity and Certificate Management

### 4.1 Deployment Topologies and Certificate Strategies

For private LAN environments using RFC 1918 addresses, public Web PKI CAs cannot issue certificates. We evaluate two viable architectural alternatives for establishing server trust:

```
Alternative A: Locally Managed Dedicated Root CA
  +--------------------+        Issues       +----------------------+
  | Local DARKPOOL CA  | ------------------> | Server Certificate   |
  | (Root Cert + Key)  |                     | (SAN: IP:192.168.1.50)
  +--------------------+                     +----------------------+
            |                                           |
    Distributed to                                  Presented to
            v                                           v
  +--------------------+        Verifies     +----------------------+
  | Client Trust Store | ==================> | Client Connection    |
  +--------------------+                     +----------------------+

Alternative B: Self-Signed Server Certificate with Pinned Fingerprint
  +----------------------+      Presented    +----------------------+
  | Server Certificate   | ----------------> | Client Connection    |
  | (Self-signed + Key)  |                   |                      |
  +----------------------+                   +----------------------+
            |                                           |
      Fingerprint                                   SHA-256 Hash
      Out-of-band                                    Comparison
            v                                           v
  +--------------------+       Validates     +----------------------+
  | Expected SHA-256   | ==================> | Pinned Verification  |
  +--------------------+                     +----------------------+
```

#### Comparison of Architectural Alternatives

| Dimension | Alternative A: Local Dedicated Root CA | Alternative B: Self-Signed with Pinned Fingerprint | Alternative C: Public ACME / Let's Encrypt |
|---|---|---|---|
| **Mechanism** | An ephemeral or organization CA signs the server certificate with an IP/DNS SAN. Clients install the CA root cert in a client-specific trust bundle. | Server generates a self-signed leaf certificate. Server displays its SHA-256 fingerprint; client supplies `--server-fingerprint <HEX>` or pins it in a config file. | Automated ACME DNS-01 or HTTP-01 challenge against a public domain name. |
| **Trust Model** | Standard X.509 chain verification anchored in custom root CA. | Cryptographic pinning: direct match of certificate digest, bypassing hierarchical CA chain. | Public Web PKI hierarchical trust. |
| **Private IP Compatibility** | **Excellent.** Local CA can issue SANs with `IP:192.168.1.50` or `IP:10.0.0.25`. | **Excellent.** Leaf certificate can specify IP SAN; client directly verifies fingerprint and IP. | **Incompatible with RFC 1918.** Public CAs are prohibited from issuing certificates for private IP space (RFC 1918 / RFC 5737). |
| **Operational Overhead** | Moderate: requires generating CA root once and providing CA root certificate file to clients. | Low: requires passing one hash string or fingerprint file to clients out-of-band. | High: requires public domain, public DNS control, split-horizon DNS, and internet connectivity. |
| **Security Guarantees** | High: prevents MITM if CA private key remains confidential. | High: mathematically immune to CA compromise or untrusted intermediate CAs; strictly pins the exact endpoint key. | High for public internet; unworkable for offline or closed LAN environments. |

#### Architectural Recommendation
- **Primary Recommendation for S9 (Private LAN):** **Alternative A (Local Dedicated CA)** as the default standard X.509 verification path, with **Alternative B (Pinned SHA-256 Fingerprint)** supported as a convenient operational option for quick ad-hoc LAN matches.
- **Public ACME (Alternative C):** Explicitly deferred to future remote-internet milestones when public FQDNs and publicly routable endpoints are deployed.

### 4.2 Certificate Validation and Strict SAN Requirements

To prevent impersonation and man-in-the-middle (MITM) attacks:
1. **Mandatory Peer Verification:** Clients connecting over TLS must enable `SSL_VERIFY_PEER`. Under no circumstances may an insecure `--insecure` or "skip certificate validation" flag be provided in the official client binary for secure endpoints.
2. **IP-Address SAN Matching:** For connections to an IP address (e.g. `192.168.1.50`), the TLS library must be instructed to verify the peer IP against the certificate's `subjectAltName` extension (`iPAddress` entries). Common Name (CN) matching is deprecated and must not be used for IP verification.
3. **Fail-Closed Semantics:** Any certificate validation error—including expired certificates, untrusted CA roots, hostname/IP mismatches, revocation failures, or malformed extensions—must immediately abort the TLS handshake and terminate the network connection with an explicit diagnostic message to stderr.
4. **Credential Isolation:** Test certificates and test private keys used in automated integration tests must be generated deterministically in temporary test directories with explicit expiration limits. No private keys or production certificates may ever be checked into the source repository.

### 4.3 Expiry and Rotation Design Requirements

- **Server-Side Dynamic Reload:** In long-running deployments, the server architecture should provide an explicit interface (e.g., `SIGHUP` signal handler or admin API command) to reload leaf certificates and private keys into the TLS context without restarting the process or terminating existing active match connections.
- **Clock Validation & Determinism in Tests:** For automated testing of expired or not-yet-valid certificates, test harnesses must use mock verification callbacks or synthetic certificates with intentionally skewed validity windows (e.g. `notAfter` in the past) to assert deterministic failure without altering host system time.

---

## 5. Client Identity and Moderator Authentication Alternatives

### 5.1 Analysis of Client Identity Models

Currently, players join matches without credentials; the server allocates an anonymous session and assigns an internal player ID. The moderator supplies an application-level shared secret (`DARKPOOL_MODERATOR_TOKEN`) inside the JSON connect payload (`{"type": "connect", "role": "moderator", "token": "..."}`).

We evaluate three design alternatives for client authentication under TLS:

```
Alternative 1: Server-Authenticated TLS + Application Token inside TLS
  Client -------------- [TLS 1.3 Handshake: Server Cert Verified] ------------> Server
  Client -- {"type": "connect", "role": "moderator", "token": "secret"} (Encrypted) -> Server

Alternative 2: Mutual TLS (mTLS) with Client Certificates
  Client <------------- [TLS 1.3 Handshake: Server Cert Verified] ------------> Server
  Client ------------- [Client Certificate & Proof of Private Key] -----------> Server

Alternative 3: Cryptographic Player Identity System (Public Key Tokens / Accounts)
  Client -------------- [TLS 1.3 Handshake: Server Cert Verified] ------------> Server
  Client -- {"type": "connect", "role": "player", "player_pubkey": "...", ...} ----> Server
```

#### Comparison Matrix

| Dimension | Alternative 1: Server-Auth TLS + App Token | Alternative 2: Mutual TLS (mTLS) | Alternative 3: Explicit Player Identity / Accounts |
|---|---|---|---|
| **Mechanism** | Server proves identity via certificate. Moderator sends token inside encrypted TLS channel. Constant-time string comparison in server. | Server and client both present X.509 certificates during TLS handshake. Server validates client cert against Moderator CA or Player CA. | Server-authenticated TLS channel. Players sign challenge nonces using asymmetric keypairs (Ed25519) or present JWT/session tokens. |
| **Mitigated Threats** | - Network sniffing of token.<br>- In-flight tampering with player commands.<br>- MITM server spoofing. | - Stolen application token.<br>- Unauthorized moderator connection.<br>- Client impersonation across LAN. | - Sybil attacks.<br>- Replay across player sessions.<br>- Player impersonation across restarts. |
| **Unmitigated Threats** | - Token leakage at endpoints.<br>- Cannot distinguish different players if one client shares connection. | - Does not eliminate need for match-level authorization.<br>- Client certificate distribution complexity. | - Overkill for casual party/LAN game.<br>- Requires persistent identity store and recovery flows. |
| **Operational Complexity** | **Low.** Preserves existing environment variable workflow (`DARKPOOL_MODERATOR_TOKEN`). Zero client-side certificate management for players. | **High.** Requires provisioning, distributing, and managing client certificates and private keys on moderator and player machines. | **Very High.** Requires identity registry, key management, account recovery, and persistent database storage. |
| **Impact on Architecture** | Minimal: wraps existing transport; zero change to session or coordinator logic. | Substantial: requires client certificate extraction from TLS context and routing to auth layer. | Disruptive: introduces account states, persistent identity tables, and external dependencies. |

#### Architectural Recommendation
- **Proposed S9 Baseline:** **Alternative 1 (Server-Authenticated TLS + Application Moderator Token inside TLS)**.
  - *Rationale:* Protects the moderator token and all gameplay messages against passive eavesdropping and network tampering without introducing massive certificate provisioning hurdles for casual multi-machine players.
- **Optional Moderator Hardening:** Support **Alternative 2 (mTLS)** specifically for the moderator connection as an optional configuration flag (`--require-moderator-mtls`), while keeping players on server-authenticated TLS.
- **Alternative 3 (Persistent Identity / Accounts):** Rejected for S9 as a violation of project non-goals (persistent accounts are explicitly deferred in REQ-11 / DEC-11).

---

## 6. TLS and Epoll Nonblocking Architecture

### 6.1 Epoll Reactor Integration Challenges

Integrating TLS into a nonblocking single-threaded event loop requires addressing fundamental differences between raw TCP stream sockets and TLS record streams:

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
        |-- SSL_ERROR_WANT_READ  --> Wait for socket read readiness --------|
        |-- SSL_ERROR_WANT_WRITE -> Wait for socket write readiness (EPOLLOUT)
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

1. **Nonlinear Readiness (`WANT_READ` / `WANT_WRITE`):**
   - In TLS, reading application data via `SSL_read()` may require writing transport data (e.g. TLS renegotiation or post-handshake messages). If `SSL_read()` returns `SSL_ERROR_WANT_WRITE`, epoll must temporarily monitor `EPOLLOUT` for that client, even though the application is trying to read.
   - Conversely, calling `SSL_write()` may return `SSL_ERROR_WANT_READ` if a TLS record boundary or transport acknowledgement requires inbound network data. Epoll must monitor `EPOLLIN` before retrying the write.
2. **Buffer Bloat & Double Buffering:**
   - Raw bytes must not be read into unbounded userspace buffers. Decrypted data must be streamed directly into the existing `FrameDecoder`, which strictly enforces the 4,096-byte maximum frame boundary before accumulating payload memory.
   - The outbound `OutputQueue` (capped at 64 KiB) must govern unencrypted application responses. Encryption occurs on demand during `write_client()` execution.
3. **Nonblocking Handshake State Machine:**
   - Client connections accepted via `accept4(SOCK_NONBLOCK | SOCK_CLOEXEC)` must not execute blocking handshakes (`SSL_accept()`). Instead, accepted sockets must be assigned a `TlsState` struct and registered with epoll.
   - The handshake must progress incrementally as `EPOLLIN`/`EPOLLOUT` events fire.
4. **Handshake Timeout and Deadlines:**
   - A client that connects over TCP but fails to complete the TLS 1.3 handshake must be subject to an aggressive handshake deadline (e.g., `kHandshakeTimeout = 5s`).
   - If the handshake fails or times out, the server must immediately close the socket, free the TLS session context, and release descriptor resources without lingering open files or leaking memory.

### 6.2 Library Alternatives and Evaluation

| Criterion | OpenSSL 3.x | BoringSSL | wolfSSL |
|---|---|---|---|
| **License** | Apache 2.0 | Mixed (OpenSSL / ISC / MIT) | GPLv2 / Commercial |
| **Ubuntu Availability** | Standard system package (`libssl-dev`) on Ubuntu 22.04 / 24.04. | Requires building from Google source repository / submodule. | Requires separate package installation or source build. |
| **TLS 1.3 Support** | Complete, mature, default in 3.0+. | Complete, optimized for Chromium/Boring crypto. | Complete, lightweight embedded focus. |
| **C++ / CMake Integration** | Native `find_package(OpenSSL REQUIRED)` built into standard CMake. | Custom CMake targets required. | Requires custom FindWolfSSL module. |
| **Memory Footprint** | Moderate (~2–4 MB dynamic library footprint). | Low to moderate. | Very low (< 100 kB embedded configurations). |
| **Nonblocking API** | Industry standard `SSL_read()`, `SSL_write()`, `SSL_get_error()`. | Identical OpenSSL-compatible API. | OpenSSL compatibility layer available. |

#### Architectural Recommendation
- **Recommended Library for Implementation:** **OpenSSL 3.x** (`libssl-dev`).
- *Rationale:* OpenSSL 3 is the standard cryptographic library on Ubuntu 22.04 and 24.04 LTS (the project's target deployment platform). It requires zero custom submodules or build gymnastics and integrates seamlessly with CMake's standard `find_package(OpenSSL)`.

---

## 7. Proposed Verification Matrix and Test Strategy

When S9 implementation is formally scheduled, verification must adhere to the project's evidence-based test discipline:

| Test ID | Test Category | Scenario / Objective | Expected Deterministic Outcome | Environment |
|---|---|---|---|---|
| **T-S9-01** | Unit / Integration | Normal TLS 1.3 handshake with trusted root CA and valid IP SAN. | Handshake completes; negotiated cipher is TLS 1.3 (e.g. `TLS_AES_256_GCM_SHA384`); full 5-player match completes. | Single-host automated CTest |
| **T-S9-02** | Security Negative | Client connects with untrusted CA root. | Client fails closed during certificate validation; handshake terminates; 0 bytes sent; server logs TLS alert. | Single-host automated CTest |
| **T-S9-03** | Security Negative | Server certificate IP SAN does not match connected IP address (e.g. SAN is `10.0.0.1`, connected to `192.168.1.50`). | Client fails closed with `IP_ADDRESS_MISMATCH`; connection aborts immediately. | Single-host automated CTest |
| **T-S9-04** | Security Negative | Server certificate validity dates in the past (`notAfter` expired). | Client certificate verification fails closed; connection aborts with `CERT_HAS_EXPIRED`. | Single-host automated CTest |
| **T-S9-05** | Security Negative | Plaintext client connects to TLS-enforced port. | Server detects non-TLS record header (invalid record type); connection closed cleanly without crash or hung loop. | Single-host automated CTest |
| **T-S9-06** | Security Negative | Client attempts to negotiate TLS 1.2 or earlier. | Server rejects handshake; protocol version alert sent; connection closed. | Single-host automated CTest |
| **T-S9-07** | Boundary / Reliability | Handshake stall / slowloris (client opens TCP connection but sends no TLS client hello). | Monotonic handshake timer expires at 5.0 seconds; server evicts stalled connection; baseline FDs settle cleanly. | Single-host automated CTest |
| **T-S9-08** | Framing / Transport | Fragmented TLS records and coalesced JSON application frames within TLS payload. | `FrameDecoder` correctly reassembles frames across TLS record chunks; all requests processed in order. | Single-host automated CTest |
| **T-S9-09** | Backpressure | Slow reader / stalled client receiving encrypted data. | Server output queue reaches 64 KiB; slow client evicted at ~105 ms; healthy sibling match completes with 0 failures. | Single-host automated CTest |
| **T-S9-10** | Flood / Rate Limiting | Inbound flood of 100 requests through encrypted TLS session. | Token bucket rate limiter accepts 40, rejects 60 with `rate limit exceeded`; zero state mutation; sibling match isolated. | Single-host automated CTest |
| **T-S9-11** | Audit / Canary | Sensitive information redaction in encrypted mode. | Role secrets, clues, and tokens verified absent from server stdout/stderr logs and sibling match transcripts. | Single-host automated CTest |
| **T-S9-12** | Load & Performance | 64-player single match and 4-match 256-player concurrent load benchmark over TLS 1.3. | Measure encryption overhead: compare CPU utilization and p95 latency against unencrypted baseline (L4/L6). | Dedicated benchmark harness |
| **T-S9-13** | Acceptance (LAN) | Physical two-computer cross-network acceptance over Wi-Fi/Ethernet with TLS 1.3. | Complete 5-player and 8-player matches across two distinct physical machines with trusted certificate. | Physical 2-computer LAN (`NOT RUN`) |

---

## 8. Security Claims, Residual Risks, and Non-Goals

### 8.1 What TLS 1.3 Guarantees
- **Channel Confidentiality:** Third parties on the local network (e.g. Wi-Fi eavesdroppers) cannot read player roles, clues, trade orders, or moderator authentication tokens.
- **Channel Integrity:** In-flight message modification, truncation, or injection by network adversaries is detected and causes connection termination via cryptographic MAC verification.
- **Server Authentication:** Clients are guaranteed to communicate with the authentic server matching the configured certificate or pinned fingerprint.

### 8.2 What TLS 1.3 Does NOT Guarantee (Residual Risks)
- **Application Authorization:** TLS does not determine whether a client is authorized to submit a vote or trade. Server-side session validation, role checking, and phase verification remain mandatory.
- **Host Compromise:** If an attacker gains root or memory inspection capabilities on the server host or client terminal, cryptographic keys and unencrypted memory can be extracted.
- **Denial of Service (DoS):** While the server rate-limits application requests (20 req/s, burst 40) and enforces output queue limits (64 KiB), an attacker flooding raw TCP connections can still consume server CPU cycles performing asymmetric TLS cryptographic handshakes. Production public deployments require upstream firewalling, SYN cookies, and connection rate limiting.
- **Player Sybil Resistance:** Server-authenticated TLS does not prevent a single user from running multiple player clients on the same machine or across multiple machines to manipulate vote outcomes.

---

## 9. Migration Strategy and Backward Compatibility

To avoid disrupting existing development workflows and test harnesses during future rollout:

1. **Phase 1: Dual-Listener Configuration (Recommended Transition):**
   - UNIX-domain stream sockets (`/tmp/darkpool.sock`) remain unencrypted and default for local unit/integration tests.
   - Loopback TCP (`127.0.0.1:4782`) defaults to plaintext for debugging.
   - A dedicated CLI flag (e.g. `--tls-cert <path> --tls-key <path>`) activates TLS on the TCP port or binds a dedicated secure port (e.g. 4783).
2. **Phase 2: Deprecation of Plaintext Non-Loopback Binding:**
   - Once TLS transport is stable, binding non-loopback private interfaces (`--bind-address <RFC1918>`) without TLS certificates will be flagged with a deprecation warning.
   - In subsequent releases, non-loopback binding will strictly require TLS configuration.
3. **Client CLI Usability:**
   - The CLI client (`src/client_main.cpp`) will accept a `--ca-cert <path>` or `--server-fingerprint <sha256>` argument to seamlessly connect to secure servers.

---

## 10. Unresolved Architectural Decisions

The following architectural questions remain open and require explicit human stakeholder consensus before implementation commences:

| Question ID | Architectural Question | Options | Trade-offs | Recommendation |
|---|---|---|---|---|
| **AD-S9-01** | Should secure TCP be bound to the existing port 4782 or a distinct dedicated secure port (e.g. 4783)? | (A) Dual-mode port (requires config flag)<br>(B) Distinct dedicated port 4783 | Dedicated port allows simultaneous plaintext loopback and encrypted LAN listeners during migration. | **Option B: Dedicated Port 4783** |
| **AD-S9-02** | Should moderator mTLS be mandatory or optional? | (A) Mandatory mTLS for moderators<br>(B) Optional via flag<br>(C) Shared token inside server-auth TLS only | Mandatory mTLS adds certificate provisioning overhead for operators; shared token inside TLS provides sufficient transport secrecy. | **Option C Baseline, with Option B as configurable hardening** |
| **AD-S9-03** | How should client certificate verification be packaged for ad-hoc LAN play? | (A) Embed local CA script in `tools/`<br>(B) Pinned SHA-256 fingerprint in client CLI<br>(C) Both | Fingerprint pinning allows zero-install ad-hoc play; CA bundle fits structured deployments. | **Option C: Both** |
| **AD-S9-04** | Should the server support dynamic certificate reload via SIGHUP in S9? | (A) Defer to operations milestone<br>(B) Implement in S9 core TLS loop | SIGHUP handling adds signal-safety complexity; restarting process is simple for LAN games. | **Option A: Defer dynamic reload** |

---

## 11. Acceptance Gate and Approval Checklist

Implementation of S9 transport security must **NOT** begin until the following approval criteria are met:
- [ ] Formal review and written approval of this design proposal (`docs/s9-transport-security-design.md`) by project stakeholders.
- [ ] Explicit consensus on Unresolved Architectural Decisions AD-S9-01 through AD-S9-04.
- [ ] Explicit approval to introduce OpenSSL 3.x (`find_package(OpenSSL)`) into `CMakeLists.txt`.
- [ ] Creation of a dedicated implementation branch (e.g. `feature/s9-tls-transport-security`).
