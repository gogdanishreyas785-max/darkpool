#!/usr/bin/env python3
"""Drive a protected 64-player TLS match from a separate Linux client host.

The moderator controls match creation and phase transitions in the normal CLI.
This tool starts 64 normal player CLIs on the player host and checks the
observable results without recording or printing private response contents.
"""

from __future__ import annotations

import argparse
import os
import queue
import re
import signal
import shutil
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path


PLAYER_COUNT = 64
EXPECTED_ROLES = {"Insider": 8, "Investigator": 4, "Employee": 52}
DEFAULT_TIMEOUT = 45.0


class AcceptanceFailure(RuntimeError):
    """Failure with a safe stage/session description, never raw client output."""


class PlayerSession:
    _PATTERNS = {
        "connected": re.compile(r"\[CONNECTED\] Session established as player"),
        "joined": re.compile(r"\[MATCH JOINED\] Match #"),
        "error": re.compile(r"\[ERROR\]"),
        "match_started": re.compile(
            r"\[MATCH STARTED\] Match has begun\. Round (\d+) \(AwaitingEvent\)"
        ),
        "player_id": re.compile(r"Player ID\s*:\s*(\d+)"),
        "role": re.compile(r"Private Role\s*:\s*(Insider|Investigator|Employee)"),
        "discussion": re.compile(r"\[PHASE START\] Discussion(?: — Round (\d+))?"),
        "trading": re.compile(r"\[PHASE START\] Trading(?: — Round (\d+))?"),
        "clue": re.compile(r"\[PHASE START\] Clue(?: — Round (\d+))?"),
        "voting": re.compile(r"\[VOTING STARTED\].*Round (\d+)"),
        "transition": re.compile(r"\[TRANSITION INTERVAL\] Upcoming phase: Voting"),
        "phase_warning": re.compile(r"\[COUNTDOWN WARNING\]"),
        "market_snapshot": re.compile(r"\[MARKET SNAPSHOT\] Match #\d+ \(Version (\d+)\)"),
        "status": re.compile(r"MATCH #\d+ STATUS"),
        "vote_resolution": re.compile(r"\[VOTE RESOL(?:UTION|VED)\]"),
        "round_start": re.compile(r"\[ROUND START\] Round (\d+) has begun"),
        "match_finished": re.compile(r"\[MATCH FINISHED\] Final outcome: InsiderWin"),
        "trade_success": re.compile(r"\[TRADE SUCCESS\]"),
        "vote_recorded": re.compile(r"\[VOTE RECORDED\]"),
        "insider_hint_private": re.compile(r"\[CONFIDENTIAL INSIDER HINT\]"),
        "clue_private": re.compile(r"\[CONFIDENTIAL INVESTIGATION RESULT\]"),
    }

    def __init__(self, index: int, command: list[str], env: dict[str, str]):
        self.index = index
        self.proc = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
            env=env,
            start_new_session=True,
        )
        self.events: queue.Queue[tuple[str, str]] = queue.Queue()
        self.seen: set[str] = set()
        self.pending: list[tuple[str, str]] = []
        self.reader = threading.Thread(target=self._read_output)
        self.reader.start()

    def _read_output(self) -> None:
        assert self.proc.stdout is not None
        try:
            for line in self.proc.stdout:
                for name, pattern in self._PATTERNS.items():
                    match = pattern.search(line)
                    if match:
                        value = match.group(1) if match.lastindex else ""
                        self.events.put((name, value))
        except (OSError, ValueError):
            return

    def send(self, command: str) -> None:
        if self.proc.poll() is not None or self.proc.stdin is None:
            raise AcceptanceFailure(f"session {self.index}: client exited before command {command!r}")
        try:
            self.proc.stdin.write(command + "\n")
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError) as exc:
            raise AcceptanceFailure(f"session {self.index}: could not send command {command!r}") from exc

    def wait(self, name: str, timeout: float = DEFAULT_TIMEOUT) -> str:
        deadline = time.monotonic() + timeout
        while True:
            for index, (event, value) in enumerate(self.pending):
                if event == name:
                    del self.pending[index]
                    self.seen.add(event)
                    return value
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise AcceptanceFailure(f"session {self.index}: timed out waiting for {name}")
            try:
                event, value = self.events.get(timeout=min(remaining, 0.25))
                if event == "error" and name == "joined":
                    raise AcceptanceFailure(f"session {self.index}: server rejected match admission")
                if event == name:
                    self.seen.add(event)
                    return value
                self.seen.add(event)
                self.pending.append((event, value))
            except queue.Empty:
                if self.proc.poll() is not None:
                    raise AcceptanceFailure(f"session {self.index}: client exited while waiting for {name}")

    def collect_pending(self) -> list[tuple[str, str]]:
        while True:
            try:
                self.pending.append(self.events.get_nowait())
            except queue.Empty:
                return self.pending

    def close(self) -> None:
        forced = False
        try:
            if self.proc.poll() is None and self.proc.stdin is not None:
                self.proc.stdin.write("quit\n")
                self.proc.stdin.flush()
                self.proc.stdin.close()
            if self.proc.poll() is None:
                self.proc.wait(timeout=3.0)
        except (OSError, subprocess.TimeoutExpired):
            forced = True
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=2.0)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait()
        finally:
            if self.proc.stdin is not None:
                try:
                    self.proc.stdin.close()
                except OSError:
                    pass
            self.reader.join(timeout=2.0)
            if self.reader.is_alive():
                raise AcceptanceFailure(f"session {self.index}: output reader did not terminate")
            if self.proc.poll() is None:
                raise AcceptanceFailure(f"session {self.index}: process did not terminate")
            if self.proc.stdout is not None:
                self.proc.stdout.close()
        if forced and self.proc.returncode is None:
            raise AcceptanceFailure(f"session {self.index}: forced cleanup did not report process exit")


