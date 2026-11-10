# systemd service deployment

The Linux server optionally sends `READY=1`, `WATCHDOG=1`, and `STOPPING=1`
notifications over the socket named by systemd's `NOTIFY_SOCKET`. Without that
variable, the server runs in the foreground as before. A watchdog ping is
enabled only when `WATCHDOG_USEC` is a positive, representable integer and an
optional `WATCHDOG_PID` matches the server process. Pings are scheduled from
the existing event-loop tick at half the watchdog interval, and the epoll wait
is capped to that deadline when needed. No extra thread or `libsystemd`
dependency is used. Socket send errors are nonfatal and produce throttled
diagnostics that do not include the socket address.

The example unit in [`deploy/systemd/darkpool.service`](../deploy/systemd/darkpool.service)
uses `Type=notify`, a 30-second watchdog, and a private runtime directory. The
install must provide the `darkpool` service account and place the server binary
at `/usr/local/bin/darkpool_server`. The server creates its UNIX socket with
owner-only permissions, so clients must run
as the `darkpool` service account or receive access through an appropriate
local operating-system mechanism. The example disables TCP and uses
`/run/darkpool/darkpool.sock`; adapt the unit only for a trusted deployment.
If TCP is enabled, apply the existing TLS and bind-address guidance in the
[user guide](user-guide.md).

systemd restarting the process does **not** recover in-memory matches. All
active match state is lost when the server exits; this integration adds
lifecycle reporting only and does not add persistence or crash recovery.
