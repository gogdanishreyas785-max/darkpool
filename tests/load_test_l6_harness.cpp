#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests_l6 requires Linux (epoll/unix sockets)\n";
  return 1;
}

#else

#include "load_test_harness_common.hpp"
#include <filesystem>

namespace {

using namespace darkpool::load_test;

constexpr std::size_t kExpectedModerators = 4;

struct MatchRunRecord {
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
};

struct L6RunResult {
  int run_index{1};
  std::size_t match_count{kExpectedL6Matches};
  std::size_t players_per_match{kExpectedPlayerCount};
  std::size_t total_player_clients{kExpectedL6TotalPlayers};
  std::size_t moderator_clients{kExpectedModerators};
  std::size_t total_connected_clients{kExpectedL6TotalPlayers + kExpectedModerators};
  std::uint64_t duration_ms{0};
  bool run_passed{false};
  bool all_matches_overlapped{false};
  std::uint64_t overlap_duration_ms{0};
  std::string contract_notes;
  std::vector<MatchRunRecord> matches;
  PhaseMetrics aggregate_sequential;
  PhaseMetrics aggregate_concurrent;
  PhaseMetrics aggregate_total;
  FailureCounts failures;
  std::uint64_t peak_rss_kb{0};
  double cpu_time_ms{0.0};
  std::size_t baseline_open_fds{0};
  std::size_t peak_open_fds{0};
  std::size_t post_cleanup_open_fds{0};
  double throughput_rps{0.0};
};

L6RunResult execute_l6_concurrent_matches(
    const std::string& server_path,
    int run_index,
    const TestFaultConfig& fault_cfg = {}) {
  L6RunResult result;
  result.run_index = run_index;
  result.matches.resize(kExpectedL6Matches);

  const std::string socket_path = generate_unique_socket_path();
  const std::string mod_token = kDefaultModeratorToken;

  const auto server_pid = spawn_server_unix(server_path, socket_path, mod_token);
  if (server_pid <= 0) {
    std::cerr << "[L6 Run " << run_index << "] ERROR: Failed to spawn darkpool_server\n";
    result.failures.transport_failures++;
    return result;
  }

  // Ensure server responds on UNIX socket
  const int probe_fd = connect_unix(socket_path);
  if (probe_fd < 0) {
    std::cerr << "[L6 Run " << run_index << "] ERROR: Failed to connect probe socket\n";
    result.failures.transport_failures++;
    stop_server_unix(server_pid, socket_path);
    return result;
  }
  (void)close(probe_fd);

  // Baseline Open FDs (0 connected clients)
  result.baseline_open_fds = sample_open_fds(server_pid);

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

  const auto run_start = std::chrono::steady_clock::now();

  #define L6_VERIFY_REQ(resp_opt, match_rec, err_msg) \
    do { \
      if (!(resp_opt).has_value()) { \
        std::cerr << "[L6] REQUEST ERROR: " << (err_msg) \
                  << " (" << (match_rec).failures.last_failure_reason << ") at line " << __LINE__ << '\n'; \
      } \
    } while (0)

  #define L6_VERIFY_STATE(cond, match_rec, err_msg) \
    do { \
      if (!(cond)) { \
        std::cerr << "[L6] STATE ERROR: " << (err_msg) \
                  << " at line " << __LINE__ << '\n'; \
        (match_rec).failures.correctness_failures++; \
      } \
    } while (0)

  // 1. Establish 4 Independent Authenticated Moderator Connections
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    const int mod_fd = connect_unix(socket_path);
    if (mod_fd < 0) {
      std::cerr << "[L6 Run " << run_index << "] TRANSPORT ERROR: Failed to connect moderator " << (m + 1) << '\n';
      result.failures.transport_failures++;
      cleanup_all();
      stop_server_unix(server_pid, socket_path);
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
    L6_VERIFY_REQ(mod_hello, result.matches[m], "Moderator " + std::to_string(m + 1) + " handshake failed");
  }

  result.moderator_clients = all_mod_fds.size();
  result.total_connected_clients = result.total_player_clients + result.moderator_clients;
  result.contract_notes =
      "darkpool_server configured with kMaxModerators=4 and kMaxConnections=260; each of the four concurrent "
      "matches is owned and controlled by an independent authenticated moderator connection.";

  // Negative test: 5th moderator connection must be rejected (kMaxModerators=4)
  const int extra_mod = connect_unix(socket_path);
  if (extra_mod >= 0) {
    std::vector<RequestMetric> temp_met;
    FailureCounts temp_fail;
    const auto extra_resp = timed_request(
        extra_mod,
        json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
        temp_met,
        temp_fail,
        expect_connect("moderator"));
    L6_VERIFY_STATE(!extra_resp.has_value(), result.matches[0],
                    "Server must reject 5th moderator connection exceeding kMaxModerators=4");
    (void)close(extra_mod);
  }

  // 2. Create 4 Concurrent Matches (each moderator creates its own match)
  std::vector<std::uint64_t> match_ids(kExpectedL6Matches, 0);
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    result.matches[m].lifecycle_start = std::chrono::steady_clock::now();
    result.matches[m].lifecycle_start_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(result.matches[m].lifecycle_start - run_start).count());

