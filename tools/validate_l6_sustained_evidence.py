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
from typing import Dict, List, Any, Optional


class ValidationError(Exception):
    """Raised when evidence validation fails."""
    pass


def fail(msg: str) -> None:
    raise ValidationError(msg)


def check_strict_int(val: Any, name: str, min_val: Optional[int] = None, max_val: Optional[int] = None) -> int:
    if not isinstance(val, int) or isinstance(val, bool):
        fail(f"{name} must be a strict integer (got {type(val).__name__}: {val!r})")
    if min_val is not None and val < min_val:
        fail(f"{name} value {val} is less than minimum allowed {min_val}")
    if max_val is not None and val > max_val:
        fail(f"{name} value {val} exceeds maximum allowed {max_val}")
    return val


def check_strict_bool(val: Any, name: str, expected: Optional[bool] = None) -> bool:
    if not isinstance(val, bool):
        fail(f"{name} must be a strict boolean (got {type(val).__name__}: {val!r})")
    if expected is not None and val is not expected:
        fail(f"{name} is {val}, expected {expected}")
    return val


def check_strict_float_or_int(val: Any, name: str, min_val: Optional[float] = None) -> float:
    if isinstance(val, bool) or not isinstance(val, (int, float)):
        fail(f"{name} must be a number (got {type(val).__name__}: {val!r})")
    if min_val is not None and val < min_val:
        fail(f"{name} value {val} is less than minimum allowed {min_val}")
    return float(val)


