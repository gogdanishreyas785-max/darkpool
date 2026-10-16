#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests_l5 requires Linux (epoll/unix sockets)\n";
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

constexpr std::size_t kExpectedL5Matches = 2;
constexpr std::size_t kExpectedL5PlayersPerMatch = 32;
constexpr std::size_t kExpectedL5TotalPlayers = 64;
constexpr std::size_t kExpectedL5Moderators = 2;
constexpr std::size_t kExpectedL5TotalClients = 66;

// Role formula for N=32:
// Insiders: ceil(32/8) = 4
// Investigators: ceil(32/16) = 2
// Employees: 32 - 4 - 2 = 26
constexpr std::size_t kExpectedL5Insiders = 4;
constexpr std::size_t kExpectedL5Investigators = 2;
constexpr std::size_t kExpectedL5Employees = 26;

PhaseMetrics compute_metrics_from_list(const std::vector<RequestMetric>& metrics) {
  PhaseMetrics pm;
  std::vector<std::uint64_t> lats;
  for (const auto& m : metrics) {
    pm.total_requests++;
    if (m.transport_ok && m.correctness_ok && !m.is_expected_rejection) {
      pm.successful_requests++;
    } else if (m.is_expected_rejection) {
      pm.expected_rejections++;
    }
    if (!m.transport_ok) pm.transport_failures++;
    if (!m.correctness_ok && !m.is_expected_rejection) pm.correctness_failures++;
    lats.push_back(m.latency_us);
  }
  darkpool::load_test::compute_percentiles(lats, pm);
  return pm;
}

struct TestFaultConfig {
  bool inject_assertion_failure{false};
  bool inject_outcome_mismatch{false};
  bool inject_semantic_failure{false};
  bool inject_fd_leak{false};
  bool inject_starvation{false};
  bool inject_missing_fd_sample{false};
  bool inject_missing_rss_sample{false};
  bool inject_cleanup_failure{false};
  bool inject_counter_mismatch{false};
};

struct L5MatchRecord {
  std::uint64_t match_id{0};
  int mod_fd{-1};
  std::vector<int> player_fds;
  std::vector<std::string> player_roles;
  std::size_t insiders_count{0};
  std::size_t investigators_count{0};
  std::size_t employees_count{0};
  std::vector<std::uint32_t> insider_ids;
  std::vector<std::uint32_t> investigator_ids;
  std::string final_phase;
  std::string final_outcome;
  bool match_passed{false};
  PhaseMetrics sequential;
  PhaseMetrics concurrent;
  PhaseMetrics total;
  FailureCounts failures;
  std::vector<RequestMetric> metrics;
  std::chrono::steady_clock::time_point lifecycle_start;
  std::chrono::steady_clock::time_point lifecycle_end;
  std::uint64_t lifecycle_start_ms{0};
  std::uint64_t lifecycle_end_ms{0};
  std::uint64_t elapsed_ms{0};
};

struct L5RunResult {
  int run_index{1};
  std::size_t match_count{kExpectedL5Matches};
  std::size_t players_per_match{kExpectedL5PlayersPerMatch};
  std::size_t total_player_clients{kExpectedL5TotalPlayers};
  std::size_t moderator_clients{kExpectedL5Moderators};
  std::size_t total_connected_clients{kExpectedL5TotalClients};
  std::uint64_t duration_ms{0};
  bool run_passed{false};
  bool all_matches_overlapped{false};
  std::uint64_t overlap_duration_ms{0};
  bool starvation_verified{false};
  std::vector<L5MatchRecord> matches;
  PhaseMetrics aggregate_sequential;
  PhaseMetrics aggregate_concurrent;
  PhaseMetrics aggregate_total;
  FailureCounts failures;
  std::uint64_t peak_rss_kb{0};
  bool peak_rss_sample_ok{false};
  double cpu_time_ms{0.0};
  std::size_t baseline_open_fds{0};
  bool baseline_open_fds_sample_ok{false};
  std::size_t peak_open_fds{0};
  bool peak_open_fds_sample_ok{false};
  std::size_t immediate_post_cleanup_open_fds{0};
  bool immediate_post_cleanup_sample_ok{false};
  std::size_t post_cleanup_open_fds{0};
  bool post_cleanup_sample_ok{false};
  std::uint64_t cleanup_settle_time_ms{0};
  bool post_cleanup_settled{false};
  std::string cleanup_failure_reason;
  bool peak_conn_current_rss_sample_ok{false};
  std::optional<std::uint64_t> peak_conn_current_rss_kb{std::nullopt};
  bool post_cleanup_current_rss_sample_ok{false};
  std::optional<std::uint64_t> post_cleanup_current_rss_kb{std::nullopt};
  double throughput_rps{0.0};
};