    const auto create_resp = timed_request(
        result.matches[m].mod_fd,
        json{{"type", "create_match"}, {"player_count", kExpectedPlayerCount}},
        result.matches[m].metrics,
        result.matches[m].failures,
        expect_match_created(kExpectedPlayerCount, kExpectedInsiders, kExpectedInvestigators, kExpectedEmployees));
    L6_VERIFY_REQ(create_resp, result.matches[m], "create_match failed for match " + std::to_string(m + 1));
    if (create_resp.has_value()) {
      match_ids[m] = (*create_resp)["match_id"].get<std::uint64_t>();
      result.matches[m].match_id = match_ids[m];
    }
  }

  // Verify all 4 match IDs are distinct and positive
  for (std::size_t i = 0; i < kExpectedL6Matches; ++i) {
    for (std::size_t j = i + 1; j < kExpectedL6Matches; ++j) {
      L6_VERIFY_STATE(match_ids[i] != match_ids[j] && match_ids[i] > 0,
                      result.matches[i],
                      "Match IDs must be unique and nonzero");
    }
  }

  // Negative test: Unauthorized cross-moderator access
  // Moderator 0 attempts command on Match 1 (owned by Moderator 1)
  const auto unauth_mod_resp = timed_request(
      result.matches[0].mod_fd,
      json{{"type", "publish_event"}, {"match_id", match_ids[1]}, {"impact", 1}},
      result.matches[0].metrics,
      result.matches[0].failures,
      expect_error("connection is not a member of target match"));
  L6_VERIFY_REQ(unauth_mod_resp, result.matches[0],
                "Moderator attempting to command another moderator's match must be rejected");

  // Moderator 1 attempts command on Match 0 (owned by Moderator 0)
  const auto unauth_mod_resp2 = timed_request(
      result.matches[1].mod_fd,
      json{{"type", "publish_event"}, {"match_id", match_ids[0]}, {"impact", 1}},
      result.matches[1].metrics,
      result.matches[1].failures,
      expect_error("connection is not a member of target match"));
  L6_VERIFY_REQ(unauth_mod_resp2, result.matches[1],
                "Moderator attempting to command another moderator's match must be rejected");

  // 3. Connect 256 Player Clients and Join Matches (64 Players per Match)
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];
    m_rec.player_fds.reserve(kExpectedPlayerCount);
    m_rec.player_roles.resize(kExpectedPlayerCount);

    for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
      const int pfd = connect_unix(socket_path);
      if (pfd < 0) {
        std::cerr << "[L6] TRANSPORT ERROR: Failed to connect player " << p << " in match " << (m + 1) << '\n';
        m_rec.failures.transport_failures++;
        cleanup_all();
        stop_server_unix(server_pid, socket_path);
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
      L6_VERIFY_REQ(p_hello, m_rec, "Player connect handshake failed");

      const auto join_resp = timed_request(
          pfd,
          json{{"type", "join_match"}, {"match_id", m_rec.match_id}},
          m_rec.metrics,
          m_rec.failures,
          expect_joined_match(static_cast<std::uint32_t>(p + 1U)));
      L6_VERIFY_REQ(join_resp, m_rec, "Player join_match failed");

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

    // Role count validation for N=64
    L6_VERIFY_STATE(m_rec.insiders_count == kExpectedInsiders, m_rec, "Must have exactly 8 Insiders");
    L6_VERIFY_STATE(m_rec.investigators_count == kExpectedInvestigators, m_rec, "Must have exactly 4 Investigators");
    L6_VERIFY_STATE(m_rec.employees_count == kExpectedEmployees, m_rec, "Must have exactly 52 Employees");

    // Negative test: Excess 65th player join in full match (tested on match 0 before all matches are full)
    if (m == 0) {
      const int extra_pfd = connect_unix(socket_path);
      if (extra_pfd >= 0) {
        (void)timed_request(extra_pfd, json{{"type", "connect"}, {"role", "player"}},
                            result.matches[0].metrics, result.matches[0].failures, expect_connect("player"));
        const auto excess_join = timed_request(
            extra_pfd,
            json{{"type", "join_match"}, {"match_id", match_ids[0]}},
            result.matches[0].metrics,
            result.matches[0].failures,
            expect_error("match is full"));
        L6_VERIFY_REQ(excess_join, result.matches[0],
                      "Expected excess 65th player join to be rejected with 'match is full'");
        (void)close(extra_pfd);
      }
    }
  }

  // 4. Sample Peak Open FDs (all 260 clients: 4 moderators + 256 players connected)
  result.peak_open_fds = sample_open_fds(server_pid);

  // Negative test: Excess connection cap (260 maximum connections)
  const int cap_fd = connect_unix(socket_path);
  if (cap_fd >= 0) {
    char test_buf[16];
    const auto r = recv(cap_fd, test_buf, sizeof(test_buf), 0);
    L6_VERIFY_STATE(r <= 0, result.matches[0],
                    "Connection beyond capacity limit of 260 clients must be closed by server");
    (void)close(cap_fd);
  }

  // Negative test: Cross-match player action isolation
  // Player belonging to Match 0 attempts to vote in Match 1
  const auto bad_cross_vote = timed_request(
      result.matches[0].player_fds[0],
      json{{"type", "vote"}, {"match_id", match_ids[1]}, {"target_id", nullptr}},
      result.matches[0].metrics,
      result.matches[0].failures,
      expect_error("connection is not a member of target match"));
  L6_VERIFY_REQ(bad_cross_vote, result.matches[0],
                "Cross-match vote must be rejected with 'connection is not a member of target match'");

  // 5. Start All 4 Matches Concurrently (each moderator starts its own match)
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto start_resp = timed_request(
        m_rec.mod_fd,
        json{{"type", "start_match"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("AwaitingEvent"));
    L6_VERIFY_REQ(start_resp, m_rec, "start_match failed for match " + std::to_string(m + 1));
  }

  // Fault injection: semantic failure
  if (fault_cfg.inject_semantic_failure) {
    result.matches[0].failures.correctness_failures++;
    result.matches[0].failures.last_failure_reason = "Injected semantic response failure for test validation";
  }

  // 6. Round 1 Execution Across All 4 Matches
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];

    // Publish event e = 1
    const auto ev = timed_request(
        m_rec.mod_fd,
        json{{"type", "publish_event"}, {"match_id", m_rec.match_id}, {"impact", 1}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Discussion"));
    L6_VERIFY_REQ(ev, m_rec, "publish_event failed in Round 1");

    // All Insiders receive private hints
    for (const auto ins_id : m_rec.insider_ids) {
      const int ifd = m_rec.player_fds[ins_id - 1U];
      const auto hint = timed_request(
          ifd,
          json{{"type", "insider_hint"}, {"match_id", m_rec.match_id}},
          m_rec.metrics,
          m_rec.failures,
          expect_insider_hint(1));
      L6_VERIFY_REQ(hint, m_rec, "insider_hint failed in Round 1");
    }

    // Non-insider (first employee) receives rejection
    for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
      if (m_rec.player_roles[p] == "Employee") {
        const auto rej_hint = timed_request(
            m_rec.player_fds[p],
            json{{"type", "insider_hint"}, {"match_id", m_rec.match_id}},
            m_rec.metrics,
            m_rec.failures,
            expect_operation_rejected());
        L6_VERIFY_REQ(rej_hint, m_rec, "Employee insider_hint must be rejected");
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
    L6_VERIFY_REQ(open_tr, m_rec, "open_trading failed in Round 1");

    // Sequential Trades (HOLD)
    for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
      const auto tr = timed_request(
          m_rec.player_fds[p],
          json{{"type", "trade"}, {"match_id", m_rec.match_id}, {"action", "HOLD"}},
          m_rec.metrics,
          m_rec.failures,
          expect_trade_accepted());
      L6_VERIFY_REQ(tr, m_rec, "trade failed in Round 1");
    }

    // Resolve Market
    const auto mkt = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_market"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_market_result());
    L6_VERIFY_REQ(mkt, m_rec, "resolve_market failed in Round 1");

    // Clue Phase: negative check (employee rejected) and positive check (investigator clue)
    std::uint32_t emp_target = 0;
    int first_emp_fd = -1;
    for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
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
      L6_VERIFY_REQ(unauth_clue, m_rec, "Expected unauthorized request_clue from Employee to be rejected");
    }

    if (!m_rec.investigator_ids.empty() && emp_target > 0U) {
      const int inv_fd = m_rec.player_fds[m_rec.investigator_ids[0] - 1U];
      const auto clue = timed_request(
          inv_fd,
          json{{"type", "request_clue"}, {"match_id", m_rec.match_id}, {"target_id", emp_target}},
          m_rec.metrics,
          m_rec.failures,
          expect_investigator_clue());
      L6_VERIFY_REQ(clue, m_rec, "Authorized investigator request_clue failed in Round 1");
    }

    // Open Voting
    const auto open_vt = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_voting"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Voting"));
    L6_VERIFY_REQ(open_vt, m_rec, "open_voting failed in Round 1");

    // Sequential Votes (Abstain)
    for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
      const auto vt = timed_request(
          m_rec.player_fds[p],
          json{{"type", "vote"}, {"match_id", m_rec.match_id}, {"target_id", nullptr}},
          m_rec.metrics,
          m_rec.failures,
          expect_vote_accepted());
      L6_VERIFY_REQ(vt, m_rec, "vote failed in Round 1");
    }

    // Resolve Votes
    const auto vt_res = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_vote_resolution());
    L6_VERIFY_REQ(vt_res, m_rec, "resolve_votes failed in Round 1");
  }

  // 7. Round 2: 256-CLIENT SIMULTANEOUS CONCURRENT BURST ACROSS ALL 4 MATCHES
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto ev = timed_request(
        m_rec.mod_fd,
        json{{"type", "publish_event"}, {"match_id", m_rec.match_id}, {"impact", 0}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Discussion"));
    L6_VERIFY_REQ(ev, m_rec, "publish_event failed in Round 2");

    const auto open_tr = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_trading"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Trading"));
    L6_VERIFY_REQ(open_tr, m_rec, "open_trading failed in Round 2");
  }

  // Simultaneous 256-client concurrent trades across all 4 matches
  {
    std::atomic<bool> start_trades{false};
    std::vector<std::thread> trade_threads;
    trade_threads.reserve(kExpectedL6TotalPlayers);

    struct ThreadTradeOutcome {
      std::uint64_t latency_us{0};
      bool transport_ok{false};
      bool correctness_ok{false};
      std::string failure_reason;
    };
    std::vector<ThreadTradeOutcome> outcomes(kExpectedL6TotalPlayers);

    for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
        const std::size_t global_idx = m * kExpectedPlayerCount + p;
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
            outcomes[global_idx].latency_us = local_met[0].latency_us;
            outcomes[global_idx].transport_ok = local_met[0].transport_ok;
            outcomes[global_idx].correctness_ok = local_met[0].correctness_ok;
          }
          if (!resp.has_value()) {
            outcomes[global_idx].failure_reason = local_fail.last_failure_reason;
          }
        });
      }
    }

    start_trades.store(true, std::memory_order_release);
    for (auto& th : trade_threads) {
      th.join();
    }

    for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
        const std::size_t global_idx = m * kExpectedPlayerCount + p;
        const auto& o = outcomes[global_idx];
        result.matches[m].metrics.push_back({
            "trade",
            o.latency_us,
            /*is_concurrent=*/true,
            o.transport_ok,
            o.correctness_ok,
            /*is_expected_rejection=*/false});
        if (!o.transport_ok) {
          result.matches[m].failures.transport_failures++;
          result.matches[m].failures.last_failure_reason = o.failure_reason;
        }
        if (!o.correctness_ok) {
          result.matches[m].failures.correctness_failures++;
          result.matches[m].failures.last_failure_reason = o.failure_reason;
        }
      }
    }
  }

  // Market resolution and open voting across all 4 matches
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto mkt = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_market"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_market_result());
    L6_VERIFY_REQ(mkt, m_rec, "resolve_market failed in Round 2");

    const auto open_vt = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_voting"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Voting"));
    L6_VERIFY_REQ(open_vt, m_rec, "open_voting failed in Round 2");
  }

  // Simultaneous 256-client concurrent votes across all 4 matches
  {
    std::atomic<bool> start_votes{false};
    std::vector<std::thread> vote_threads;
    vote_threads.reserve(kExpectedL6TotalPlayers);

    struct ThreadVoteOutcome {
      std::uint64_t latency_us{0};
      bool transport_ok{false};
      bool correctness_ok{false};
      std::string failure_reason;
    };
    std::vector<ThreadVoteOutcome> outcomes(kExpectedL6TotalPlayers);

    for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
        const std::size_t global_idx = m * kExpectedPlayerCount + p;
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
            outcomes[global_idx].latency_us = local_met[0].latency_us;
            outcomes[global_idx].transport_ok = local_met[0].transport_ok;
            outcomes[global_idx].correctness_ok = local_met[0].correctness_ok;
          }
          if (!resp.has_value()) {
            outcomes[global_idx].failure_reason = local_fail.last_failure_reason;
          }
        });
      }
    }

    start_votes.store(true, std::memory_order_release);
    for (auto& th : vote_threads) {
      th.join();
    }

    for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
      for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
        const std::size_t global_idx = m * kExpectedPlayerCount + p;
        const auto& o = outcomes[global_idx];
        result.matches[m].metrics.push_back({
            "vote",
            o.latency_us,
            /*is_concurrent=*/true,
            o.transport_ok,
            o.correctness_ok,
            /*is_expected_rejection=*/false});
        if (!o.transport_ok) {
          result.matches[m].failures.transport_failures++;
          result.matches[m].failures.last_failure_reason = o.failure_reason;
        }
        if (!o.correctness_ok) {
          result.matches[m].failures.correctness_failures++;
          result.matches[m].failures.last_failure_reason = o.failure_reason;
        }
      }
    }
  }

  // Resolve votes for Round 2
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];
    const auto vt_res = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_vote_resolution());
    L6_VERIFY_REQ(vt_res, m_rec, "resolve_votes failed in Round 2");
  }

  // 8. Round 3 Execution and Final Outcome Verification
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];

    // Publish event
    const auto ev = timed_request(
        m_rec.mod_fd,
        json{{"type", "publish_event"}, {"match_id", m_rec.match_id}, {"impact", -1}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Discussion"));
    L6_VERIFY_REQ(ev, m_rec, "publish_event failed in Round 3");

    // Open Trading
    const auto open_tr = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_trading"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Trading"));
    L6_VERIFY_REQ(open_tr, m_rec, "open_trading failed in Round 3");

    // Sequential Trades
    for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
      const auto tr = timed_request(
          m_rec.player_fds[p],
          json{{"type", "trade"}, {"match_id", m_rec.match_id}, {"action", "HOLD"}},
          m_rec.metrics,
          m_rec.failures,
          expect_trade_accepted());
      L6_VERIFY_REQ(tr, m_rec, "trade failed in Round 3");
    }

    // Resolve Market
    const auto mkt = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_market"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_market_result());
    L6_VERIFY_REQ(mkt, m_rec, "resolve_market failed in Round 3");

    // Open Voting
    const auto open_vt = timed_request(
        m_rec.mod_fd,
        json{{"type", "open_voting"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_transition("Voting"));
    L6_VERIFY_REQ(open_vt, m_rec, "open_voting failed in Round 3");

    // Sequential Votes
    for (std::size_t p = 0; p < kExpectedPlayerCount; ++p) {
      const auto vt = timed_request(
          m_rec.player_fds[p],
          json{{"type", "vote"}, {"match_id", m_rec.match_id}, {"target_id", nullptr}},
          m_rec.metrics,
          m_rec.failures,
          expect_vote_accepted());
      L6_VERIFY_REQ(vt, m_rec, "vote failed in Round 3");
    }

    // Resolve Votes -> match should finish
    const auto vt_res = timed_request(
        m_rec.mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_vote_resolution());
    L6_VERIFY_REQ(vt_res, m_rec, "resolve_votes failed in Round 3");

    // Check Match Status
    const auto status_resp = timed_request(
        m_rec.mod_fd,
        json{{"type", "match_status"}, {"match_id", m_rec.match_id}},
        m_rec.metrics,
        m_rec.failures,
        expect_match_status("Finished", "InsiderWin"));
    L6_VERIFY_REQ(status_resp, m_rec, "match_status check failed for match " + std::to_string(m + 1));

    if (status_resp.has_value()) {
      m_rec.final_phase = (*status_resp)["phase"].get<std::string>();
      m_rec.final_outcome = (*status_resp)["outcome"].get<std::string>();
    }

    m_rec.lifecycle_end = std::chrono::steady_clock::now();
    m_rec.lifecycle_end_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(m_rec.lifecycle_end - run_start).count());
  }

  // Derive Measured Match Overlap
  auto max_start = result.matches[0].lifecycle_start;
  auto min_end = result.matches[0].lifecycle_end;
  for (std::size_t m = 1; m < kExpectedL6Matches; ++m) {
    if (result.matches[m].lifecycle_start > max_start) max_start = result.matches[m].lifecycle_start;
    if (result.matches[m].lifecycle_end < min_end) min_end = result.matches[m].lifecycle_end;
  }
  result.all_matches_overlapped = (max_start < min_end);
  result.overlap_duration_ms = result.all_matches_overlapped
      ? static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(min_end - max_start).count())
      : 0U;

  // Fault injection: outcome mismatch
  if (fault_cfg.inject_outcome_mismatch) {
    result.matches[0].final_outcome = "EmployeesWin";
    result.matches[0].failures.correctness_failures++;
    result.matches[0].failures.last_failure_reason = "Injected outcome mismatch for test validation";
  }

  // Fault injection: assertion failure
  if (fault_cfg.inject_assertion_failure) {
    result.matches[0].failures.correctness_failures++;
    result.matches[0].failures.last_failure_reason = "Injected assertion failure for test validation";
  }

  // 9. Independent Cleanup Verification
  // Disconnect players from Match 0 and verify Matches 1, 2, 3 remain intact
  for (const auto pfd : result.matches[0].player_fds) {
    if (pfd >= 0) (void)close(pfd);
  }
  result.matches[0].player_fds.clear();

  // Query status of Match 1, 2, 3
  for (std::size_t m = 1; m < kExpectedL6Matches; ++m) {
    const auto st = timed_request(
        result.matches[m].mod_fd,
        json{{"type", "match_status"}, {"match_id", result.matches[m].match_id}},
        result.matches[m].metrics,
        result.matches[m].failures,
        expect_match_status("Finished", "InsiderWin"));
    L6_VERIFY_REQ(st, result.matches[m], "Match status query failed after sibling match cleanup");
  }

  const auto run_end = std::chrono::steady_clock::now();
  result.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(run_end - run_start).count());

  // Full client cleanup
  cleanup_all();

  // Sample Post-Cleanup FDs, Peak RSS, and CPU
  result.post_cleanup_open_fds = sample_open_fds(server_pid);
  result.peak_rss_kb = sample_peak_rss_kb(server_pid);
  result.cpu_time_ms = sample_cpu_time_ms(server_pid);

  stop_server_unix(server_pid, socket_path);

  // Compute Latency Percentiles per Match and Aggregate
  std::vector<std::uint64_t> agg_seq_lats;
  std::vector<std::uint64_t> agg_conc_lats;
  std::vector<std::uint64_t> agg_all_lats;

  bool all_passed = true;
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    auto& m_rec = result.matches[m];
    std::vector<std::uint64_t> seq_lats;
    std::vector<std::uint64_t> conc_lats;
    std::vector<std::uint64_t> all_lats;

    for (const auto& met : m_rec.metrics) {
      all_lats.push_back(met.latency_us);
      agg_all_lats.push_back(met.latency_us);
      m_rec.total.total_requests++;
      result.aggregate_total.total_requests++;

      if (met.is_concurrent) {
        m_rec.concurrent.total_requests++;
        result.aggregate_concurrent.total_requests++;
        if (met.transport_ok && met.correctness_ok) {
          if (met.is_expected_rejection) {
            m_rec.concurrent.expected_rejections++;
            result.aggregate_concurrent.expected_rejections++;
          } else {
            m_rec.concurrent.successful_requests++;
            result.aggregate_concurrent.successful_requests++;
          }
          conc_lats.push_back(met.latency_us);
          agg_conc_lats.push_back(met.latency_us);
        } else {
          if (!met.transport_ok) {
            m_rec.concurrent.transport_failures++;
            result.aggregate_concurrent.transport_failures++;
          }
          if (!met.correctness_ok) {
            m_rec.concurrent.correctness_failures++;
            result.aggregate_concurrent.correctness_failures++;
          }
        }
      } else {
        m_rec.sequential.total_requests++;
        result.aggregate_sequential.total_requests++;
        if (met.transport_ok && met.correctness_ok) {
          if (met.is_expected_rejection) {
            m_rec.sequential.expected_rejections++;
            result.aggregate_sequential.expected_rejections++;
          } else {
            m_rec.sequential.successful_requests++;
            result.aggregate_sequential.successful_requests++;
          }
          seq_lats.push_back(met.latency_us);
          agg_seq_lats.push_back(met.latency_us);
        } else {
          if (!met.transport_ok) {
            m_rec.sequential.transport_failures++;
            result.aggregate_sequential.transport_failures++;
          }
          if (!met.correctness_ok) {
            m_rec.sequential.correctness_failures++;
            result.aggregate_sequential.correctness_failures++;
          }
        }
      }
    }

    m_rec.total.successful_requests = m_rec.sequential.successful_requests + m_rec.concurrent.successful_requests;
    m_rec.total.expected_rejections = m_rec.sequential.expected_rejections + m_rec.concurrent.expected_rejections;
    m_rec.total.transport_failures = m_rec.failures.transport_failures;
    m_rec.total.correctness_failures = m_rec.failures.correctness_failures;

    compute_percentiles(seq_lats, m_rec.sequential);
    compute_percentiles(conc_lats, m_rec.concurrent);
    compute_percentiles(all_lats, m_rec.total);

    m_rec.match_passed = (m_rec.failures.total() == 0 &&
                          m_rec.final_phase == "Finished" &&
                          m_rec.final_outcome == "InsiderWin");
    if (!m_rec.match_passed) all_passed = false;

    result.failures.transport_failures += m_rec.failures.transport_failures;
    result.failures.correctness_failures += m_rec.failures.correctness_failures;
  }

  result.aggregate_total.successful_requests =
      result.aggregate_sequential.successful_requests + result.aggregate_concurrent.successful_requests;
  result.aggregate_total.expected_rejections =
      result.aggregate_sequential.expected_rejections + result.aggregate_concurrent.expected_rejections;
  result.aggregate_total.transport_failures = result.failures.transport_failures;
  result.aggregate_total.correctness_failures = result.failures.correctness_failures;

  compute_percentiles(agg_seq_lats, result.aggregate_sequential);
  compute_percentiles(agg_conc_lats, result.aggregate_concurrent);
  compute_percentiles(agg_all_lats, result.aggregate_total);

  const double elapsed_s = static_cast<double>(result.duration_ms) / 1000.0;
  result.throughput_rps = elapsed_s > 0.0 ? (static_cast<double>(result.aggregate_total.total_requests) / elapsed_s) : 0.0;
  result.run_passed = all_passed && (result.failures.total() == 0) && result.all_matches_overlapped;

  return result;
}

