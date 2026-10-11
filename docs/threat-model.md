# DARKPOOL threat model

## Objectives and assets

The server is the trust boundary and official source of truth. Protect identity/session binding; membership; roles and Insider hint; Investigator clue; private chat; cash, shares, health, price, trades, votes and outcomes; timers; audit logs; bounded process resources. Keep match IDs isolated.

No claim is made against compromised host/root or all timing side channels.

## Boundaries and actors

Boundaries: local client to UNIX socket/parser; authenticated session to match router/state; player output to moderator/audit records; future remote client to deferred TCP/TLS adapter.

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
| Future interception | Later TCP requires TLS 1.3 and explicit client identity. | If S9 approved: reject plaintext and invalid certificates. |

## Residual risks

Same-UID peer credentials do not distinguish humans. Moderator visibility, log retention/integrity, reconnect policy and audit privacy need decisions. Local caps reduce accidental/unprivileged abuse but cannot guarantee DOS resistance from privileged attackers. Advanced side-channel mitigation and sandboxing are deferred. Single-process design has no failover; a process loss ends active matches absent future persistence.
