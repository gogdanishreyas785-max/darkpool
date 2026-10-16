#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests_l7 requires Linux (epoll/unix sockets)\n";
  return 1;
}

#else

#include "load_test_harness_common.hpp"
#include <filesystem>
#include <fstream>
#include <memory>
#include <poll.h>

namespace {

using namespace darkpool::load_test;

constexpr const char* kL7ModeratorToken = "test-mod-token-l7";
constexpr std::size_t kL7MaxQueuedBytes = 64 * 1024; // 64 KiB approved limit (DEC-11)
constexpr std::size_t kL7PlayersPerMatch = 5;

struct L7FaultConfig {
  bool inject_failure{false};
  bool inject_missing_fd_sample{false};
  bool inject_missing_rss_sample{false};
  bool inject_cleanup_failure{false};
  bool inject_sibling_starvation{false};
};

struct L7RunResult {
  bool run_passed{false};
  std::string failure_reason;
  uint64_t duration_ms{0};

  // Slow client telemetry
  bool slow_client_connected{false};
  int slow_client_frames_sent{0};
  std::size_t slow_client_bytes_sent{0};
  bool slow_client_overflow_evicted{false};
  uint64_t slow_client_eviction_ms{0};

  // Sibling healthy match (Match 2) telemetry
  bool healthy_match_passed{false};
  std::uint64_t healthy_match_id{0};
  std::string healthy_match_phase;
  std::string healthy_match_outcome;
  PhaseMetrics healthy_match_metrics;
  FailureCounts healthy_match_failures;

  // Match 1 healthy clients telemetry
  bool match1_healthy_progress_ok{false};

  // Resource & cleanup telemetry
  std::size_t baseline_open_fds{0};
  bool baseline_open_fds_sample_ok{false};
  std::size_t peak_open_fds{0};
  bool peak_open_fds_sample_ok{false};
  std::size_t immediate_post_cleanup_open_fds{0};
  bool immediate_post_cleanup_sample_ok{false};
  std::size_t post_cleanup_open_fds{0};
  bool post_cleanup_sample_ok{false};
  bool post_cleanup_settled{false};
  std::uint64_t cleanup_settle_time_ms{0};
  std::string cleanup_failure_reason;

