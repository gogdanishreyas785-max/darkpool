#!/usr/bin/env python3
"""Focused tests for the 64-player LAN acceptance driver output handling."""

import os
import sys
import unittest
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


if __name__ == "__main__":
    unittest.main()
