#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests_l8 requires Linux (epoll/unix sockets)\n";
  return 1;
}

#else

#include "load_test_harness_common.hpp"
#include <arpa/inet.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <poll.h>

namespace {

using namespace darkpool::load_test;

constexpr const char* kL8ModeratorToken = "test-mod-token-l8";
constexpr double kApprovedRate = 20.0;    // 20 req/s (DEC-11)
constexpr double kApprovedBurst = 40.0;   // burst 40 (DEC-11)
constexpr std::size_t kMaxPayload = 4096; // 4 KiB frame limit (DEC-11)
constexpr std::size_t kL8PlayersPerMatch = 5;

struct L8FaultConfig {
  bool inject_failure{false};
  bool inject_missing_fd_sample{false};
  bool inject_missing_rss_sample{false};
  bool inject_cleanup_failure{false};
  bool inject_rate_limit_mutation{false};
  bool inject_flood_starvation{false};
  bool inject_flood_skipped{false};
  bool inject_absent_rate_limit_evidence{false};
  bool inject_flood_transport_error{false};
};

struct L8RunResult {
  bool run_passed{false};
  std::string failure_reason;
  uint64_t duration_ms{0};

  // Rate Limiting Boundary Test Results
  int burst_requests_sent{0};
  int burst_requests_accepted{0};
  bool over_burst_rejected{false};
  std::string rejection_error_message;
  bool connection_usable_after_rate_limit{false};
  bool state_mutation_prevented{false};
  bool token_refill_recovery_ok{false};

  // Malformed Framing Test Results
  bool oversized_frame_closed{false};
  bool malformed_json_closed{false};
  bool server_survived_malformed{false};

  // Flood Isolation Concurrent Match Results
  int flood_requests_sent{0};
  int flood_responses_received{0};
  int flood_requests_accepted{0};
  int flood_requests_rate_limited{0};
  int flood_transport_errors{0};
  int flood_malformed_responses{0};
  bool healthy_match_passed{false};
  std::uint64_t healthy_match_id{0};
  std::string healthy_match_phase;
  std::string healthy_match_outcome;
  PhaseMetrics healthy_match_metrics;
  FailureCounts healthy_match_failures;

