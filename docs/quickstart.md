# DARKPOOL Quickstart Guide

This guide provides a verified, end-to-end procedure to build, configure, run, and play **DARKPOOL** on Ubuntu Linux (22.04 / 24.04 LTS or WSL2).

---

## 1. Prerequisites and System Requirements

DARKPOOL requires:
- Linux kernel with `epoll` and UNIX domain sockets support (native Linux or WSL2)
- C++20 compliant compiler (`g++-11+` or `clang++-14+`)
- CMake 3.20 or newer
- OpenSSL 3.0 or newer (headers and libraries: `libssl-dev`)
- Python 3.8+ (for automated tests and evidence verification)

Install dependencies on Ubuntu / Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake libssl-dev git python3
```

---

## 2. Configure and Build a Release Binary

Clone the repository and build using CMake in Release mode:

```bash
# From the repository root:
cmake -B build -DCMAKE_BUILD_TYPE=Release -DDARKPOOL_WARNINGS_AS_ERRORS=ON
cmake --build build -j$(nproc)
```

Verify that all build artifacts were created:

```bash
ls -l build/darkpool_server build/darkpool_client
```

Run the automated test suite:

```bash
ctest --test-dir build --output-on-failure
```

---

## 3. Generate TLS 1.3 Server Certificate and Client Trust

DARKPOOL enforces strict TLS 1.3 transport security for all network communication. Both the server and clients strictly validate certificates, reject expired or improperly scoped credentials, and enforce Subject Alternative Name (SAN) matching against the target IP address.

> [!IMPORTANT]
> **SAN Matching Rule:** The IP address passed to the client CLI must exactly match an `IP:<address>` entry in the certificate's `subjectAltName` extension. If the client connects to `127.0.0.1` but the SAN is `192.168.1.50` (or vice-versa), the TLS handshake will strictly fail closed (`Identity mismatch`).

### Mode A: Localhost / Loopback Single-Host Setup (Verified Default)

Use this mode when running the server and client processes on the same Linux host or WSL2 environment:

```bash
mkdir -p /tmp/darkpool_certs
openssl req -x509 -newkey rsa:2048 -nodes \
  -keyout /tmp/darkpool_certs/server.key \
  -out /tmp/darkpool_certs/server.crt \
  -days 30 \
  -subj "/CN=127.0.0.1" \
  -addext "subjectAltName=IP:127.0.0.1"

# Restrict private key permissions to owner-only (mandatory 0600 check):
chmod 0600 /tmp/darkpool_certs/server.key
```

### Mode B: Private LAN Multi-Host Deployment

When hosting a game for players across a physical local area network:
1. Determine the server host's private IP address (e.g., `192.168.1.50` via `ip addr`).
2. Generate the certificate with the server's LAN IP in both the CN and the SAN:
   ```bash
   mkdir -p /tmp/darkpool_certs
   openssl req -x509 -newkey rsa:2048 -nodes \
     -keyout /tmp/darkpool_certs/server.key \
     -out /tmp/darkpool_certs/server.crt \
     -days 30 \
     -subj "/CN=192.168.1.50" \
     -addext "subjectAltName=IP:192.168.1.50"
   chmod 0600 /tmp/darkpool_certs/server.key
   ```
3. Configure the host firewall to permit inbound TCP traffic on port `4783` (e.g., `sudo ufw allow 4783/tcp`).
4. **Testing Status Notice:** Automated continuous integration tests and reproducible harnesses in this repository execute over loopback (`127.0.0.1`). Physical cross-machine multi-device testing was verified over LAN plaintext socket transport (DEC-10); running TLS 1.3 across separate physical machines requires network reachability, firewall clearance, and correct IP SAN configuration as described above.

### Step 3.2: Obtain SHA-256 Leaf Certificate Fingerprint

Obtain the 64-character hexadecimal SHA-256 fingerprint:

```bash
openssl x509 -noout -fingerprint -sha256 -in /tmp/darkpool_certs/server.crt | tr -d ':' | awk -F= '{print tolower($2)}'
```

Example output:
```text
c4b8e2194d216f40b2f6efba983a005bc1e71dd6cf3d15bba1a52044238e8ec2
```

#### Setting the Fingerprint in Separate Terminals:
- **Option 1 (Copy-Paste across any terminal or machine):** Copy the printed 64-character string and export it in each terminal:
  ```bash
  export FINGERPRINT="c4b8e2194d216f40b2f6efba983a005bc1e71dd6cf3d15bba1a52044238e8ec2"
  ```
- **Option 2 (Direct shell evaluation on the same host):**
  ```bash
  export FINGERPRINT=$(openssl x509 -noout -fingerprint -sha256 -in /tmp/darkpool_certs/server.crt | tr -d ':' | awk -F= '{print tolower($2)}')
  ```

---

## 4. Start the Authoritative Game Server

Set the moderator authentication secret (`DARKPOOL_MODERATOR_TOKEN`) and launch `darkpool_server`.

For **Loopback Mode**:
```bash
# Terminal 0 (Server Host):
export DARKPOOL_MODERATOR_TOKEN="admin-secret-token-2026"

