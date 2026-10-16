#!/usr/bin/env python3
"""
DARKPOOL L6 Sustained Benchmark Evidence Validator

Validates that human-readable summary reports and cycle-level JSONL evidence
files reconcile with complete auditability, accurate source provenance,
and strict FD/RSS resource thresholds.

Exit code 0 on success; non-zero exit code on validation failure.
"""

import argparse
import hashlib
import json
import os
import statistics
import sys
from typing import Dict, List, Any


def fail(msg: str) -> None:
    print(f"VALIDATION ERROR: {msg}", file=sys.stderr)
    sys.exit(1)


def sha256_file(path: str) -> str:
    hasher = hashlib.sha256()
    try:
        with open(path, "rb") as f:
            data = f.read()
            # If checked out on Windows with CRLF line endings, normalize to LF (\n)
            # to ensure deterministic checksum verification across environments.
            if b"\r\n" in data:
                data = data.replace(b"\r\n", b"\n")
            hasher.update(data)
    except Exception as e:
        fail(f"Failed to read file for checksum ({path}): {e}")
    return hasher.hexdigest()


def validate_report_and_evidence(summary_path: str, repo_root: str) -> None:
    print(f"--> Validating summary report: {summary_path}")

    if not os.path.isfile(summary_path):
        fail(f"Summary report file missing or not a file: {summary_path}")

    try:
        with open(summary_path, "r", encoding="utf-8") as f:
            summary = json.load(f)
    except Exception as e:
        fail(f"Summary report {summary_path} is malformed or unreadable: {e}")

    # 1. Top-level status and metadata
    status = summary.get("status")
    if status != "PASS":
        fail(f"Summary {summary_path} status is '{status}', expected 'PASS'")

    stage_name = summary.get("stage_name", "")
    source_commit = summary.get("source_commit")
    git_dirty = summary.get("git_dirty")
    expected_commit = "a443d0a349a6ca8436fa9ce3751ae1ac3f7803ec"
    if source_commit != expected_commit:
        fail(f"Summary {summary_path} source_commit '{source_commit}' does not match expected tested commit '{expected_commit}'")
    if git_dirty is not False:
        fail(f"Summary {summary_path} git_dirty is {git_dirty}, expected False (clean provenance)")

    completed_cycles = summary.get("completed_cycles")
    completed_matches = summary.get("completed_matches")
    if not isinstance(completed_cycles, int) or completed_cycles <= 0:
        fail(f"Invalid completed_cycles count: {completed_cycles}")
    if not isinstance(completed_matches, int) or completed_matches != completed_cycles * 4:
        fail(f"Completed matches ({completed_matches}) does not equal 4 * completed_cycles ({completed_cycles * 4})")

    # 2. Check evidence file block
    evidence_meta = summary.get("evidence_file")
    if not isinstance(evidence_meta, dict):
        fail(f"Summary {summary_path} missing 'evidence_file' metadata object")

    rel_evidence_path = evidence_meta.get("relative_path")
    if not rel_evidence_path:
        fail(f"Summary {summary_path} missing evidence_file.relative_path")

    evidence_abs_path = os.path.join(repo_root, rel_evidence_path)
    if not os.path.isfile(evidence_abs_path):
        fail(f"Cycle evidence file missing at: {evidence_abs_path}")

    expected_records = evidence_meta.get("expected_records")
    if expected_records != completed_cycles:
        fail(f"evidence_file.expected_records ({expected_records}) != completed_cycles ({completed_cycles})")

    ev_source_commit = evidence_meta.get("source_commit")
    ev_git_dirty = evidence_meta.get("git_dirty")
    if ev_source_commit != source_commit:
        fail(f"Evidence metadata source_commit '{ev_source_commit}' != summary source_commit '{source_commit}'")
    if ev_git_dirty != git_dirty:
        fail(f"Evidence metadata git_dirty ({ev_git_dirty}) != summary git_dirty ({git_dirty})")

    expected_sha256 = evidence_meta.get("sha256")
    if not expected_sha256:
        fail(f"Summary {summary_path} missing evidence_file.sha256")

    actual_sha256 = sha256_file(evidence_abs_path)
    if actual_sha256.lower() != expected_sha256.lower():
        fail(f"Evidence file checksum mismatch for {evidence_abs_path}:\n  Recorded: {expected_sha256}\n  Computed: {actual_sha256}")

    # 3. Read and validate each cycle in JSONL
    print(f"    Parsing and auditing {completed_cycles} cycle records from {rel_evidence_path}...")
    baseline_open_fds = summary.get("resources", {}).get("baseline_open_fds")
    if not isinstance(baseline_open_fds, int) or baseline_open_fds <= 0:
        fail(f"Summary resources missing valid baseline_open_fds (got: {baseline_open_fds})")

    cycles: List[Dict[str, Any]] = []
    line_num = 0
    accum_total_requests = 0
    accum_successful_requests = 0
    accum_expected_rejections = 0
    accum_transport_failures = 0
    accum_correctness_failures = 0
    accum_matches = 0
    rss_series: List[int] = []

    try:
        with open(evidence_abs_path, "r", encoding="utf-8") as f_ev:
            for line in f_ev:
                line_num += 1
                line_str = line.strip()
                if not line_str:
                    fail(f"Empty line in JSONL at line {line_num} in {rel_evidence_path}")
                try:
                    cyc = json.loads(line_str)
                except Exception as e:
                    fail(f"Malformed JSON on line {line_num} of {rel_evidence_path}: {e}")

                cycles.append(cyc)

                # Sequential cycle index check
                cyc_idx = cyc.get("cycle_index")
                if cyc_idx != line_num:
                    fail(f"Cycle index out of sequence at line {line_num} (got {cyc_idx}, expected {line_num})")

                # Completed and match count check
                if cyc.get("completed") is not True:
                    fail(f"Cycle {cyc_idx} completed flag is not True: {cyc.get('failure_reason')}")
                matches_done = cyc.get("matches_completed")
                if matches_done != 4:
                    fail(f"Cycle {cyc_idx} matches_completed is {matches_done}, expected 4")
                accum_matches += matches_done

                # FD sampling and threshold checks
                if cyc.get("peak_connections_fd_sample_ok") is not True:
                    fail(f"Cycle {cyc_idx} peak_connections_fd_sample_ok is not True")
                peak_fds = cyc.get("peak_connections_open_fds")
                if peak_fds is None or not isinstance(peak_fds, int) or peak_fds <= 0:
                    fail(f"Cycle {cyc_idx} missing or invalid peak_connections_open_fds: {peak_fds}")

                if cyc.get("settled_post_cleanup_fd_sample_ok") is not True:
                    fail(f"Cycle {cyc_idx} settled_post_cleanup_fd_sample_ok is not True")
                settled_fds = cyc.get("settled_post_cleanup_open_fds")
                if settled_fds is None or not isinstance(settled_fds, int) or settled_fds <= 0:
                    fail(f"Cycle {cyc_idx} missing or invalid settled_post_cleanup_open_fds: {settled_fds}")

                if cyc.get("cleanup_passed") is not True:
                    fail(f"Cycle {cyc_idx} cleanup_passed is not True: {cyc.get('cleanup_failure_reason')}")

                max_allowed_fds = baseline_open_fds + 2
                if settled_fds > max_allowed_fds:
                    fail(f"Cycle {cyc_idx} settled_open_fds ({settled_fds}) exceeds baseline + 2 tolerance ({max_allowed_fds})")

                if cyc.get("immediate_post_close_fd_sample_ok") is True:
                    if cyc.get("immediate_post_close_open_fds") is None:
                        fail(f"Cycle {cyc_idx} immediate_post_close_fd_sample_ok=True but value is None")

                # RSS sampling checks
                if cyc.get("peak_conn_rss_sample_ok") is not True:
                    fail(f"Cycle {cyc_idx} peak_conn_rss_sample_ok is not True")
                if cyc.get("peak_conn_current_rss_kb") is None:
                    fail(f"Cycle {cyc_idx} peak_conn_current_rss_kb is None")

                if cyc.get("post_cleanup_current_rss_sample_ok") is not True:
                    fail(f"Cycle {cyc_idx} post_cleanup_current_rss_sample_ok is not True")
                post_rss = cyc.get("post_cleanup_current_rss_kb")
                if post_rss is None or not isinstance(post_rss, int) or post_rss <= 0:
                    fail(f"Cycle {cyc_idx} missing or invalid post_cleanup_current_rss_kb: {post_rss}")
                rss_series.append(post_rss)

                if cyc.get("post_cleanup_peak_rss_sample_ok") is not True:
                    fail(f"Cycle {cyc_idx} post_cleanup_peak_rss_sample_ok is not True")
                if cyc.get("post_cleanup_peak_rss_kb") is None:
                    fail(f"Cycle {cyc_idx} post_cleanup_peak_rss_kb is None")

                # Request reconciliation per cycle
                reqs = cyc.get("requests", {})
                c_tot = reqs.get("total_requests", 0)
                c_succ = reqs.get("successful_requests", 0)
                c_rej = reqs.get("expected_rejections", 0)
                c_tf = reqs.get("transport_failures", 0)
                c_cf = reqs.get("correctness_failures", 0)

                if c_tot != (c_succ + c_rej + c_tf + c_cf):
                    fail(f"Cycle {cyc_idx} requests reconciliation failed: {c_tot} != {c_succ} (succ) + {c_rej} (rej) + {c_tf} (tf) + {c_cf} (cf)")

                accum_total_requests += c_tot
                accum_successful_requests += c_succ
                accum_expected_rejections += c_rej
                accum_transport_failures += c_tf
                accum_correctness_failures += c_cf
    except Exception as e:
        fail(f"Error reading cycle evidence {evidence_abs_path}: {e}")

    # 4. Reconcile evidence counts with summary counts
    if len(cycles) != completed_cycles:
        fail(f"Evidence file line count ({len(cycles)}) != summary completed_cycles ({completed_cycles})")
    if accum_matches != completed_matches:
        fail(f"Accumulated matches ({accum_matches}) != summary completed_matches ({completed_matches})")

    sum_tot = summary.get("total_requests")
    sum_succ = summary.get("successful_requests")
    sum_rej = summary.get("expected_rejections")
    sum_tf = summary.get("transport_failures")
    sum_cf = summary.get("correctness_failures")

    if sum_tot != accum_total_requests:
        fail(f"Total requests mismatch: summary={sum_tot}, evidence={accum_total_requests}")
    if sum_succ != accum_successful_requests:
        fail(f"Successful requests mismatch: summary={sum_succ}, evidence={accum_successful_requests}")
    if sum_rej != accum_expected_rejections:
        fail(f"Expected rejections mismatch: summary={sum_rej}, evidence={accum_expected_rejections}")
    if sum_tf != accum_transport_failures:
        fail(f"Transport failures mismatch: summary={sum_tf}, evidence={accum_transport_failures}")
    if sum_cf != accum_correctness_failures:
        fail(f"Correctness failures mismatch: summary={sum_cf}, evidence={accum_correctness_failures}")

    # Verify requests sub-object matches top-level
    req_obj = summary.get("requests", {})
    if req_obj.get("total_requests") != sum_tot or req_obj.get("successful_requests") != sum_succ:
        fail(f"Summary requests sub-object differs from summary top-level requests")

    # 5. Validate RSS summary statistics against underlying series
    res_obj = summary.get("resources", {})
    rss_stats = res_obj.get("rss_summary")
    if not isinstance(rss_stats, dict):
        fail(f"Summary {summary_path} resources missing 'rss_summary' object")

    if rss_stats.get("sample_count") != len(rss_series):
        fail(f"RSS sample_count ({rss_stats.get('sample_count')}) != len(rss_series) ({len(rss_series)})")
    if rss_stats.get("min_kb") != min(rss_series):
        fail(f"RSS min_kb ({rss_stats.get('min_kb')}) != actual min ({min(rss_series)})")
    if rss_stats.get("max_kb") != max(rss_series):
        fail(f"RSS max_kb ({rss_stats.get('max_kb')}) != actual max ({max(rss_series)})")
    if rss_stats.get("mean_kb") != round(statistics.mean(rss_series), 1):
        fail(f"RSS mean_kb ({rss_stats.get('mean_kb')}) != actual mean ({round(statistics.mean(rss_series), 1)})")
    if rss_stats.get("median_kb") != statistics.median(rss_series):
        fail(f"RSS median_kb ({rss_stats.get('median_kb')}) != actual median ({statistics.median(rss_series)})")

    if len(rss_series) >= 50:
        exp_f50 = round(statistics.mean(rss_series[:50]), 1)
        exp_l50 = round(statistics.mean(rss_series[-50:]), 1)
        if rss_stats.get("first_50_mean_kb") != exp_f50:
            fail(f"first_50_mean_kb mismatch: expected {exp_f50}, got {rss_stats.get('first_50_mean_kb')}")
        if rss_stats.get("last_50_mean_kb") != exp_l50:
            fail(f"last_50_mean_kb mismatch: expected {exp_l50}, got {rss_stats.get('last_50_mean_kb')}")

    initial_rss = res_obj.get("initial_current_rss_kb")
    end_rss = res_obj.get("end_current_rss_kb")
    peak_rss = res_obj.get("peak_rss_kb")

    if initial_rss is None or end_rss is None or peak_rss is None:
        fail(f"Summary resources missing initial, end, or peak RSS values")
    if peak_rss < max(rss_series):
        fail(f"Peak VmHWM ({peak_rss}) is less than max cycle VmRSS ({max(rss_series)})")

    print(f"    [OK] Verified {len(cycles)} cycle records, SHA-256 checksum {actual_sha256[:12]}...,")
    print(f"         request reconciliation ({sum_tot} reqs), FD tolerance (all settled <= {max_allowed_fds}),")
    print(f"         and derived RSS statistics (mean {rss_stats.get('mean_kb')} kB, envelope {rss_stats.get('min_kb')}..{rss_stats.get('max_kb')} kB).")