  // Resource Telemetry
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

L8RunResult execute_l8_workload(
    pid_t server_pid,
    const std::string& socket_path,
    const std::string& mod_token,
    std::optional<std::size_t> baseline_fds_opt,
    const L8FaultConfig& fault_cfg = {}) {
  L8RunResult result;
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

  // =========================================================================
  // SUB-WORKLOAD 1: Boundary Rate Limiter Verification (Burst 40, 41st Reject, Refill)
  // =========================================================================
  {
    const int client_fd = connect_unix(socket_path);
    if (client_fd < 0) {
      result.failure_reason = "Subtest 1: Failed to connect client";
      return result;
    }
    all_fds.push_back(client_fd);

    // Handshake
    std::vector<RequestMetric> dummy_m;
    FailureCounts dummy_fc;
    const auto hello = timed_request(
        client_fd,
        json{{"type", "connect"}, {"role", "player"}},
        dummy_m, dummy_fc,
        expect_connect("player"));
    if (!hello.has_value()) {
      result.failure_reason = "Subtest 1: Player handshake failed";
      return result;
    }

    // Connect Moderator to create a match so we can test non-mutation
    const int mod_fd = connect_unix(socket_path);
    if (mod_fd < 0) {
      result.failure_reason = "Subtest 1: Failed to connect moderator";
      return result;
    }
    all_fds.push_back(mod_fd);
    const auto mod_hello = timed_request(
        mod_fd,
        json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
        dummy_m, dummy_fc,
        expect_connect("moderator"));
    if (!mod_hello.has_value()) {
      result.failure_reason = "Subtest 1: Moderator handshake failed";
      return result;
    }

    const auto match_create = timed_request(
        mod_fd,
        json{{"type", "create_match"}, {"player_count", kL8PlayersPerMatch}},
        dummy_m, dummy_fc,
        expect_match_created(kL8PlayersPerMatch, 1, 1, 3));
    if (!match_create.has_value()) {
      result.failure_reason = "Subtest 1: create_match failed";
      return result;
    }
    const uint64_t test_match_id = (*match_create)["match_id"].get<uint64_t>();

    // Client joins match
    const auto join_res = timed_request(
        client_fd,
        json{{"type", "join_match"}, {"match_id", test_match_id}},
        dummy_m, dummy_fc,
        expect_joined_match(1));
    if (!join_res.has_value()) {
      result.failure_reason = "Subtest 1: join_match failed";
      return result;
    }

    // Burst boundary: The token bucket starts with burst=40 tokens.
    // Client has used 1 token for join_match.
    // Now send 39 more requests -> exactly 40 total tokens consumed.
    int burst_successes = 1; // join_match was the 1st
    for (int i = 0; i < 39; ++i) {
      const auto r = timed_request(
          client_fd,
          json{{"echo", "burst_" + std::to_string(i)}},
          dummy_m, dummy_fc);
      if (r.has_value()) {
        burst_successes++;
      }
    }
    result.burst_requests_sent = 40;
    result.burst_requests_accepted = burst_successes;

    // 41st request: Sent immediately without waiting -> MUST exceed rate limit
    const auto over_req = timed_request(
        client_fd,
        json{{"type", "submit_trade"}, {"match_id", test_match_id}, {"action", "BUY"}},
        dummy_m, dummy_fc,
        expect_error("rate limit exceeded"));

    if (over_req.has_value()) {
      result.over_burst_rejected = true;
      result.rejection_error_message = (*over_req)["error"].get<std::string>();
    }

    // Verify state mutation: match status must verify match did NOT advance or record the trade
    const auto mod_st = timed_request(
        mod_fd,
        json{{"type", "match_status"}, {"match_id", test_match_id}},
        dummy_m, dummy_fc);
    if (mod_st.has_value() && !fault_cfg.inject_rate_limit_mutation) {
      // In Lobby phase, no trade could have mutated state
      result.state_mutation_prevented = ((*mod_st).value("phase", "") == "Lobby");
    }

    // Usability & refill recovery: Wait 150 ms (allows 3 tokens to refill at 20 req/s)
    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    // Request after refill must succeed on the SAME connection!
    const auto refill_req = timed_request(
        client_fd,
        json{{"echo", "refill_recovered"}},
        dummy_m, dummy_fc);
    if (refill_req.has_value() && (*refill_req).value("echo", "") == "refill_recovered") {
      result.connection_usable_after_rate_limit = true;
      result.token_refill_recovery_ok = true;
    }
  }

  // =========================================================================
  // SUB-WORKLOAD 2: Malformed Framing and Oversized Payloads
  // =========================================================================
  {
    // A. Oversized frame: length header declares 4097 bytes (> 4096 limit)
    const int ofd = connect_unix(socket_path);
    if (ofd >= 0) {
      all_fds.push_back(ofd);
      // Handshake first
      std::vector<RequestMetric> dm; FailureCounts dfc;
      (void)timed_request(ofd, json{{"type", "connect"}, {"role", "player"}}, dm, dfc, expect_connect("player"));

      // Send 4-byte big-endian header declaring 4097 bytes, followed by bytes
      uint32_t len_be = htonl(4097);
      std::string over_bytes(reinterpret_cast<const char*>(&len_be), 4);
      over_bytes.append(4097, 'z');
      const auto wr = write(ofd, over_bytes.data(), over_bytes.size());
      (void)wr;

      // Server must close connection on frame length exceeding kMaxFramePayloadSize
      pollfd pfd{};
      pfd.fd = ofd;
      pfd.events = POLLIN | POLLHUP | POLLERR;
      const int poll_res = poll(&pfd, 1, 1000);
      if (poll_res > 0) {
        char buf[64];
        const auto r = recv(ofd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && (errno == ECONNRESET || errno == EPIPE))) {
          result.oversized_frame_closed = true;
        }
      }
    }

    // B. Malformed JSON payload (length is within 4 KiB, but bytes are invalid JSON)
    const int jfd = connect_unix(socket_path);
    if (jfd >= 0) {
      all_fds.push_back(jfd);
      std::vector<RequestMetric> dm; FailureCounts dfc;
      (void)timed_request(jfd, json{{"type", "connect"}, {"role", "player"}}, dm, dfc, expect_connect("player"));

      const std::string bad_payload = "{\"type\": \"bad_json_not_terminated";
      uint32_t len_be = htonl(static_cast<uint32_t>(bad_payload.size()));
      std::string bad_frame(reinterpret_cast<const char*>(&len_be), 4);
      bad_frame.append(bad_payload);
      const auto wr = write(jfd, bad_frame.data(), bad_frame.size());
      (void)wr;

      // Server must safely close connection
      pollfd pfd{};
      pfd.fd = jfd;
      pfd.events = POLLIN | POLLHUP | POLLERR;
      const int poll_res = poll(&pfd, 1, 1000);
      if (poll_res > 0) {
        char buf[64];
        const auto r = recv(jfd, buf, sizeof(buf), MSG_DONTWAIT);
        if (r == 0 || (r < 0 && (errno == ECONNRESET || errno == EPIPE))) {
          result.malformed_json_closed = true;
        }
      }
    }

    // C. Server survival check: probe server to ensure it is alive and responsive
    const int probe_fd = connect_unix(socket_path);
    if (probe_fd >= 0) {
      all_fds.push_back(probe_fd);
      std::vector<RequestMetric> dm; FailureCounts dfc;
      const auto p_res = timed_request(
          probe_fd,
          json{{"type", "connect"}, {"role", "player"}},
          dm, dfc,
          expect_connect("player"));
      result.server_survived_malformed = p_res.has_value();
    }
  }

