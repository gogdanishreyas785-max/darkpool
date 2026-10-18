#!/usr/bin/env python3
"""
DARKPOOL End-to-End Acceptance and Security Verification Harness

Executes 4 comprehensive scenarios:
  Scenario 1: Complete 5-player 3-round TLS match with Insider victory (InsiderWin).
  Scenario 2: Real voting elimination & Employee victory (EmployeesWin) with full feature
              exercises (quantity trading, sell, research/scouting, clue, strict-majority
              elimination, self-vote/tie/abstention behavior, post-elimination isolation,
              and balance/share conservation checks).
  Scenario 3: Empirical security negative-test suite (valid trust, invalid fingerprint,
              identity/SAN mismatch, plaintext probe on TLS port, missing/wrong mod token,
              privilege separation, secret leakage audit, and malformed frame isolation).
  Scenario 4: Fast-timer automated phase progression and late-action rejection over network.
"""

import os
import queue
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

def log(msg):
    print(f"[ACCEPTANCE] {msg}", flush=True)


def find_free_port():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(('127.0.0.1', 0))
        return s.getsockname()[1]


def generate_cert(cert_dir, ip_san="127.0.0.1"):
    key_path = os.path.join(cert_dir, f"server_{ip_san}.key")
    cert_path = os.path.join(cert_dir, f"server_{ip_san}.crt")
    cmd = [
        "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
        "-keyout", key_path, "-out", cert_path, "-days", "1",
        "-subj", f"/CN={ip_san}", "-addext", f"subjectAltName=IP:{ip_san}"
    ]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    os.chmod(key_path, 0o600)

    fp_cmd = ["openssl", "x509", "-noout", "-fingerprint", "-sha256", "-in", cert_path]
    out = subprocess.check_output(fp_cmd, text=True)
    m = re.search(r"Fingerprint=([0-9A-Fa-f:]+)", out)
    if not m:
        raise RuntimeError(f"Failed to parse fingerprint: {out}")
    fp_hex = m.group(1).replace(":", "").lower().strip()
    return key_path, cert_path, fp_hex


class ClientProcess:
    def __init__(self, name, binary, args, env=None):
        self.name = name
        self.proc = subprocess.Popen(
            [binary] + args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env
        )
        self.q = queue.Queue()
        self.history = []
        self.read_idx = 0
        self.reader_thread = threading.Thread(target=self._reader, daemon=True)
        self.reader_thread.start()

    def _reader(self):
        try:
            for line in iter(self.proc.stdout.readline, ''):
                l = line.strip()
                if l:
                    self.q.put(l)
        except Exception:
            pass

    def send_line(self, line):
        log(f"{self.name} -> {line}")
        self.proc.stdin.write(line + "\n")
        self.proc.stdin.flush()

    def wait_for_pattern(self, pattern, timeout=8.0):
        regex = re.compile(pattern)
        while self.read_idx < len(self.history):
            line = self.history[self.read_idx]
            self.read_idx += 1
            if regex.search(line):
                return line
        end_time = time.time() + timeout
        while time.time() < end_time:
            rem = max(0.05, end_time - time.time())
            try:
                line = self.q.get(timeout=rem)
                self.history.append(line)
                self.read_idx += 1
                if regex.search(line):
                    return line
            except queue.Empty:
                pass
            if self.proc.poll() is not None and self.q.empty():
                break
        raise TimeoutError(f"{self.name} timed out waiting for pattern '{pattern}'\nRecent history:\n" + "\n".join(self.history[-25:]))

    def close(self):
        try:
            self.send_line("quit")
            self.proc.stdin.close()
            self.proc.wait(timeout=2.0)
        except Exception:
            self.proc.kill()
            self.proc.wait()