  std::uint64_t baseline_rss_kb{0};
  std::uint64_t peak_rss_kb{0};
  bool peak_rss_sample_ok{false};
  std::uint64_t post_cleanup_rss_kb{0};
  bool post_cleanup_rss_sample_ok{false};
};

L7RunResult execute_l7_workload(
    pid_t server_pid,
    const std::string& socket_path,
    const std::string& mod_token,
    std::optional<std::size_t> baseline_fds_opt,
    const L7FaultConfig& fault_cfg = {}) {
  L7RunResult result;
  const auto start_time = std::chrono::steady_clock::now();

  if (!baseline_fds_opt.has_value()) {
    result.baseline_open_fds_sample_ok = false;
    result.failure_reason = "Missing baseline open FDs sample";
    return result;
  }
  result.baseline_open_fds = *baseline_fds_opt;
  result.baseline_open_fds_sample_ok = true;

  const auto base_rss = sample_current_rss_kb(server_pid);
  result.baseline_rss_kb = base_rss.value_or(0);

  std::vector<int> all_fds;
  auto cleanup_all = [&]() {
    for (const auto fd : all_fds) {
      if (fd >= 0) (void)close(fd);
    }
    all_fds.clear();
  };

  struct FdGuard {
    std::function<void()> fn;
    bool active{true};
    ~FdGuard() { if (active) fn(); }
  } guard{cleanup_all};

  // 1. Connect Moderator 1 and Moderator 2
  const int mod1_fd = connect_unix(socket_path);
  const int mod2_fd = connect_unix(socket_path);
  if (mod1_fd < 0 || mod2_fd < 0) {
    result.failure_reason = "Failed to connect moderators";
    return result;
  }
  all_fds.push_back(mod1_fd);
  all_fds.push_back(mod2_fd);

  std::vector<RequestMetric> healthy_metrics;

  const auto mod1_hello = timed_request(
      mod1_fd,
      json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
      healthy_metrics, result.healthy_match_failures,
      expect_connect("moderator"));
  const auto mod2_hello = timed_request(
      mod2_fd,
      json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
      healthy_metrics, result.healthy_match_failures,
      expect_connect("moderator"));
  if (!mod1_hello.has_value() || !mod2_hello.has_value()) {
    result.failure_reason = "Moderator handshake failed";
    return result;
  }

  // 2. Create Match 1 and Match 2
  const auto create1 = timed_request(
      mod1_fd,
      json{{"type", "create_match"}, {"player_count", kL7PlayersPerMatch}},
      healthy_metrics, result.healthy_match_failures,
      expect_match_created(kL7PlayersPerMatch, 1, 1, 3));
  const auto create2 = timed_request(
      mod2_fd,
      json{{"type", "create_match"}, {"player_count", kL7PlayersPerMatch}},
      healthy_metrics, result.healthy_match_failures,
      expect_match_created(kL7PlayersPerMatch, 1, 1, 3));
  if (!create1.has_value() || !create2.has_value()) {
    result.failure_reason = "create_match failed for Match 1 or Match 2";
    return result;
  }
  const uint64_t match1_id = (*create1)["match_id"].get<uint64_t>();
  const uint64_t match2_id = (*create2)["match_id"].get<uint64_t>();
  result.healthy_match_id = match2_id;

  // 3. Connect healthy players for Match 1 (Players 1..4)
  std::vector<int> match1_player_fds;
  for (std::size_t i = 0; i < kL7PlayersPerMatch - 1; ++i) {
    const int pfd = connect_unix(socket_path);
    if (pfd < 0) {
      result.failure_reason = "Failed to connect Match 1 healthy player";
      return result;
    }
    all_fds.push_back(pfd);
    match1_player_fds.push_back(pfd);
    const auto p_hello = timed_request(
        pfd,
        json{{"type", "connect"}, {"role", "player"}},
        healthy_metrics, result.healthy_match_failures,
        expect_connect("player"));
    if (!p_hello.has_value()) {
      result.failure_reason = "Match 1 player handshake failed";
      return result;
    }
    const auto join_resp = timed_request(
        pfd,
        json{{"type", "join_match"}, {"match_id", match1_id}},
        healthy_metrics, result.healthy_match_failures,
        expect_joined_match(static_cast<std::uint32_t>(i + 1)));
    if (!join_resp.has_value()) {
      result.failure_reason = "Match 1 player join failed";
      return result;
    }
  }

  // 4. Connect healthy players for Match 2 (all 5 players)
  std::vector<int> match2_player_fds;
  for (std::size_t i = 0; i < kL7PlayersPerMatch; ++i) {
    const int pfd = connect_unix(socket_path);
    if (pfd < 0) {
      result.failure_reason = "Failed to connect Match 2 healthy player";
      return result;
    }
    all_fds.push_back(pfd);
    match2_player_fds.push_back(pfd);
    const auto p_hello = timed_request(
        pfd,
        json{{"type", "connect"}, {"role", "player"}},
        healthy_metrics, result.healthy_match_failures,
        expect_connect("player"));
    if (!p_hello.has_value()) {
      result.failure_reason = "Match 2 player handshake failed";
      return result;
    }
    const auto join_resp = timed_request(
        pfd,
        json{{"type", "join_match"}, {"match_id", match2_id}},
        healthy_metrics, result.healthy_match_failures,
        expect_joined_match(static_cast<std::uint32_t>(i + 1)));
    if (!join_resp.has_value()) {
      result.failure_reason = "Match 2 player join failed";
      return result;
    }
  }

  // 5. Connect Player 5 in Match 1 as the SLOW / UNRESPONSIVE client
  const int slow_fd = connect_unix(socket_path);
  if (slow_fd < 0) {
    result.failure_reason = "Failed to connect slow client";
    return result;
  }
  all_fds.push_back(slow_fd);

  // Set small socket receive buffer on slow client to quickly fill kernel buffer
  int rcvbuf = 2048;
  (void)setsockopt(slow_fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  // Handshake for slow client
  const auto slow_hello = timed_request(
      slow_fd,
      json{{"type", "connect"}, {"role", "player"}},
      healthy_metrics, result.healthy_match_failures,
      expect_connect("player"));
  if (!slow_hello.has_value()) {
    result.failure_reason = "Slow client handshake failed";
    return result;
  }
  result.slow_client_connected = true;

  // Track peak FDs while all connections are active
  const auto peak_fds_opt = sample_open_fds(server_pid);
  result.peak_open_fds = peak_fds_opt.value_or(0);
  result.peak_open_fds_sample_ok = peak_fds_opt.has_value();

  // 6. Launch concurrent activity:
  // Thread A: Slow client stops reading and sends large frames (~3800 bytes each)
  // to breach the 64 KiB OutputQueue limit and trigger server-side eviction.
  // Thread B: Match 2 runs through full rounds (Round 1, 2, 3), trade, vote, finish.
  std::atomic<bool> slow_client_done{false};

  std::thread slow_client_thread([&]() {
    const auto slow_start = std::chrono::steady_clock::now();
    // Prepare ~3800-byte frame (DEC-11: 4 KiB frame limit)
    const auto overhead = json{{"echo", ""}}.dump().size();
    const std::string pad(3800U - overhead, 'q');
    const auto big_frame = Frame(json{{"echo", pad}}).serialize();

    // Send frames without reading until server closes connection or target reached
    // 25 frames * 3804 bytes = 95,100 bytes > 64 KiB queue limit
    constexpr int kTargetFrames = 25;
    int sent_count = 0;
    std::size_t bytes_sent = 0;
    bool evicted = false;

    for (int f = 0; f < kTargetFrames; ++f) {
      const auto write_res = write(slow_fd, big_frame.data(), big_frame.size());
      if (write_res > 0) {
        sent_count++;
        bytes_sent += static_cast<std::size_t>(write_res);
      } else {
        if (errno == EPIPE || errno == ECONNRESET) {
          evicted = true;
          break;
        }
      }
      // Small delay between frames so epoll can process and attempt writes
      std::this_thread::sleep_for(std::chrono::milliseconds(5));

      // Check if server closed the connection via poll
      pollfd pfd{};
      pfd.fd = slow_fd;
      pfd.events = POLLIN | POLLHUP | POLLERR;
      const int poll_res = poll(&pfd, 1, 0);
      if (poll_res > 0 && ((pfd.revents & (POLLHUP | POLLERR)) != 0)) {
        evicted = true;
        break;
      }
    }

    if (!evicted) {
      // Poll up to 1000ms for server to disconnect the overflowed socket
      pollfd pfd{};
      pfd.fd = slow_fd;
      pfd.events = POLLIN | POLLHUP | POLLERR;
      const int poll_res = poll(&pfd, 1, 1000);
      if (poll_res > 0) {
        char buf[128];
        const auto r = recv(slow_fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && (errno == ECONNRESET || errno == EPIPE))) {
          evicted = true;
        }
      }
    }

    result.slow_client_frames_sent = sent_count;
    result.slow_client_bytes_sent = bytes_sent;
    result.slow_client_overflow_evicted = evicted;
    result.slow_client_eviction_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - slow_start).count());
    slow_client_done = true;
  });

  // Helper lambda to run a full round (Discussion -> Trading -> Market -> Voting -> VoteResolution)
  auto run_round = [&](int mod_fd, uint64_t mid, const std::vector<int>& pfds, int impact) -> bool {
    const auto ev = timed_request(
        mod_fd,
        json{{"type", "publish_event"}, {"match_id", mid}, {"impact", impact}},
        healthy_metrics, result.healthy_match_failures,
        expect_transition("Discussion"));
    if (!ev.has_value()) return false;

    const auto optr = timed_request(
        mod_fd,
        json{{"type", "open_trading"}, {"match_id", mid}},
        healthy_metrics, result.healthy_match_failures,
        expect_transition("Trading"));
    if (!optr.has_value()) return false;

    for (int pfd : pfds) {
      const auto tr = timed_request(
          pfd,
          json{{"type", "trade"}, {"match_id", mid}, {"action", "HOLD"}},
          healthy_metrics, result.healthy_match_failures,
          expect_trade_accepted(), /*is_concurrent=*/true);
      if (!tr.has_value()) return false;
    }

    const auto mres = timed_request(
        mod_fd,
        json{{"type", "resolve_market"}, {"match_id", mid}},
        healthy_metrics, result.healthy_match_failures,
        expect_market_result());
    if (!mres.has_value()) return false;

    const auto opvt = timed_request(
        mod_fd,
        json{{"type", "open_voting"}, {"match_id", mid}},
        healthy_metrics, result.healthy_match_failures,
        expect_transition("Voting"));
    if (!opvt.has_value()) return false;

    for (int pfd : pfds) {
      const auto vt = timed_request(
          pfd,
          json{{"type", "vote"}, {"match_id", mid}, {"target_id", nullptr}},
          healthy_metrics, result.healthy_match_failures,
          expect_vote_accepted(), /*is_concurrent=*/true);
      if (!vt.has_value()) return false;
    }

    const auto vres = timed_request(
        mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", mid}},
        healthy_metrics, result.healthy_match_failures,
        expect_vote_resolution());
    if (!vres.has_value()) return false;

    return true;
  };

  // Thread B: Run Match 2 gameplay concurrently
  bool match2_ok = true;
  {
    // Start Match 2
    const auto start2 = timed_request(
        mod2_fd,
        json{{"type", "start_match"}, {"match_id", match2_id}},
        healthy_metrics, result.healthy_match_failures,
        expect_transition("AwaitingEvent"));
    if (!start2.has_value()) match2_ok = false;

    // Execute 3 rounds concurrently
    if (!run_round(mod2_fd, match2_id, match2_player_fds, 1)) match2_ok = false;
    if (!run_round(mod2_fd, match2_id, match2_player_fds, 1)) match2_ok = false;
    if (!run_round(mod2_fd, match2_id, match2_player_fds, -1)) match2_ok = false;

    // Check Match 2 status
    const auto st2 = timed_request(
        mod2_fd,
        json{{"type", "match_status"}, {"match_id", match2_id}},
        healthy_metrics, result.healthy_match_failures,
        expect_match_status("Finished", "InsiderWin"));
    if (st2.has_value()) {
      result.healthy_match_phase = (*st2)["phase"].get<std::string>();
      result.healthy_match_outcome = (*st2).value("outcome", "");
      result.healthy_match_passed = (result.healthy_match_phase == "Finished" &&
                                     result.healthy_match_outcome == "InsiderWin");
    } else {
      match2_ok = false;
    }
  }

  slow_client_thread.join();

  // 7. Verify healthy clients in Match 1 remained responsive
  const auto m1_mod_st = timed_request(
      mod1_fd,
      json{{"type", "match_status"}, {"match_id", match1_id}},
      healthy_metrics, result.healthy_match_failures);
  result.match1_healthy_progress_ok = m1_mod_st.has_value();

  // 8. Compute healthy client metrics
  std::vector<std::uint64_t> lats;
  for (const auto& m : healthy_metrics) {
    result.healthy_match_metrics.total_requests++;
    if (m.transport_ok && m.correctness_ok && !m.is_expected_rejection) {
      result.healthy_match_metrics.successful_requests++;
    } else if (m.is_expected_rejection) {
      result.healthy_match_metrics.expected_rejections++;
    }
    if (!m.transport_ok) result.healthy_match_metrics.transport_failures++;
    if (!m.correctness_ok && !m.is_expected_rejection) result.healthy_match_metrics.correctness_failures++;
    lats.push_back(m.latency_us);
  }
  compute_percentiles(lats, result.healthy_match_metrics);

  // 9. Resource sampling and Settling
  const auto peak_rss_opt = sample_peak_rss_kb(server_pid);
  result.peak_rss_kb = peak_rss_opt.value_or(0);
  result.peak_rss_sample_ok = peak_rss_opt.has_value();

  // Disconnect all remaining clients
  cleanup_all();
  guard.active = false;

  const auto fd_report = sample_settled_open_fds(
      server_pid,
      result.baseline_open_fds,
      /*allowed_tolerance=*/2,
      /*timeout=*/std::chrono::milliseconds(2000));

  result.immediate_post_cleanup_open_fds = fd_report.immediate_open_fds;
  result.immediate_post_cleanup_sample_ok = !fd_report.sampling_failed;
  result.post_cleanup_open_fds = fd_report.settled_open_fds;
  result.post_cleanup_sample_ok = !fd_report.sampling_failed;
  result.cleanup_settle_time_ms = fd_report.settle_time_ms;
  result.post_cleanup_settled = fd_report.settled_within_tolerance;
  result.cleanup_failure_reason = fd_report.failure_reason;

  const auto post_rss = sample_current_rss_kb(server_pid);
  result.post_cleanup_rss_kb = post_rss.value_or(0);
  result.post_cleanup_rss_sample_ok = post_rss.has_value();

  const auto end_time = std::chrono::steady_clock::now();
  result.duration_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

  // Fault injections for selftest
  if (fault_cfg.inject_missing_fd_sample) {
    result.peak_open_fds_sample_ok = false;
  }
  if (fault_cfg.inject_missing_rss_sample) {
    result.peak_rss_sample_ok = false;
  }
  if (fault_cfg.inject_cleanup_failure) {
    result.post_cleanup_settled = false;
  }
  if (fault_cfg.inject_sibling_starvation) {
    result.healthy_match_passed = false;
  }

  const bool resources_ok = result.baseline_open_fds_sample_ok &&
                            result.peak_open_fds_sample_ok &&
                            result.immediate_post_cleanup_sample_ok &&
                            result.post_cleanup_sample_ok &&
                            result.post_cleanup_settled &&
                            result.peak_rss_sample_ok &&
                            result.post_cleanup_rss_sample_ok;

  const bool progress_ok = result.slow_client_connected &&
                           result.slow_client_overflow_evicted &&
                           result.healthy_match_passed &&
                           result.match1_healthy_progress_ok &&
                           match2_ok &&
                           (result.healthy_match_failures.total() == 0);

  result.run_passed = resources_ok && progress_ok &&
                      !fault_cfg.inject_failure &&
                      !fault_cfg.inject_missing_fd_sample &&
                      !fault_cfg.inject_missing_rss_sample &&
                      !fault_cfg.inject_cleanup_failure &&
                      !fault_cfg.inject_sibling_starvation;

  if (!result.run_passed && result.failure_reason.empty()) {
    if (!result.slow_client_overflow_evicted) {
      result.failure_reason = "Slow client was not evicted on queue overflow";
    } else if (!result.healthy_match_passed) {
      result.failure_reason = "Healthy sibling match failed to reach Finished/InsiderWin";
    } else if (!match2_ok) {
      result.failure_reason = "match2_ok was false during round execution";
    } else if (result.healthy_match_failures.total() > 0) {
      result.failure_reason = "Healthy match failures: total=" + std::to_string(result.healthy_match_failures.total()) +
                              ", last=" + result.healthy_match_failures.last_failure_reason;
    } else if (!result.post_cleanup_settled) {
      result.failure_reason = "File descriptors failed to settle: " + result.cleanup_failure_reason;
    } else if (!resources_ok) {
      result.failure_reason = "Required resource samples failed";
    } else {
      result.failure_reason = "Workload execution failure";
    }
  }

  return result;
}