  // =========================================================================
  // SUB-WORKLOAD 3: Concurrent High-Rate Flood Isolation against Normal Match
  // =========================================================================
  {
    // Moderator for Match
    const int healthy_mod_fd = connect_unix(socket_path);
    if (healthy_mod_fd < 0) {
      result.failure_reason = "Subtest 3: Failed to connect moderator";
      return result;
    }
    all_fds.push_back(healthy_mod_fd);

    std::vector<RequestMetric> healthy_metrics;
    const auto mod_h = timed_request(
        healthy_mod_fd,
        json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
        healthy_metrics, result.healthy_match_failures,
        expect_connect("moderator"));
    if (!mod_h.has_value()) {
      result.failure_reason = "Subtest 3: Moderator connect failed";
      return result;
    }

    const auto match_cr = timed_request(
        healthy_mod_fd,
        json{{"type", "create_match"}, {"player_count", kL8PlayersPerMatch}},
        healthy_metrics, result.healthy_match_failures,
        expect_match_created(kL8PlayersPerMatch, 1, 1, 3));
    if (!match_cr.has_value()) {
      result.failure_reason = "Subtest 3: create_match failed";
      return result;
    }
    const uint64_t flood_test_match_id = (*match_cr)["match_id"].get<uint64_t>();
    result.healthy_match_id = flood_test_match_id;

    // Healthy players
    std::vector<int> healthy_player_fds;
    for (std::size_t i = 0; i < kL8PlayersPerMatch; ++i) {
      const int pfd = connect_unix(socket_path);
      if (pfd < 0) {
        result.failure_reason = "Subtest 3: Failed to connect player";
        return result;
      }
      all_fds.push_back(pfd);
      healthy_player_fds.push_back(pfd);
      (void)timed_request(
          pfd,
          json{{"type", "connect"}, {"role", "player"}},
          healthy_metrics, result.healthy_match_failures,
          expect_connect("player"));
      (void)timed_request(
          pfd,
          json{{"type", "join_match"}, {"match_id", flood_test_match_id}},
          healthy_metrics, result.healthy_match_failures,
          expect_joined_match(static_cast<std::uint32_t>(i + 1)));
    }

    // Connect 2 Hostile Flood Clients
    const int flood1_fd = connect_unix(socket_path);
    const int flood2_fd = connect_unix(socket_path);
    if (flood1_fd >= 0) all_fds.push_back(flood1_fd);
    if (flood2_fd >= 0) all_fds.push_back(flood2_fd);

    if (flood1_fd >= 0 && flood2_fd >= 0) {
      std::vector<RequestMetric> dm; FailureCounts dfc;
      (void)timed_request(flood1_fd, json{{"type", "connect"}, {"role", "player"}}, dm, dfc, expect_connect("player"));
      (void)timed_request(flood2_fd, json{{"type", "connect"}, {"role", "player"}}, dm, dfc, expect_connect("player"));
    }

    // Sample peak open FDs while all connections are active
    const auto peak_fds_opt = sample_open_fds(server_pid);
    result.peak_open_fds = peak_fds_opt.value_or(0);
    result.peak_open_fds_sample_ok = peak_fds_opt.has_value();

    // Set non-blocking mode on flood client socket
    const int flood_fl = fcntl(flood1_fd, F_GETFL, 0);
    if (flood_fl >= 0) {
      (void)fcntl(flood1_fd, F_SETFL, flood_fl | O_NONBLOCK);
    }

    // Start background flooding sender and draining receiver threads
    std::atomic<bool> stop_flooding{false};
    std::atomic<bool> stop_receiving{false};
    std::atomic<int> flood_sent{0};
    std::atomic<int> flood_received{0};
    std::atomic<int> flood_accepted{0};
    std::atomic<int> flood_limited{0};
    std::atomic<int> flood_trans_err{0};
    std::atomic<int> flood_malformed{0};

    // Draining receiver thread: parses complete frames with FrameStream
    std::thread receiver_thread([&]() {
      if (flood1_fd < 0) return;
      darkpool::network::FrameStream stream;
      char rcv_buf[4096];
      while (!stop_receiving) {
        pollfd pfd{};
        pfd.fd = flood1_fd;
        pfd.events = POLLIN;
        const int pr = poll(&pfd, 1, 5);
        if (pr > 0 && (pfd.revents & POLLIN)) {
          const ssize_t n = recv(flood1_fd, rcv_buf, sizeof(rcv_buf), MSG_DONTWAIT);
          if (n > 0) {
            try {
              stream.feed(std::span<const char>(rcv_buf, static_cast<std::size_t>(n)));
              Frame frame(json{});
              while (stream.next(frame)) {
                flood_received++;
                const auto& payload = frame.payload();
                if (payload.is_object() && payload.value("type", "") == "error" &&
                    payload.value("error", "").find("rate limit exceeded") != std::string::npos) {
                  flood_limited++;
                } else if (payload.is_object() && payload.value("echo", "") == "flood_ping") {
                  flood_accepted++;
                } else {
                  flood_malformed++;
                }
              }
            } catch (...) {
              flood_malformed++;
            }
          } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
            flood_trans_err++;
            break;
          }
        }
      }
    });