def _wait_all(sessions: list[PlayerSession], event: str, timeout: float = DEFAULT_TIMEOUT) -> list[str]:
    deadline = time.monotonic() + timeout
    values = []
    for session in sessions:
        values.append(session.wait(event, max(0.1, deadline - time.monotonic())))
    return values


def _send_all(sessions: list[PlayerSession], command: str, result: str) -> None:
    for session in sessions:
        session.send(command)
    _wait_all(sessions, result)


def _cleanup_sessions(sessions: list[PlayerSession]) -> None:
    errors: list[str] = []
    for session in sessions:
        try:
            session.close()
        except AcceptanceFailure as exc:
            errors.append(str(exc))
    if errors or any(
        session.proc.poll() is None or session.reader.is_alive() for session in sessions
    ):
        detail = f": {errors[0]}" if errors else ""
        raise AcceptanceFailure(
            "stage player cleanup: not all started player processes and output readers terminated"
            + detail
        )


def _operator_step(label: str, command: str) -> None:
    print(f"\nModerator: run `{command}`. After it is accepted, press Enter here to verify {label}.", flush=True)
    try:
        input()
    except EOFError as exc:
        raise AcceptanceFailure(f"operator confirmation missing for {label}") from exc


def _barrier(
    sessions: list[PlayerSession],
    expected: list[tuple[str, str | None]] | None = None,
    timeout: float = DEFAULT_TIMEOUT,
) -> list[list[tuple[str, str]]]:
    # The server enqueues a command response before draining notifications
    # generated by that command. A later status response therefore follows all
    # earlier notifications on the same connection.
    for session in sessions:
        session.send("status")
    _wait_all(sessions, "status", timeout)
    observed = [session.collect_pending()[:] for session in sessions]
    for session in sessions:
        session.pending.clear()
    if expected is not None:
        expected_names = [name for name, _ in expected]
        for session, events in zip(sessions, observed):
            actual_names = [name for name, _ in events]
            if actual_names != expected_names:
                raise AcceptanceFailure(
                    f"notification sequence mismatch for session {session.index}: "
                    f"expected {expected_names}, received {actual_names}"
                )
            for (name, expected_value), (_, actual_value) in zip(expected, events):
                if expected_value is not None and actual_value != expected_value:
                    raise AcceptanceFailure(
                        f"session {session.index}: {name} was for an unexpected round"
                    )
    return observed


def _verify_private(
    sessions: list[PlayerSession], authorized: int, private_event: str, timeout: float
) -> None:
    observed = _barrier(sessions, timeout=timeout)
    unauthorized = [
        sessions[index].index
        for index, session_events in enumerate(observed)
        if index != authorized and any(name == private_event for name, _ in session_events)
    ]
    if unauthorized:
        raise AcceptanceFailure(
            f"private {private_event} notification reached unauthorized session(s) {unauthorized[:8]}"
        )
    authorized_count = sum(name == private_event for name, _ in observed[authorized])
    if authorized_count != 1:
        raise AcceptanceFailure(
            f"authorized session {sessions[authorized].index} received "
            f"{authorized_count} {private_event} notifications, expected exactly one"
        )


def _verify_server_stopped(server_ip: str, timeout: float) -> None:
    try:
        connection = socket.create_connection((server_ip, 4783), timeout=min(timeout, 5.0))
    except OSError:
        print("PASS: TLS server TCP listener is no longer accepting connections.", flush=True)
        return
    connection.close()
    raise AcceptanceFailure("stage shutdown: the server still accepts TCP connections on port 4783")