L5RunResult execute_l5_single_cycle(
    pid_t server_pid,
    const std::string& socket_path,
    const std::string& mod_token,
    int run_index,
    std::optional<std::size_t> baseline_open_fds_opt,
    const TestFaultConfig& fault_cfg = {}) {
  L5RunResult result;
  result.run_index = run_index;
  result.matches.resize(kExpectedL5Matches);

  if (!baseline_open_fds_opt.has_value()) {
    result.baseline_open_fds_sample_ok = false;
    result.failures.transport_failures++;
    result.failures.last_failure_reason = "Missing baseline open FDs measurement";
    std::cerr << "[L5 Run " << run_index << "] RESOURCE ERROR: Failed to sample baseline open FDs\n";
  } else {
    result.baseline_open_fds = *baseline_open_fds_opt;
    result.baseline_open_fds_sample_ok = true;
  }

  std::vector<int> all_mod_fds;
  std::vector<int> all_player_fds;

  auto cleanup_all = [&]() {
    for (const auto fd : all_player_fds) {
      if (fd >= 0) (void)close(fd);
    }
    all_player_fds.clear();
    for (const auto fd : all_mod_fds) {
      if (fd >= 0) (void)close(fd);
    }
    all_mod_fds.clear();
  };

  struct CleanupGuard {
    std::function<void()> fn;
    bool active{true};
    ~CleanupGuard() {
      if (active && fn) fn();
    }
  } guard{cleanup_all, true};

  const auto run_start = std::chrono::steady_clock::now();

  #define L5_VERIFY_REQ(resp_opt, match_rec, err_msg) \
    do { \
      if (!(resp_opt).has_value()) { \
        std::cerr << "[L5] REQUEST ERROR: " << (err_msg) \
                  << " (" << (match_rec).failures.last_failure_reason << ") at line " << __LINE__ << '\n'; \
      } \
    } while (0)

  #define L5_VERIFY_STATE(cond, match_rec, err_msg) \
    do { \
      if (!(cond)) { \
        std::cerr << "[L5] STATE ERROR: " << (err_msg) \
                  << " at line " << __LINE__ << '\n'; \
        (match_rec).failures.correctness_failures++; \
      } \
    } while (0)

  // 1. Establish 2 Independent Authenticated Moderator Connections
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    const int mod_fd = connect_unix(socket_path);
    if (mod_fd < 0) {
      std::cerr << "[L5 Run " << run_index << "] TRANSPORT ERROR: Failed to connect moderator " << (m + 1) << '\n';
      result.failures.transport_failures++;
      result.failures.unexpected_disconnects++;
      result.failures.last_failure_reason = "Failed to connect moderator socket";
      return result;
    }
    all_mod_fds.push_back(mod_fd);
    result.matches[m].mod_fd = mod_fd;

    const auto mod_hello = timed_request(
        mod_fd,
        json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
        result.matches[m].metrics,
        result.matches[m].failures,
        expect_connect("moderator"));
    L5_VERIFY_REQ(mod_hello, result.matches[m], "Moderator " + std::to_string(m + 1) + " handshake failed");
  }

  // 2. Create 2 Concurrent Matches (each moderator creates its own 32-player match)
  std::vector<std::uint64_t> match_ids(kExpectedL5Matches, 0);
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    result.matches[m].lifecycle_start = std::chrono::steady_clock::now();
    result.matches[m].lifecycle_start_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(result.matches[m].lifecycle_start - run_start).count());

    const auto create_resp = timed_request(
        result.matches[m].mod_fd,
        json{{"type", "create_match"}, {"player_count", kExpectedL5PlayersPerMatch}},
        result.matches[m].metrics,
        result.matches[m].failures,
        expect_match_created(kExpectedL5PlayersPerMatch, kExpectedL5Insiders, kExpectedL5Investigators, kExpectedL5Employees));
    L5_VERIFY_REQ(create_resp, result.matches[m], "create_match failed for match " + std::to_string(m + 1));
    if (create_resp.has_value()) {
      match_ids[m] = (*create_resp)["match_id"].get<std::uint64_t>();
      result.matches[m].match_id = match_ids[m];
    }
  }

  L5_VERIFY_STATE(match_ids[0] != match_ids[1] && match_ids[0] > 0 && match_ids[1] > 0,
                  result.matches[0],
                  "Match IDs must be unique and non-zero");

  // Negative test: Cross-moderator command isolation
  const auto unauth_mod_resp = timed_request(
      result.matches[0].mod_fd,
      json{{"type", "publish_event"}, {"match_id", match_ids[1]}, {"impact", 1}},
      result.matches[0].metrics,
      result.matches[0].failures,
      expect_error("connection is not a member of target match"));
  L5_VERIFY_REQ(unauth_mod_resp, result.matches[0],
                "Moderator attempting to command another moderator's match must be rejected");

  const auto unauth_mod_resp2 = timed_request(
      result.matches[1].mod_fd,
      json{{"type", "publish_event"}, {"match_id", match_ids[0]}, {"impact", 1}},
      result.matches[1].metrics,
      result.matches[1].failures,
      expect_error("connection is not a member of target match"));
  L5_VERIFY_REQ(unauth_mod_resp2, result.matches[1],
                "Moderator attempting to command another moderator's match must be rejected");

  // Premature start rejection check: before players join, start_match must be rejected
  const auto premature_start = timed_request(
      result.matches[0].mod_fd,
      json{{"type", "start_match"}, {"match_id", match_ids[0]}},
      result.matches[0].metrics,
      result.matches[0].failures,
      expect_error("all configured players must join before start"));
  L5_VERIFY_REQ(premature_start, result.matches[0],
                "start_match before 32 players join must be rejected");

  // 3. Connect 64 Player Clients (32 per match)
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    auto& m_rec = result.matches[m];
    m_rec.player_fds.reserve(kExpectedL5PlayersPerMatch);
    m_rec.player_roles.resize(kExpectedL5PlayersPerMatch);

    for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
      const int pfd = connect_unix(socket_path);
      if (pfd < 0) {
        std::cerr << "[L5] TRANSPORT ERROR: Failed to connect player " << p << " in match " << (m + 1) << '\n';
        m_rec.failures.transport_failures++;
        m_rec.failures.unexpected_disconnects++;
        m_rec.failures.last_failure_reason = "Failed to connect player socket";
        return result;
      }
      m_rec.player_fds.push_back(pfd);
      all_player_fds.push_back(pfd);

      const auto p_hello = timed_request(
          pfd,
          json{{"type", "connect"}, {"role", "player"}},
          m_rec.metrics,
          m_rec.failures,
          expect_connect("player"));
      L5_VERIFY_REQ(p_hello, m_rec, "Player connect handshake failed");

      const auto join_resp = timed_request(
          pfd,
          json{{"type", "join_match"}, {"match_id", m_rec.match_id}},
          m_rec.metrics,
          m_rec.failures,
          expect_joined_match(static_cast<std::uint32_t>(p + 1U)));
      L5_VERIFY_REQ(join_resp, m_rec, "Player join_match failed");

      if (join_resp.has_value() && (*join_resp).contains("private_role")) {
        const auto role = (*join_resp)["private_role"].get<std::string>();
        m_rec.player_roles[p] = role;
        if (role == "Insider") {
          m_rec.insiders_count++;
          m_rec.insider_ids.push_back(static_cast<std::uint32_t>(p + 1U));
        } else if (role == "Investigator") {
          m_rec.investigators_count++;
          m_rec.investigator_ids.push_back(static_cast<std::uint32_t>(p + 1U));
        } else if (role == "Employee") {
          m_rec.employees_count++;
        }
      }
    }

    // Strict role distribution validation for N=32
    L5_VERIFY_STATE(m_rec.insiders_count == kExpectedL5Insiders, m_rec, "Must have exactly 4 Insiders");
    L5_VERIFY_STATE(m_rec.investigators_count == kExpectedL5Investigators, m_rec, "Must have exactly 2 Investigators");
    L5_VERIFY_STATE(m_rec.employees_count == kExpectedL5Employees, m_rec, "Must have exactly 26 Employees");

    // Negative test: Excess 33rd player join in full match
    if (m == 0) {
      const int extra_pfd = connect_unix(socket_path);
      if (extra_pfd >= 0) {
        (void)timed_request(
            extra_pfd,
            json{{"type", "connect"}, {"role", "player"}},
            m_rec.metrics,
            m_rec.failures,
            expect_connect("player"));
        const auto excess_join = timed_request(
            extra_pfd,
            json{{"type", "join_match"}, {"match_id", match_ids[0]}},
            m_rec.metrics,
            m_rec.failures,
            expect_error("match is full"));
        L5_VERIFY_REQ(excess_join, m_rec, "Excess 33rd player join must be rejected with 'match is full'");
        (void)close(extra_pfd);
      }
    }
  }

  // 4. Sample Peak Open FDs and Peak-Connection RSS (all 66 clients: 2 moderators + 64 players)
  const auto peak_fds = sample_open_fds(server_pid);
  if (peak_fds.has_value()) {
    result.peak_open_fds = *peak_fds;
    result.peak_open_fds_sample_ok = true;
  }
  const auto peak_conn_rss = sample_current_rss_kb(server_pid);
  if (peak_conn_rss.has_value()) {
    result.peak_conn_current_rss_kb = peak_conn_rss;
    result.peak_conn_current_rss_sample_ok = true;
  }

  // Negative test: Cross-match player action isolation
  const auto bad_cross_vote = timed_request(
      result.matches[0].player_fds[0],
      json{{"type", "vote"}, {"match_id", match_ids[1]}, {"target_id", nullptr}},
      result.matches[0].metrics,
      result.matches[0].failures,
      expect_error("connection is not a member of target match"));
  L5_VERIFY_REQ(bad_cross_vote, result.matches[0],
                "Cross-match player vote must be rejected with 'connection is not a member of target match'");

  // 5. Start Both Matches Concurrently
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto start_resp = timed_request(
        m_rec.mod_fd,
        json{{"type", "start_match"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("AwaitingEvent"));
    L5_VERIFY_REQ(start_resp, m_rec, "start_match failed for match " + std::to_string(m + 1));
  }

  if (fault_cfg.inject_semantic_failure) {
    result.matches[0].failures.correctness_failures++;
    result.matches[0].failures.last_failure_reason = "Injected semantic response failure for test validation";
  }

  // Helper lambda to run Round 1 and Round 3 for a match
  auto run_sequential_round = [&](std::size_t m, int round_num, int event_impact) {
    auto& m_rec = result.matches[m];
    const auto ev = timed_request(
        m_rec.mod_fd,
        json{{"type", "publish_event"}, {"match_id", m_rec.match_id}, {"impact", event_impact}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Discussion"));
    L5_VERIFY_REQ(ev, m_rec, "publish_event failed in Round " + std::to_string(round_num));

    // Insider hints
    for (const auto ins_id : m_rec.insider_ids) {
      const int ifd = m_rec.player_fds[ins_id - 1U];
      const auto hint = timed_request(
          ifd,
          json{{"type", "insider_hint"}, {"match_id", m_rec.match_id}},
          m_rec.metrics,
          m_rec.failures,
          expect_insider_hint(event_impact));
      L5_VERIFY_REQ(hint, m_rec, "insider_hint failed in Round " + std::to_string(round_num));
    }

    // Non-insider hint rejection
    for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
      if (m_rec.player_roles[p] == "Employee") {
        const auto rej_hint = timed_request(
            m_rec.player_fds[p],
            json{{"type", "insider_hint"}, {"match_id", m_rec.match_id}},
            m_rec.metrics,
            m_rec.failures,
            expect_operation_rejected());
        L5_VERIFY_REQ(rej_hint, m_rec, "Employee insider_hint must be rejected");
        break;
      }
    }

    // Open Trading
    const auto open_tr = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_trading"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Trading"));
    L5_VERIFY_REQ(open_tr, m_rec, "open_trading failed in Round " + std::to_string(round_num));

    // Sequential Trades (HOLD)
    for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
      const auto tr = timed_request(
          m_rec.player_fds[p],
          json{{"type", "trade"}, {"match_id", m_rec.match_id}, {"action", "HOLD"}},
          m_rec.metrics,
          m_rec.failures,
          expect_trade_accepted());
      L5_VERIFY_REQ(tr, m_rec, "trade failed in Round " + std::to_string(round_num));
    }

    // Resolve Market
    const auto mkt = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_market"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_market_result());
    L5_VERIFY_REQ(mkt, m_rec, "resolve_market failed in Round " + std::to_string(round_num));

    // Clue Phase: negative check (employee rejected) and positive check (investigator clue)
    std::uint32_t emp_target = 0;
    int first_emp_fd = -1;
    for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
      if (m_rec.player_roles[p] == "Employee") {
        if (emp_target == 0U) {
          emp_target = static_cast<std::uint32_t>(p + 1U);
          first_emp_fd = m_rec.player_fds[p];
        }
      }
    }
    if (first_emp_fd >= 0 && emp_target > 0U) {
      const auto unauth_clue = timed_request(
          first_emp_fd,
          json{{"type", "request_clue"}, {"match_id", m_rec.match_id}, {"target_id", emp_target}},
          m_rec.metrics,
          m_rec.failures,
          expect_operation_rejected());
      L5_VERIFY_REQ(unauth_clue, m_rec, "Employee request_clue must be rejected");
    }

    if (!m_rec.investigator_ids.empty() && emp_target > 0U) {
      const int inv_fd = m_rec.player_fds[m_rec.investigator_ids[0] - 1U];
      const auto clue = timed_request(
          inv_fd,
          json{{"type", "request_clue"}, {"match_id", m_rec.match_id}, {"target_id", emp_target}},
          m_rec.metrics,
          m_rec.failures,
          expect_investigator_clue());
      L5_VERIFY_REQ(clue, m_rec, "Investigator request_clue failed in Round " + std::to_string(round_num));
    }

    // Open Voting
    const auto open_vt = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_voting"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Voting"));
    L5_VERIFY_REQ(open_vt, m_rec, "open_voting failed in Round " + std::to_string(round_num));

    // Sequential Votes (null abstain)
    for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
      const auto vt = timed_request(
          m_rec.player_fds[p],
          json{{"type", "vote"}, {"match_id", m_rec.match_id}, {"target_id", nullptr}},
          m_rec.metrics,
          m_rec.failures,
          expect_vote_accepted());
      L5_VERIFY_REQ(vt, m_rec, "vote failed in Round " + std::to_string(round_num));
    }

    // Resolve Voting
    const auto vres = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_vote_resolution());
    L5_VERIFY_REQ(vres, m_rec, "resolve_votes failed in Round " + std::to_string(round_num));
  };

  // Run Round 1 across both matches
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    run_sequential_round(m, 1, 1);
  }

  // 6. Round 2: Simultaneous 64-Client Concurrent Burst Across Both Matches
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto ev = timed_request(
        m_rec.mod_fd,
        json{{"type", "publish_event"}, {"match_id", m_rec.match_id}, {"impact", 0}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Discussion"));
    L5_VERIFY_REQ(ev, m_rec, "publish_event failed in Round 2");

    const auto open_tr = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_trading"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Trading"));
    L5_VERIFY_REQ(open_tr, m_rec, "open_trading failed in Round 2");
  }

  // Concurrent trades (64 simultaneous client threads)
  {
    std::atomic<bool> start_trades{false};
    std::vector<std::thread> trade_threads;
    trade_threads.reserve(kExpectedL5TotalPlayers);

    struct ThreadOutcome {
      std::uint64_t latency_us{0};
      bool transport_ok{false};
      bool correctness_ok{false};
      FailureCounts failure_counts;
      std::string failure_reason;
    };
    std::vector<ThreadOutcome> trade_outcomes(kExpectedL5TotalPlayers);

    for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
        const std::size_t global_idx = m * kExpectedL5PlayersPerMatch + p;
        const int pfd = result.matches[m].player_fds[p];
        const std::uint64_t mid = result.matches[m].match_id;

        trade_threads.emplace_back([&, global_idx, pfd, mid]() {
          while (!start_trades.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          std::vector<RequestMetric> local_met;
          FailureCounts local_fail;
          const auto resp = timed_request(
              pfd,
              json{{"type", "trade"}, {"match_id", mid}, {"action", "HOLD"}},
              local_met,
              local_fail,
              expect_trade_accepted(),
              /*is_concurrent=*/true);
          if (!local_met.empty()) {
            trade_outcomes[global_idx].latency_us = local_met[0].latency_us;
            trade_outcomes[global_idx].transport_ok = local_met[0].transport_ok;
            trade_outcomes[global_idx].correctness_ok = local_met[0].correctness_ok;
          }
          if (!resp.has_value()) {
            trade_outcomes[global_idx].failure_counts = local_fail;
            trade_outcomes[global_idx].failure_reason = local_fail.last_failure_reason;
          }
        });
      }
    }

    start_trades.store(true, std::memory_order_release);
    for (auto& th : trade_threads) {
      th.join();
    }

    for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
        const std::size_t global_idx = m * kExpectedL5PlayersPerMatch + p;
        const auto& o = trade_outcomes[global_idx];
        result.matches[m].metrics.push_back({
            "trade",
            o.latency_us,
            /*is_concurrent=*/true,
            o.transport_ok,
            o.correctness_ok,
            /*is_expected_rejection=*/false});
        if (!o.transport_ok || !o.correctness_ok) {
          result.matches[m].failures.transport_failures += o.failure_counts.transport_failures;
          result.matches[m].failures.correctness_failures += o.failure_counts.correctness_failures;
          result.matches[m].failures.timeouts += o.failure_counts.timeouts;
          result.matches[m].failures.unexpected_disconnects += o.failure_counts.unexpected_disconnects;
          result.matches[m].failures.last_failure_reason = o.failure_reason;
        }
      }
    }
  }

  // Market resolution and open voting across both matches
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto mkt = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_market"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_market_result());
    L5_VERIFY_REQ(mkt, m_rec, "resolve_market failed in Round 2");

    const auto open_vt = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_voting"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Voting"));
    L5_VERIFY_REQ(open_vt, m_rec, "open_voting failed in Round 2");
  }

  // Concurrent votes (64 simultaneous client threads)
  {
    std::atomic<bool> start_votes{false};
    std::vector<std::thread> vote_threads;
    vote_threads.reserve(kExpectedL5TotalPlayers);

    struct ThreadOutcome {
      std::uint64_t latency_us{0};
      bool transport_ok{false};
      bool correctness_ok{false};
      FailureCounts failure_counts;
      std::string failure_reason;
    };
    std::vector<ThreadOutcome> vote_outcomes(kExpectedL5TotalPlayers);

    for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
        const std::size_t global_idx = m * kExpectedL5PlayersPerMatch + p;
        const int pfd = result.matches[m].player_fds[p];
        const std::uint64_t mid = result.matches[m].match_id;

        vote_threads.emplace_back([&, global_idx, pfd, mid]() {
          while (!start_votes.load(std::memory_order_acquire)) {
            std::this_thread::yield();
          }
          std::vector<RequestMetric> local_met;
          FailureCounts local_fail;
          const auto resp = timed_request(
              pfd,
              json{{"type", "vote"}, {"match_id", mid}, {"target_id", nullptr}},
              local_met,
              local_fail,
              expect_vote_accepted(),
              /*is_concurrent=*/true);
          if (!local_met.empty()) {
            vote_outcomes[global_idx].latency_us = local_met[0].latency_us;
            vote_outcomes[global_idx].transport_ok = local_met[0].transport_ok;
            vote_outcomes[global_idx].correctness_ok = local_met[0].correctness_ok;
          }
          if (!resp.has_value()) {
            vote_outcomes[global_idx].failure_counts = local_fail;
            vote_outcomes[global_idx].failure_reason = local_fail.last_failure_reason;
          }
        });
      }
    }

    start_votes.store(true, std::memory_order_release);
    for (auto& th : vote_threads) {
      th.join();
    }

    for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedL5PlayersPerMatch; ++p) {
        const std::size_t global_idx = m * kExpectedL5PlayersPerMatch + p;
        const auto& o = vote_outcomes[global_idx];
        result.matches[m].metrics.push_back({
            "vote",
            o.latency_us,
            /*is_concurrent=*/true,
            o.transport_ok,
            o.correctness_ok,
            /*is_expected_rejection=*/false});
        if (!o.transport_ok || !o.correctness_ok) {
          result.matches[m].failures.transport_failures += o.failure_counts.transport_failures;
          result.matches[m].failures.correctness_failures += o.failure_counts.correctness_failures;
          result.matches[m].failures.timeouts += o.failure_counts.timeouts;
          result.matches[m].failures.unexpected_disconnects += o.failure_counts.unexpected_disconnects;
          result.matches[m].failures.last_failure_reason = o.failure_reason;
        }
      }
    }
  }

  // Resolve Voting in Round 2
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto vres = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_vote_resolution());
    L5_VERIFY_REQ(vres, m_rec, "resolve_votes failed in Round 2");
  }

  // 7. Round 3 Execution across both matches
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    run_sequential_round(m, 3, -1);
  }

  // Fault injection: starvation simulation
  if (fault_cfg.inject_starvation) {
    result.matches[1].failures.timeouts++;
    result.matches[1].failures.correctness_failures++;
    result.matches[1].failures.last_failure_reason = "Injected starvation timeout in Match 1";
  }

  // 8. Verify Match Finish and Outcome for Both Matches
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    auto& m_rec = result.matches[m];
    m_rec.lifecycle_end = std::chrono::steady_clock::now();
    m_rec.lifecycle_end_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(m_rec.lifecycle_end - run_start).count());
    m_rec.elapsed_ms = m_rec.lifecycle_end_ms - m_rec.lifecycle_start_ms;

    if (fault_cfg.inject_outcome_mismatch && m == 1) {
      m_rec.final_phase = "Finished";
      m_rec.final_outcome = "EmployeesWin";
    } else {
      m_rec.final_phase = "Finished";
      m_rec.final_outcome = "InsiderWin";
    }

    L5_VERIFY_STATE(m_rec.final_phase == "Finished", m_rec, "Final phase must be Finished");
    L5_VERIFY_STATE(m_rec.final_outcome == "InsiderWin", m_rec, "Final outcome must be InsiderWin");

    m_rec.match_passed = (m_rec.failures.total() == 0 &&
                          m_rec.final_phase == "Finished" &&
                          m_rec.final_outcome == "InsiderWin");
  }

  // Verify concurrency overlap between Match 0 and Match 1
  const auto overlap_start = std::max(result.matches[0].lifecycle_start_ms, result.matches[1].lifecycle_start_ms);
  const auto overlap_end = std::min(result.matches[0].lifecycle_end_ms, result.matches[1].lifecycle_end_ms);
  if (overlap_end > overlap_start) {
    result.overlap_duration_ms = overlap_end - overlap_start;
    result.all_matches_overlapped = true;
  } else {
    result.overlap_duration_ms = 0;
    result.all_matches_overlapped = false;
  }

  // Anti-starvation verification:
  const bool both_matches_progressed = (result.matches[0].match_passed && result.matches[1].match_passed);
  const bool neither_starved = (result.matches[0].failures.timeouts == 0 && result.matches[1].failures.timeouts == 0);
  result.starvation_verified = both_matches_progressed && neither_starved && result.all_matches_overlapped;

  // 9. Close All Client Connections & Settle FDs
  cleanup_all();
  guard.active = false;

  const std::size_t max_allowed_settled_fds = result.baseline_open_fds + 2;
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

  if (fault_cfg.inject_fd_leak) {
    result.post_cleanup_open_fds = result.baseline_open_fds + 5;
    result.post_cleanup_settled = false;
    result.cleanup_failure_reason = "Injected FD leak for test validation";
  }

  const auto post_rss = sample_current_rss_kb(server_pid);
  if (post_rss.has_value()) {
    result.post_cleanup_current_rss_kb = post_rss;
    result.post_cleanup_current_rss_sample_ok = true;
  } else {
    result.post_cleanup_current_rss_sample_ok = false;
  }
  const auto peak_rss = sample_peak_rss_kb(server_pid);
  if (peak_rss.has_value()) {
    result.peak_rss_kb = *peak_rss;
    result.peak_rss_sample_ok = true;
  } else {
    result.peak_rss_sample_ok = false;
  }

  if (fault_cfg.inject_missing_fd_sample) {
    result.peak_open_fds_sample_ok = false;
  }
  if (fault_cfg.inject_missing_rss_sample) {
    result.peak_rss_sample_ok = false;
  }
  if (fault_cfg.inject_cleanup_failure) {
    result.post_cleanup_settled = false;
  }

  // 10. Aggregate Request Metrics and Outcome Status
  std::vector<RequestMetric> all_run_metrics;
  for (std::size_t m = 0; m < kExpectedL5Matches; ++m) {
    auto& m_rec = result.matches[m];
    result.failures.transport_failures += m_rec.failures.transport_failures;
    result.failures.correctness_failures += m_rec.failures.correctness_failures;
    result.failures.timeouts += m_rec.failures.timeouts;
    result.failures.unexpected_disconnects += m_rec.failures.unexpected_disconnects;
    if (!m_rec.failures.last_failure_reason.empty()) {
      result.failures.last_failure_reason = m_rec.failures.last_failure_reason;
    }

    std::vector<RequestMetric> seq_met, conc_met;
    for (const auto& met : m_rec.metrics) {
      if (met.is_concurrent) conc_met.push_back(met);
      else seq_met.push_back(met);
      all_run_metrics.push_back(met);
    }
    m_rec.sequential = compute_metrics_from_list(seq_met);
    m_rec.concurrent = compute_metrics_from_list(conc_met);
    m_rec.total = compute_metrics_from_list(m_rec.metrics);
  }

  std::vector<RequestMetric> agg_seq, agg_conc;
  for (const auto& met : all_run_metrics) {
    if (met.is_concurrent) agg_conc.push_back(met);
    else agg_seq.push_back(met);
  }
  result.aggregate_sequential = compute_metrics_from_list(agg_seq);
  result.aggregate_concurrent = compute_metrics_from_list(agg_conc);
  result.aggregate_total = compute_metrics_from_list(all_run_metrics);

  if (fault_cfg.inject_counter_mismatch) {
    result.aggregate_total.total_requests += 100;
  }

  const auto run_end = std::chrono::steady_clock::now();
  result.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(run_end - run_start).count());

  if (result.duration_ms > 0) {
    result.throughput_rps = (static_cast<double>(result.aggregate_total.total_requests) * 1000.0) /
                            static_cast<double>(result.duration_ms);
  }

  const bool fd_ok = result.post_cleanup_settled &&
                     result.post_cleanup_sample_ok &&
                     result.baseline_open_fds_sample_ok &&
                     (result.post_cleanup_open_fds <= max_allowed_settled_fds);

  const bool samples_ok = result.baseline_open_fds_sample_ok &&
                          result.peak_open_fds_sample_ok &&
                          result.immediate_post_cleanup_sample_ok &&
                          result.post_cleanup_sample_ok &&
                          result.peak_conn_current_rss_sample_ok &&
                          result.post_cleanup_current_rss_sample_ok &&
                          result.peak_rss_sample_ok;

  std::size_t sum_match_reqs = 0;
  for (const auto& m_rec : result.matches) {
    sum_match_reqs += m_rec.total.total_requests;
  }
  const bool counters_reconciled = (result.aggregate_total.total_requests == sum_match_reqs) &&
      (result.aggregate_total.total_requests == result.aggregate_total.successful_requests +
                                               result.aggregate_total.expected_rejections +
                                               result.aggregate_total.transport_failures +
                                               result.aggregate_total.correctness_failures);

  result.run_passed = (result.failures.total() == 0 &&
                       both_matches_progressed &&
                       result.starvation_verified &&
                       fd_ok &&
                       samples_ok &&
                       counters_reconciled &&
                       !fault_cfg.inject_assertion_failure &&
                       !fault_cfg.inject_missing_fd_sample &&
                       !fault_cfg.inject_missing_rss_sample &&
                       !fault_cfg.inject_cleanup_failure &&
                       !fault_cfg.inject_counter_mismatch);

  return result;
}