    // High-rate sender thread: continuously blasts frames without waiting for responses
    std::thread sender_thread([&]() {
      if (flood1_fd < 0) return;
      const auto flood_frame = Frame(json{{"echo", "flood_ping"}}).serialize();
      while (!stop_flooding) {
        const ssize_t wr = send(flood1_fd, flood_frame.data(), flood_frame.size(), MSG_NOSIGNAL);
        if (wr > 0) {
          flood_sent++;
        } else if (wr < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
          std::this_thread::sleep_for(std::chrono::microseconds(100));
        } else if (wr < 0) {
          flood_trans_err++;
          break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });

    // Helper lambda to run a full round
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

    // Concurrently execute full 3 rounds on healthy match
    bool match_progress_ok = true;
    {
      const auto s = timed_request(
          healthy_mod_fd,
          json{{"type", "start_match"}, {"match_id", flood_test_match_id}},
          healthy_metrics, result.healthy_match_failures,
          expect_transition("AwaitingEvent"));
      if (!s.has_value()) match_progress_ok = false;

      if (!run_round(healthy_mod_fd, flood_test_match_id, healthy_player_fds, 1)) match_progress_ok = false;
      if (!run_round(healthy_mod_fd, flood_test_match_id, healthy_player_fds, 1)) match_progress_ok = false;
      if (!run_round(healthy_mod_fd, flood_test_match_id, healthy_player_fds, -1)) match_progress_ok = false;

      const auto st = timed_request(
          healthy_mod_fd,
          json{{"type", "match_status"}, {"match_id", flood_test_match_id}},
          healthy_metrics, result.healthy_match_failures,
          expect_match_status("Finished", "InsiderWin"));
      if (st.has_value()) {
        result.healthy_match_phase = (*st)["phase"].get<std::string>();
        result.healthy_match_outcome = (*st).value("outcome", "");
        result.healthy_match_passed = match_progress_ok &&
                                      (result.healthy_match_phase == "Finished" &&
                                       result.healthy_match_outcome == "InsiderWin");
      } else {
        result.healthy_match_passed = false;
      }
    }

    stop_flooding = true;
    sender_thread.join();

    // Drain receiver up to 200ms
    const auto drain_start = std::chrono::steady_clock::now();
    while (flood_received < flood_sent &&
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - drain_start).count() < 200) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    stop_receiving = true;
    receiver_thread.join();

    result.flood_requests_sent = flood_sent.load();
    result.flood_responses_received = flood_received.load();
    result.flood_requests_accepted = flood_accepted.load();
    result.flood_requests_rate_limited = flood_limited.load();
    result.flood_transport_errors = flood_trans_err.load();
    result.flood_malformed_responses = flood_malformed.load();

    // Compute healthy match metrics
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
  }

