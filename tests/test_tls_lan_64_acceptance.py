#!/usr/bin/env python3
"""Focused tests for the 64-player LAN acceptance driver output handling."""

import os
import sys
import unittest
from unittest import mock
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import tls_lan_64_acceptance as acceptance  # noqa: E402


class PlayerSessionTests(unittest.TestCase):
    def setUp(self):
        self.fake_cli = (
            "import sys; "
            "print('[MATCH JOINED] Match #7', flush=True); "
            "print('  Player ID   : 41', flush=True); "
            "print('  Private Role: Investigator', flush=True); "
            "print('[ROUND START] Round 1 has begun.', flush=True); "
            "print('[ROUND START] Round 2 has begun.', flush=True); "
            "print('[CONFIDENTIAL INVESTIGATION RESULT]', flush=True); "
            "print('private-value-that-must-not-be-retained', flush=True); "
            "sys.stdin.readline()"
        )
        self.session = acceptance.PlayerSession(
            1,
            [sys.executable, "-u", "-c", self.fake_cli],
            os.environ.copy(),
        )

    def tearDown(self):
        self.session.close()

    def test_keeps_interleaved_identity_and_repeated_round_events(self):
        self.assertEqual(self.session.wait("joined"), "")
        self.assertEqual(self.session.wait("player_id"), "41")
        self.assertEqual(self.session.wait("role"), "Investigator")
        self.assertEqual(self.session.wait("round_start"), "1")
        self.assertEqual(self.session.wait("round_start"), "2")

    def test_match_started_captures_actual_cli_round(self):
        session = self._session_with_output(
            "[MATCH STARTED] Match has begun. Round 1 (AwaitingEvent).\n"
        )
        try:
            self.assertEqual(session.wait("match_started"), "1")
        finally:
            session.close()

    def test_private_markers_are_detected_without_retaining_output_text(self):
        self.assertEqual(self.session.wait("clue_private"), "")
        self.assertIn("clue_private", self.session.seen)
        self.assertFalse(hasattr(self.session, "history"))
        self.assertNotIn("private-value-that-must-not-be-retained", repr(self.session.__dict__))

    def test_wait_timeout_is_bounded_and_descriptive(self):
        with self.assertRaisesRegex(acceptance.AcceptanceFailure, "session 1: timed out waiting for voting"):
            self.session.wait("voting", timeout=0.03)

    def test_admission_rejection_does_not_include_server_text(self):
        rejected = acceptance.PlayerSession(
            2,
            [sys.executable, "-u", "-c", "import sys; print('[ERROR] invalid token contents', flush=True); sys.stdin.readline()"],
            os.environ.copy(),
        )
        try:
            with self.assertRaisesRegex(acceptance.AcceptanceFailure, "session 2: server rejected match admission") as failure:
                rejected.wait("joined")
            self.assertNotIn("invalid token contents", str(failure.exception))
        finally:
            rejected.close()

    def _session_with_output(self, output, respond_to_status=True):
        response_loop = f"""import sys
print({output!r}, end='', flush=True)
for line in sys.stdin:
    if line.strip() == 'status':
{"        print('MATCH #7 STATUS', flush=True)" if respond_to_status else "        pass"}
"""
        return acceptance.PlayerSession(
            3,
            [sys.executable, "-u", "-c", response_loop],
            os.environ.copy(),
        )

    def test_status_barrier_rejects_stale_phase_event(self):
        session = self._session_with_output(
            "[PHASE START] Discussion — Round 1\n[MARKET SNAPSHOT] Match #7 (Version 2)\n"
        )
        try:
            with self.assertRaisesRegex(acceptance.AcceptanceFailure, "unexpected round"):
                acceptance._barrier(
                    [session],
                    [("discussion", "2"), ("market_snapshot", None)],
                    timeout=1,
                )
        finally:
            session.close()

    def test_status_barrier_rejects_duplicate_public_notification(self):
        session = self._session_with_output(
            "[MARKET SNAPSHOT] Match #7 (Version 2)\n"
            "[MARKET SNAPSHOT] Match #7 (Version 2)\n"
        )
        try:
            with self.assertRaisesRegex(acceptance.AcceptanceFailure, "received"):
                acceptance._barrier(
                    [session], [("market_snapshot", None)], timeout=1
                )
        finally:
            session.close()

    def test_transition_barrier_validates_warning_then_snapshot_after_transition(self):
        session = self._session_with_output(
            "[COUNTDOWN WARNING] Clue phase has 10 seconds remaining!\n"
            "[TRANSITION INTERVAL] Upcoming phase: Voting (10s remaining).\n"
            "[MARKET SNAPSHOT] Match #7 (Version 2)\n"
        )
        try:
            self.assertEqual(session.wait("transition"), "")
            observed = acceptance._barrier(
                [session],
                [("phase_warning", None), ("market_snapshot", "2")],
                timeout=1,
            )
            self.assertEqual(
                [name for name, _ in observed[0]],
                ["phase_warning", "market_snapshot"],
            )
        finally:
            session.close()

    def test_private_delivery_requires_one_authorized_and_zero_unauthorized(self):
        authorized = self._session_with_output(
            "[CONFIDENTIAL INSIDER HINT]\n"
        )
        unauthorized = self._session_with_output(
            "[CONFIDENTIAL INSIDER HINT]\n"
        )
        unauthorized.index = 4
        try:
            with self.assertRaisesRegex(acceptance.AcceptanceFailure, "unauthorized"):
                acceptance._verify_private(
                    [authorized, unauthorized], 0, "insider_hint_private", timeout=1
                )
        finally:
            authorized.close()
            unauthorized.close()

    def test_private_delivery_rejects_missing_authorized_notification(self):
        authorized = self._session_with_output("")
        unauthorized = self._session_with_output("")
        unauthorized.index = 4
        try:
            with self.assertRaisesRegex(acceptance.AcceptanceFailure, "received 0"):
                acceptance._verify_private(
                    [authorized, unauthorized], 0, "insider_hint_private", timeout=1
                )
        finally:
            authorized.close()
            unauthorized.close()

    def test_cleanup_terminates_started_process_and_reader(self):
        session = acceptance.PlayerSession(
            5,
            [sys.executable, "-u", "-c", "import time; time.sleep(60)"],
            os.environ.copy(),
        )
        session.close()
        self.assertIsNotNone(session.proc.poll())
        self.assertFalse(session.reader.is_alive())

    def test_partial_start_cleanup_stops_every_started_session(self):
        first = acceptance.PlayerSession(
            6,
            [sys.executable, "-u", "-c", "import time; time.sleep(60)"],
            os.environ.copy(),
        )
        second = acceptance.PlayerSession(
            7,
            [sys.executable, "-u", "-c", "import time; time.sleep(60)"],
            os.environ.copy(),
        )
        try:
            acceptance._cleanup_sessions([first, second])
            self.assertIsNotNone(first.proc.poll())
            self.assertIsNotNone(second.proc.poll())
        finally:
            first.close()
            second.close()

    def test_token_synchronization_failure_is_explicit(self):
        with mock.patch(
            "tls_lan_64_acceptance.subprocess.run",
            return_value=mock.Mock(returncode=1),
        ):
            with self.assertRaisesRegex(acceptance.AcceptanceFailure, "token synchronization"):
                acceptance._sync_tokens(Path("."), 7, os.environ.copy())


if __name__ == "__main__":
    unittest.main()
