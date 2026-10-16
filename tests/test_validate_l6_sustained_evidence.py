#!/usr/bin/env python3
"""
Automated regression tests for tools/validate_l6_sustained_evidence.py

Uses standard-library-only unittest and temporary directories.
Does not mutate or depend on mutating committed evidence files.
"""

import copy
import hashlib
import json
import os
import shutil
import sys
import tempfile
import unittest

# Ensure repository root is on sys.path
REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
if REPO_ROOT not in sys.path:
    sys.path.insert(0, REPO_ROOT)

from tools.validate_l6_sustained_evidence import (
    validate_report_and_evidence,
    ValidationError,
    sha256_file,
)


def create_minimal_valid_fixture(tmp_dir: str, num_cycles: int = 5):
    """
    Creates a minimal, self-consistent synthetic 4-match sustained smoke fixture
    in tmp_dir and returns (summary_path, evidence_rel_path).
    """
    ev_rel_path = "docs/evidence/test_smoke_evidence.jsonl"
    ev_abs_path = os.path.join(tmp_dir, ev_rel_path)
    os.makedirs(os.path.dirname(ev_abs_path), exist_ok=True)

    cycles = []
    baseline_fds = 9
    tot_requests = 0
    succ_requests = 0
    exp_rejections = 0
    rss_values = [4200 + (i * 10) for i in range(num_cycles)]

    with open(ev_abs_path, "w", encoding="utf-8") as f:
        for idx in range(1, num_cycles + 1):
            c_succ = 2000
            c_rej = 20
            c_tot = c_succ + c_rej
            tot_requests += c_tot
            succ_requests += c_succ
            exp_rejections += c_rej

            rec = {
                "cycle_index": idx,
                "elapsed_ms": idx * 1000,
                "cycle_duration_ms": 500,
                "completed": True,
                "matches_completed": 4,
                "matches_overlapped": True,
                "overlap_duration_ms": 400,
                "failure_reason": "",
                "cleanup_failure_reason": "",
                "cleanup_passed": True,
                "cleanup_settle_time_ms": 10,
                "peak_connections_fd_sample_ok": True,
                "peak_connections_open_fds": 268,
                "immediate_post_close_fd_sample_ok": True,
                "immediate_post_close_open_fds": 100,
                "settled_post_cleanup_fd_sample_ok": True,
                "settled_post_cleanup_open_fds": 8,
                "peak_conn_rss_sample_ok": True,
                "peak_conn_current_rss_kb": 4100,
                "post_cleanup_current_rss_sample_ok": True,
                "post_cleanup_current_rss_kb": rss_values[idx - 1],
                "post_cleanup_peak_rss_sample_ok": True,
                "post_cleanup_peak_rss_kb": 4350,
                "requests": {
                    "total_requests": c_tot,
                    "successful_requests": c_succ,
                    "expected_rejections": c_rej,
                    "transport_failures": 0,
                    "correctness_failures": 0,
                },
            }
            cycles.append(rec)
            f.write(json.dumps(rec) + "\n")

    ev_sha256 = sha256_file(ev_abs_path)
    import statistics
    mean_rss = round(statistics.mean(rss_values), 1)
    median_rss = float(statistics.median(rss_values))

    summary = {
        "status": "PASS",
        "stage_name": "smoke_10m",
        "target_duration_s": 600,
        "actual_duration_ms": 600500,
        "actual_duration_s": 600.5,
        "source_commit": "a443d0a349a6ca8436fa9ce3751ae1ac3f7803ec",
        "git_dirty": False,
        "completed_cycles": num_cycles,
        "completed_matches": num_cycles * 4,
        "concurrency_topology": {
            "concurrent_matches": 4,
            "players_per_match": 64,
            "total_player_clients": 256,
            "moderator_clients": 4,
            "total_connected_clients": 260,
            "connection_cap": 260,
            "persistent_server": True,
        },
        "cycle_diagnostics": {
            "completed_cycles": num_cycles,
            "completed_matches": num_cycles * 4,
            "total_cycles_attempted": num_cycles,
            "evidence_records_count": num_cycles,
            "match_limit_retries": 0,
            "all_cycles_fd_cleanup_passed": True,
            "all_cycles_overlapped": True,
        },
        "total_requests": tot_requests,
        "successful_requests": succ_requests,
        "expected_rejections": exp_rejections,
        "transport_failures": 0,
        "correctness_failures": 0,
        "requests": {
            "total_requests": tot_requests,
            "successful_requests": succ_requests,
            "expected_rejections": exp_rejections,
            "transport_failures": 0,
            "correctness_failures": 0,
        },
        "failures": {
            "transport_failures": 0,
            "correctness_failures": 0,
            "timeouts": 0,
            "unexpected_disconnects": 0,
        },
        "resources": {
            "all_cycles_fd_cleanup_passed": True,
            "baseline_open_fds": baseline_fds,
            "baseline_open_fds_sample_ok": True,
            "resource_sampling_ok": True,
            "initial_current_rss_sample_ok": True,
            "end_current_rss_sample_ok": True,
            "peak_rss_sample_ok": True,
            "current_rss_sample_count": num_cycles,
            "peak_open_fds": 268,
            "immediate_post_cleanup_open_fds": 100,
            "settled_post_cleanup_fds": 8,
            "initial_current_rss_kb": 3580,
            "initial_rss_kb": 3580,
            "end_current_rss_kb": rss_values[-1],
            "end_rss_kb": rss_values[-1],
            "peak_rss_kb": 4350,
            "rss_summary": {
                "sample_count": num_cycles,
                "min_kb": min(rss_values),
                "max_kb": max(rss_values),
                "mean_kb": mean_rss,
                "median_kb": median_rss,
            },
        },
        "evidence_file": {
            "relative_path": ev_rel_path,
            "format": "JSONL (one independently parseable JSON object per cycle)",
            "schema_version": "1.0",
            "expected_records": num_cycles,
            "sha256": ev_sha256,
            "source_commit": "a443d0a349a6ca8436fa9ce3751ae1ac3f7803ec",
            "git_dirty": False,
        },
    }

    sum_rel_path = "docs/test_smoke_results.json"
    sum_abs_path = os.path.join(tmp_dir, sum_rel_path)
    os.makedirs(os.path.dirname(sum_abs_path), exist_ok=True)
    with open(sum_abs_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2)

    return sum_abs_path, ev_abs_path, summary, cycles