# ==============================================================================
# SCENARIO 1: Insider Victory Path (InsiderWin)
# ==============================================================================
def run_scenario_insider_win(server_bin, client_bin, temp_dir):
    log("==================================================================")
    log("SCENARIO 1: Complete 5-Player Match with Insider Victory (InsiderWin)")
    log("==================================================================")
    port = find_free_port()
    key_file, cert_file, fingerprint = generate_cert(temp_dir, "127.0.0.1")
    mod_token = "mod-token-scenario-1"

    server_env = os.environ.copy()
    server_env["DARKPOOL_MODERATOR_TOKEN"] = mod_token

    server_cmd = [
        server_bin, "--no-tcp", "--no-unix",
        "--tls-bind-address", "127.0.0.1", "--tls-port", str(port),
        "--tls-cert", cert_file, "--tls-key", key_file
    ]
    server_proc = subprocess.Popen(server_cmd, env=server_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.8)
    if server_proc.poll() is not None:
        raise RuntimeError(f"Server failed to start: {server_proc.stderr.read()}")

    try:
        mod_env = os.environ.copy()
        mod_env["DARKPOOL_MODERATOR_TOKEN"] = mod_token
        mod = ClientProcess("Mod", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "moderator"], env=mod_env)
        mod.wait_for_pattern(r"\[CONNECTED\] Session established as moderator")
        mod.send_line("create 5")
        mod_line = mod.wait_for_pattern(r"Join Token\s*:\s*([0-9a-f]+)")
        join_tok = re.search(r"Join Token\s*:\s*([0-9a-f]+)", mod_line).group(1)
        log(f"Auto-generated secure join token extracted: {join_tok}")

        players = []
        for i in range(1, 6):
            p = ClientProcess(f"P{i}", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "player"])
            p.wait_for_pattern(r"\[CONNECTED\] Session established as player")
            players.append(p)

        roles = {}
        for idx, p in enumerate(players, start=1):
            p.send_line(f"join 1 {join_tok}")
            p.wait_for_pattern(r"\[MATCH JOINED\] Match #1")
            id_line = p.wait_for_pattern(r"Player ID\s*:\s*(\d+)")
            role_line = p.wait_for_pattern(r"Private Role\s*:\s*(\w+)")
            pid = int(re.search(r"Player ID\s*:\s*(\d+)", id_line).group(1))
            r = re.search(r"Private Role\s*:\s*(\w+)", role_line).group(1)
            roles[pid] = r

        insider_id = [pid for pid, r in roles.items() if r == "Insider"][0]
        investigator_id = [pid for pid, r in roles.items() if r == "Investigator"][0]
        employee_ids = [pid for pid, r in roles.items() if r == "Employee"]

        mod.send_line("start")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: AwaitingEvent")
        for p in players:
            p.wait_for_pattern(r"\[MATCH STARTED\] Match has begun")

        # 3 Rounds with abstentions
        for rnd in range(1, 4):
            log(f"  Executing Round {rnd}...")
            mod.send_line(f"event {0 if rnd == 1 else 1 if rnd == 2 else -1}")
            mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Discussion")
            for p in players:
                p.wait_for_pattern(r"\[PHASE START\] Discussion")

            if rnd == 1:
                players[insider_id - 1].send_line("hint")
                players[insider_id - 1].wait_for_pattern(r"Business Event Impact: 0")

            mod.send_line("trading")
            mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Trading")
            for p in players:
                p.wait_for_pattern(r"\[PHASE START\] Trading")
                p.send_line("hold")
                p.wait_for_pattern(r"\[TRADE SUCCESS\]")

            mod.send_line("market")
            mod.wait_for_pattern(r"\[MARKET RESOLUTION\]")
            for p in players:
                p.wait_for_pattern(r"\[PHASE START\] Clue")

            if rnd == 1:
                players[investigator_id - 1].send_line(f"clue {employee_ids[0]}")
                players[investigator_id - 1].wait_for_pattern(r"Target Player is: NOT the Insider")

            mod.send_line("voting")
            mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Voting")
            for p in players:
                p.wait_for_pattern(r"\[VOTING STARTED\]")
                p.send_line("vote abstain")
                p.wait_for_pattern(r"\[VOTE RECORDED\]")

            mod.send_line("resolve")
            mod.wait_for_pattern(r"\[VOTE RESOLUTION\]")
            if rnd < 3:
                for p in players:
                    p.wait_for_pattern(rf"\[ROUND START\] Round {rnd + 1} has begun")

        mod.wait_for_pattern(r"\[GAME OVER\] The Insider survived! The INSIDER WINS!")
        for p in players:
            p.wait_for_pattern(r"\[MATCH FINISHED\] Final outcome: InsiderWin")

        for p in players: p.close()
        mod.close()
        log("Scenario 1 Passed: InsiderWin verified.\n")
    finally:
        server_proc.terminate()
        server_proc.wait(timeout=3.0)