def check_strict_str(val: Any, name: str, allow_empty: bool = False, expected: Optional[str] = None) -> str:
    if not isinstance(val, str):
        fail(f"{name} must be a string (got {type(val).__name__}: {val!r})")
    if not allow_empty and not val:
        fail(f"{name} cannot be empty string")
    if expected is not None and val != expected:
        fail(f"{name} is '{val}', expected '{expected}'")
    return val


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

    # 1. Top-level status, stage, duration and metadata
    status = check_strict_str(summary.get("status"), "status", expected="PASS")
    stage_name = check_strict_str(summary.get("stage_name"), "stage_name")
    if stage_name not in ("smoke_10m", "reliability_30m"):
        fail(f"Unexpected stage_name '{stage_name}', expected 'smoke_10m' or 'reliability_30m'")

    expected_duration_s = 600 if stage_name == "smoke_10m" else 1800
    target_duration_s = check_strict_int(summary.get("target_duration_s"), "target_duration_s", min_val=1)
    if target_duration_s != expected_duration_s:
        fail(f"target_duration_s {target_duration_s} != expected {expected_duration_s} for {stage_name}")

    actual_duration_ms = check_strict_int(summary.get("actual_duration_ms"), "actual_duration_ms", min_val=target_duration_s * 1000)
    actual_duration_s = check_strict_float_or_int(summary.get("actual_duration_s"), "actual_duration_s", min_val=float(target_duration_s))
    if abs(actual_duration_s - (actual_duration_ms / 1000.0)) > 0.002:
        fail(f"actual_duration_s ({actual_duration_s}) inconsistent with actual_duration_ms ({actual_duration_ms})")

    source_commit = check_strict_str(summary.get("source_commit"), "source_commit")
    expected_commit = "a443d0a349a6ca8436fa9ce3751ae1ac3f7803ec"
    if source_commit != expected_commit:
        fail(f"Summary {summary_path} source_commit '{source_commit}' does not match expected tested commit '{expected_commit}'")
    git_dirty = check_strict_bool(summary.get("git_dirty"), "git_dirty", expected=False)

    completed_cycles = check_strict_int(summary.get("completed_cycles"), "completed_cycles", min_val=1)
    completed_matches = check_strict_int(summary.get("completed_matches"), "completed_matches", min_val=4)
    if completed_matches != completed_cycles * 4:
        fail(f"Completed matches ({completed_matches}) does not equal 4 * completed_cycles ({completed_cycles * 4})")

    # Topology checks
    topology = summary.get("concurrency_topology")
    if not isinstance(topology, dict):
        fail(f"Summary {summary_path} missing concurrency_topology object")
    check_strict_int(topology.get("concurrent_matches"), "concurrency_topology.concurrent_matches", min_val=4, max_val=4)
    check_strict_int(topology.get("players_per_match"), "concurrency_topology.players_per_match", min_val=64, max_val=64)
    check_strict_int(topology.get("total_player_clients"), "concurrency_topology.total_player_clients", min_val=256, max_val=256)
    check_strict_int(topology.get("moderator_clients"), "concurrency_topology.moderator_clients", min_val=4, max_val=4)
    check_strict_int(topology.get("total_connected_clients"), "concurrency_topology.total_connected_clients", min_val=260, max_val=260)
    check_strict_int(topology.get("connection_cap"), "concurrency_topology.connection_cap", min_val=260, max_val=260)
    check_strict_bool(topology.get("persistent_server"), "concurrency_topology.persistent_server", expected=True)

    # 2. Check evidence file block
    evidence_meta = summary.get("evidence_file")
    if not isinstance(evidence_meta, dict):
        fail(f"Summary {summary_path} missing 'evidence_file' metadata object")

    rel_evidence_path = check_strict_str(evidence_meta.get("relative_path"), "evidence_file.relative_path")
    evidence_abs_path = os.path.join(repo_root, rel_evidence_path)
    if not os.path.isfile(evidence_abs_path):
        fail(f"Cycle evidence file missing at: {evidence_abs_path}")

    expected_records = check_strict_int(evidence_meta.get("expected_records"), "evidence_file.expected_records", min_val=1)
    if expected_records != completed_cycles:
        fail(f"evidence_file.expected_records ({expected_records}) != completed_cycles ({completed_cycles})")

    ev_source_commit = check_strict_str(evidence_meta.get("source_commit"), "evidence_file.source_commit", expected=source_commit)
    ev_git_dirty = check_strict_bool(evidence_meta.get("git_dirty"), "evidence_file.git_dirty", expected=False)
    expected_sha256 = check_strict_str(evidence_meta.get("sha256"), "evidence_file.sha256")

    actual_sha256 = sha256_file(evidence_abs_path)
    if actual_sha256.lower() != expected_sha256.lower():
        fail(f"Evidence file checksum mismatch for {evidence_abs_path}:\n  Recorded: {expected_sha256}\n  Computed: {actual_sha256}")

    # 3. Read and validate each cycle in JSONL
    print(f"    Parsing and auditing {completed_cycles} cycle records from {rel_evidence_path}...")
    res_obj = summary.get("resources")
    if not isinstance(res_obj, dict):
        fail(f"Summary {summary_path} missing 'resources' object")

    baseline_open_fds = check_strict_int(res_obj.get("baseline_open_fds"), "resources.baseline_open_fds", min_val=1)
    max_allowed_settled_fds = baseline_open_fds + 2

    cycles: List[Dict[str, Any]] = []
    line_num = 0
    accum_total_requests = 0
    accum_successful_requests = 0
    accum_expected_rejections = 0
    accum_transport_failures = 0
    accum_correctness_failures = 0
    accum_matches = 0
    rss_series: List[int] = []
    prev_elapsed_ms = -1

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

                if not isinstance(cyc, dict):
                    fail(f"Line {line_num} of {rel_evidence_path} must be a JSON object")

                cycles.append(cyc)

                # Sequential cycle index check
                cyc_idx = check_strict_int(cyc.get("cycle_index"), f"Line {line_num} cycle_index", min_val=1)
                if cyc_idx != line_num:
                    fail(f"Cycle index out of sequence at line {line_num} (got {cyc_idx}, expected {line_num})")

                # Elapsed time check (strictly increasing)
                elapsed_ms = check_strict_int(cyc.get("elapsed_ms"), f"Cycle {cyc_idx} elapsed_ms", min_val=0)
                if elapsed_ms < prev_elapsed_ms:
                    fail(f"Cycle {cyc_idx} elapsed_ms ({elapsed_ms}) decreased from previous ({prev_elapsed_ms})")
                prev_elapsed_ms = elapsed_ms

                check_strict_int(cyc.get("cycle_duration_ms"), f"Cycle {cyc_idx} cycle_duration_ms", min_val=0)

                # Completed and match count check
                check_strict_bool(cyc.get("completed"), f"Cycle {cyc_idx} completed", expected=True)
                matches_done = check_strict_int(cyc.get("matches_completed"), f"Cycle {cyc_idx} matches_completed", min_val=4, max_val=4)
                accum_matches += matches_done

                check_strict_bool(cyc.get("matches_overlapped"), f"Cycle {cyc_idx} matches_overlapped", expected=True)
                check_strict_int(cyc.get("overlap_duration_ms"), f"Cycle {cyc_idx} overlap_duration_ms", min_val=0)
                check_strict_str(cyc.get("failure_reason"), f"Cycle {cyc_idx} failure_reason", allow_empty=True, expected="")
                check_strict_str(cyc.get("cleanup_failure_reason"), f"Cycle {cyc_idx} cleanup_failure_reason", allow_empty=True, expected="")
                check_strict_bool(cyc.get("cleanup_passed"), f"Cycle {cyc_idx} cleanup_passed", expected=True)
                check_strict_int(cyc.get("cleanup_settle_time_ms"), f"Cycle {cyc_idx} cleanup_settle_time_ms", min_val=0)

                # FD sampling and threshold checks
                check_strict_bool(cyc.get("peak_connections_fd_sample_ok"), f"Cycle {cyc_idx} peak_connections_fd_sample_ok", expected=True)
                peak_fds = check_strict_int(cyc.get("peak_connections_open_fds"), f"Cycle {cyc_idx} peak_connections_open_fds", min_val=baseline_open_fds)

                check_strict_bool(cyc.get("settled_post_cleanup_fd_sample_ok"), f"Cycle {cyc_idx} settled_post_cleanup_fd_sample_ok", expected=True)
                settled_fds = check_strict_int(cyc.get("settled_post_cleanup_open_fds"), f"Cycle {cyc_idx} settled_post_cleanup_open_fds", min_val=1)

                if settled_fds > max_allowed_settled_fds:
                    fail(f"Cycle {cyc_idx} settled_open_fds ({settled_fds}) exceeds baseline + 2 tolerance ({max_allowed_settled_fds})")

                check_strict_bool(cyc.get("immediate_post_close_fd_sample_ok"), f"Cycle {cyc_idx} immediate_post_close_fd_sample_ok", expected=True)
                check_strict_int(cyc.get("immediate_post_close_open_fds"), f"Cycle {cyc_idx} immediate_post_close_open_fds", min_val=1)

                # RSS sampling checks
                check_strict_bool(cyc.get("peak_conn_rss_sample_ok"), f"Cycle {cyc_idx} peak_conn_rss_sample_ok", expected=True)
                check_strict_int(cyc.get("peak_conn_current_rss_kb"), f"Cycle {cyc_idx} peak_conn_current_rss_kb", min_val=1)

                check_strict_bool(cyc.get("post_cleanup_current_rss_sample_ok"), f"Cycle {cyc_idx} post_cleanup_current_rss_sample_ok", expected=True)
                post_rss = check_strict_int(cyc.get("post_cleanup_current_rss_kb"), f"Cycle {cyc_idx} post_cleanup_current_rss_kb", min_val=1)
                rss_series.append(post_rss)

                check_strict_bool(cyc.get("post_cleanup_peak_rss_sample_ok"), f"Cycle {cyc_idx} post_cleanup_peak_rss_sample_ok", expected=True)
                post_peak = check_strict_int(cyc.get("post_cleanup_peak_rss_kb"), f"Cycle {cyc_idx} post_cleanup_peak_rss_kb", min_val=post_rss)

                # Request reconciliation per cycle (all 5 counters)
                reqs = cyc.get("requests")
                if not isinstance(reqs, dict):
                    fail(f"Cycle {cyc_idx} missing 'requests' object")

                c_tot = check_strict_int(reqs.get("total_requests"), f"Cycle {cyc_idx} requests.total_requests", min_val=1)
                c_succ = check_strict_int(reqs.get("successful_requests"), f"Cycle {cyc_idx} requests.successful_requests", min_val=0)
                c_rej = check_strict_int(reqs.get("expected_rejections"), f"Cycle {cyc_idx} requests.expected_rejections", min_val=0)
                c_tf = check_strict_int(reqs.get("transport_failures"), f"Cycle {cyc_idx} requests.transport_failures", min_val=0)
                c_cf = check_strict_int(reqs.get("correctness_failures"), f"Cycle {cyc_idx} requests.correctness_failures", min_val=0)

                if c_tot != (c_succ + c_rej + c_tf + c_cf):
                    fail(f"Cycle {cyc_idx} requests reconciliation failed: {c_tot} != {c_succ} (succ) + {c_rej} (rej) + {c_tf} (tf) + {c_cf} (cf)")

                accum_total_requests += c_tot
                accum_successful_requests += c_succ
                accum_expected_rejections += c_rej
                accum_transport_failures += c_tf
                accum_correctness_failures += c_cf
    except ValidationError:
        raise
    except Exception as e:
        fail(f"Error reading cycle evidence {evidence_abs_path}: {e}")

    # 4. Reconcile evidence counts with summary counts
    if len(cycles) != completed_cycles:
        fail(f"Evidence file line count ({len(cycles)}) != summary completed_cycles ({completed_cycles})")
    if accum_matches != completed_matches:
        fail(f"Accumulated matches ({accum_matches}) != summary completed_matches ({completed_matches})")

    sum_tot = check_strict_int(summary.get("total_requests"), "summary.total_requests", min_val=1)
    sum_succ = check_strict_int(summary.get("successful_requests"), "summary.successful_requests", min_val=1)
    sum_rej = check_strict_int(summary.get("expected_rejections"), "summary.expected_rejections", min_val=0)
    sum_tf = check_strict_int(summary.get("transport_failures"), "summary.transport_failures", min_val=0)
    sum_cf = check_strict_int(summary.get("correctness_failures"), "summary.correctness_failures", min_val=0)

    if sum_tot != (sum_succ + sum_rej + sum_tf + sum_cf):
        fail(f"Summary top-level request reconciliation failed: {sum_tot} != {sum_succ} + {sum_rej} + {sum_tf} + {sum_cf}")

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

    # Verify requests sub-object matches top-level and evidence
    req_obj = summary.get("requests")
    if not isinstance(req_obj, dict):
        fail(f"Summary {summary_path} missing 'requests' sub-object")

    r_tot = check_strict_int(req_obj.get("total_requests"), "summary.requests.total_requests")
    r_succ = check_strict_int(req_obj.get("successful_requests"), "summary.requests.successful_requests")
    r_rej = check_strict_int(req_obj.get("expected_rejections"), "summary.requests.expected_rejections")
    r_tf = check_strict_int(req_obj.get("transport_failures"), "summary.requests.transport_failures")
    r_cf = check_strict_int(req_obj.get("correctness_failures"), "summary.requests.correctness_failures")

    if (r_tot, r_succ, r_rej, r_tf, r_cf) != (sum_tot, sum_succ, sum_rej, sum_tf, sum_cf):
        fail("Summary requests sub-object counters differ from summary top-level counters")

    # Verify summary failures sub-object
    fail_obj = summary.get("failures")
    if not isinstance(fail_obj, dict):
        fail(f"Summary {summary_path} missing 'failures' sub-object")
    f_tf = check_strict_int(fail_obj.get("transport_failures"), "summary.failures.transport_failures")
    f_cf = check_strict_int(fail_obj.get("correctness_failures"), "summary.failures.correctness_failures")
    check_strict_int(fail_obj.get("timeouts"), "summary.failures.timeouts", min_val=0, max_val=0)
    check_strict_int(fail_obj.get("unexpected_disconnects"), "summary.failures.unexpected_disconnects", min_val=0, max_val=0)
    if (f_tf, f_cf) != (sum_tf, sum_cf):
        fail("Summary failures sub-object counters differ from summary transport/correctness counters")

    # 5. Validate summary cycle_diagnostics against complete evidence
    diag = summary.get("cycle_diagnostics")
    if not isinstance(diag, dict):
        fail(f"Summary {summary_path} missing 'cycle_diagnostics' object")

    check_strict_int(diag.get("completed_cycles"), "cycle_diagnostics.completed_cycles", min_val=completed_cycles, max_val=completed_cycles)
    check_strict_int(diag.get("completed_matches"), "cycle_diagnostics.completed_matches", min_val=completed_matches, max_val=completed_matches)
    check_strict_int(diag.get("total_cycles_attempted"), "cycle_diagnostics.total_cycles_attempted", min_val=completed_cycles, max_val=completed_cycles)
    check_strict_int(diag.get("evidence_records_count"), "cycle_diagnostics.evidence_records_count", min_val=completed_cycles, max_val=completed_cycles)
    check_strict_int(diag.get("match_limit_retries"), "cycle_diagnostics.match_limit_retries", min_val=0, max_val=0)
    check_strict_bool(diag.get("all_cycles_fd_cleanup_passed"), "cycle_diagnostics.all_cycles_fd_cleanup_passed", expected=True)
    check_strict_bool(diag.get("all_cycles_overlapped"), "cycle_diagnostics.all_cycles_overlapped", expected=True)

    # 6. Validate summary resources and sampling flags
    check_strict_bool(res_obj.get("all_cycles_fd_cleanup_passed"), "resources.all_cycles_fd_cleanup_passed", expected=True)
    check_strict_bool(res_obj.get("baseline_open_fds_sample_ok"), "resources.baseline_open_fds_sample_ok", expected=True)
    check_strict_bool(res_obj.get("resource_sampling_ok"), "resources.resource_sampling_ok", expected=True)
    check_strict_bool(res_obj.get("initial_current_rss_sample_ok"), "resources.initial_current_rss_sample_ok", expected=True)
    check_strict_bool(res_obj.get("end_current_rss_sample_ok"), "resources.end_current_rss_sample_ok", expected=True)
    check_strict_bool(res_obj.get("peak_rss_sample_ok"), "resources.peak_rss_sample_ok", expected=True)

    check_strict_int(res_obj.get("current_rss_sample_count"), "resources.current_rss_sample_count", min_val=len(cycles), max_val=len(cycles))

    # FD checks in summary resources
    expected_peak_fds = max(c["peak_connections_open_fds"] for c in cycles)
    check_strict_int(res_obj.get("peak_open_fds"), "resources.peak_open_fds", min_val=expected_peak_fds, max_val=expected_peak_fds)

    last_cycle_settled_fds = cycles[-1]["settled_post_cleanup_open_fds"]
    check_strict_int(res_obj.get("settled_post_cleanup_fds"), "resources.settled_post_cleanup_fds", min_val=last_cycle_settled_fds, max_val=last_cycle_settled_fds)

    last_cycle_immed_fds = cycles[-1]["immediate_post_close_open_fds"]
    check_strict_int(res_obj.get("immediate_post_cleanup_open_fds"), "resources.immediate_post_cleanup_open_fds", min_val=last_cycle_immed_fds, max_val=last_cycle_immed_fds)

    # 7. Validate RSS summary statistics against underlying series
    rss_stats = res_obj.get("rss_summary")
    if not isinstance(rss_stats, dict):
        fail(f"Summary {summary_path} resources missing 'rss_summary' object")

    check_strict_int(rss_stats.get("sample_count"), "rss_summary.sample_count", min_val=len(rss_series), max_val=len(rss_series))
    check_strict_int(rss_stats.get("min_kb"), "rss_summary.min_kb", min_val=min(rss_series), max_val=min(rss_series))
    check_strict_int(rss_stats.get("max_kb"), "rss_summary.max_kb", min_val=max(rss_series), max_val=max(rss_series))

    expected_mean = round(statistics.mean(rss_series), 1)
    actual_mean = check_strict_float_or_int(rss_stats.get("mean_kb"), "rss_summary.mean_kb")
    if round(actual_mean, 1) != expected_mean:
        fail(f"RSS mean_kb ({actual_mean}) != actual mean ({expected_mean})")

    expected_median = float(statistics.median(rss_series))
    actual_median = check_strict_float_or_int(rss_stats.get("median_kb"), "rss_summary.median_kb")
    if actual_median != expected_median:
        fail(f"RSS median_kb ({actual_median}) != actual median ({expected_median})")

    if len(rss_series) >= 50:
        exp_f50 = round(statistics.mean(rss_series[:50]), 1)
        exp_l50 = round(statistics.mean(rss_series[-50:]), 1)
        act_f50 = check_strict_float_or_int(rss_stats.get("first_50_mean_kb"), "rss_summary.first_50_mean_kb")
        act_l50 = check_strict_float_or_int(rss_stats.get("last_50_mean_kb"), "rss_summary.last_50_mean_kb")
        if act_f50 != exp_f50:
            fail(f"first_50_mean_kb mismatch: expected {exp_f50}, got {act_f50}")
        if act_l50 != exp_l50:
            fail(f"last_50_mean_kb mismatch: expected {exp_l50}, got {act_l50}")

    initial_rss = check_strict_int(res_obj.get("initial_current_rss_kb"), "resources.initial_current_rss_kb", min_val=1)
    end_rss = check_strict_int(res_obj.get("end_current_rss_kb"), "resources.end_current_rss_kb", min_val=1)
    peak_rss = check_strict_int(res_obj.get("peak_rss_kb"), "resources.peak_rss_kb", min_val=max(rss_series))

    # Check aliases initial_rss_kb and end_rss_kb match
    check_strict_int(res_obj.get("initial_rss_kb"), "resources.initial_rss_kb", min_val=initial_rss, max_val=initial_rss)
    check_strict_int(res_obj.get("end_rss_kb"), "resources.end_rss_kb", min_val=end_rss, max_val=end_rss)

    # Final cycle post cleanup RSS should match end_current_rss_kb
    if cycles[-1]["post_cleanup_current_rss_kb"] != end_rss:
        fail(f"Final cycle post_cleanup_current_rss_kb ({cycles[-1]['post_cleanup_current_rss_kb']}) != end_current_rss_kb ({end_rss})")

    print(f"    [OK] Verified {len(cycles)} cycle records, SHA-256 checksum {actual_sha256[:12]}...,")
    print(f"         request reconciliation ({sum_tot} reqs), FD tolerance (all settled <= {max_allowed_settled_fds}),")
    print(f"         and derived RSS statistics (mean {actual_mean} kB, envelope {rss_stats.get('min_kb')}..{rss_stats.get('max_kb')} kB).")


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

    try:
        validate_report_and_evidence(smoke_path, repo_root)
        print()
        validate_report_and_evidence(reliability_path, repo_root)
    except ValidationError as e:
        print(f"\nVALIDATION ERROR: {e}", file=sys.stderr)
        sys.exit(1)
    except Exception as e:
        print(f"\nUNEXPECTED ERROR: {e}", file=sys.stderr)
        sys.exit(1)

    print("\n=================================================================")
    print(" ALL L6 SUSTAINED EVIDENCE AUDITS PASSED (100% RECONCILED)")
    print("=================================================================\n")


if __name__ == "__main__":
    main()