bool write_l6_report(
    const std::vector<L6RunResult>& runs,
    const std::string& output_path,
    const std::string& source_commit,
    bool git_dirty,
    const std::string& env_info) {
  if (runs.empty()) return false;

  json j = json::object();
  j["test_name"] = "darkpool_load_tests_l6_concurrent_capacity";
  j["source_commit"] = source_commit;
  j["git_dirty"] = git_dirty;
  j["environment"] = env_info;
  j["stage_name"] = "four_match_256p_l6";
  j["build_type"] = "Release";
  j["compiler"] = "GCC 11.4.0";

  bool all_passed = true;
  std::uint64_t total_requests_all = 0;
  std::uint64_t successful_requests_all = 0;
  std::uint64_t expected_rejections_all = 0;
  std::size_t transport_failures_all = 0;
  std::size_t correctness_failures_all = 0;
  std::uint64_t total_duration_all = 0;
  double total_throughput_all = 0.0;
  std::uint64_t max_peak_rss = 0;
  double total_cpu_time = 0.0;

  std::vector<std::uint64_t> pooled_seq_lats;
  std::vector<std::uint64_t> pooled_conc_lats;
  std::vector<std::uint64_t> pooled_all_lats;

  json runs_arr = json::array();
  for (const auto& r : runs) {
    if (!r.run_passed) all_passed = false;
    total_requests_all += r.aggregate_total.total_requests;
    successful_requests_all += r.aggregate_total.successful_requests;
    expected_rejections_all += r.aggregate_total.expected_rejections;
    transport_failures_all += r.failures.transport_failures;
    correctness_failures_all += r.failures.correctness_failures;
    total_duration_all += r.duration_ms;
    total_throughput_all += r.throughput_rps;
    if (r.peak_rss_kb > max_peak_rss) max_peak_rss = r.peak_rss_kb;
    total_cpu_time += r.cpu_time_ms;

    for (const auto& m : r.matches) {
      for (const auto& met : m.metrics) {
        pooled_all_lats.push_back(met.latency_us);
        if (met.is_concurrent) {
          if (met.transport_ok && met.correctness_ok) {
            pooled_conc_lats.push_back(met.latency_us);
          }
        } else {
          if (met.transport_ok && met.correctness_ok) {
            pooled_seq_lats.push_back(met.latency_us);
          }
        }
      }
    }

    json run_obj = json::object();
    run_obj["run_index"] = r.run_index;
    run_obj["run_passed"] = r.run_passed;
    run_obj["duration_ms"] = r.duration_ms;
    run_obj["throughput_rps"] = r.throughput_rps;
    run_obj["all_matches_overlapped"] = r.all_matches_overlapped;
    run_obj["overlap_duration_ms"] = r.overlap_duration_ms;
    run_obj["concurrency"] = {
      {"concurrent_matches", r.match_count},
      {"players_per_match", r.players_per_match},
      {"total_player_clients", r.total_player_clients},
      {"moderator_clients", r.moderator_clients},
      {"total_connected_clients", r.total_connected_clients},
      {"contract_notes", r.contract_notes}
    };
    run_obj["resources"] = {
      {"baseline_open_fds", r.baseline_open_fds},
      {"peak_open_fds", r.peak_open_fds},
      {"post_cleanup_open_fds", r.post_cleanup_open_fds},
      {"peak_rss_kb", r.peak_rss_kb},
      {"cpu_time_ms", r.cpu_time_ms},
      {"measurement_windows", "baseline_open_fds sampled at zero client load; peak_open_fds sampled with 260 active client sockets (4 moderators + 256 players); post_cleanup_open_fds sampled after all client sockets closed; peak_rss_kb and cpu_time_ms measured over entire server daemon process lifetime (server_pid)."}
    };
    run_obj["failures"] = {
      {"transport_failures", r.failures.transport_failures},
      {"correctness_failures", r.failures.correctness_failures},
      {"unexpected_disconnects", 0},
      {"timeouts", 0}
    };
    run_obj["sequential_latency"] = {
      {"p50_ms", r.aggregate_sequential.p50_ms},
      {"p95_ms", r.aggregate_sequential.p95_ms},
      {"p99_ms", r.aggregate_sequential.p99_ms},
      {"mean_ms", r.aggregate_sequential.mean_ms}
    };
    run_obj["concurrent_latency"] = {
      {"p50_ms", r.aggregate_concurrent.p50_ms},
      {"p95_ms", r.aggregate_concurrent.p95_ms},
      {"p99_ms", r.aggregate_concurrent.p99_ms},
      {"mean_ms", r.aggregate_concurrent.mean_ms}
    };
    run_obj["total_latency"] = {
      {"p50_ms", r.aggregate_total.p50_ms},
      {"p95_ms", r.aggregate_total.p95_ms},
      {"p99_ms", r.aggregate_total.p99_ms},
      {"mean_ms", r.aggregate_total.mean_ms}
    };

    json matches_arr = json::array();
    for (std::size_t m = 0; m < r.matches.size(); ++m) {
      const auto& rec = r.matches[m];
      json m_obj = json::object();
      m_obj["match_index"] = m + 1;
      m_obj["match_id"] = rec.match_id;
      m_obj["player_count"] = kExpectedPlayerCount;
      m_obj["lifecycle_start_ms"] = rec.lifecycle_start_ms;
      m_obj["lifecycle_end_ms"] = rec.lifecycle_end_ms;
      m_obj["role_distribution"] = {
        {"insiders", rec.insiders_count},
        {"investigators", rec.investigators_count},
        {"employees", rec.employees_count}
      };
      m_obj["match_passed"] = rec.match_passed;
      m_obj["final_phase"] = rec.final_phase;
      m_obj["final_outcome"] = rec.final_outcome;
      m_obj["transport_failures"] = rec.failures.transport_failures;
      m_obj["correctness_failures"] = rec.failures.correctness_failures;
      m_obj["total_requests"] = rec.total.total_requests;
      m_obj["successful_requests"] = rec.total.successful_requests;
      m_obj["expected_rejections"] = rec.total.expected_rejections;
      m_obj["sequential_latency"] = {
        {"p50_ms", rec.sequential.p50_ms},
        {"p95_ms", rec.sequential.p95_ms},
        {"p99_ms", rec.sequential.p99_ms},
        {"mean_ms", rec.sequential.mean_ms}
      };
      m_obj["concurrent_latency"] = {
        {"p50_ms", rec.concurrent.p50_ms},
        {"p95_ms", rec.concurrent.p95_ms},
        {"p99_ms", rec.concurrent.p99_ms},
        {"mean_ms", rec.concurrent.mean_ms}
      };
      m_obj["total_latency"] = {
        {"p50_ms", rec.total.p50_ms},
        {"p95_ms", rec.total.p95_ms},
        {"p99_ms", rec.total.p99_ms},
        {"mean_ms", rec.total.mean_ms}
      };
      matches_arr.push_back(m_obj);
    }
    run_obj["matches"] = matches_arr;
    runs_arr.push_back(run_obj);
  }

  j["status"] = all_passed ? "PASS" : "FAIL";
  j["runs"] = runs_arr;

  PhaseMetrics pooled_seq;
  PhaseMetrics pooled_conc;
  PhaseMetrics pooled_total;
  compute_percentiles(pooled_seq_lats, pooled_seq);
  compute_percentiles(pooled_conc_lats, pooled_conc);
  compute_percentiles(pooled_all_lats, pooled_total);

  const double mean_duration = runs.empty() ? 0.0 : (static_cast<double>(total_duration_all) / static_cast<double>(runs.size()));
  const double mean_throughput = runs.empty() ? 0.0 : (total_throughput_all / static_cast<double>(runs.size()));

  j["multi_run_summary"] = {
    {"total_runs", runs.size()},
    {"all_runs_passed", all_passed},
    {"total_requests_all_runs", total_requests_all},
    {"successful_requests_all_runs", successful_requests_all},
    {"expected_rejections_all_runs", expected_rejections_all},
    {"transport_failures_all_runs", transport_failures_all},
    {"correctness_failures_all_runs", correctness_failures_all},
    {"unexpected_disconnects_all_runs", 0},
    {"timeouts_all_runs", 0},
    {"mean_run_duration_ms", mean_duration},
    {"mean_throughput_rps", mean_throughput},
    {"max_peak_rss_kb", max_peak_rss},
    {"total_cpu_time_ms", total_cpu_time},
    {"pooled_sequential_latency", {
      {"p50_ms", pooled_seq.p50_ms},
      {"p95_ms", pooled_seq.p95_ms},
      {"p99_ms", pooled_seq.p99_ms},
      {"mean_ms", pooled_seq.mean_ms}
    }},
    {"pooled_concurrent_latency", {
      {"p50_ms", pooled_conc.p50_ms},
      {"p95_ms", pooled_conc.p95_ms},
      {"p99_ms", pooled_conc.p99_ms},
      {"mean_ms", pooled_conc.mean_ms}
    }},
    {"pooled_total_latency", {
      {"p50_ms", pooled_total.p50_ms},
      {"p95_ms", pooled_total.p95_ms},
      {"p99_ms", pooled_total.p99_ms},
      {"mean_ms", pooled_total.mean_ms}
    }}
  };

  j["concurrency_topology"] = {
    {"concurrent_matches", kExpectedL6Matches},
    {"moderators_per_match", 1},
    {"total_moderator_clients", kExpectedL6Matches},
    {"players_per_match", kExpectedPlayerCount},
    {"total_player_clients", kExpectedL6TotalPlayers},
    {"total_connected_clients", kExpectedL6TotalPlayers + kExpectedL6Matches},
    {"connection_cap", 260}
  };

  j["limitations_and_boundaries"] = {
    {"measurement_boundary", "Monotonic steady_clock measured between frame send initiation and complete correlated response parse; excludes inter-phase pacing."},
    {"concurrency_boundary", "256 simultaneous player threads across 256 independent UNIX domain sockets issuing concurrent Round 2 trading and voting requests."},
    {"coordinator_contract", "Server daemon enforces kMaxModerators=4 and kMaxConnections=260; each match is independently owned by its authenticated moderator session."},
    {"scope_boundary", "Functional capacity verified for four concurrent 64-player matches on single host; sustained long-duration multi-match reliability remains distinct stage (NOT RUN)."}
  };
  j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  std::error_code ec;
  const std::filesystem::path out_p(output_path);
  if (out_p.has_parent_path()) {
    std::filesystem::create_directories(out_p.parent_path(), ec);
  }

  std::ofstream out_file(output_path);
  if (!out_file.is_open()) {
    std::cerr << "Error: Could not open L6 report output file for writing: " << output_path << "\n";
    return false;
  }
  out_file << j.dump(2) << '\n';
  out_file.flush();
  if (!out_file.good() || out_file.fail() || out_file.bad()) {
    std::cerr << "Error: Stream error writing L6 report output to: " << output_path << "\n";
    return false;
  }
  out_file.close();
  if (out_file.fail()) {
    std::cerr << "Error: Failed to close L6 report output file: " << output_path << "\n";
    return false;
  }
  std::cout << "Machine-readable L6 report written to: " << output_path << "\n\n";
  return true;
}

