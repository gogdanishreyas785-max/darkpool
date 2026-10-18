# DARKPOOL TLS 1.3 LAN Quickstart

This is the scripted path for running DARKPOOL across separate Ubuntu/Debian machines on a trusted private LAN. It builds the project, provisions TLS material, and automates certificate/token transfer. The TLS 1.3 listener uses TCP port **4783**; clients validate the server certificate against a local CA and the server IP address.

## 1. On every machine: get the repository

Clone the DARKPOOL repository, or update an existing checkout so all machines use the same revision. Run the commands below from the repository root. The setup script installs the required build packages (CMake, a C++ compiler, OpenSSL development libraries, Git, and OpenSSH), builds Release binaries with warnings-as-errors, and runs the TLS integration test.

## 2. On the server machine: one setup command

    bash scripts/setup_tls_lan.sh server

The script selects the server's private LAN IPv4 address when it can do so unambiguously. If the host has multiple private interfaces, enter the address reachable by the client machines. You can also pass it explicitly:

    bash scripts/setup_tls_lan.sh server 192.168.1.50

The server setup creates (or reuses) a private local CA, an IP-SAN server certificate, and a cryptographically random moderator token. Private files are stored under **~/.local/share/darkpool/tls-lan** with restrictive permissions; the CA private key stays on the server. The script starts the OpenSSH server for authenticated bootstrap. If UFW is already active, it adds SSH and TLS rules limited to the server interface's detected LAN subnet; otherwise it does not change firewall configuration.

Start the TLS server after setup finishes:

    bash scripts/run_tls_server.sh

Leave that terminal running.

## 3. On each client machine: one setup command

The client pulls the CA certificate through SSH. Use the server machine's Linux login name as **SERVER_SSH_USER** (and use the server IP selected above):

    bash scripts/setup_tls_lan.sh client 192.168.1.50 SERVER_SSH_USER player

On the machine that will run the moderator client, use **moderator** instead of **player**:

    bash scripts/setup_tls_lan.sh client 192.168.1.50 SERVER_SSH_USER moderator

The normal SSH host-key check and SSH authentication still apply; verify the host-key fingerprint before accepting it on first connection. SSH encrypts the bootstrap transfer. You do not manually copy certificate files, calculate fingerprints, or paste tokens into commands. Player setup downloads the public CA certificate only. Moderator setup also retrieves the moderator token over SSH and stores it with mode **600**; the script never prints the token. Only provision machines/users that you trust with SSH access to the server.

If the SSH username on the server matches the current local username, you can omit the username:

    bash scripts/setup_tls_lan.sh client 192.168.1.50

This defaults to the player role. To specify the moderator role, pass the server SSH username and then the role argument.

## 4. Launch moderator and players

On the moderator machine:

    bash scripts/run_tls_client.sh moderator

On each player machine, open a terminal and run:

    bash scripts/run_tls_client.sh player

Each client opens an interactive terminal. In the moderator terminal, enter **create** (or **create 8**) to create a match. Players enter **join 1**; the moderator then enters **start** once the configured player count has joined. Continue using the moderator/player commands documented in the main README.

## What is automatic

- Installs OS build/TLS packages and builds the application on each machine.
- Reuses the server CA and moderator token across repeated setup runs instead of rotating them unnecessarily.
- Issues/reissues the TLS server certificate when needed, with an IP Subject Alternative Name that matches the selected server address.
- Transfers the CA certificate through authenticated SSH; transfers the moderator token only during moderator setup.
- Stores generated secrets outside the Git checkout and uses restrictive file permissions.
- Configures UFW only when it is already installed and active, restricting SSH bootstrap and TLS ingress to the detected private subnet.

## Troubleshooting and security notes

- If the server has multiple private interfaces, choose the IP on the same reachable subnet as the clients. **127.0.0.1 is not a remote server address.**
- If client setup cannot connect, check that SSH is reachable on TCP **22**, the SSH username is valid, the host key is verified, and the server setup completed successfully. Existing firewall policies outside UFW may need an equivalent LAN-restricted rule.
- Ensure TCP **4783** is allowed between LAN machines. Do not expose this service or the plaintext listener on port **4782** to the public Internet.
- If the server IP changes, rerun server setup with the new assigned IP, then rerun client setup on each client using that IP.
- The server CA private key and moderator token must remain private. The script transfers the public CA certificate, not the CA private key. SSH access to the server is privileged and should be restricted to trusted operators.
- The setup is for trusted private LANs only. Public-Internet deployment and mutual TLS for players remain unsupported/deferred.