./build/darkpool_server \
  --tls-bind-address 127.0.0.1 \
  --tls-port 4783 \
  --tls-cert /tmp/darkpool_certs/server.crt \
  --tls-key /tmp/darkpool_certs/server.key
```

For **LAN Mode**:
Bind to the LAN interface (or `0.0.0.0`):
```bash
./build/darkpool_server \
  --tls-bind-address 0.0.0.0 \
  --tls-port 4783 \
  --tls-cert /tmp/darkpool_certs/server.crt \
  --tls-key /tmp/darkpool_certs/server.key
```

---

## 5. Connect the Moderator Client and Create Match

In a new terminal (Terminal 1), launch the moderator CLI using the pinned leaf fingerprint:

```bash
# Terminal 1 (Moderator):
export DARKPOOL_MODERATOR_TOKEN="admin-secret-token-2026"
export FINGERPRINT="<your_64_char_fingerprint>"

./build/darkpool_client --tls --server-fingerprint "${FINGERPRINT}" 127.0.0.1 4783 moderator
```
*(Replace `127.0.0.1` with the server's LAN IP if playing across a LAN).*

Upon connection, the moderator prompt appears:
```text
[CONNECTED] Session established as moderator
[Moderator] >
```

### Create Protected Match with Player Admission Token

By default, DARKPOOL protects all matches against unauthorized access:
- **Default (Auto-Generated Secure Token):** Running `create 5` automatically generates a cryptographically random, unpredictable 128-bit hexadecimal admission token:
  ```text
  [Moderator] > create 5
  ```
  The server responds:
  ```text
  ============================================================
  [MATCH CREATED] Match #1 created successfully.
    Roster: 1 Insiders, 1 Investigators, 3 Employees
    Join Token  : e3020ff636ee1855a5bf9fdbfe45ad1e
    Waiting for 5 players to connect and run: join 1 e3020ff636ee1855a5bf9fdbfe45ad1e
  ============================================================
  [Moderator Match #1] >
  ```
- **Operator-Specified Custom Token:** You can provide your own admission token:
  ```text
  [Moderator] > create 5 match-token-2026
  ```
- **Explicit Open-Lobby Mode:** For casual development or test environments where admission authentication is unneeded, explicitly request an open lobby:
  ```text
  [Moderator] > create 5 open
  ```
  *(Or launch `darkpool_server --open-lobby` to make open lobbies the server-wide default).*

---

## 6. Connect Five Players

Open 5 separate terminals (Terminals 2 through 6) for Players 1 through 5. In each terminal, run:

```bash
export FINGERPRINT="<your_64_char_fingerprint>"
./build/darkpool_client --tls --server-fingerprint "${FINGERPRINT}" 127.0.0.1 4783 player
```

Once connected, join Match #1 in each player terminal using the match's admission token:

```text
[Player] > join 1 match-token-2026
```

> [!NOTE]
> If a player attempts to join without the token or with an invalid token, the server rejects the request with `[ERROR] invalid or missing match join token`. Client attempts to supply spoofed `seat` or `role` fields are rejected with `[ERROR] client identity fields are not accepted`.

Each player receives their server-assigned seat and confidential private role assignment:
- Exactly **1 Insider**: receives confidential intel instructions.
- Exactly **1 Investigator**: receives confidential investigation instructions.
- Exactly **3 Employees**: receives confidential market participation instructions.

---

## 7. Play Through a Complete 3-Round Match

### Step 7.1: Start the Match
Once all 5 players have joined, the moderator starts the match:

```text
[Moderator Match #1] > start
```

All connected clients receive the `[MATCH STARTED]` event and a market snapshot showing phase `AwaitingEvent`.

### Step 7.2: Round 1 Gameplay Loop

1. **Publish Event**: The moderator introduces a market event (impact between -2 and +2):
   ```text
   [Moderator Match #1] > event 0
   ```
   Match advances to **Discussion** phase.

2. **Insider Intel**: The Insider player uses `hint` to secretly view the upcoming impact:
   ```text
   [Player (Insider)] > hint
   ```
   *(Only the Insider receives the private event impact preview).*

3. **Open Trading**: The moderator opens trading:
   ```text
   [Moderator Match #1] > trading
   ```
   Players submit trade orders:
   ```text
   [Player 1] > buy 2
   [Player 2] > sell 1
   [Player 3] > hold
   [Player 4] > buy 1
   [Player 5] > hold
   ```

4. **Resolve Market**: The moderator closes the trading window and executes market orders:
   ```text
   [Moderator Match #1] > market
   ```
   Closing stock price, company health, and reserve shares update publicly. Match advances to **Clue** phase.

5. **Investigator Clue**: The Investigator player secretly investigates a suspect player ID (e.g. Player 2):
   ```text
   [Player (Investigator)] > clue 2
   ```
   *(The private result confirms whether Player 2 is the Insider; results are strictly hidden from other players).*

6. **Open Voting**: The moderator opens elimination voting:
   ```text
   [Moderator Match #1] > voting
   ```
   Players cast votes (e.g. target a player ID or `abstain`):
   ```text
   [Player 1] > vote 2
   [Player 2] > vote abstain
   [Player 3] > vote 2
   [Player 4] > vote 2
   [Player 5] > vote 2
   ```

7. **Resolve Votes**: The moderator tallies votes:
   ```text
   [Moderator Match #1] > resolve
   ```
   If the majority votes for Player 2 and Player 2 is the Insider, the match concludes immediately with **EmployeesWin**! If players abstain or eliminate an Employee, the round advances to Round 2.

### Step 7.3: Rounds 2 and 3
Repeat the Event $\rightarrow$ Discussion $\rightarrow$ Trading $\rightarrow$ Market $\rightarrow$ Clue $\rightarrow$ Voting sequence for Rounds 2 and 3.

---

## 8. Terminal Victory Resolution

A match concludes with exactly one authoritative terminal victory state:
- **`EmployeesWin`**: All Insiders are eliminated by vote before company health reaches 0.
- **`InsiderWin`**: Company health drops to 0, or at least one Insider survives through the end of Round 3.

Both moderator and player terminals display the final victory announcement:
```text
************************************************************
[GAME OVER] The Insider survived! The INSIDER WINS!
************************************************************
```

Post-match commands are rejected by the server, preserving the terminal game outcome.

---

## 9. Start Another Match or Shut Down Safely

- To start another match, the moderator runs `create 5` again from the existing moderator session or connects a new session.
- To disconnect any client cleanly, enter `quit` or `exit`.
- To stop the server process cleanly, send `SIGINT` (`Ctrl+C`) or `SIGTERM`. The server flushes pending queues, releases all sockets, unlinks temporary UNIX domain sockets, and exits with code 0.

---

## 10. Automated End-to-End Verification Tool

You can verify the entire 5-player match automatically using the included headless acceptance runner:

```bash
python3 tools/test_playable_match.py build
```

This runs a full simulated 5-player game over TLS 1.3, testing every phase, role delivery, private information secrecy, invalid command isolation, and clean process termination.
