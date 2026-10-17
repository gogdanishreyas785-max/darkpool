# DARKPOOL threat model

## Objectives and assets

The server is the trust boundary and official source of truth. Protect identity/session binding; membership; roles and Insider hint; Investigator clue; private chat; cash, shares, health, price, trades, votes and outcomes; timers; audit logs; bounded process resources. Keep match IDs isolated.

No claim is made against compromised host/root or all timing side channels.

## Boundaries and actors

Boundaries: local client to UNIX socket/parser; authenticated session to match router/state; player output to moderator/audit records; encrypted LAN client to TLS 1.3 adapter (port 4783); future remote client to deferred public Internet deployment.

Actors: buggy/malicious client, abusive local process, same-UID process, slow/disconnected client, authorized moderator, host administrator/root. Restrictive socket permissions and least privilege are required. Peer credentials identify OS account, not a distinct human if processes share UID; local auth policy needs approval.

## Threats, controls, evidence

| Threat | Controls | Planned evidence |
|---|---|---|
| Identity spoofing | Bind server identity to authenticated session; ignore sender ID claims. | Send as another player and forge state; reject with no state change. |
| Unauthorized actions | Central phase/role/membership validation; client cannot set authoritative fields. | Wrong role/phase/member and forged cash/share/role tests. |
| Cross-match leakage | Match-owned state, routing by authenticated membership, audience-filter private events. | Canary roles/chat/clue/trade/vote/timer/log across simultaneous matches. |
| Malformed/oversized input | Check lengths before allocation; schema allowlist; reject invalid version, truncation and text. | Negative suite/fuzz; no crash or corruption. |
| Flooding | Per-session rates/quotas; bounded work and connections. | Flood test; verify other clients remain responsive, record thresholds. |
| Slow consumer | Bounded output queue, backpressure/high-water policy, disconnect persistent slow readers. | Stop client reads; measure queue/RSS and other-client latency. |
| Local socket abuse | Least privilege, restrictive socket directory/mode, safe stale-socket cleanup. | Unauthorized local account cannot connect/read protected files. |
| Secret logging | Separate player output and privileged audit; redact roles/hints/clues. | Unique secret canaries absent from unauthorized output/logs. |
| Replay/duplicate | Request-ID policy and one action per phase. | Exact retry produces one mutation; conflicting ID rejected. |
| Network interception | Dedicated TLS 1.3 listener on port 4783 (AD-S9-01) with OpenSSL 3.x encryption, strict IP SAN matching, and dual verification (local CA and leaf fingerprint pinning; AD-S9-03). Moderator token and gameplay data encrypted inside TLS. Plaintext rejected on port 4783. | Automated TLS negative suite: reject plaintext, reject untrusted CA, reject mismatched IP SAN, reject expired cert, reject TLS 1.2, verify token encrypted. |
| Private LAN transport | Non-loopback plaintext TCP binding (port 4782) restricted to RFC 1918 private subnets; plaintext transport warning; OS firewall restriction required. Plaintext protocol does not protect against LAN eavesdropping; secure LAN play must use TLS on port 4783. | Strict IP validation rejects public/unassigned IPs and 0.0.0.0; integration tests enforce RFC 1918 bounds. |

## Residual risks

Same-UID peer credentials do not distinguish humans. The legacy LAN transport on port 4782 is unencrypted plaintext: moderator tokens, player messages, and private roles/hints/clues are readable by any host on the same physical or virtual broadcast domain if plaintext TCP is explicitly enabled. While S9 TLS 1.3 on port 4783 provides transport encryption, integrity, and server authentication for LAN environments, physical cross-device two-computer LAN acceptance remains untested (`NOT RUN`), and public-Internet deployment is not supported. Mutual TLS (mTLS) for players is deferred per AD-S9-02; players use server-authenticated TLS with anonymous sessions. Dynamic in-process certificate reload is deferred per AD-S9-04; rotation requires server restart. Moderator visibility, log retention/integrity, reconnect policy and audit privacy need decisions. Local caps reduce accidental/unprivileged abuse but cannot guarantee DOS resistance from privileged attackers. Advanced side-channel mitigation and sandboxing are deferred. Single-process design has no failover; a process loss ends active matches absent future persistence.