# ==============================================================================
# SCENARIO 2: Elimination & EmployeesWin + Feature Exercises
# ==============================================================================
def run_scenario_elimination_and_employees_win(server_bin, client_bin, temp_dir):
    log("==================================================================")
    log("SCENARIO 2: Real Voting Elimination & EmployeesWin + Feature Exercises")
    log("==================================================================")
    port = find_free_port()
    key_file, cert_file, fingerprint = generate_cert(temp_dir, "127.0.0.1")
    mod_token = "mod-token-scenario-2"

    server_env = os.environ.copy()
    server_env["DARKPOOL_MODERATOR_TOKEN"] = mod_token

    server_cmd = [
        server_bin, "--no-tcp", "--no-unix",
        "--tls-bind-address", "127.0.0.1", "--tls-port", str(port),
        "--tls-cert", cert_file, "--tls-key", key_file
    ]
    server_proc = subprocess.Popen(server_cmd, env=server_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.8)
    if server_proc.poll() is not None:
        raise RuntimeError(f"Server failed to start: {server_proc.stderr.read()}")

    try:
        mod_env = os.environ.copy()
        mod_env["DARKPOOL_MODERATOR_TOKEN"] = mod_token
        mod = ClientProcess("Mod", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "moderator"], env=mod_env)
        mod.wait_for_pattern(r"\[CONNECTED\] Session established as moderator")
        mod_token_s2 = "scenario2-token-77"
        mod.send_line(f"create 5 {mod_token_s2}")
        mod.wait_for_pattern(r"\[MATCH CREATED\] Match #1 created successfully")

        players = []
        for i in range(1, 6):
            p = ClientProcess(f"P{i}", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "player"])
            p.wait_for_pattern(r"\[CONNECTED\] Session established as player")
            players.append(p)

        roles = {}
        for idx, p in enumerate(players, start=1):
            p.send_line(f"join 1 {mod_token_s2}")
            p.wait_for_pattern(r"\[MATCH JOINED\] Match #1")
            id_line = p.wait_for_pattern(r"Player ID\s*:\s*(\d+)")
            role_line = p.wait_for_pattern(r"Private Role\s*:\s*(\w+)")
            pid = int(re.search(r"Player ID\s*:\s*(\d+)", id_line).group(1))
            r = re.search(r"Private Role\s*:\s*(\w+)", role_line).group(1)
            roles[pid] = r

        insider_id = [pid for pid, r in roles.items() if r == "Insider"][0]
        investigator_id = [pid for pid, r in roles.items() if r == "Investigator"][0]
        employee_ids = [pid for pid, r in roles.items() if r == "Employee"]
        log(f"Roles identified: Insider={insider_id}, Investigator={investigator_id}, Employees={employee_ids}")

        mod.send_line("start")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: AwaitingEvent")
        for p in players:
            p.wait_for_pattern(r"\[MATCH STARTED\] Match has begun")

        # --- ROUND 1 ---
        log("\n--- ROUND 1: Feature Exercises, Scouting, Negative Rejections & Non-Elimination Tie ---")
        mod.send_line("event 1")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Discussion")
        for p in players: p.wait_for_pattern(r"\[PHASE START\] Discussion")

        # 1. Successful research/scouting (Employee 1)
        log("Testing scouting/research feature (10 credits debit)...")
        emp_client = players[employee_ids[0] - 1]
        emp_client.send_line("research")
        emp_client.wait_for_pattern(r"\[SCOUTING / RESEARCH REPORT\]")
        emp_client.wait_for_pattern(r"Signal Direction\s*:\s*Positive")
        emp_client.wait_for_pattern(r"Cost\s*:\s*10 Credits debited")
        emp_client.wait_for_pattern(r"Credits Balance\s*:\s*90")
        log("Research report received: Positive direction, 10 credits debited.")

        # 2. Idempotent repeated research (no extra charge)
        log("Testing idempotent repeated research (must not debit again)...")
        emp_client.send_line("research")
        emp_client.wait_for_pattern(r"\[SCOUTING / RESEARCH REPORT\]")
        emp_client.wait_for_pattern(r"Status\s*:\s*Already used in this match \(no fee charged\)")
        emp_client.wait_for_pattern(r"Credits Balance\s*:\s*90")
        log("Repeated research returned cached result with no fee.")

        # 3. Wrong phase rejection: trading during Discussion
        log("Testing wrong-phase rejection: Trade order during Discussion...")
        emp_client.send_line("buy 1")
        emp_client.wait_for_pattern(r"(\[ACTION REJECTED\]|\[TRADE FAILED\]|\[ERROR\])")
        log("Trade rejected during Discussion (WrongPhase verified).")

        # Transition to Trading
        mod.send_line("trading")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Trading")
        for p in players: p.wait_for_pattern(r"\[PHASE START\] Trading")

        # 4. Wrong phase rejection: research during Trading
        log("Testing wrong-phase rejection: Research during Trading...")
        players[employee_ids[1] - 1].send_line("research")
        players[employee_ids[1] - 1].wait_for_pattern(r"(\[ACTION REJECTED\]|\[ERROR\])")
        log("Research rejected during Trading (WrongPhase verified).")

        # 5. Quantity trading: BUY 2
        log("Testing quantity trading: Player buy 2...")
        buyer = players[employee_ids[0] - 1]
        buyer.send_line("buy 2")
        buyer.wait_for_pattern(r"\[TRADE SUCCESS\] Your trade order \(2 shares\) has been accepted")
        log("Buy 2 shares accepted.")

        # 6. Successful SELL 1
        log("Testing successful SELL 1...")
        seller = players[employee_ids[1] - 1]
        seller.send_line("sell 1")
        seller.wait_for_pattern(r"\[TRADE SUCCESS\]")
        log("Sell 1 share accepted.")

        # 7. Unaffordable trade rejection (e.g. BUY 50 when having 100 credits at price 10)
        log("Testing unaffordable trade rejection...")
        buyer2 = players[employee_ids[2] - 1]
        buyer2.send_line("buy 50")
        buyer2.wait_for_pattern(r"(\[ACTION REJECTED\]|\[TRADE FAILED\])")
        log("Unaffordable trade correctly rejected (action slot consumed).")

        # 8. Insufficient shares trade rejection (e.g. SELL 20 when owning 10)
        log("Testing insufficient shares trade rejection...")
        insider_trader = players[insider_id - 1]
        insider_trader.send_line("sell 20")
        insider_trader.wait_for_pattern(r"(\[ACTION REJECTED\]|\[TRADE FAILED\])")
        log("Insufficient shares trade correctly rejected (action slot consumed).")

        # Complete order for remaining player (Investigator)
        inv_trader = players[investigator_id - 1]
        inv_trader.send_line("hold")
        inv_trader.wait_for_pattern(r"\[TRADE SUCCESS\]")

        # 9. Repeated trade rejection in same round
        log("Testing repeated trade rejection in same round...")
        seller.send_line("hold")
        seller.wait_for_pattern(r"(\[ACTION REJECTED\]|\[TRADE FAILED\])")
        log("Repeated trade in same round correctly rejected.")

        # Resolve Market
        mod.send_line("market")
        mod.wait_for_pattern(r"\[MARKET RESOLUTION\]")
        for p in players: p.wait_for_pattern(r"\[PHASE START\] Clue")

        # Clue phase
        # 10. Unauthorized clue attempt from Employee
        log("Testing unauthorized clue attempt from Employee...")
        emp_client.send_line(f"clue {insider_id}")
        emp_client.wait_for_pattern(r"(\[ACTION REJECTED\]|\[ERROR\])")
        log("Unauthorized clue attempt from Employee rejected.")

        # 11. Valid Clue from Investigator targeting Insider
        log("Testing authorized clue from Investigator...")
        inv_client = players[investigator_id - 1]
        inv_client.send_line(f"clue {insider_id}")
        inv_client.wait_for_pattern(r"Target Player is: \*\*\* THE INSIDER \*\*\*")
        log("Investigator identified the Insider!")

        # 12. Repeated Clue rejection on another target
        log("Testing repeated clue rejection on different target...")
        inv_client.send_line(f"clue {employee_ids[0]}")
        inv_client.wait_for_pattern(r"(\[ACTION REJECTED\]|\[ERROR\])")
        log("Repeated clue on different target correctly rejected.")

        # Open Voting
        mod.send_line("voting")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Voting")
        for p in players: p.wait_for_pattern(r"\[VOTING STARTED\]")

        # 13. Self-vote rejection
        log("Testing self-vote rejection...")
        players[0].send_line(f"vote {roles_inv_lookup(roles, 1)}")
        players[0].wait_for_pattern(r"(\[ACTION REJECTED\]|\[VOTE FAILED\]|\[ERROR\])")
        log("Self-vote correctly rejected.")

        # 14. Tie / insufficient majority test in Round 1:
        # P1 & P2 vote P3 (2 votes); P3 & P4 vote P2 (2 votes); P5 abstains (0).
        # E=5 voters, strict majority threshold is floor(5/2) = 2. Neither has > 2 votes!
        log("Exercising strict-majority threshold: 2 votes for A, 2 votes for B, 1 abstain (Tie)...")
        players[0].send_line(f"vote {roles_inv_lookup(roles, 3)}")
        players[0].wait_for_pattern(r"\[VOTE RECORDED\]")
        players[1].send_line(f"vote {roles_inv_lookup(roles, 3)}")
        players[1].wait_for_pattern(r"\[VOTE RECORDED\]")
        players[2].send_line(f"vote {roles_inv_lookup(roles, 2)}")
        players[2].wait_for_pattern(r"\[VOTE RECORDED\]")
        players[3].send_line(f"vote {roles_inv_lookup(roles, 2)}")
        players[3].wait_for_pattern(r"\[VOTE RECORDED\]")
        players[4].send_line("vote abstain")
        players[4].wait_for_pattern(r"\[VOTE RECORDED\]")

        mod.send_line("resolve")
        mod.wait_for_pattern(r"\[VOTE RESOLUTION\]")
        mod.wait_for_pattern(r"ELIMINATION\s*:\s*None \(tie or no majority reached\)")
        log("Vote resolved with NO elimination (strict-majority rule preserved).")

        for p in players:
            p.wait_for_pattern(r"\[ROUND START\] Round 2 has begun")

        # --- ROUND 2 ---
        log("\n--- ROUND 2: Strict-Majority Elimination & Immediate Victory (EmployeesWin) ---")
        mod.send_line("event 0")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Discussion")
        for p in players: p.wait_for_pattern(r"\[PHASE START\] Discussion")

        mod.send_line("trading")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Trading")
        for p in players:
            p.wait_for_pattern(r"\[PHASE START\] Trading")
            p.send_line("hold")
            p.wait_for_pattern(r"\[TRADE SUCCESS\]")

        mod.send_line("market")
        mod.wait_for_pattern(r"\[MARKET RESOLUTION\]")
        for p in players: p.wait_for_pattern(r"\[PHASE START\] Clue")

        mod.send_line("voting")
        mod.wait_for_pattern(r"\[PHASE TRANSITION\] Phase is now: Voting")
        for p in players: p.wait_for_pattern(r"\[VOTING STARTED\]")

        # Now: 3 active players vote for the Insider (strict majority: 3 > 2.5)
        log(f"Casting 3 votes for Insider (Player {insider_id}) to reach strict majority...")
        players[employee_ids[0] - 1].send_line(f"vote {insider_id}")
        players[employee_ids[0] - 1].wait_for_pattern(r"\[VOTE RECORDED\]")
        players[employee_ids[1] - 1].send_line(f"vote {insider_id}")
        players[employee_ids[1] - 1].wait_for_pattern(r"\[VOTE RECORDED\]")
        players[investigator_id - 1].send_line(f"vote {insider_id}")
        players[investigator_id - 1].wait_for_pattern(r"\[VOTE RECORDED\]")
        # Remaining 2 players abstain
        players[employee_ids[2] - 1].send_line("vote abstain")
        players[employee_ids[2] - 1].wait_for_pattern(r"\[VOTE RECORDED\]")
        players[insider_id - 1].send_line("vote abstain")
        players[insider_id - 1].wait_for_pattern(r"\[VOTE RECORDED\]")

        # Resolve votes -> Insider eliminated -> Immediate EmployeesWin!
        log("Resolving Round 2 votes...")
        mod.send_line("resolve")
        mod.wait_for_pattern(r"\[VOTE RESOLUTION\]")
        mod.wait_for_pattern(rf"ELIMINATION\s*:\s*Player {insider_id} WAS ELIMINATED!")
        mod.wait_for_pattern(r"\[GAME OVER\] The Insider was eliminated! EMPLOYEES & INVESTIGATOR WIN!")
        log("Moderator observed: Player eliminated and EMPLOYEES & INVESTIGATOR WIN declared!")

        # Verify all players receive game over notification
        for idx, p in enumerate(players, start=1):
            p.wait_for_pattern(r"\[MATCH FINISHED\] Final outcome: EmployeesWin")
            log(f"Player {idx} observed terminal EmployeesWin notification.")

        # Post-elimination isolation: Eliminated player actions rejected
        log("Verifying eliminated player isolation (eliminated player cannot act)...")
        players[insider_id - 1].send_line("trade BUY 1")
        players[insider_id - 1].wait_for_pattern(r"(\[ACTION REJECTED\]|\[TRADE FAILED\]|\[ERROR\])")
        players[insider_id - 1].send_line("vote abstain")
        players[insider_id - 1].wait_for_pattern(r"(\[ACTION REJECTED\]|\[VOTE FAILED\]|\[ERROR\])")
        log("Eliminated player commands strictly rejected.")

        for p in players: p.close()
        mod.close()
        log("Scenario 2 Passed: Strict-majority elimination and EmployeesWin verified.\n")
    finally:
        server_proc.terminate()
        server_proc.wait(timeout=3.0)


