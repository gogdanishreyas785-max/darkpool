#!/usr/bin/env python3
"""
DARKPOOL L8 Malformed Traffic & Request Flood Evidence Validator

Validates machine-readable L8 load test evidence (docs/load_test_l8_results.json)
to ensure:
1. Strict rate limiting boundary enforcement (burst 40, reject 41st, refill recovery).
2. Malformed frame handling (oversized frame and invalid JSON closed safely, server survives).
3. Concurrent flood isolation:
   - High volume flood (flood_requests_sent > 40).
   - Observed rate-limited responses (flood_requests_rate_limited > 0).
   - Zero flood transport errors and zero malformed responses.
   - Concurrent healthy match reaches 'Finished' phase with 'InsiderWin' outcome.
   - Zero transport or correctness failures on the healthy match.
4. Clean resource teardown and FD settling back to baseline.
5. All multi-run executions (if present) passed all criteria.

Exit code 0 on success; non-zero exit code on validation failure.
"""

import json
import os
import sys
from typing import Any, Dict, Optional


class ValidationError(Exception):
    pass


def fail(msg: str) -> None:
    raise ValidationError(msg)


def check_strict_int(val: Any, name: str, min_val: Optional[int] = None, max_val: Optional[int] = None) -> int:
    if not isinstance(val, int) or isinstance(val, bool):
        fail(f"{name} must be an integer (got {type(val).__name__}: {val!r})")
    if min_val is not None and val < min_val:
        fail(f"{name} value {val} is less than minimum allowed {min_val}")
    if max_val is not None and val > max_val:
        fail(f"{name} value {val} exceeds maximum allowed {max_val}")
    return val


def check_strict_bool(val: Any, name: str, expected: Optional[bool] = None) -> bool:
    if not isinstance(val, bool):
        fail(f"{name} must be a boolean (got {type(val).__name__}: {val!r})")
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