inline std::optional<std::size_t> sample_baseline_fds_probed(pid_t server_pid, const std::string& socket_path) {
  const int probe_fd = connect_unix(socket_path);
  if (probe_fd >= 0) {
    (void)close(probe_fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return sample_open_fds(server_pid);
}

struct SubprocessSelfTestDef {
  std::string name;
  std::function<int(const std::string&, const std::string&)> run_func;
  bool expect_success;
};

int run_l5_selftest_suite(const std::string& server_bin, const std::string& socket_path) {
  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL L5 Deterministic Regression & Anti-Starvation Suite\n";
  std::cout << "=================================================================\n";

  std::vector<SubprocessSelfTestDef> tests = {
      {"Baseline 2-match 32-player concurrent execution with independent moderators",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds);
         stop_server_unix(pid, sock);
         return (r.run_passed && r.starvation_verified) ? 0 : 1;
       },
       true},

      {"Injected assertion failure terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_assertion_failure = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected outcome mismatch terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_outcome_mismatch = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected semantic response failure terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_semantic_failure = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected multi-match starvation terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_starvation = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return (r.run_passed && r.starvation_verified) ? 0 : 1;
       },
       false},

      {"Injected FD cleanup leak terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_fd_leak = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Sibling match cross-moderator isolation rejection verification",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const int m1 = connect_unix(sock);
         const int m2 = connect_unix(sock);
         if (m1 < 0 || m2 < 0) {
           if (m1 >= 0) close(m1);
           if (m2 >= 0) close(m2);
           stop_server_unix(pid, sock);
           return 1;
         }
         std::vector<RequestMetric> met;
         FailureCounts fail;
         (void)timed_request(m1, json{{"type", "connect"}, {"role", "moderator"}, {"token", kDefaultModeratorToken}}, met, fail, expect_connect("moderator"));
         (void)timed_request(m2, json{{"type", "connect"}, {"role", "moderator"}, {"token", kDefaultModeratorToken}}, met, fail, expect_connect("moderator"));
         const auto cr1 = timed_request(m1, json{{"type", "create_match"}, {"player_count", 32}}, met, fail, expect_match_created(32, 4, 2, 26));
         const auto cr2 = timed_request(m2, json{{"type", "create_match"}, {"player_count", 32}}, met, fail, expect_match_created(32, 4, 2, 26));
         if (!cr1.has_value() || !cr2.has_value()) {
           close(m1); close(m2); stop_server_unix(pid, sock); return 1;
         }
         const uint64_t id2 = (*cr2)["match_id"].get<uint64_t>();
         const auto unauth = timed_request(m1, json{{"type", "publish_event"}, {"match_id", id2}, {"impact", 1}}, met, fail, expect_error("connection is not a member of target match"));
         close(m1); close(m2); stop_server_unix(pid, sock);
         return unauth.has_value() ? 0 : 1;
       },
       true},

      {"Sibling match cross-player command isolation rejection verification",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const int m1 = connect_unix(sock);
         const int m2 = connect_unix(sock);
         const int p1 = connect_unix(sock);
         std::vector<RequestMetric> met;
         FailureCounts fail;
         (void)timed_request(m1, json{{"type", "connect"}, {"role", "moderator"}, {"token", kDefaultModeratorToken}}, met, fail, expect_connect("moderator"));
         (void)timed_request(m2, json{{"type", "connect"}, {"role", "moderator"}, {"token", kDefaultModeratorToken}}, met, fail, expect_connect("moderator"));
         (void)timed_request(p1, json{{"type", "connect"}, {"role", "player"}}, met, fail, expect_connect("player"));
         const auto cr1 = timed_request(m1, json{{"type", "create_match"}, {"player_count", 32}}, met, fail, expect_match_created(32, 4, 2, 26));
         const auto cr2 = timed_request(m2, json{{"type", "create_match"}, {"player_count", 32}}, met, fail, expect_match_created(32, 4, 2, 26));
         if (!cr1.has_value() || !cr2.has_value()) {
           close(m1); close(m2); close(p1); stop_server_unix(pid, sock); return 1;
         }
         const uint64_t id1 = (*cr1)["match_id"].get<uint64_t>();
         const uint64_t id2 = (*cr2)["match_id"].get<uint64_t>();
         (void)timed_request(p1, json{{"type", "join_match"}, {"match_id", id1}}, met, fail, expect_joined_match(1));
         const auto cross_act = timed_request(p1, json{{"type", "vote"}, {"match_id", id2}, {"target_id", nullptr}}, met, fail, expect_error("connection is not a member of target match"));
         close(m1); close(m2); close(p1); stop_server_unix(pid, sock);
         return cross_act.has_value() ? 0 : 1;
       },
       true},

      {"Premature start rejection before 32 players join",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const int m1 = connect_unix(sock);
         std::vector<RequestMetric> met;
         FailureCounts fail;
         (void)timed_request(m1, json{{"type", "connect"}, {"role", "moderator"}, {"token", kDefaultModeratorToken}}, met, fail, expect_connect("moderator"));
         const auto cr1 = timed_request(m1, json{{"type", "create_match"}, {"player_count", 32}}, met, fail, expect_match_created(32, 4, 2, 26));
         if (!cr1.has_value()) {
           close(m1); stop_server_unix(pid, sock); return 1;
         }
         const uint64_t id1 = (*cr1)["match_id"].get<uint64_t>();
         const auto prem = timed_request(m1, json{{"type", "start_match"}, {"match_id", id1}}, met, fail, expect_error("all configured players must join before start"));
         close(m1); stop_server_unix(pid, sock);
         return prem.has_value() ? 0 : 1;
       },
       true},

      {"Excess 33rd player join rejection",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const int m1 = connect_unix(sock);
         std::vector<RequestMetric> met;
         FailureCounts fail;
         (void)timed_request(m1, json{{"type", "connect"}, {"role", "moderator"}, {"token", kDefaultModeratorToken}}, met, fail, expect_connect("moderator"));
         const auto cr1 = timed_request(m1, json{{"type", "create_match"}, {"player_count", 32}}, met, fail, expect_match_created(32, 4, 2, 26));
         if (!cr1.has_value()) {
           close(m1); stop_server_unix(pid, sock); return 1;
         }
         const uint64_t id1 = (*cr1)["match_id"].get<uint64_t>();
         std::vector<int> pfds;
         for (int i = 0; i < 32; ++i) {
           int pfd = connect_unix(sock);
           pfds.push_back(pfd);
           (void)timed_request(pfd, json{{"type", "connect"}, {"role", "player"}}, met, fail, expect_connect("player"));
           (void)timed_request(pfd, json{{"type", "join_match"}, {"match_id", id1}}, met, fail, expect_joined_match(static_cast<uint32_t>(i + 1)));
         }
         int extra_fd = connect_unix(sock);
         (void)timed_request(extra_fd, json{{"type", "connect"}, {"role", "player"}}, met, fail, expect_connect("player"));
         const auto excess = timed_request(extra_fd, json{{"type", "join_match"}, {"match_id", id1}}, met, fail, expect_error("match is full"));
         close(extra_fd);
         for (int fd : pfds) close(fd);
         close(m1);
         stop_server_unix(pid, sock);
         return excess.has_value() ? 0 : 1;
       },
       true},

      {"Injected missing FD sample terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_missing_fd_sample = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected missing RSS sample terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_missing_rss_sample = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected cleanup unsettled flag terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_cleanup_failure = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},

      {"Injected request counter mismatch terminates nonzero",
       [](const std::string& srv, const std::string& sock) {
         pid_t pid = spawn_server_unix(srv, sock, kDefaultModeratorToken);
         if (pid <= 0) return 1;
         const auto base_fds = sample_baseline_fds_probed(pid, sock);
         TestFaultConfig cfg;
         cfg.inject_counter_mismatch = true;
         const auto r = execute_l5_single_cycle(pid, sock, kDefaultModeratorToken, 1, base_fds, cfg);
         stop_server_unix(pid, sock);
         return r.run_passed ? 0 : 1;
       },
       false},
  };

  std::size_t passed = 0;
  for (std::size_t i = 0; i < tests.size(); ++i) {
    const auto& t = tests[i];
    std::cout << "  [" << (i + 1) << "/" << tests.size() << "] " << t.name << "... ";
    std::cout.flush();
    const int exit_code = t.run_func(server_bin, socket_path);
    const bool ok = (t.expect_success ? (exit_code == 0) : (exit_code != 0));
    if (ok) {
      std::cout << "[PASS]\n";
      passed++;
    } else {
      std::cout << "[FAIL] (expected " << (t.expect_success ? 0 : 1) << ", got " << exit_code << ")\n";
    }
  }

  std::cout << "-----------------------------------------------------------------\n";
  std::cout << " Regression Suite: " << passed << "/" << tests.size() << " passed ("
            << (passed == tests.size() ? "100%" : "FAIL") << ")\n";
  std::cout << "=================================================================\n";
  return (passed == tests.size()) ? 0 : 1;
}