class TestEvidenceValidator(unittest.TestCase):
    def setUp(self):
        self.test_dir = tempfile.mkdtemp(prefix="darkpool_test_val_")
        self.sum_path, self.ev_path, self.summary_data, self.cycles_data = (
            create_minimal_valid_fixture(self.test_dir, num_cycles=5)
        )

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def rewrite_summary(self, data):
        with open(self.sum_path, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=2)

    def rewrite_evidence(self, cycles):
        with open(self.ev_path, "w", encoding="utf-8") as f:
            for c in cycles:
                f.write(json.dumps(c) + "\n")
        # Update checksum in summary to match rewritten evidence unless testing checksum mismatch
        new_sha = sha256_file(self.ev_path)
        with open(self.sum_path, "r", encoding="utf-8") as f:
            s = json.load(f)
        s["evidence_file"]["sha256"] = new_sha
        self.rewrite_summary(s)

    def test_valid_fixture_passes(self):
        """Minimal valid fixture must pass validation without error."""
        validate_report_and_evidence(self.sum_path, self.test_dir)

    def test_corrupted_cycle_requests_counter(self):
        """Per-cycle request reconciliation failure must raise ValidationError."""
        cycles = copy.deepcopy(self.cycles_data)
        cycles[1]["requests"]["total_requests"] += 1  # c_tot != succ + rej + tf + cf
        self.rewrite_evidence(cycles)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("requests reconciliation failed", str(ctx.exception))

    def test_summary_requests_mismatch(self):
        """Summary total_requests differing from accumulated cycle requests must fail."""
        data = copy.deepcopy(self.summary_data)
        data["total_requests"] += 10
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("request reconciliation failed", str(ctx.exception))

    def test_summary_nested_requests_mismatch(self):
        """Summary requests sub-object differing from top-level counters must fail."""
        data = copy.deepcopy(self.summary_data)
        data["requests"]["successful_requests"] -= 50
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("counters differ", str(ctx.exception))

    def test_boolean_masquerading_as_int(self):
        """Reject boolean masquerading as integer in count fields."""
        data = copy.deepcopy(self.summary_data)
        data["completed_cycles"] = True  # True is an instance of int in Python, but not strict int
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("must be a strict integer", str(ctx.exception))

    def test_non_sequential_cycle_index(self):
        """Cycle indices must be strictly sequential starting at 1."""
        cycles = copy.deepcopy(self.cycles_data)
        cycles[2]["cycle_index"] = 99
        self.rewrite_evidence(cycles)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("Cycle index out of sequence", str(ctx.exception))

    def test_decreasing_elapsed_time(self):
        """Elapsed time cannot decrease between consecutive cycles."""
        cycles = copy.deepcopy(self.cycles_data)
        cycles[2]["elapsed_ms"] = cycles[1]["elapsed_ms"] - 100
        self.rewrite_evidence(cycles)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("elapsed_ms", str(ctx.exception))
        self.assertIn("decreased", str(ctx.exception))

    def test_fd_cleanup_threshold_exceeded(self):
        """Settled FDs > baseline_open_fds + 2 must fail validation."""
        cycles = copy.deepcopy(self.cycles_data)
        # baseline is 9; max allowed is 11. Set to 12.
        cycles[0]["settled_post_cleanup_open_fds"] = 12
        self.rewrite_evidence(cycles)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("exceeds baseline + 2 tolerance", str(ctx.exception))

    def test_inconsistent_sampling_flag(self):
        """Flag peak_connections_fd_sample_ok=False must fail on a passing run."""
        cycles = copy.deepcopy(self.cycles_data)
        cycles[0]["peak_connections_fd_sample_ok"] = False
        self.rewrite_evidence(cycles)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("peak_connections_fd_sample_ok", str(ctx.exception))

    def test_missing_or_corrupt_resource_sample(self):
        """Missing or non-integer post-cleanup RSS value must fail."""
        cycles = copy.deepcopy(self.cycles_data)
        cycles[0]["post_cleanup_current_rss_kb"] = None
        self.rewrite_evidence(cycles)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("post_cleanup_current_rss_kb", str(ctx.exception))

    def test_checksum_mismatch(self):
        """Discrepancy between summary sha256 and evidence file must fail."""
        data = copy.deepcopy(self.summary_data)
        data["evidence_file"]["sha256"] = "0000000000000000000000000000000000000000000000000000000000000000"
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("Evidence file checksum mismatch", str(ctx.exception))

    def test_provenance_source_commit_mismatch(self):
        """Non-matching source commit must fail validation."""
        data = copy.deepcopy(self.summary_data)
        data["source_commit"] = "badc0ffee1111111111111111111111111111111"
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("source_commit", str(ctx.exception))

    def test_provenance_dirty_worktree(self):
        """git_dirty=True must fail validation."""
        data = copy.deepcopy(self.summary_data)
        data["git_dirty"] = True
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("git_dirty", str(ctx.exception))

    def test_target_duration_shortfall(self):
        """Actual duration less than target duration must fail."""
        data = copy.deepcopy(self.summary_data)
        data["actual_duration_ms"] = 599000  # Target is 600s = 600,000 ms
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("actual_duration_ms", str(ctx.exception))

    def test_rss_statistics_mismatch(self):
        """Tampered RSS mean or min/max statistics must fail."""
        data = copy.deepcopy(self.summary_data)
        data["resources"]["rss_summary"]["mean_kb"] += 50.0
        self.rewrite_summary(data)
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(self.sum_path, self.test_dir)
        self.assertIn("mean_kb", str(ctx.exception))

    def test_missing_summary_file(self):
        """Non-existent summary file path must raise ValidationError."""
        missing = os.path.join(self.test_dir, "nonexistent.json")
        with self.assertRaises(ValidationError) as ctx:
            validate_report_and_evidence(missing, self.test_dir)
        self.assertIn("missing", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