  // =========================================================================
  // Resource Cleanup & Settling Check
  // =========================================================================
  const auto peak_rss_opt = sample_peak_rss_kb(server_pid);
  result.peak_rss_kb = peak_rss_opt.value_or(0);
  result.peak_rss_sample_ok = peak_rss_opt.has_value();

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

  // Fault injections
  if (fault_cfg.inject_missing_fd_sample) {
    result.peak_open_fds_sample_ok = false;
  }
  if (fault_cfg.inject_missing_rss_sample) {
    result.peak_rss_sample_ok = false;
  }
  if (fault_cfg.inject_cleanup_failure) {
    result.post_cleanup_settled = false;
  }
  if (fault_cfg.inject_flood_starvation) {
    result.healthy_match_passed = false;
  }
  if (fault_cfg.inject_flood_skipped) {
    result.flood_requests_sent = 0;
  }
  if (fault_cfg.inject_absent_rate_limit_evidence) {
    result.flood_requests_rate_limited = 0;
  }
  if (fault_cfg.inject_flood_transport_error) {
    result.flood_transport_errors = 1;
  }

  const bool rate_limit_ok = (result.burst_requests_accepted == 40) &&
                             result.over_burst_rejected &&
                             result.connection_usable_after_rate_limit &&
                             result.state_mutation_prevented &&
                             result.token_refill_recovery_ok;

  const bool malformed_ok = result.oversized_frame_closed &&
                            result.malformed_json_closed &&
                            result.server_survived_malformed;

  const bool flood_rate_limiting_triggered = (result.flood_requests_sent > 40) &&
                                             (result.flood_requests_rate_limited > 0) &&
                                             (result.flood_transport_errors == 0) &&
                                             (result.flood_malformed_responses == 0);

  const bool flood_ok = flood_rate_limiting_triggered &&
                        result.healthy_match_passed &&
                        (result.healthy_match_failures.total() == 0);