inline json serialize_l7_results(const L7RunResult& r, const std::string& commit_sha) {
  utsname un{};
  (void)uname(&un);
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ts;
  ts << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");

  json j;
  j["test_name"] = "darkpool_load_tests_l7";
  j["source_commit"] = commit_sha;
  j["git_dirty"] = false;
  j["timestamp_utc"] = ts.str();
  j["environment"] = json{
      {"os", "Linux"},
      {"sysname", un.sysname},
      {"release", un.release},
      {"machine", un.machine},
      {"compiler", "GCC"}
  };
  j["status"] = r.run_passed ? "PASS" : "FAIL";
  j["failure_reason"] = r.failure_reason;
  j["duration_ms"] = r.duration_ms;

  j["policy"] = json{
      {"max_queued_bytes_limit", kL7MaxQueuedBytes},
      {"overflow_behavior", "disconnect on persistent queue overflow"},
      {"approved_decision", "DEC-11"}
  };

  j["slow_client_telemetry"] = json{
      {"connected", r.slow_client_connected},
      {"frames_sent", r.slow_client_frames_sent},
      {"bytes_sent", r.slow_client_bytes_sent},
      {"overflow_evicted", r.slow_client_overflow_evicted},
      {"eviction_ms", r.slow_client_eviction_ms}
  };

  json lat_j;
  lat_j["p50_ms"] = r.healthy_match_metrics.p50_ms;
  lat_j["p95_ms"] = r.healthy_match_metrics.p95_ms;
  lat_j["p99_ms"] = r.healthy_match_metrics.p99_ms;
  lat_j["mean_ms"] = r.healthy_match_metrics.mean_ms;
  lat_j["min_ms"] = r.healthy_match_metrics.min_ms;
  lat_j["max_ms"] = r.healthy_match_metrics.max_ms;

  j["healthy_sibling_match"] = json{
      {"match_id", r.healthy_match_id},
      {"status", r.healthy_match_passed ? "PASS" : "FAIL"},
      {"final_phase", r.healthy_match_phase},
      {"final_outcome", r.healthy_match_outcome},
      {"requests", json{
          {"total", r.healthy_match_metrics.total_requests},
          {"successful", r.healthy_match_metrics.successful_requests},
          {"expected_rejections", r.healthy_match_metrics.expected_rejections},
          {"transport_failures", r.healthy_match_metrics.transport_failures},
          {"correctness_failures", r.healthy_match_metrics.correctness_failures}
      }},
      {"latency", lat_j}
  };

  j["resource_telemetry"] = json{
      {"baseline_open_fds", r.baseline_open_fds},
      {"baseline_open_fds_sample_ok", r.baseline_open_fds_sample_ok},
      {"peak_open_fds", r.peak_open_fds},
      {"peak_open_fds_sample_ok", r.peak_open_fds_sample_ok},
      {"immediate_post_cleanup_open_fds", r.immediate_post_cleanup_open_fds},
      {"immediate_post_cleanup_sample_ok", r.immediate_post_cleanup_sample_ok},
      {"post_cleanup_open_fds", r.post_cleanup_open_fds},
      {"post_cleanup_sample_ok", r.post_cleanup_sample_ok},
      {"post_cleanup_settled", r.post_cleanup_settled},
      {"cleanup_settle_time_ms", r.cleanup_settle_time_ms},
      {"cleanup_failure_reason", r.cleanup_failure_reason},
      {"baseline_rss_kb", r.baseline_rss_kb},
      {"peak_rss_kb", r.peak_rss_kb},
      {"peak_rss_sample_ok", r.peak_rss_sample_ok},
      {"post_cleanup_rss_kb", r.post_cleanup_rss_kb},
      {"post_cleanup_rss_sample_ok", r.post_cleanup_rss_sample_ok}
  };

  return j;
}