def main() -> None:
    parser = argparse.ArgumentParser(description="Validate DARKPOOL L6 Sustained Evidence")
    parser.add_argument("--repo-root", default=".", help="Repository root path")
    parser.add_argument("--smoke-report", default="docs/load_test_l6_smoke_10m_results.json", help="10-minute smoke summary JSON")
    parser.add_argument("--reliability-report", default="docs/load_test_l6_reliability_30m_results.json", help="30-minute reliability summary JSON")
    args = parser.parse_args()

    repo_root = os.path.abspath(args.repo_root)
    smoke_path = os.path.join(repo_root, args.smoke_report)
    reliability_path = os.path.join(repo_root, args.reliability_report)

    print("=================================================================")
    print(" DARKPOOL L6 Sustained Benchmark Evidence Validator")
    print("=================================================================")
    print(f" Repository Root     : {repo_root}")
    print(f" Smoke Summary       : {smoke_path}")
    print(f" Reliability Summary : {reliability_path}")
    print("=================================================================\n")

    validate_report_and_evidence(smoke_path, repo_root)
    print()
    validate_report_and_evidence(reliability_path, repo_root)

    print("\n=================================================================")
    print(" ALL L6 SUSTAINED EVIDENCE AUDITS PASSED (100% RECONCILED)")
    print("=================================================================\n")


if __name__ == "__main__":
    main()