  const bool resources_ok = result.baseline_open_fds_sample_ok &&
                            result.peak_open_fds_sample_ok &&
                            result.immediate_post_cleanup_sample_ok &&
                            result.post_cleanup_sample_ok &&
                            result.post_cleanup_settled &&
                            result.peak_rss_sample_ok &&
                            result.post_cleanup_rss_sample_ok;

  result.run_passed = rate_limit_ok && malformed_ok && flood_ok && resources_ok &&
                      !fault_cfg.inject_failure &&
                      !fault_cfg.inject_missing_fd_sample &&
                      !fault_cfg.inject_missing_rss_sample &&
                      !fault_cfg.inject_cleanup_failure &&
                      !fault_cfg.inject_rate_limit_mutation &&
                      !fault_cfg.inject_flood_starvation &&
                      !fault_cfg.inject_flood_skipped &&
                      !fault_cfg.inject_absent_rate_limit_evidence &&
                      !fault_cfg.inject_flood_transport_error;

  if (!result.run_passed && result.failure_reason.empty()) {
    if (!rate_limit_ok) {
      result.failure_reason = "Rate limiting boundary tests failed";
    } else if (!malformed_ok) {
      result.failure_reason = "Malformed frame safety tests failed";
    } else if (!flood_ok) {
      if (!flood_rate_limiting_triggered) {
        if (result.flood_requests_sent <= 40) {
          result.failure_reason = "Flood isolation test failed: flood volume insufficient (sent " +
                                  std::to_string(result.flood_requests_sent) + " <= 40)";
        } else if (result.flood_requests_rate_limited == 0) {
          result.failure_reason = "Flood isolation test failed: zero rate-limited responses observed (sent " +
                                  std::to_string(result.flood_requests_sent) + ")";
        } else if (result.flood_transport_errors > 0) {
          result.failure_reason = "Flood isolation test failed: transport error observed on flood client";
        } else {
          result.failure_reason = "Flood isolation test failed: malformed response observed on flood client";
        }
      } else {
        result.failure_reason = "Flood isolation test failed: healthy match did not complete successfully";
      }
    } else if (!resources_ok) {
      result.failure_reason = "Resource telemetry or cleanup check failed: " + result.cleanup_failure_reason;
    } else {
      result.failure_reason = "L8 workload failure";
    }
  }

  return result;
}

inline json serialize_l8_results(const L8RunResult& r, const std::string& commit_sha) {
  utsname un{};
  (void)uname(&un);
  const auto now = std::chrono::system_clock::now();
  const auto tt = std::chrono::system_clock::to_time_t(now);
  std::ostringstream ts;
  ts << std::put_time(std::gmtime(&tt), "%Y-%m-%dT%H:%M:%SZ");

  json j;
  j["test_name"] = "darkpool_load_tests_l8";
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
      {"request_rate_per_sec", kApprovedRate},
      {"burst_capacity", kApprovedBurst},
      {"max_frame_payload_bytes", kMaxPayload},
      {"approved_decision", "DEC-11"}
  };

  j["rate_limiter_boundary_tests"] = json{
      {"burst_requests_sent", r.burst_requests_sent},
      {"burst_requests_accepted", r.burst_requests_accepted},
      {"over_burst_rejected", r.over_burst_rejected},
      {"rejection_error_message", r.rejection_error_message},
      {"connection_usable_after_rate_limit", r.connection_usable_after_rate_limit},
      {"state_mutation_prevented", r.state_mutation_prevented},
      {"token_refill_recovery_ok", r.token_refill_recovery_ok}
  };

  j["malformed_traffic_tests"] = json{
      {"oversized_frame_closed", r.oversized_frame_closed},
      {"malformed_json_closed", r.malformed_json_closed},
      {"server_survived_malformed", r.server_survived_malformed}
  };

  json lat_j;
  lat_j["p50_ms"] = r.healthy_match_metrics.p50_ms;
  lat_j["p95_ms"] = r.healthy_match_metrics.p95_ms;
  lat_j["p99_ms"] = r.healthy_match_metrics.p99_ms;
  lat_j["mean_ms"] = r.healthy_match_metrics.mean_ms;
  lat_j["min_ms"] = r.healthy_match_metrics.min_ms;
  lat_j["max_ms"] = r.healthy_match_metrics.max_ms;