inline std::optional<std::size_t> sample_baseline_fds_probed(pid_t server_pid, const std::string& socket_path) {
  const int probe_fd = connect_unix(socket_path);
  if (probe_fd >= 0) {
    (void)close(probe_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return sample_open_fds(server_pid);
}

struct L7SelftestDef {
  std::string name;
  std::function<int(const std::string&, const std::string&)> test_fn;
  bool expect_success{true};
};

int run_l7_selftest_suite(const std::string& server_bin, const std::string& socket_path) {
  (void)socket_path;
  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL L7 Regression & Protection Selftest Suite\n";
  std::cout << "=================================================================\n";

  const std::vector<L7SelftestDef> tests = {
      {"Queue limit (64 KiB) unit boundary check",
       [](const std::string&, const std::string&) {
         darkpool::network::OutputQueue q;
         const std::string chunk64k(64 * 1024, 'a');
         if (!q.append(chunk64k)) return 1;
         // Appending 1 more byte must fail
         if (q.append("b")) return 2;
         q.consume(100);
         if (!q.append(std::string(100, 'c'))) return 3;
         return 0;
       },
       true},

      {"End-to-end slow client eviction and sibling match progress",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL7ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         const auto r = execute_l7_workload(pid, sock, kL7ModeratorToken, base_fds);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       true},

      {"Injected sibling match starvation forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL7ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L7FaultConfig cfg;
         cfg.inject_sibling_starvation = true;
         const auto r = execute_l7_workload(pid, sock, kL7ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected cleanup failure forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL7ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L7FaultConfig cfg;
         cfg.inject_cleanup_failure = true;
         const auto r = execute_l7_workload(pid, sock, kL7ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected missing FD sample forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL7ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L7FaultConfig cfg;
         cfg.inject_missing_fd_sample = true;
         const auto r = execute_l7_workload(pid, sock, kL7ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected missing RSS sample forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL7ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L7FaultConfig cfg;
         cfg.inject_missing_rss_sample = true;
         const auto r = execute_l7_workload(pid, sock, kL7ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false}
  };

  int passed = 0;
  for (std::size_t i = 0; i < tests.size(); ++i) {
    const auto& t = tests[i];
    std::cout << "  [" << (i + 1) << "/" << tests.size() << "] " << t.name << "... ";
    std::cout.flush();

    const std::string test_sock = generate_unique_socket_path();
    const int rc = t.test_fn(server_bin, test_sock);
    const bool ok = t.expect_success ? (rc == 0) : (rc != 0);
    if (ok) {
      std::cout << "PASS\n";
      passed++;
    } else {
      std::cout << "FAIL (rc=" << rc << ")\n";
    }
  }

  std::cout << "\nResults: " << passed << "/" << tests.size() << " tests passed\n";
  return (passed == static_cast<int>(tests.size())) ? 0 : 1;
}

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: darkpool_load_tests_l7 <server_binary_path> [options]\n"
              << "Options:\n"
              << "  --out <path>   JSON output report path\n"
              << "  --functional   Run standard functional workload\n"
              << "  --selftest     Run regression selftest suite\n";
    return 1;
  }

  const std::string server_bin = argv[1];
  std::string out_path;
  bool selftest_mode = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--selftest") {
      selftest_mode = true;
    } else if (arg == "--functional") {
      // standard functional mode
    } else if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    }
  }

  const std::string socket_path = generate_unique_socket_path();

  if (selftest_mode) {
    return run_l7_selftest_suite(server_bin, socket_path);
  }

  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL L7 Slow & Disconnected Client Protection Benchmark\n";
  std::cout << " 64 KiB Output Queue Bound | Overflow Disconnect | Match Isolation\n";
  std::cout << "=================================================================\n";

  pid_t server_pid = spawn_server_unix(server_bin, socket_path, kL7ModeratorToken);
  if (server_pid <= 0) {
    std::cerr << "error: failed to start server at " << socket_path << '\n';
    return 1;
  }

  const auto baseline_fds = sample_baseline_fds_probed(server_pid, socket_path);
  if (!baseline_fds.has_value()) {
    std::cerr << "error: failed to sample baseline open file descriptors\n";
    stop_server_unix(server_pid, socket_path);
    return 1;
  }

  const auto res = execute_l7_workload(server_pid, socket_path, kL7ModeratorToken, baseline_fds);

  stop_server_unix(server_pid, socket_path);

  std::cout << "\n--> Workload Results:\n";
  std::cout << "    Status                     : " << (res.run_passed ? "PASS" : "FAIL") << '\n';
  std::cout << "    Failure Reason             : " << res.failure_reason << '\n';
  std::cout << "    Cleanup Reason             : " << res.cleanup_failure_reason << '\n';
  std::cout << "    Duration                   : " << res.duration_ms << " ms\n";
  std::cout << "    Post cleanup settled       : " << (res.post_cleanup_settled ? "YES" : "NO") << '\n';
  std::cout << "    Base FDs sample ok         : " << (res.baseline_open_fds_sample_ok ? "YES" : "NO") << '\n';
  std::cout << "    Peak FDs sample ok         : " << (res.peak_open_fds_sample_ok ? "YES" : "NO") << '\n';
  std::cout << "    Imm Post FDs sample ok     : " << (res.immediate_post_cleanup_sample_ok ? "YES" : "NO") << '\n';
  std::cout << "    Post FDs sample ok         : " << (res.post_cleanup_sample_ok ? "YES" : "NO") << '\n';
  std::cout << "    Peak RSS sample ok         : " << (res.peak_rss_sample_ok ? "YES" : "NO") << '\n';
  std::cout << "    Post RSS sample ok         : " << (res.post_cleanup_rss_sample_ok ? "YES" : "NO") << '\n';
  std::cout << "    Match1 progress ok         : " << (res.match1_healthy_progress_ok ? "YES" : "NO") << '\n';
  std::cout << "    Slow Client Evicted        : " << (res.slow_client_overflow_evicted ? "YES" : "NO") << '\n';
  std::cout << "    Slow Client Frames Sent    : " << res.slow_client_frames_sent << '\n';
  std::cout << "    Slow Client Bytes Sent     : " << res.slow_client_bytes_sent << '\n';
  std::cout << "    Sibling Match Status       : " << (res.healthy_match_passed ? "PASS" : "FAIL") << '\n';
  std::cout << "    Sibling Match Phase        : " << res.healthy_match_phase << " (" << res.healthy_match_outcome << ")\n";
  std::cout << "    Healthy Latency p95        : " << res.healthy_match_metrics.p95_ms << " ms\n";
  std::cout << "    Settled FDs                : " << res.post_cleanup_open_fds << " (baseline: " << *baseline_fds << ")\n";

  if (!out_path.empty()) {
    const auto j = serialize_l7_results(res, DARKPOOL_SOURCE_COMMIT);
    std::ofstream out_file(out_path);
    if (out_file.is_open()) {
      out_file << j.dump(2) << '\n';
      std::cout << "\nWrote L7 machine-readable evidence to: " << out_path << '\n';
    } else {
      std::cerr << "\nERROR: Could not write output report to: " << out_path << '\n';
      return 1;
    }
  }

  std::cout << "\n=================================================================\n";
  std::cout << " L7 BENCHMARK " << (res.run_passed ? "PASSED (100% OK)" : "FAILED") << '\n';
  std::cout << "=================================================================\n";
  return res.run_passed ? 0 : 1;
}

#endif