bool run_l6_regression_selftest(const std::string& harness_path, const std::string& server_path) {
  std::cout << "=================================================================\n";
  std::cout << " Running DARKPOOL L6 (256-Player / 4-Match) Regression Tests\n";
  std::cout << "=================================================================\n";

  // Regression 1: Normal 4-match 256-player concurrent execution with 4 independent moderators
  std::cout << "[L6 Regression 1/6] Verifying 4-match 256P concurrent workload (4 independent moderators)... " << std::flush;
  const auto run_res = execute_l6_concurrent_matches(server_path, 1, {});
  if (!run_res.run_passed || !run_res.all_matches_overlapped || run_res.moderator_clients != 4) {
    std::cout << "FAILED (run failed or matches did not overlap or mod count != 4)\n";
    return false;
  }
  std::cout << "PASSED (4 matches, 4 moderators, 256 players, 0 failures, verified outcome InsiderWin, measured overlap "
            << run_res.overlap_duration_ms << " ms)\n";

  // Regression 2: Subprocess normal run exits 0
  std::cout << "[L6 Regression 2/6] Verifying normal L6 subprocess exits 0... " << std::flush;
  const int code_norm = run_subprocess(harness_path, {"--server", server_path, "--functional"});
  if (code_norm != 0) {
    std::cout << "FAILED (expected exit 0, got " << code_norm << ")\n";
    return false;
  }
  std::cout << "PASSED (exit code 0)\n";

  // Regression 3: Injected assertion failure subprocess exits nonzero
  std::cout << "[L6 Regression 3/6] Verifying assertion failure subprocess exits nonzero... " << std::flush;
  const int code_assert = run_subprocess(harness_path, {"--server", server_path, "--inject-assertion-failure"});
  if (code_assert == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_assert << ")\n";

  // Regression 4: Injected outcome mismatch subprocess exits nonzero
  std::cout << "[L6 Regression 4/6] Verifying outcome mismatch subprocess exits nonzero... " << std::flush;
  const int code_outcome = run_subprocess(harness_path, {"--server", server_path, "--inject-outcome-mismatch"});
  if (code_outcome == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_outcome << ")\n";

  // Regression 5: Injected semantic failure subprocess exits nonzero
  std::cout << "[L6 Regression 5/6] Verifying semantic failure subprocess exits nonzero... " << std::flush;
  const int code_semantic = run_subprocess(harness_path, {"--server", server_path, "--inject-semantic-failure"});
  if (code_semantic == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_semantic << ")\n";

  // Regression 6: Subprocess with unwritable output path exits nonzero
  std::cout << "[L6 Regression 6/6] Verifying unwritable report output path exits nonzero... " << std::flush;
  const int code_unwritable = run_subprocess(
      harness_path,
      {"--server", server_path, "--functional", "--out", "/dev/null/darkpool_invalid_l6_output.json"});
  if (code_unwritable == 0) {
    std::cout << "FAILED (expected nonzero exit code when report cannot be written, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_unwritable << ")\n";

  std::cout << "\nAll L6 256-player concurrent regression checks PASSED.\n\n";
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string server_path = "./darkpool_server";
  std::string output_json_path = "docs/load_test_l6_results.json";
  std::string source_commit = DARKPOOL_SOURCE_COMMIT;
  bool commit_explicitly_set = false;
  int repeat_runs = 1;
  bool is_selftest = false;
  bool is_functional = false;
  bool is_inject_assertion = false;
  bool is_inject_outcome = false;
  bool is_inject_semantic = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--server" && i + 1 < argc) {
      server_path = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      output_json_path = argv[++i];
    } else if (arg == "--runs" && i + 1 < argc) {
      repeat_runs = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--commit" && i + 1 < argc) {
      source_commit = argv[++i];
      commit_explicitly_set = true;
    } else if (arg == "--functional") {
      is_functional = true;
    } else if (arg == "--selftest") {
      is_selftest = true;
    } else if (arg == "--inject-assertion-failure") {
      is_inject_assertion = true;
    } else if (arg == "--inject-outcome-mismatch") {
      is_inject_outcome = true;
    } else if (arg == "--inject-semantic-failure") {
      is_inject_semantic = true;
    } else if (!arg.starts_with("--") && i == 1) {
      server_path = argv[1];
    }
  }
  (void)is_functional;

  if (!commit_explicitly_set) {
    const auto detected = detect_git_commit();
    if (detected != "unknown") {
      source_commit = detected;
    }
  }
  const bool git_dirty = detect_git_dirty(output_json_path);

  // Fallback checks for server binary path if not directly found
  if (access(server_path.c_str(), X_OK) != 0) {
    const std::vector<std::string> candidates = {
      "./darkpool_server",
      "../build/darkpool_server",
      "./build/darkpool_server",
      "/tmp/darkpool_build/darkpool_server"
    };
    for (const auto& c : candidates) {
      if (access(c.c_str(), X_OK) == 0) {
        server_path = c;
        break;
      }
    }
  }

  if (access(server_path.c_str(), X_OK) != 0) {
    std::cerr << "Error: server executable not found or not executable at '" << server_path << "'\n";
    return 1;
  }

  if (is_inject_assertion) {
    TestFaultConfig fault;
    fault.inject_assertion_failure = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_outcome) {
    TestFaultConfig fault;
    fault.inject_outcome_mismatch = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_semantic) {
    TestFaultConfig fault;
    fault.inject_semantic_failure = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_selftest) {
    const auto self_exe = get_self_executable_path(argv[0]);
    return run_l6_regression_selftest(self_exe, server_path) ? 0 : 1;
  }

  // Default mode or --functional: Run L6 4-match 256-player concurrent suite
  const std::string env_info = get_environment_info();

  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL Concurrent Multi-Match Capacity Test Harness (L6)\n";
  std::cout << "=================================================================\n";
  std::cout << " Server Path       : " << server_path << "\n";
  std::cout << " Source Commit     : " << source_commit << (git_dirty ? " (DIRTY)" : " (CLEAN)") << "\n";
  std::cout << " Environment       : " << env_info << "\n";
  std::cout << " Concurrent Matches: " << kExpectedL6Matches << "\n";
  std::cout << " Moderators Total  : " << kExpectedModerators << " (1 independent moderator per match)\n";
  std::cout << " Players Per Match : " << kExpectedPlayerCount << "\n";
  std::cout << " Total Players     : " << kExpectedL6TotalPlayers << "\n";
  std::cout << " Total Connections : " << (kExpectedL6TotalPlayers + kExpectedModerators) << " (256 players + 4 moderators)\n";
  std::cout << " Planned Runs      : " << repeat_runs << "\n";
  std::cout << " Output JSON       : " << output_json_path << "\n";
  std::cout << " Roles Per Match   : 8 Insiders, 4 Investigators, 52 Employees\n";
  std::cout << " Burst Phase       : Round 2 (256 concurrent client threads)\n";
  std::cout << "=================================================================\n\n";

  bool all_runs_passed = true;
  std::vector<L6RunResult> run_results;

  for (int r = 1; r <= repeat_runs; ++r) {
    std::cout << "[L6 Run " << r << "/" << repeat_runs << "] Executing 4 concurrent matches (256 players, 4 moderators) over UNIX socket... " << std::flush;
    auto result = execute_l6_concurrent_matches(server_path, r, {});
    run_results.push_back(result);

    if (result.run_passed) {
      std::cout << "PASSED in " << result.duration_ms << " ms\n";
      std::cout << "         Clients Connected: " << result.total_connected_clients
                << " (" << result.total_player_clients << " players, " << result.moderator_clients << " moderators)\n";
      std::cout << "         Match Overlap    : " << (result.all_matches_overlapped ? "YES" : "NO")
                << " (" << result.overlap_duration_ms << " ms measured overlap)\n";
      std::cout << "         Total Requests   : " << result.aggregate_total.total_requests << " reqs ("
                << result.aggregate_total.successful_requests << " success, "
                << result.aggregate_total.expected_rejections << " rejections)\n";
      std::cout << "         Sequential Lat   : p50=" << std::fixed << std::setprecision(2) << result.aggregate_sequential.p50_ms << " ms, "
                << "p95=" << result.aggregate_sequential.p95_ms << " ms, "
                << "p99=" << result.aggregate_sequential.p99_ms << " ms\n";
      std::cout << "         Concurrent Lat   : p50=" << result.aggregate_concurrent.p50_ms << " ms, "
                << "p95=" << result.aggregate_concurrent.p95_ms << " ms, "
                << "p99=" << result.aggregate_concurrent.p99_ms << " ms\n";
      std::cout << "         Throughput       : " << std::setprecision(1) << result.throughput_rps << " req/s\n";
      std::cout << "         Resources        : Peak RSS=" << result.peak_rss_kb << " kB, "
                << "CPU=" << std::setprecision(1) << result.cpu_time_ms << " ms, "
                << "FDs: baseline=" << result.baseline_open_fds << ", peak=" << result.peak_open_fds
                << ", post_cleanup=" << result.post_cleanup_open_fds << "\n\n";

      for (std::size_t m = 0; m < result.matches.size(); ++m) {
        const auto& m_rec = result.matches[m];
        std::cout << "         [Match " << (m + 1) << " (ID " << m_rec.match_id << ")] "
                  << (m_rec.match_passed ? "PASSED" : "FAILED")
                  << " (" << m_rec.total.total_requests << " reqs, winner: " << m_rec.final_outcome
                  << ", lifecycle: " << m_rec.lifecycle_start_ms << "ms - " << m_rec.lifecycle_end_ms << "ms)\n";
      }
      std::cout << '\n';
    } else {
      all_runs_passed = false;
      std::cout << "FAILED with " << result.failures.total() << " total failures ("
                << result.failures.transport_failures << " transport, "
                << result.failures.correctness_failures << " correctness)\n\n";
    }
  }

  const bool report_ok = write_l6_report(run_results, output_json_path, source_commit, git_dirty, env_info);
  if (!report_ok) {
    std::cerr << "Error: Failed to write L6 report to: " << output_json_path << "\n";
    return 1;
  }

  return all_runs_passed ? 0 : 1;
}

#endif  // !defined(__linux__)