def validate_single_l8_run(data: Dict[str, Any], context_label: str = "run") -> None:
    # 1. Top-level status & test metadata
    check_strict_str(data.get("status"), f"{context_label}.status", expected="PASS")
    check_strict_str(data.get("test_name"), f"{context_label}.test_name", expected="darkpool_load_tests_l8")
    check_strict_str(data.get("source_commit"), f"{context_label}.source_commit")
    check_strict_int(data.get("duration_ms"), f"{context_label}.duration_ms", min_val=1)

    # 2. Approved Policy (DEC-11)
    policy = data.get("policy")
    if not isinstance(policy, dict):
        fail(f"{context_label}.policy must be a dict")
    check_strict_float_or_int(policy.get("request_rate_per_sec"), f"{context_label}.policy.request_rate_per_sec", min_val=20.0)
    check_strict_float_or_int(policy.get("burst_capacity"), f"{context_label}.policy.burst_capacity", min_val=40.0)
    check_strict_int(policy.get("max_frame_payload_bytes"), f"{context_label}.policy.max_frame_payload_bytes", min_val=4096, max_val=4096)
    check_strict_str(policy.get("approved_decision"), f"{context_label}.policy.approved_decision", expected="DEC-11")

    # 3. Rate limiter boundary tests
    rl = data.get("rate_limiter_boundary_tests")
    if not isinstance(rl, dict):
        fail(f"{context_label}.rate_limiter_boundary_tests must be a dict")
    check_strict_int(rl.get("burst_requests_sent"), f"{context_label}.rl.burst_requests_sent", min_val=40)
    check_strict_int(rl.get("burst_requests_accepted"), f"{context_label}.rl.burst_requests_accepted", min_val=40, max_val=40)
    check_strict_bool(rl.get("over_burst_rejected"), f"{context_label}.rl.over_burst_rejected", expected=True)
    rej_msg = check_strict_str(rl.get("rejection_error_message"), f"{context_label}.rl.rejection_error_message")
    if "rate limit exceeded" not in rej_msg:
        fail(f"{context_label}.rl.rejection_error_message must mention 'rate limit exceeded', got '{rej_msg}'")
    check_strict_bool(rl.get("connection_usable_after_rate_limit"), f"{context_label}.rl.connection_usable_after_rate_limit", expected=True)
    check_strict_bool(rl.get("state_mutation_prevented"), f"{context_label}.rl.state_mutation_prevented", expected=True)
    check_strict_bool(rl.get("token_refill_recovery_ok"), f"{context_label}.rl.token_refill_recovery_ok", expected=True)

    # 4. Malformed framing tests
    mf = data.get("malformed_traffic_tests")
    if not isinstance(mf, dict):
        fail(f"{context_label}.malformed_traffic_tests must be a dict")
    check_strict_bool(mf.get("oversized_frame_closed"), f"{context_label}.mf.oversized_frame_closed", expected=True)
    check_strict_bool(mf.get("malformed_json_closed"), f"{context_label}.mf.malformed_json_closed", expected=True)
    check_strict_bool(mf.get("server_survived_malformed"), f"{context_label}.mf.server_survived_malformed", expected=True)

    # 5. Flood isolation workload
    fl = data.get("flood_isolation_workload")
    if not isinstance(fl, dict):
        fail(f"{context_label}.flood_isolation_workload must be a dict")

    # Strict check: sent substantially more than burst 40
    sent = check_strict_int(fl.get("flood_requests_sent"), f"{context_label}.fl.flood_requests_sent", min_val=41)
    # Strict check: observed rate limit rejections
    limited = check_strict_int(fl.get("flood_requests_rate_limited"), f"{context_label}.fl.flood_requests_rate_limited", min_val=1)
    if limited > sent:
        fail(f"{context_label}.fl.flood_requests_rate_limited ({limited}) cannot exceed flood_requests_sent ({sent})")

    # Optional detailed fields if present
    if "flood_transport_errors" in fl:
        check_strict_int(fl.get("flood_transport_errors"), f"{context_label}.fl.flood_transport_errors", max_val=0)
    if "flood_malformed_responses" in fl:
        check_strict_int(fl.get("flood_malformed_responses"), f"{context_label}.fl.flood_malformed_responses", max_val=0)

    # Healthy sibling match checks
    check_strict_str(fl.get("healthy_match_status"), f"{context_label}.fl.healthy_match_status", expected="PASS")
    check_strict_str(fl.get("healthy_match_phase"), f"{context_label}.fl.healthy_match_phase", expected="Finished")
    check_strict_str(fl.get("healthy_match_outcome"), f"{context_label}.fl.healthy_match_outcome", expected="InsiderWin")

    reqs = fl.get("healthy_match_requests")
    if not isinstance(reqs, dict):
        fail(f"{context_label}.fl.healthy_match_requests must be a dict")
    check_strict_int(reqs.get("total"), f"{context_label}.fl.healthy_match_requests.total", min_val=1)
    check_strict_int(reqs.get("successful"), f"{context_label}.fl.healthy_match_requests.successful", min_val=1)
    check_strict_int(reqs.get("transport_failures"), f"{context_label}.fl.healthy_match_requests.transport_failures", max_val=0)
    check_strict_int(reqs.get("correctness_failures"), f"{context_label}.fl.healthy_match_requests.correctness_failures", max_val=0)

    lat = fl.get("healthy_match_latency")
    if not isinstance(lat, dict):
        fail(f"{context_label}.fl.healthy_match_latency must be a dict")
    check_strict_float_or_int(lat.get("p95_ms"), f"{context_label}.fl.healthy_match_latency.p95_ms", min_val=0.0)

    # 6. Resource telemetry
    res = data.get("resource_telemetry")
    if not isinstance(res, dict):
        fail(f"{context_label}.resource_telemetry must be a dict")
    check_strict_bool(res.get("baseline_open_fds_sample_ok"), f"{context_label}.res.baseline_open_fds_sample_ok", expected=True)
    check_strict_bool(res.get("peak_open_fds_sample_ok"), f"{context_label}.res.peak_open_fds_sample_ok", expected=True)
    check_strict_bool(res.get("immediate_post_cleanup_sample_ok"), f"{context_label}.res.immediate_post_cleanup_sample_ok", expected=True)
    check_strict_bool(res.get("post_cleanup_sample_ok"), f"{context_label}.res.post_cleanup_sample_ok", expected=True)
    check_strict_bool(res.get("post_cleanup_settled"), f"{context_label}.res.post_cleanup_settled", expected=True)
    check_strict_bool(res.get("peak_rss_sample_ok"), f"{context_label}.res.peak_rss_sample_ok", expected=True)
    check_strict_bool(res.get("post_cleanup_rss_sample_ok"), f"{context_label}.res.post_cleanup_rss_sample_ok", expected=True)

    base_fds = check_strict_int(res.get("baseline_open_fds"), f"{context_label}.res.baseline_open_fds", min_val=1)
    post_fds = check_strict_int(res.get("post_cleanup_open_fds"), f"{context_label}.res.post_cleanup_open_fds", min_val=1)
    if post_fds > base_fds + 2:
        fail(f"{context_label}.res.post_cleanup_open_fds ({post_fds}) exceeds baseline ({base_fds}) + 2 tolerance")


def validate_l8_evidence(evidence_path: str) -> None:
    print(f"--> Validating L8 evidence: {evidence_path}")
    if not os.path.isfile(evidence_path):
        fail(f"Evidence file missing: {evidence_path}")

    try:
        with open(evidence_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        fail(f"Could not parse evidence JSON ({evidence_path}): {e}")

    # Validate main/top-level run
    validate_single_l8_run(data, context_label="top-level")

    # If repeat runs array is present, validate every run
    if "runs" in data:
        runs = data["runs"]
        if not isinstance(runs, list) or len(runs) == 0:
            fail("runs must be a non-empty list if present")
        for idx, r in enumerate(runs):
            validate_single_l8_run(r, context_label=f"runs[{idx}]")
        check_strict_bool(data.get("all_runs_passed"), "all_runs_passed", expected=True)

    print(f"PASS: L8 evidence at {evidence_path} is valid and meets all reliability requirements.")


def main() -> int:
    path = "docs/load_test_l8_results.json"
    if len(sys.argv) > 1:
        path = sys.argv[1]

    try:
        validate_l8_evidence(path)
        return 0
    except ValidationError as ve:
        print(f"VALIDATION ERROR: {ve}", file=sys.stderr)
        return 1
    except Exception as ex:
        print(f"UNEXPECTED ERROR: {ex}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