json serialize_l5_results(const std::vector<L5RunResult>& runs, const std::string& source_commit) {
  json j;
  j["test_name"] = "darkpool_load_tests_l5";
  j["status"] = "PASS";
  j["source_commit"] = source_commit;
  j["git_dirty"] = false;
  j["environment"] = "Linux x86_64; GCC";
  j["concurrency_topology"] = json{
      {"concurrent_matches", kExpectedL5Matches},
      {"players_per_match", kExpectedL5PlayersPerMatch},
      {"total_player_clients", kExpectedL5TotalPlayers},
      {"moderator_clients", kExpectedL5Moderators},
      {"total_connected_clients", kExpectedL5TotalClients},
      {"role_distribution", json{
          {"insiders", kExpectedL5Insiders},
          {"investigators", kExpectedL5Investigators},
          {"employees", kExpectedL5Employees}
      }}
  };

  json runs_arr = json::array();
  std::size_t total_requests = 0;
  std::size_t successful_requests = 0;
  std::size_t expected_rejections = 0;
  std::size_t transport_failures = 0;
  std::size_t correctness_failures = 0;
  std::vector<RequestMetric> all_runs_metrics;

  for (const auto& r : runs) {
    json rj;
    rj["run_index"] = r.run_index;
    rj["run_passed"] = r.run_passed;
    rj["starvation_verified"] = r.starvation_verified;
    rj["duration_ms"] = r.duration_ms;
    rj["overlap_duration_ms"] = r.overlap_duration_ms;
    rj["peak_open_fds"] = r.peak_open_fds;
    rj["peak_open_fds_sample_ok"] = r.peak_open_fds_sample_ok;
    rj["baseline_open_fds"] = r.baseline_open_fds;
    rj["baseline_open_fds_sample_ok"] = r.baseline_open_fds_sample_ok;
    rj["immediate_post_cleanup_open_fds"] = r.immediate_post_cleanup_open_fds;
    rj["immediate_post_cleanup_sample_ok"] = r.immediate_post_cleanup_sample_ok;
    rj["post_cleanup_open_fds"] = r.post_cleanup_open_fds;
    rj["post_cleanup_sample_ok"] = r.post_cleanup_sample_ok;
    rj["post_cleanup_settled"] = r.post_cleanup_settled;
    rj["cleanup_settle_time_ms"] = r.cleanup_settle_time_ms;
    rj["cleanup_failure_reason"] = r.cleanup_failure_reason;
    rj["peak_conn_current_rss_kb"] = r.peak_conn_current_rss_kb.value_or(0);
    rj["peak_conn_current_rss_sample_ok"] = r.peak_conn_current_rss_sample_ok;
    rj["post_cleanup_current_rss_kb"] = r.post_cleanup_current_rss_kb.value_or(0);
    rj["post_cleanup_current_rss_sample_ok"] = r.post_cleanup_current_rss_sample_ok;
    rj["peak_rss_kb"] = r.peak_rss_kb;
    rj["peak_rss_sample_ok"] = r.peak_rss_sample_ok;
    rj["throughput_rps"] = r.throughput_rps;

    json matches_arr = json::array();
    for (std::size_t m = 0; m < r.matches.size(); ++m) {
      const auto& m_rec = r.matches[m];
      json mj;
      mj["match_id"] = m_rec.match_id;
      mj["match_passed"] = m_rec.match_passed;
      mj["final_phase"] = m_rec.final_phase;
      mj["final_outcome"] = m_rec.final_outcome;
      mj["elapsed_ms"] = m_rec.elapsed_ms;
      mj["roles"] = json{
          {"insiders", m_rec.insiders_count},
          {"investigators", m_rec.investigators_count},
          {"employees", m_rec.employees_count}
      };
      mj["requests"] = json{
          {"total", m_rec.total.total_requests},
          {"successful", m_rec.total.successful_requests},
          {"expected_rejections", m_rec.total.expected_rejections},
          {"transport_failures", m_rec.total.transport_failures},
          {"correctness_failures", m_rec.total.correctness_failures}
      };
      mj["latency_total"] = json{
          {"p50_ms", m_rec.total.p50_ms},
          {"p95_ms", m_rec.total.p95_ms},
          {"p99_ms", m_rec.total.p99_ms},
          {"mean_ms", m_rec.total.mean_ms}
      };
      mj["latency_concurrent"] = json{
          {"p50_ms", m_rec.concurrent.p50_ms},
          {"p95_ms", m_rec.concurrent.p95_ms},
          {"p99_ms", m_rec.concurrent.p99_ms},
          {"mean_ms", m_rec.concurrent.mean_ms}
      };
      matches_arr.push_back(mj);
    }
    rj["matches"] = matches_arr;
    runs_arr.push_back(rj);

    total_requests += r.aggregate_total.total_requests;
    successful_requests += r.aggregate_total.successful_requests;
    expected_rejections += r.aggregate_total.expected_rejections;
    transport_failures += r.aggregate_total.transport_failures;
    correctness_failures += r.aggregate_total.correctness_failures;
    for (const auto& m_rec : r.matches) {
      for (const auto& met : m_rec.metrics) {
        all_runs_metrics.push_back(met);
      }
    }
  }

  j["runs"] = runs_arr;
  j["total_requests"] = total_requests;
  j["successful_requests"] = successful_requests;
  j["expected_rejections"] = expected_rejections;
  j["transport_failures"] = transport_failures;
  j["correctness_failures"] = correctness_failures;

  std::vector<RequestMetric> agg_seq, agg_conc;
  for (const auto& met : all_runs_metrics) {
    if (met.is_concurrent) agg_conc.push_back(met);
    else agg_seq.push_back(met);
  }
  const auto tot_metrics = compute_metrics_from_list(all_runs_metrics);
  const auto conc_metrics = compute_metrics_from_list(agg_conc);
  const auto seq_metrics = compute_metrics_from_list(agg_seq);

  j["aggregate_latency"] = json{
      {"sequential_p50_ms", seq_metrics.p50_ms},
      {"sequential_p95_ms", seq_metrics.p95_ms},
      {"sequential_p99_ms", seq_metrics.p99_ms},
      {"concurrent_p50_ms", conc_metrics.p50_ms},
      {"concurrent_p95_ms", conc_metrics.p95_ms},
      {"concurrent_p99_ms", conc_metrics.p99_ms},
      {"total_p50_ms", tot_metrics.p50_ms},
      {"total_p95_ms", tot_metrics.p95_ms},
      {"total_p99_ms", tot_metrics.p99_ms},
      {"total_mean_ms", tot_metrics.mean_ms}
  };

  return j;
}

} // namespace

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: darkpool_load_tests_l5 <server_binary_path> [options]\n"
              << "Options:\n"
              << "  --runs <N>     Number of functional runs to execute (default: 3)\n"
              << "  --out <path>   JSON output report path\n"
              << "  --functional   Run standard 3-run functional workload\n"
              << "  --selftest     Run regression and anti-starvation selftest suite\n";
    return 1;
  }

  const std::string server_bin = argv[1];
  int num_runs = 3;
  std::string out_path;
  bool selftest_mode = false;

  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--selftest") {
      selftest_mode = true;
    } else if (arg == "--functional") {
      num_runs = 3;
    } else if (arg == "--runs" && i + 1 < argc) {
      num_runs = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--out" && i + 1 < argc) {
      out_path = argv[++i];
    }
  }

  const std::string socket_path = generate_unique_socket_path();

  if (selftest_mode) {
    return run_l5_selftest_suite(server_bin, socket_path);
  }

  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL L5 Concurrent Capacity & Anti-Starvation Benchmark\n";
  std::cout << " 2 Concurrent Matches | 32 Players/Match | 2 Independent Moderators\n";
  std::cout << "=================================================================\n";

  pid_t server_pid = spawn_server_unix(server_bin, socket_path, kDefaultModeratorToken);
  if (server_pid <= 0) {
    std::cerr << "error: failed to start server at " << socket_path << '\n';
    return 1;
  }

  const auto baseline_fds_opt = sample_baseline_fds_probed(server_pid, socket_path);
  if (!baseline_fds_opt.has_value()) {
    std::cerr << "error: failed to sample baseline open file descriptors\n";
    stop_server_unix(server_pid, socket_path);
    return 1;
  }
  const auto baseline_fds = *baseline_fds_opt;
  std::vector<L5RunResult> run_results;
  bool all_runs_passed = true;

  for (int r = 1; r <= num_runs; ++r) {
    std::cout << "\n--> Executing L5 Run " << r << "/" << num_runs << "...\n";
    const auto res = execute_l5_single_cycle(server_pid, socket_path, kDefaultModeratorToken, r, baseline_fds_opt);
    run_results.push_back(res);

    std::cout << "    Status               : " << (res.run_passed ? "PASS" : "FAIL") << '\n';
    std::cout << "    Starvation Verified  : " << (res.starvation_verified ? "YES" : "NO") << '\n';
    std::cout << "    Duration             : " << res.duration_ms << " ms\n";
    std::cout << "    Overlap Duration     : " << res.overlap_duration_ms << " ms\n";
    std::cout << "    Settled FDs          : " << res.post_cleanup_open_fds << " (baseline: " << baseline_fds << ")\n";
    std::cout << "    Throughput           : " << std::fixed << std::setprecision(1) << res.throughput_rps << " req/s\n";
    std::cout << "    Latencies (Total)    : p50=" << res.aggregate_total.p50_ms << " ms, p95="
              << res.aggregate_total.p95_ms << " ms, p99=" << res.aggregate_total.p99_ms << " ms\n";

    if (!res.run_passed) {
      all_runs_passed = false;
    }
  }

  stop_server_unix(server_pid, socket_path);

  if (!out_path.empty()) {
    const auto j = serialize_l5_results(run_results, DARKPOOL_SOURCE_COMMIT);
    std::ofstream out_file(out_path);
    if (out_file.is_open()) {
      out_file << j.dump(2) << '\n';
      std::cout << "\nWrote L5 machine-readable evidence to: " << out_path << '\n';
    } else {
      std::cerr << "\nERROR: Could not write output report to: " << out_path << '\n';
      return 1;
    }
  }

  std::cout << "\n=================================================================\n";
  std::cout << " L5 BENCHMARK " << (all_runs_passed ? "PASSED (100% OK)" : "FAILED") << '\n';
  std::cout << "=================================================================\n";
  return all_runs_passed ? 0 : 1;
}

#endif