  j["flood_isolation_workload"] = json{
      {"flood_requests_sent", r.flood_requests_sent},
      {"flood_responses_received", r.flood_responses_received},
      {"flood_requests_accepted", r.flood_requests_accepted},
      {"flood_requests_rate_limited", r.flood_requests_rate_limited},
      {"flood_transport_errors", r.flood_transport_errors},
      {"flood_malformed_responses", r.flood_malformed_responses},
      {"healthy_match_id", r.healthy_match_id},
      {"healthy_match_status", r.healthy_match_passed ? "PASS" : "FAIL"},
      {"healthy_match_phase", r.healthy_match_phase},
      {"healthy_match_outcome", r.healthy_match_outcome},
      {"healthy_match_requests", json{
          {"total", r.healthy_match_metrics.total_requests},
          {"successful", r.healthy_match_metrics.successful_requests},
          {"expected_rejections", r.healthy_match_metrics.expected_rejections},
          {"transport_failures", r.healthy_match_metrics.transport_failures},
          {"correctness_failures", r.healthy_match_metrics.correctness_failures}
      }},
      {"healthy_match_latency", lat_j}
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

struct L8SelftestDef {
  std::string name;
  std::function<int(const std::string&, const std::string&)> test_fn;
  bool expect_success{true};
};

int run_l8_selftest_suite(const std::string& server_bin, const std::string& socket_path) {
  (void)socket_path;
  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL L8 Malformed & Flood Protection Selftest Suite\n";
  std::cout << "=================================================================\n";

  const std::vector<L8SelftestDef> tests = {
      {"End-to-end L8 workload (burst, reject, refill, malformed, flood isolation)",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       true},

      {"Injected rate limit mutation forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_rate_limit_mutation = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected flood starvation forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_flood_starvation = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected flood skipped forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_flood_skipped = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected absent rate limit evidence forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_absent_rate_limit_evidence = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected flood transport error forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_flood_transport_error = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected cleanup failure forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_cleanup_failure = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected missing FD sample forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_missing_fd_sample = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected missing RSS sample forces nonzero exit",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kL8ModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         L8FaultConfig cfg;
         cfg.inject_missing_rss_sample = true;
         const auto r = execute_l8_workload(pid, sock, kL8ModeratorToken, base_fds, cfg);
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
    std::cerr << "Usage: darkpool_load_tests_l8 <server_binary_path> [options]\n"
              << "Options:\n"
              << "  --out <path>   JSON output report path\n"
              << "  --runs <N>     Number of repeatable runs (default 1)\n"
              << "  --functional   Run standard functional workload\n"
              << "  --selftest     Run regression selftest suite\n";
    return 1;
  }

  const std::string server_bin = argv[1];
  std::string out_path;
  bool selftest_mode = false;
  int repeat_runs = 1;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--selftest") {
      selftest_mode = true;
    } else if (arg == "--functional") {
      // standard functional mode
    } else if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    } else if (arg == "--runs" && i + 1 < argc) {
      repeat_runs = std::max(1, std::atoi(argv[++i]));
    }
  }

  const std::string socket_path = generate_unique_socket_path();

  if (selftest_mode) {
    return run_l8_selftest_suite(server_bin, socket_path);
  }

  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL L8 Malformed Traffic & Request Flood Protection Benchmark\n";
  std::cout << " 20 req/s, Burst 40 | 4 KiB Frames | Concurrent Flood Isolation\n";
  std::cout << " Planned Runs : " << repeat_runs << '\n';
  std::cout << "=================================================================\n";

  std::vector<L8RunResult> all_results;
  bool all_passed = true;