def _sync_tokens(repo: Path, match_id: int, env: dict[str, str]) -> None:
    result = subprocess.run(
        ["bash", str(repo / "scripts/run_tls_client.sh"), "player", "--sync-only", str(match_id)],
        cwd=repo,
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        timeout=20,
        check=False,
    )
    if result.returncode != 0:
        raise AcceptanceFailure("stage token synchronization: existing player SSH/SCP provisioning failed")


def run(args: argparse.Namespace) -> None:
    repo = Path(args.repo).resolve()
    data_dir = Path(args.data_dir).expanduser()
    token_dir = Path(args.token_dir).expanduser()
    binary = Path(args.client_binary).resolve()
    ca_file = Path(args.ca_cert).expanduser() if args.ca_cert else data_dir / "ca-cert.pem"
    ip_file = data_dir / "server-ip"
    moderator_credential_paths = {
        data_dir / "moderator.token",
        Path.home() / ".local/share/darkpool/tls-lan/moderator.token",
    }

    if sys.platform != "linux":
        raise AcceptanceFailure("this acceptance driver requires a Linux client host")
    if args.players != PLAYER_COUNT:
        raise AcceptanceFailure("this acceptance procedure requires exactly 64 players")
    if args.match_id < 1:
        raise AcceptanceFailure("match ID must be a positive integer")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        raise AcceptanceFailure("stage prerequisites: darkpool_client binary is missing or not executable")
    if not ca_file.is_file() or not ip_file.is_file():
        raise AcceptanceFailure("stage prerequisites: TLS client setup is incomplete")
    if any(path.exists() for path in moderator_credential_paths) or os.environ.get("DARKPOOL_MODERATOR_TOKEN"):
        raise AcceptanceFailure("security check: moderator credential is present on the player host; remove it before continuing")

    provision_ip = ip_file.read_text(encoding="utf-8").strip()
    server_ip = args.server_address or provision_ip
    if not provision_ip or not server_ip:
        raise AcceptanceFailure("stage prerequisites: configured server address is empty")

    token_dir.mkdir(mode=0o700, parents=True, exist_ok=True)
    os.chmod(token_dir, 0o700)
    player_env = os.environ.copy()
    for secret_name in (
        "DARKPOOL_MODERATOR_TOKEN",
        "DARKPOOL_ENROLLMENT_TOKEN",
        "DARKPOOL_PLAYER_SECRET",
        "DARKPOOL_PLAYER_TOKEN",
        "DARKPOOL_JOIN_TOKEN",
        "DARKPOOL_MATCH_JOIN_TOKEN",
    ):
        player_env.pop(secret_name, None)
    player_env["DARKPOOL_TOKEN_DIR"] = str(token_dir)
    player_env["DARKPOOL_AUTO_JOIN"] = "1"
    if shutil.which("bash") is None:
        raise AcceptanceFailure("stage prerequisites: bash is required for the existing TLS token sync helper")

    print("DARKPOOL 64-player TLS LAN acceptance", flush=True)
    print(f"Server: {server_ip}:4783 | Match: {args.match_id} | Players: 64", flush=True)
    print("Moderator credential check: absent from this player host.", flush=True)
    if args.skip_token_sync:
        print("Skipping SSH/SCP token synchronization for an explicitly requested negative test.", flush=True)
    else:
        print("Synchronizing the protected match admission token over the configured SSH/SCP path...", flush=True)
        try:
            _sync_tokens(repo, args.match_id, player_env)
        except subprocess.TimeoutExpired as exc:
            raise AcceptanceFailure("stage token synchronization: SSH/SCP timed out") from exc
    token_file = token_dir / f"match_{args.match_id}.token"
    if not token_file.is_file() or token_file.stat().st_size == 0:
        raise AcceptanceFailure("stage token synchronization: expected match token is unavailable")
    os.chmod(token_file, 0o600)

    command = [str(binary), "--tls", "--ca-cert", str(ca_file), server_ip, "4783", "player"]
    sessions: list[PlayerSession] = []
    match_succeeded = False
    try:
        print("Opening 64 certificate-verified TLS player sessions...", flush=True)
        for i in range(1, PLAYER_COUNT + 1):
            try:
                sessions.append(PlayerSession(i, command, player_env))
            except OSError as exc:
                raise AcceptanceFailure(f"stage TLS connect: could not start player session {i}") from exc
        _wait_all(sessions, "connected", args.timeout)
        for session in sessions:
            session.send(f"join {args.match_id}")
        _wait_all(sessions, "joined", args.timeout)
        roles = [session.wait("role", args.timeout) for session in sessions]
        player_ids = [session.wait("player_id", args.timeout) for session in sessions]
        if len(set(player_ids)) != PLAYER_COUNT:
            raise AcceptanceFailure("stage admission: server did not assign 64 unique player identities")
        counts = {role: roles.count(role) for role in EXPECTED_ROLES}
        if counts != EXPECTED_ROLES:
            raise AcceptanceFailure(f"stage admission: role totals were {counts}, expected {EXPECTED_ROLES}")
        _barrier(sessions, [("market_snapshot", None)], args.timeout)
        print(f"PASS: 64 unique seats joined; role totals {counts}. The moderator remains operator-managed outside the player process pool.", flush=True)

        _operator_step("match start", "start")
        _barrier(sessions, [("match_started", "1"), ("market_snapshot", None)], args.timeout)
        for round_number in range(1, 4):
            _operator_step(f"round {round_number} discussion", "event 0")
            _barrier(sessions, [("discussion", str(round_number)), ("market_snapshot", None)], args.timeout)

            if round_number == 1:
                insider_index = roles.index("Insider")
                sessions[insider_index].send("hint")
                _verify_private(sessions, insider_index, "insider_hint_private", args.timeout)
                print("PASS: Insider hint reached only the authorized session.", flush=True)

            _operator_step(f"round {round_number} trading", "trading")
            _barrier(sessions, [("trading", str(round_number)), ("market_snapshot", None)], args.timeout)
            _send_all(sessions, "hold", "trade_success")
            _barrier(sessions, expected=[], timeout=args.timeout)

            _operator_step(f"round {round_number} market resolution", "market")
            _barrier(
                sessions,
                [("market_snapshot", None), ("clue", str(round_number)), ("phase_warning", None)],
                args.timeout,
            )

            if round_number == 1:
                investigator_index = roles.index("Investigator")
                target_id = player_ids[roles.index("Employee")]
                sessions[investigator_index].send(f"clue {target_id}")
                _verify_private(sessions, investigator_index, "clue_private", args.timeout)
                print("PASS: Investigator clue reached only the authorized session.", flush=True)

            _wait_all(sessions, "transition", args.timeout + 35.0)
            _barrier(
                sessions,
                [("phase_warning", None), ("market_snapshot", None)],
                args.timeout,
            )
            _wait_all(sessions, "voting", args.timeout + 15.0)
            _barrier(sessions, [("market_snapshot", None)], args.timeout)
            _send_all(sessions, "vote abstain", "vote_recorded")
            _wait_all(sessions, "vote_resolution", args.timeout + 65.0)
            if round_number < 3:
                _barrier(sessions, [("round_start", str(round_number + 1)), ("market_snapshot", None)], args.timeout)
            else:
                _barrier(sessions, [("match_finished", None), ("market_snapshot", None)], args.timeout)

        print("PASS: all sessions observed the final InsiderWin outcome.", flush=True)
        match_succeeded = True
    finally:
        print("Closing player sessions...", flush=True)
        _cleanup_sessions(sessions)
        print(f"Cleanup complete: {len(sessions)} player process(es) stopped.", flush=True)
    if match_succeeded:
        _operator_step(
            "TCP listener closure and server process termination",
            "stop the server, verify the listener is closed, then verify `systemctl is-active darkpool.service` is inactive or the server PID has exited",
        )
        _verify_server_stopped(server_ip, args.timeout)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--match-id", type=int, required=True, help="protected match created by the separate moderator")
    parser.add_argument("--players", type=int, default=PLAYER_COUNT, help="must be 64")
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="bounded connect/admission/phase timeout in seconds")
    parser.add_argument("--repo", default=str(Path(__file__).resolve().parent.parent), help="repository checkout containing scripts/")
    parser.add_argument("--client-binary", default=None, help="darkpool_client path (default: <repo>/build/darkpool_client)")
    parser.add_argument("--data-dir", default="~/.local/share/darkpool/tls-lan")
    parser.add_argument("--token-dir", default="~/.darkpool/tokens")
    parser.add_argument("--ca-cert", default=None, help="CA override for isolated TLS identity failure checks")
    parser.add_argument("--server-address", default=None, help="connection target override; token sync still uses configured server")
    parser.add_argument("--skip-token-sync", action="store_true", help="negative testing only; use a disposable token directory")
    args = parser.parse_args()
    if args.client_binary is None:
        args.client_binary = str(Path(args.repo) / "build/darkpool_client")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    return args


if __name__ == "__main__":
    try:
        run(parse_args())
    except AcceptanceFailure as exc:
        print(f"FAIL: {exc}", file=sys.stderr, flush=True)
        raise SystemExit(1)
    except KeyboardInterrupt:
        print("BLOCKED: interrupted by operator; player sessions were cleaned up.", file=sys.stderr, flush=True)
        raise SystemExit(130)
