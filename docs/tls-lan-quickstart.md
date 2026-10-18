# DARKPOOL Setup: One Computer or Private LAN

The maintained setup instructions and troubleshooting are in the [DARKPOOL User, Game, and Operator Guide](user-guide.md).

This page remains as a stable link for existing references and the TLS setup script's user-facing documentation path. It intentionally avoids duplicating the full command reference, rules, and test/evidence reports.

## One computer

~~~bash
bash scripts/setup_local.sh
bash scripts/run_local_server.sh
~~~

In separate terminals:

~~~bash
bash scripts/run_local_client.sh moderator
bash scripts/run_local_client.sh player
~~~

Run a player client once per seat. The moderator creates a match; players join with the displayed match ID and match join token. The moderator and player launchers use the same local UNIX-domain socket.

## Multiple computers with TLS 1.3

On the server computer:

~~~bash
bash scripts/setup_tls_lan.sh server
bash scripts/run_tls_server.sh
~~~

On each player computer:

~~~bash
bash scripts/setup_tls_lan.sh client SERVER_LAN_IPV4 SERVER_SSH_USER player
bash scripts/run_tls_client.sh player
~~~

On the moderator computer:

~~~bash
bash scripts/setup_tls_lan.sh client SERVER_LAN_IPV4 SERVER_SSH_USER moderator
bash scripts/run_tls_client.sh moderator
~~~

Each client computer requires a repository checkout and authorized SSH access to the server for setup. The scripts install dependencies, build the program, establish TLS server trust, and retrieve the moderator credential only for moderator setup. They do **not** distribute each newly created match's join token automatically; the moderator must securely share the match ID and match token with intended players.

Use TLS port 4783 and trusted private networking only. Do not expose the server to the public Internet. For full details, see the [user guide](user-guide.md#3-option-b-multiple-computers-over-a-private-lan).
