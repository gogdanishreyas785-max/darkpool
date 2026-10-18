# DARKPOOL Setup: Single Machine or Any Number of LAN Machines

DARKPOOL supports two launch topologies:

- **Single machine:** the server, moderator, and all player clients run on one Linux machine and communicate over the local UNIX-domain socket. No LAN IP, SSH, certificates, or client provisioning is required.
- **Private LAN:** one machine runs the server; the moderator and any number of player client processes can run on that machine or on other machines. The scripts do not require a fixed number of client machines. Secure LAN clients use TLS 1.3 on TCP port **4783** and fetch trust material over SSH.

The configured game capacity still applies regardless of machine count: each match accepts **3–64 players**, with a maximum of **four concurrent matches** and **256 active player connections plus four moderator connections** across the server. Multiple players can run on one machine by opening multiple terminals and running the client launcher once per seat.

## Option A — Run the entire game on one machine

Clone or update the repository, then run these commands from the repository root.

### 1. One-time setup

    bash scripts/setup_local.sh

This installs Ubuntu/Debian build dependencies, builds DARKPOOL in Release mode with warnings-as-errors, runs the TLS integration test suite, and generates a local moderator token. It does not install or enable an SSH server. Build parallelism defaults to two jobs; set \`DARKPOOL_BUILD_JOBS=1\` for a smaller VM, or increase it if more memory is available.

### 2. Start the server

In Terminal 1:

    bash scripts/run_local_server.sh

Keep it running. The local launcher uses the default local UNIX-domain socket at \`/tmp/darkpool.sock\`.

### 3. Start the moderator and players

In Terminal 2:

    bash scripts/run_local_client.sh moderator

Open a separate terminal for each player seat, on the **same machine**:

    bash scripts/run_local_client.sh player

Launch at least three player clients and at most 64 for a single match. There is no machine-to-machine credential sharing in this mode. In the moderator terminal, type \`create\` (or \`create 8\`), each player types \`join 1\`, and the moderator types \`start\` once all configured players have joined. Use the commands described in the main README for the rest of the game.

## Option B — Run across any number of private-LAN machines

Use this mode when player clients and/or the moderator will run on other machines. All participating machines should run a compatible checkout of the DARKPOOL repository on Ubuntu/Debian Linux and be connected through a trusted private network.

### 1. Set up the server machine once

On the machine that will host the server:

    bash scripts/setup_tls_lan.sh server

The script chooses the only detected private LAN IPv4 address when unambiguous. If multiple private IPv4 addresses exist, it asks you to choose the one other machines can reach. You can also specify it explicitly:

    bash scripts/setup_tls_lan.sh server 192.168.1.50

The server setup installs build dependencies, enables OpenSSH, builds DARKPOOL, runs the TLS integration test, creates/reuses a private local CA and an IP-SAN server certificate, and generates/reuses a moderator token. Secrets remain outside the repository under \`~/.local/share/darkpool/tls-lan\` with restrictive permissions; the CA private key remains on the server. If UFW is already active, it adds SSH and TLS rules limited to the detected LAN subnet. If UFW is not active, the script does not change firewall configuration.

Start the server in a terminal and leave it running:

    bash scripts/run_tls_server.sh

### 2. Provision each moderator/player machine

There is no two-machine pairing or machine-count configuration. Repeat the relevant client setup command independently on **every machine that will run a client**. Substitute the server's selected LAN IP and the Linux login name used to SSH into the server.

Player machine:

    bash scripts/setup_tls_lan.sh client 192.168.1.50 SERVER_SSH_USER player

Moderator machine:

    bash scripts/setup_tls_lan.sh client 192.168.1.50 SERVER_SSH_USER moderator

If the remote SSH username matches the local username, the player setup command can be shortened to:

    bash scripts/setup_tls_lan.sh client 192.168.1.50

The second argument can also be \`player\` or \`moderator\` when using the local default SSH username. First-time SSH host-key verification and authentication are still required. Verify the host-key fingerprint before accepting it. The script transfers the public CA certificate over SSH and obtains the moderator token only for moderator setup; no one needs to calculate fingerprints, manually copy certificate files, or paste a token into a command. Player setup removes a stale local moderator token if one is present.

### 3. Launch clients on every machine that will participate

On the moderator machine:

    bash scripts/run_tls_client.sh moderator

On every player machine:

    bash scripts/run_tls_client.sh player

Run the player launcher multiple times on any machine that needs to host multiple seats. The server does not need to know how many physical machines are participating; it assigns seats as clients connect, subject to the match/server capacity noted above. Create the match from the moderator terminal, have each player type \`join 1\`, and start after all seats have joined.

## Security and troubleshooting

- **Server address:** use the server's assigned private LAN IPv4 address. \`127.0.0.1\` refers to each machine itself, not the remote server.
- **Ports:** SSH bootstrap uses TCP **22**, and encrypted DARKPOOL game traffic uses TLS 1.3 on TCP **4783**. Do not expose DARKPOOL or SSH to the public Internet; public-Internet deployment is unsupported. Plaintext port **4782** is not encrypted and should not be used for untrusted networks.
- **Firewall:** if UFW was already active during setup, the script adds rules for the server interface's detected subnet. Other firewalls or subnet differences may require matching LAN-restricted rules for TCP 22 and 4783.
- **SSH access:** each client machine needs authorized SSH access to the server using the supplied Linux username. Limit that access to trusted operators. The moderator token is sent only over SSH and TLS; do not share it with player machines.
- **Changing the server address:** rerun server setup with the new assigned address, then rerun client setup on each participating client machine.
- **Multiple games:** the server supports four concurrent matches. Players join the appropriate match ID; machine count does not increase the configured server capacity.
- **Memory-constrained VMs:** prefix setup commands with \`DARKPOOL_BUILD_JOBS=1\`; the default is two build jobs.

For game commands, phases, and rules, see the main [README](../README.md).