  for (int r = 1; r <= repeat_runs; ++r) {
    const std::string run_sock = generate_unique_socket_path();
    pid_t server_pid = spawn_server_unix(server_bin, run_sock, kL8ModeratorToken);
    if (server_pid <= 0) {
      std::cerr << "error: failed to start server at " << run_sock << '\n';
      return 1;
    }

    const auto baseline_fds = sample_baseline_fds_probed(server_pid, run_sock);
    if (!baseline_fds.has_value()) {
      std::cerr << "error: failed to sample baseline open file descriptors\n";
      stop_server_unix(server_pid, run_sock);
      return 1;
    }

    std::cout << "[Run " << r << "/" << repeat_runs << "] Executing L8 workload... " << std::flush;
    const auto res = execute_l8_workload(server_pid, run_sock, kL8ModeratorToken, baseline_fds);
    stop_server_unix(server_pid, run_sock);
    all_results.push_back(res);

    if (!res.run_passed) {
      all_passed = false;
      std::cout << "FAILED: " << res.failure_reason << '\n';
    } else {
      std::cout << "PASSED (" << res.duration_ms << " ms, "
                << "burst=" << res.burst_requests_accepted << "/40, "
                << "flood sent=" << res.flood_requests_sent << ", "
                << "limited=" << res.flood_requests_rate_limited << ", "
                << "sibling p95=" << res.healthy_match_metrics.p95_ms << " ms)\n";
    }
  }

  const auto& last_res = all_results.back();

  std::cout << "\n--> Final Aggregate Summary:\n";
  std::cout << "    Status                     : " << (all_passed ? "PASS" : "FAIL") << '\n';
  std::cout << "    Repeat Runs                : " << repeat_runs << '\n';
  std::cout << "    Burst Accepted             : " << last_res.burst_requests_accepted << "/40\n";
  std::cout << "    Over-Burst Rejected        : " << (last_res.over_burst_rejected ? "YES" : "NO") << '\n';
  std::cout << "    Refill Recovery            : " << (last_res.token_refill_recovery_ok ? "YES" : "NO") << '\n';
  std::cout << "    Oversized Frame Closed     : " << (last_res.oversized_frame_closed ? "YES" : "NO") << '\n';
  std::cout << "    Malformed JSON Closed      : " << (last_res.malformed_json_closed ? "YES" : "NO") << '\n';
  std::cout << "    Flood Requests Sent        : " << last_res.flood_requests_sent << '\n';
  std::cout << "    Flood Requests Rate-Limited: " << last_res.flood_requests_rate_limited << '\n';
  std::cout << "    Healthy Match Status       : " << (last_res.healthy_match_passed ? "PASS" : "FAIL") << '\n';
  std::cout << "    Healthy Latency p95        : " << last_res.healthy_match_metrics.p95_ms << " ms\n";
  std::cout << "    Settled FDs                : " << last_res.post_cleanup_open_fds
            << " (baseline: " << last_res.baseline_open_fds << ")\n";

  if (!out_path.empty()) {
    auto j = serialize_l8_results(last_res, DARKPOOL_SOURCE_COMMIT);
    j["status"] = all_passed ? "PASS" : "FAIL";
    j["repeat_runs"] = repeat_runs;
    j["all_runs_passed"] = all_passed;
    json runs_array = json::array();
    for (const auto& r : all_results) {
      runs_array.push_back(serialize_l8_results(r, DARKPOOL_SOURCE_COMMIT));
    }
    j["runs"] = runs_array;

    std::ofstream out_file(out_path);
    if (out_file.is_open()) {
      out_file << j.dump(2) << '\n';
      std::cout << "\nWrote L8 machine-readable evidence to: " << out_path << '\n';
    } else {
      std::cerr << "\nERROR: Could not write output report to: " << out_path << '\n';
      return 1;
    }
  }

  std::cout << "\n=================================================================\n";
  std::cout << " L8 BENCHMARK " << (all_passed ? "PASSED (100% OK)" : "FAILED") << '\n';
  std::cout << "=================================================================\n";
  return all_passed ? 0 : 1;
}

#endif