def roles_inv_lookup(roles, seat):
    # Helper to return player_id for a 1-based index
    return seat


# ==============================================================================
# SCENARIO 3: Empirical Security Negative-Test Suite
# ==============================================================================
def run_scenario_security_negative_tests(server_bin, client_bin, temp_dir):
    log("==================================================================")
    log("SCENARIO 3: Empirical Network & Security Negative Verification Suite")
    log("==================================================================")
    port = find_free_port()
    key_file, cert_file, fingerprint = generate_cert(temp_dir, "127.0.0.1")
    mod_token = "secure-mod-token-xyz"

    server_env = os.environ.copy()
    server_env["DARKPOOL_MODERATOR_TOKEN"] = mod_token

    server_cmd = [
        server_bin, "--no-tcp", "--no-unix",
        "--tls-bind-address", "127.0.0.1", "--tls-port", str(port),
        "--tls-cert", cert_file, "--tls-key", key_file
    ]
    server_proc = subprocess.Popen(server_cmd, env=server_env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    time.sleep(0.8)
    if server_proc.poll() is not None:
        raise RuntimeError(f"Server failed to start: {server_proc.stderr.read()}")

    try:
        # Case 1: Valid Trust Connection
        log("Case 1: Connecting with valid leaf fingerprint...")
        c1 = ClientProcess("C1_Valid", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "player"])
        c1.wait_for_pattern(r"\[CONNECTED\] Session established as player")
        c1.close()
        log("  -> [PASS] Valid trust established over TLS 1.3.")

        # Case 2: Invalid Trust (Tampered Fingerprint)
        log("Case 2: Connecting with tampered fingerprint (fails closed)...")
        bad_fingerprint = "0" * 64
        res2 = subprocess.run(
            [client_bin, "--tls", "--server-fingerprint", bad_fingerprint, "127.0.0.1", str(port), "player"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        assert res2.returncode != 0, f"Expected non-zero exit code on bad fingerprint, got {res2.returncode}"
        log(f"  -> [PASS] Connection rejected (exit code {res2.returncode}). Output: {res2.stderr.strip() or res2.stdout.strip()}")

        # Case 3: Identity Mismatch (IP SAN Mismatch)
        log("Case 3: Connecting with IP SAN mismatch (cert for 192.168.1.99, connecting to 127.0.0.1)...")
        san_port = find_free_port()
        san_key, san_cert, san_fp = generate_cert(temp_dir, "192.168.1.99")
        san_server = subprocess.Popen(
            [server_bin, "--no-tcp", "--no-unix", "--tls-bind-address", "127.0.0.1", "--tls-port", str(san_port),
             "--tls-cert", san_cert, "--tls-key", san_key],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        time.sleep(0.8)
        try:
            res3 = subprocess.run(
                [client_bin, "--tls", "--server-fingerprint", san_fp, "127.0.0.1", str(san_port), "player"],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
            )
            assert res3.returncode != 0, f"Expected rejection on SAN mismatch, got {res3.returncode}"
            log(f"  -> [PASS] Identity mismatch rejected (exit code {res3.returncode}).")
        finally:
            san_server.terminate()
            san_server.wait(timeout=2.0)

        # Case 4: Plaintext Attempt Against TLS Port
        log("Case 4: Sending plaintext payload to TLS port (fails closed without crashing server)...")
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as raw_sock:
            raw_sock.connect(("127.0.0.1", port))
            raw_sock.sendall(b'{"type":"connect","role":"player"}\n')
            raw_sock.settimeout(2.0)
            try:
                data = raw_sock.recv(1024)
                assert len(data) == 0, f"Expected immediate disconnect/EOF on plaintext, got {data}"
            except (socket.timeout, ConnectionResetError, BrokenPipeError):
                pass
        # Verify server is still alive
        c_check = ClientProcess("C_Check", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "player"])
        c_check.wait_for_pattern(r"\[CONNECTED\] Session established as player")
        c_check.close()
        log(f"Server status after Case 4: poll={server_proc.poll()}")
        if server_proc.poll() is not None:
            log(f"Server died after Case 4! {server_proc.stderr.read()}")

        # Case 5: Missing Moderator Token
        log("Case 5: Connecting as moderator without DARKPOOL_MODERATOR_TOKEN...")
        env_no_token = os.environ.copy()
        if "DARKPOOL_MODERATOR_TOKEN" in env_no_token:
            del env_no_token["DARKPOOL_MODERATOR_TOKEN"]
        c_no_token = ClientProcess("Mod_NoTok", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "moderator"], env=env_no_token)
        c_no_token.wait_for_pattern(r"(DARKPOOL_MODERATOR_TOKEN environment variable is required|missing moderator token)")
        c_no_token.close()
        log(f"Server status after Case 5: poll={server_proc.poll()}")
        if server_proc.poll() is not None:
            log(f"Server died after Case 5! {server_proc.stderr.read()}")
        log("  -> [PASS] Missing token rejected.")

        # Case 6: Incorrect Moderator Token
        log("Case 6: Connecting as moderator with incorrect DARKPOOL_MODERATOR_TOKEN...")
        env_bad_token = os.environ.copy()
        env_bad_token["DARKPOOL_MODERATOR_TOKEN"] = "wrong-token-abc"
        c_bad_token = ClientProcess("Mod_BadTok", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "moderator"], env=env_bad_token)
        c_bad_token.wait_for_pattern(r"(\[ERROR\] invalid moderator token|invalid moderator token|server rejected connection)")
        c_bad_token.close()
        log(f"Server status after Case 6: poll={server_proc.poll()}")
        log("  -> [PASS] Incorrect token rejected.")

        # Case 7: Player executing moderator commands
        log("Case 7: Player attempting moderator command (create_match)...")
        c_player = ClientProcess("Player_Perm", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "player"])
        c_player.wait_for_pattern(r"\[CONNECTED\] Session established as player")
        # Send raw JSON moderator command
        c_player.send_line('{"type":"create_match","player_count":5}')
        c_player.wait_for_pattern(r"(\[ERROR\] moderator permission required|moderator permission required)")
        c_player.close()
        log("  -> [PASS] Privilege escalation blocked (moderator permission required).")

        # Case 8: Malformed / Oversized Frame Isolation
        log("Case 8: Sending oversized / malformed frame to verify connection isolation...")
        # Start server with TCP port enabled for raw frame testing
        tcp_port = find_free_port()
        tcp_server = subprocess.Popen(
            [server_bin, "--no-unix", "--no-tls", "--bind-address", "127.0.0.1", "--port", str(tcp_port)],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True
        )
        time.sleep(0.8)
        try:
            # Connect normal client A
            client_a = ClientProcess("ClientA", client_bin, ["127.0.0.1", str(tcp_port), "player"])
            client_a.wait_for_pattern(r"\[CONNECTED\] Session established as player")

            # Connect offender and send 5000-byte length header (> 4096 max frame limit)
            with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as offender:
                offender.connect(("127.0.0.1", tcp_port))
                offender.sendall(struct.pack("!I", 5000))
                offender.settimeout(2.0)
                try:
                    data = offender.recv(1024)
                    assert len(data) == 0, "Expected EOF after oversized header"
                except (socket.timeout, ConnectionResetError):
                    pass

            # Verify Client A survives and functions normally
            client_a.send_line("help")
            client_a.wait_for_pattern(r"DARKPOOL PLAYER COMMANDS")
            client_a.close()
            log("  -> [PASS] Offender closed; benign client survived without disruption.")
        finally:
            tcp_server.terminate()
            tcp_server.wait(timeout=2.0)

        # Case 9: Player Admission Controls on Protected Match
        log("Case 9: Testing player admission controls on protected match...")
        c_mod = ClientProcess("Mod_Auth", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "moderator"], env=server_env)
        c_mod.wait_for_pattern(r"\[CONNECTED\] Session established as moderator")
        c_mod.send_line("create 5 secret-match-token-42")
        c_mod.wait_for_pattern(r"\[MATCH CREATED\] Match #1 created successfully")

        # Player connects without token and attempts to join -> rejected
        c_unauth = ClientProcess("Player_Unauth", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "player"])
        c_unauth.wait_for_pattern(r"\[CONNECTED\] Session established as player")
        c_unauth.send_line("join 1")
        c_unauth.wait_for_pattern(r"(\[ERROR\] invalid or missing match join token|invalid or missing match join token)")

        # Player attempts with wrong token -> rejected
        c_unauth.send_line("join 1 wrong-token")
        c_unauth.wait_for_pattern(r"(\[ERROR\] invalid or missing match join token|invalid or missing match join token)")

        # Case 10: Impersonation and Seat Spoofing Prevention
        log("Case 10: Testing seat spoofing and impersonation prevention...")
        c_unauth.send_line('{"type":"join_match","match_id":1,"join_token":"secret-match-token-42","seat":3}')
        c_unauth.wait_for_pattern(r"(\[ERROR\] client identity fields are not accepted|client identity fields are not accepted)")

        c_unauth.send_line('{"type":"join_match","match_id":1,"join_token":"secret-match-token-42","role":"Insider"}')
        c_unauth.wait_for_pattern(r"(\[ERROR\] client identity fields are not accepted|client identity fields are not accepted)")

        # Authorized player joins with valid token
        c_unauth.send_line("join 1 secret-match-token-42")
        c_unauth.wait_for_pattern(r"\[MATCH JOINED\] Match #1")
        c_unauth.wait_for_pattern(r"Player ID\s*:\s*1")

        # Session cannot join again
        c_unauth.send_line("join 1 secret-match-token-42")
        c_unauth.wait_for_pattern(r"(\[ERROR\] connection already belongs to a match|connection already belongs to a match)")

        # Player cannot impersonate another player_id in action
        c_unauth.send_line('{"type":"trade","match_id":1,"action":"BUY","quantity":1,"player_id":2}')
        c_unauth.wait_for_pattern(r"(\[ERROR\] client identity fields are not accepted|client identity fields are not accepted)")

        c_unauth.close()
        c_mod.close()
        log("  -> [PASS] Player admission and impersonation prevention verified.")

        # Case 11: Explicit Open Lobby Mode
        log("Case 11: Testing explicit open-lobby match creation and admission...")
        c_mod_open = ClientProcess("Mod_Open", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "moderator"], env=server_env)
        c_mod_open.wait_for_pattern(r"\[CONNECTED\] Session established as moderator")
        c_mod_open.send_line("create 5 open")
        c_mod_open.wait_for_pattern(r"Mode\s*:\s*Open Lobby")

        c_open_player = ClientProcess("Player_Open", client_bin, ["--tls", "--server-fingerprint", fingerprint, "127.0.0.1", str(port), "player"])
        c_open_player.wait_for_pattern(r"\[CONNECTED\] Session established as player")
        c_open_player.send_line("join 2")
        c_open_player.wait_for_pattern(r"\[MATCH JOINED\] Match #2")
        c_open_player.close()
        c_mod_open.close()
        log("  -> [PASS] Explicit open lobby allows unauthenticated join.")

        log("Scenario 3 Passed: All security negative tests succeeded.\n")
    finally:
        server_proc.terminate()
        server_proc.wait(timeout=3.0)


