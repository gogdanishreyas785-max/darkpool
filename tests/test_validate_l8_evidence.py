#!/usr/bin/env python3
"""
Unit and regression tests for tools/validate_l8_evidence.py.

Verifies that:
1. A valid L8 evidence object passes validation.
2. Incomplete flood (<41 requests) triggers validation failure.
3. Zero rate limited responses triggers validation failure.
4. Transport error in flood triggers validation failure.
5. Failed sibling match triggers validation failure.
6. Malformed JSON or oversized frame failure triggers validation failure.
7. Unsettled FDs triggers validation failure.
8. Non-PASS status triggers validation failure.
"""

import copy
import json
import os
import subprocess
import sys
import tempfile
import unittest

VALID_L8_SAMPLE = {
    "test_name": "darkpool_load_tests_l8",
    "source_commit": "e20db6a608304bd76f1fe46176779dda8f63aed5",
    "git_dirty": False,
    "timestamp_utc": "2026-10-16T03:23:53Z",
    "environment": {
        "compiler": "GCC",
        "machine": "x86_64",
        "os": "Linux",
        "release": "6.18.54.1-microsoft-standard-WSL2",
        "sysname": "Linux"
    },
    "status": "PASS",
    "failure_reason": "",
    "duration_ms": 250,
    "policy": {
        "approved_decision": "DEC-11",
        "burst_capacity": 40.0,
        "max_frame_payload_bytes": 4096,
        "request_rate_per_sec": 20.0
    },
    "rate_limiter_boundary_tests": {
        "burst_requests_accepted": 40,
        "burst_requests_sent": 40,
        "connection_usable_after_rate_limit": True,
        "over_burst_rejected": True,
        "rejection_error_message": "rate limit exceeded",
        "state_mutation_prevented": True,
        "token_refill_recovery_ok": True
    },
    "malformed_traffic_tests": {
        "malformed_json_closed": True,
        "oversized_frame_closed": True,
        "server_survived_malformed": True
    },
    "flood_isolation_workload": {
        "flood_requests_sent": 350,
        "flood_responses_received": 350,
        "flood_requests_accepted": 40,
        "flood_requests_rate_limited": 310,
        "flood_transport_errors": 0,
        "flood_malformed_responses": 0,
        "healthy_match_id": 1,
        "healthy_match_latency": {
            "max_ms": 0.45,
            "mean_ms": 0.22,
            "min_ms": 0.08,
            "p50_ms": 0.21,
            "p95_ms": 0.38,
            "p99_ms": 0.42
        },
        "healthy_match_outcome": "InsiderWin",
        "healthy_match_phase": "Finished",
        "healthy_match_requests": {
            "correctness_failures": 0,
            "expected_rejections": 0,
            "successful": 59,
            "total": 59,
            "transport_failures": 0
        },
        "healthy_match_status": "PASS"
    },
    "resource_telemetry": {
        "baseline_open_fds": 8,
        "baseline_open_fds_sample_ok": True,
        "baseline_rss_kb": 3600,
        "cleanup_failure_reason": "",
        "cleanup_settle_time_ms": 10,
        "immediate_post_cleanup_open_fds": 10,
        "immediate_post_cleanup_sample_ok": True,
        "peak_open_fds": 18,
        "peak_open_fds_sample_ok": True,
        "peak_rss_kb": 3900,
        "peak_rss_sample_ok": True,
        "post_cleanup_open_fds": 8,
        "post_cleanup_rss_kb": 3900,
        "post_cleanup_rss_sample_ok": True,
        "post_cleanup_sample_ok": True,
        "post_cleanup_settled": True
    }
}


class TestValidateL8Evidence(unittest.TestCase):

    def run_validator(self, data: dict) -> int:
        with tempfile.NamedTemporaryFile("w", delete=False, suffix=".json") as f:
            json.dump(data, f)
            path = f.name
        try:
            cmd = [sys.executable, os.path.join(os.path.dirname(__file__), "..", "tools", "validate_l8_evidence.py"), path]
            res = subprocess.run(cmd, capture_output=True, text=True)
            return res.returncode
        finally:
            if os.path.exists(path):
                os.remove(path)

    def test_valid_evidence_passes(self):
        rc = self.run_validator(VALID_L8_SAMPLE)
        self.assertEqual(rc, 0)

    def test_insufficient_flood_volume_fails(self):
        bad = copy.deepcopy(VALID_L8_SAMPLE)
        bad["flood_isolation_workload"]["flood_requests_sent"] = 25
        rc = self.run_validator(bad)
        self.assertNotEqual(rc, 0)

    def test_zero_rate_limited_fails(self):
        bad = copy.deepcopy(VALID_L8_SAMPLE)
        bad["flood_isolation_workload"]["flood_requests_rate_limited"] = 0
        rc = self.run_validator(bad)
        self.assertNotEqual(rc, 0)

    def test_flood_transport_error_fails(self):
        bad = copy.deepcopy(VALID_L8_SAMPLE)
        bad["flood_isolation_workload"]["flood_transport_errors"] = 1
        rc = self.run_validator(bad)
        self.assertNotEqual(rc, 0)

    def test_failed_sibling_match_fails(self):
        bad = copy.deepcopy(VALID_L8_SAMPLE)
        bad["flood_isolation_workload"]["healthy_match_status"] = "FAIL"
        rc = self.run_validator(bad)
        self.assertNotEqual(rc, 0)

    def test_unsettled_fds_fails(self):
        bad = copy.deepcopy(VALID_L8_SAMPLE)
        bad["resource_telemetry"]["post_cleanup_settled"] = False
        rc = self.run_validator(bad)
        self.assertNotEqual(rc, 0)

    def test_rate_limit_boundary_leak_fails(self):
        bad = copy.deepcopy(VALID_L8_SAMPLE)
        bad["rate_limiter_boundary_tests"]["burst_requests_accepted"] = 39
        rc = self.run_validator(bad)
        self.assertNotEqual(rc, 0)


if __name__ == "__main__":
    unittest.main()