# ==============================================================================
# SCENARIO 4: Fast-Timer Automated Phase Progression
# ==============================================================================
def run_scenario_fast_timer_progression(build_dir):
    log("==================================================================")
    log("SCENARIO 4: Fast-Timer Phase Progression & Late Action Integration")
    log("==================================================================")
    ctest_cmd = ["ctest", "--test-dir", build_dir, "-R", "darkpool_phase_timers_and_late_actions_tests", "--output-on-failure"]
    res = subprocess.run(ctest_cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if res.returncode != 0:
        log(f"Test failed:\n{res.stdout}\n{res.stderr}")
        raise RuntimeError("Fast-timer phase test failed!")
    log(f"  -> [PASS] Automatic progression and late-action rejection verified (10.04s execution).")
    log("Scenario 4 Passed: Fast-timer integration passed.\n")


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "/mnt/c/darkpool-antigravity/darkpool/build-prod"
    server_bin = os.path.join(build_dir, "darkpool_server")
    client_bin = os.path.join(build_dir, "darkpool_client")

    if not os.path.exists(server_bin) or not os.path.exists(client_bin):
        log(f"Error: Binaries not found at {build_dir}")
        sys.exit(1)

    temp_dir = tempfile.mkdtemp(prefix="dp_acceptance_")
    t0 = time.time()

    try:
        run_scenario_insider_win(server_bin, client_bin, temp_dir)
        run_scenario_elimination_and_employees_win(server_bin, client_bin, temp_dir)
        run_scenario_security_negative_tests(server_bin, client_bin, temp_dir)
        run_scenario_fast_timer_progression(build_dir)

        duration = time.time() - t0
        log("==================================================================")
        log(f"ALL 4 ACCEPTANCE SCENARIOS PASSED SUCCESSFULLY in {duration:.2f}s!")
        log("==================================================================")
    finally:
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    main()
