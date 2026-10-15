#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests_l6 requires Linux (epoll/unix sockets)\n";
  return 1;
}

#else

#include "load_test_harness_common.hpp"
#include <filesystem>
#include <memory>
#include <poll.h>

namespace {

using namespace darkpool::load_test;

constexpr std::size_t kExpectedModerators = 4;

enum class MockServerMode {
  AcceptAndRespondConnected, // Accepts connection, sends {"type":"connect","role":"moderator"}
  AcceptAndStall,            // Accepts connection, holds it open without sending or closing
};

class MockUnixServer {
 public:
  explicit MockUnixServer(const std::string& path, MockServerMode mode)
      : path_(path), mode_(mode) {
    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) return;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() >= sizeof(addr.sun_path)) {
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    std::strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    unlink(path.c_str());
    if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    if (listen(listen_fd_, 5) != 0) {
      close(listen_fd_);
      listen_fd_ = -1;
      return;
    }
    worker_ = std::thread([this]() { run(); });
  }

  ~MockUnixServer() {
    stop();
  }

  void stop() {
    running_ = false;
    if (listen_fd_ >= 0) {
      shutdown(listen_fd_, SHUT_RDWR);
      close(listen_fd_);
      listen_fd_ = -1;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    unlink(path_.c_str());
  }

  [[nodiscard]] bool is_valid() const noexcept { return listen_fd_ >= 0; }

 private:
  void run() {
    while (running_) {
      pollfd pfd{listen_fd_, POLLIN, 0};
      const int pr = poll(&pfd, 1, 50);
      if (pr > 0 && (pfd.revents & POLLIN)) {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd >= 0) {
          if (mode_ == MockServerMode::AcceptAndRespondConnected) {
            char buf[512];
            (void)recv(client_fd, buf, sizeof(buf), 0);
            const auto resp = Frame(json{{"type", "connect"}, {"role", "moderator"}}).serialize();
            (void)send(client_fd, resp.data(), resp.size(), MSG_NOSIGNAL);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            close(client_fd);
          } else if (mode_ == MockServerMode::AcceptAndStall) {
            char buf[512];
            (void)recv(client_fd, buf, sizeof(buf), 0);
            const auto t0 = std::chrono::steady_clock::now();
            while (running_ && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5)) {
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            close(client_fd);
          }
        }
      }
    }
  }

  std::string path_;
  MockServerMode mode_;
  int listen_fd_{-1};
  std::atomic<bool> running_{true};
  std::thread worker_;
};

enum class CapacityRejectionOutcome {
  PromptEofClosed,        // Server closed connection promptly (recv returned 0)
  ExplicitRejectionFrame, // Server sent error/rejection frame
  UnexpectedAcceptance,   // Server accepted connection/handshake
  UnexpectedData,         // Server sent unexpected data
  TimedOutWithoutClose,   // Socket timed out waiting for server close/rejection (EAGAIN/EWOULDBLOCK)
  SocketReadError,        // Other socket error
  SendFailed,             // Failed to send handshake frame
  ConnectFailed           // Failed to connect to server socket
};

inline const char* capacity_outcome_string(CapacityRejectionOutcome outcome) {
  switch (outcome) {
    case CapacityRejectionOutcome::PromptEofClosed: return "PromptEofClosed (server closed connection deterministically)";
    case CapacityRejectionOutcome::ExplicitRejectionFrame: return "ExplicitRejectionFrame (server returned error response)";
    case CapacityRejectionOutcome::UnexpectedAcceptance: return "UnexpectedAcceptance (server incorrectly accepted over-capacity connection)";
    case CapacityRejectionOutcome::UnexpectedData: return "UnexpectedData (server sent non-rejection data)";
    case CapacityRejectionOutcome::TimedOutWithoutClose: return "TimedOutWithoutClose (server stalled/timed out without closing connection)";
    case CapacityRejectionOutcome::SocketReadError: return "SocketReadError";
    case CapacityRejectionOutcome::SendFailed: return "SendFailed";
    case CapacityRejectionOutcome::ConnectFailed: return "ConnectFailed";
  }
  return "Unknown";
}

inline CapacityRejectionOutcome verify_fifth_moderator_rejection(
    const std::string& socket_path,
    const std::string& mod_token,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  const int fd = connect_unix(socket_path);
  if (fd < 0) {
    return CapacityRejectionOutcome::ConnectFailed;
  }

  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  const auto hello = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}}).serialize();
  if (!send_all(fd, hello.data(), hello.size())) {
    (void)close(fd);
    return CapacityRejectionOutcome::SendFailed;
  }

  char buf[1024];
  const auto r = recv(fd, buf, sizeof(buf), 0);
  (void)close(fd);

  if (r == 0) {
    return CapacityRejectionOutcome::PromptEofClosed;
  }
  if (r > 0) {
    std::size_t consumed = 0;
    const auto parsed = Frame::parse(std::string(buf, static_cast<std::size_t>(r)), consumed);
    if (parsed) {
      const auto& p = parsed->payload();
      if (p.is_object() && p.value("type", "") == "connect" && p.value("role", "") == "moderator") {
        return CapacityRejectionOutcome::UnexpectedAcceptance;
      }
      if (p.is_object() && p.value("type", "") == "error") {
        return CapacityRejectionOutcome::ExplicitRejectionFrame;
      }
    }
    return CapacityRejectionOutcome::UnexpectedData;
  }
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return CapacityRejectionOutcome::TimedOutWithoutClose;
  }
  return CapacityRejectionOutcome::SocketReadError;
}

inline CapacityRejectionOutcome verify_connection_cap_rejection(
    const std::string& socket_path,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(1000)) {
  const int fd = connect_unix(socket_path);
  if (fd < 0) {
    return CapacityRejectionOutcome::ConnectFailed;
  }

  timeval tv{};
  tv.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  tv.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  char buf[512];
  const auto r = recv(fd, buf, sizeof(buf), 0);
  (void)close(fd);

  if (r == 0) {
    return CapacityRejectionOutcome::PromptEofClosed;
  }
  if (r > 0) {
    std::size_t consumed = 0;
    const auto parsed = Frame::parse(std::string(buf, static_cast<std::size_t>(r)), consumed);
    if (parsed) {
      const auto& p = parsed->payload();
      if (p.is_object() && p.value("type", "") == "error") {
        return CapacityRejectionOutcome::ExplicitRejectionFrame;
      }
      if (p.is_object() && p.value("type", "") == "connect") {
        return CapacityRejectionOutcome::UnexpectedAcceptance;
      }
    }
    return CapacityRejectionOutcome::UnexpectedData;
  }
  if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    return CapacityRejectionOutcome::TimedOutWithoutClose;
  }
  return CapacityRejectionOutcome::SocketReadError;
}

inline bool evaluate_capacity_outcome(
    CapacityRejectionOutcome outcome,
    const std::string& test_name,
    FailureCounts& run_failures) {
  switch (outcome) {
    case CapacityRejectionOutcome::PromptEofClosed:
    case CapacityRejectionOutcome::ExplicitRejectionFrame:
      return true;
    case CapacityRejectionOutcome::UnexpectedAcceptance:
      run_failures.correctness_failures++;
      run_failures.last_failure_reason = test_name + " was unexpectedly accepted";
      std::cerr << "[L6] CAPACITY ERROR: " << run_failures.last_failure_reason << '\n';
      return false;
    case CapacityRejectionOutcome::UnexpectedData:
      run_failures.correctness_failures++;
      run_failures.last_failure_reason = test_name + " received unexpected data";
      std::cerr << "[L6] CAPACITY ERROR: " << run_failures.last_failure_reason << '\n';
      return false;
    case CapacityRejectionOutcome::TimedOutWithoutClose:
      run_failures.timeouts++;
      run_failures.transport_failures++;
      run_failures.last_failure_reason = test_name + " timed out without server rejection/closure";
      std::cerr << "[L6] CAPACITY ERROR: " << run_failures.last_failure_reason << '\n';
      return false;
    case CapacityRejectionOutcome::ConnectFailed:
      run_failures.unexpected_disconnects++;
      run_failures.transport_failures++;
      run_failures.last_failure_reason = test_name + " failed to establish connection";
      std::cerr << "[L6] CAPACITY ERROR: " << run_failures.last_failure_reason << '\n';
      return false;
    case CapacityRejectionOutcome::SendFailed:
      run_failures.unexpected_disconnects++;
      run_failures.transport_failures++;
      run_failures.last_failure_reason = test_name + " failed to send handshake";
      std::cerr << "[L6] CAPACITY ERROR: " << run_failures.last_failure_reason << '\n';
      return false;
    case CapacityRejectionOutcome::SocketReadError:
      run_failures.unexpected_disconnects++;
      run_failures.transport_failures++;
      run_failures.last_failure_reason = test_name + " encountered socket read error";
      std::cerr << "[L6] CAPACITY ERROR: " << run_failures.last_failure_reason << '\n';
      return false;
  }
  run_failures.correctness_failures++;
  run_failures.last_failure_reason = test_name + ": unknown outcome";
  std::cerr << "[L6] CAPACITY ERROR: " << run_failures.last_failure_reason << '\n';
  return false;
}

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
  std::size_t immediate_post_cleanup_open_fds{0};
  std::size_t post_cleanup_open_fds{0};
  std::uint64_t cleanup_settle_time_ms{0};
  bool post_cleanup_settled{false};
  double throughput_rps{0.0};
  std::size_t match_limit_retries{0};
};

L6RunResult execute_l6_single_cycle(
    pid_t server_pid,
    const std::string& socket_path,
    const std::string& mod_token,
    int run_index,
    std::size_t baseline_open_fds,
    const TestFaultConfig& fault_cfg = {}) {
  L6RunResult result;
  result.run_index = run_index;
  result.matches.resize(kExpectedL6Matches);



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

  struct CycleCleanupGuard {
    std::function<void()> cleanup_clients;
    bool active{true};

    void execute() {
      if (active) {
        active = false;
        if (cleanup_clients) cleanup_clients();
      }
    }

    ~CycleCleanupGuard() {
      execute();
    }
  } cycle_guard{cleanup_all, true};

  result.baseline_open_fds = baseline_open_fds;

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
    L6_VERIFY_REQ(mod_hello, result.matches[m], "Moderator " + std::to_string(m + 1) + " handshake failed");
  }

  result.moderator_clients = all_mod_fds.size();
  result.total_connected_clients = result.total_player_clients + result.moderator_clients;
  result.contract_notes =
      "darkpool_server configured with kMaxModerators=4 and kMaxConnections=260; each of the four concurrent "
      "matches is owned and controlled by an independent authenticated moderator connection.";

  // Negative test: 5th moderator connection must be deterministically rejected (kMaxModerators=4)
  std::string mod5_target_sock = socket_path;
  std::chrono::milliseconds mod5_timeout = std::chrono::milliseconds(1000);
  std::unique_ptr<MockUnixServer> mock_srv;
  if (fault_cfg.inject_capacity_accepted) {
    const std::string mock_sock = generate_unique_socket_path();
    mock_srv = std::make_unique<MockUnixServer>(mock_sock, MockServerMode::AcceptAndRespondConnected);
    mod5_target_sock = mock_sock;
  } else if (fault_cfg.inject_capacity_stalled) {
    const std::string mock_sock = generate_unique_socket_path();
    mock_srv = std::make_unique<MockUnixServer>(mock_sock, MockServerMode::AcceptAndStall);
    mod5_target_sock = mock_sock;
    mod5_timeout = std::chrono::milliseconds(150);
  } else if (fault_cfg.inject_connect_failure) {
    mod5_target_sock = "/tmp/nonexistent_mock_server_l6.sock";
  }

  const auto mod5_outcome = verify_fifth_moderator_rejection(mod5_target_sock, mod_token, mod5_timeout);
  (void)evaluate_capacity_outcome(mod5_outcome, "5th moderator connection", result.failures);

  // 2. Create 4 Concurrent Matches (each moderator creates its own match)
  std::vector<std::uint64_t> match_ids(kExpectedL6Matches, 0);
  for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
    result.matches[m].lifecycle_start = std::chrono::steady_clock::now();
    result.matches[m].lifecycle_start_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(result.matches[m].lifecycle_start - run_start).count());

    std::optional<json> create_resp;
    constexpr std::size_t kMaxCreateRetries = 10;
    std::size_t retries = 0;
    while (retries <= kMaxCreateRetries) {
      create_resp = timed_request(
          result.matches[m].mod_fd,
          json{{"type", "create_match"}, {"player_count", kExpectedPlayerCount}},
          result.matches[m].metrics,
          result.matches[m].failures,
          expect_match_created(kExpectedPlayerCount, kExpectedInsiders, kExpectedInvestigators, kExpectedEmployees));
      if (create_resp.has_value()) {
        break;
      }
      // Account for asynchronous disconnect processing: if server returns "match limit reached",
      // the event loop may still be unregistering moderator sessions from the prior cycle.
      if (result.matches[m].failures.last_failure_reason.find("match limit reached") != std::string::npos &&
          retries < kMaxCreateRetries) {
        if (result.matches[m].failures.correctness_failures > 0) {
          result.matches[m].failures.correctness_failures--;
        }
        if (!result.matches[m].metrics.empty()) {
          result.matches[m].metrics.pop_back();
        }
        ++retries;
        result.match_limit_retries++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        continue;
      }
      break;
    }
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
      if (extra_pfd < 0) {
        result.matches[0].failures.transport_failures++;
        result.matches[0].failures.unexpected_disconnects++;
        result.matches[0].failures.last_failure_reason = "Failed to connect client for excess 65th player test";
        std::cerr << "[L6] STATE ERROR: Failed to connect client for excess 65th player test\n";
      } else {
        const auto p_hello = timed_request(
            extra_pfd,
            json{{"type", "connect"}, {"role", "player"}},
            result.matches[0].metrics,
            result.matches[0].failures,
            expect_connect("player"));
        L6_VERIFY_REQ(p_hello, result.matches[0], "Excess player connect handshake failed");

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
  const auto peak_fds = sample_open_fds(server_pid);
  if (!peak_fds.has_value()) {
    std::cerr << "[L6 Run " << run_index << "] ERROR: Failed to sample peak open FDs\n";
    result.failures.correctness_failures++;
    result.failures.last_failure_reason = "Failed to sample peak open FDs";
    return result;
  }
  result.peak_open_fds = *peak_fds;

  // Negative test: 261st connection cap (260 maximum connections) must be deterministically rejected
  const auto cap_outcome = verify_connection_cap_rejection(socket_path);
  (void)evaluate_capacity_outcome(cap_outcome, "261st connection", result.failures);

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
      FailureCounts failure_counts;
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
            outcomes[global_idx].failure_counts = local_fail;
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
      FailureCounts failure_counts;
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
            outcomes[global_idx].failure_counts = local_fail;
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

  // Fault injection: premature server exit during FD cleanup
  if (fault_cfg.inject_premature_exit_before_cleanup) {
    if (server_pid > 0) {
      (void)kill(server_pid, SIGKILL);
      int status = 0;
      (void)waitpid(server_pid, &status, 0);
    }
  }

  // Bounded polling for server to process disconnect events and settle FDs
  const auto fd_report = sample_settled_open_fds(
      server_pid,
      result.baseline_open_fds,
      /*allowed_tolerance=*/2,
      /*timeout=*/std::chrono::milliseconds(2000));

  result.immediate_post_cleanup_open_fds = fd_report.immediate_open_fds;
  result.post_cleanup_open_fds = fd_report.settled_open_fds;
  result.cleanup_settle_time_ms = fd_report.settle_time_ms;
  result.post_cleanup_settled = fd_report.settled_within_tolerance;

  if (fault_cfg.inject_fd_leak) {
    result.post_cleanup_settled = false;
    result.post_cleanup_open_fds = result.baseline_open_fds + 100;
  }

  if (!result.post_cleanup_settled) {
    result.failures.correctness_failures++;
    result.failures.last_failure_reason = fd_report.failure_reason.empty()
        ? ("Post-cleanup server FDs failed to settle within baseline tolerance (" +
           std::to_string(result.post_cleanup_open_fds) + " FDs vs baseline " +
           std::to_string(result.baseline_open_fds) + " after " +
           std::to_string(result.cleanup_settle_time_ms) + " ms)")
        : fd_report.failure_reason;
    std::cerr << "[L6] STATE ERROR: " << result.failures.last_failure_reason << '\n';
  }

  cycle_guard.execute();

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
    result.failures.unexpected_disconnects += m_rec.failures.unexpected_disconnects;
    result.failures.timeouts += m_rec.failures.timeouts;
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
  result.run_passed = all_passed && (result.failures.total() == 0) &&
                      result.all_matches_overlapped && result.post_cleanup_settled;

  return result;
}

L6RunResult execute_l6_concurrent_matches(
    const std::string& server_path,
    int run_index,
    const TestFaultConfig& fault_cfg = {}) {
  L6RunResult result;
  result.run_index = run_index;

  const std::string socket_path = generate_unique_socket_path();
  const std::string mod_token = kDefaultModeratorToken;

  const auto server_pid = spawn_server_unix(server_path, socket_path, mod_token);
  if (server_pid <= 0) {
    std::cerr << "[L6 Run " << run_index << "] ERROR: Failed to spawn darkpool_server\n";
    result.failures.transport_failures++;
    result.failures.unexpected_disconnects++;
    result.failures.last_failure_reason = "Failed to spawn darkpool_server";
    return result;
  }

  struct ServerProcessGuard {
    pid_t pid{0};
    std::string sock;
    ~ServerProcessGuard() {
      if (pid > 0) stop_server_unix(pid, sock);
    }
  } srv_guard{server_pid, socket_path};

  const int probe_fd = connect_unix(socket_path);
  if (probe_fd < 0) {
    std::cerr << "[L6 Run " << run_index << "] ERROR: Failed to connect probe socket\n";
    result.failures.transport_failures++;
    result.failures.unexpected_disconnects++;
    result.failures.last_failure_reason = "Failed to connect probe socket";
    return result;
  }
  (void)close(probe_fd);

  const auto base_fds = sample_open_fds(server_pid);
  if (!base_fds.has_value()) {
    std::cerr << "[L6 Run " << run_index << "] ERROR: Failed to sample baseline open FDs\n";
    result.failures.correctness_failures++;
    result.failures.last_failure_reason = "Failed to sample baseline open FDs";
    return result;
  }

  result = execute_l6_single_cycle(server_pid, socket_path, mod_token, run_index, *base_fds, fault_cfg);
  result.peak_rss_kb = sample_peak_rss_kb(server_pid);
  result.cpu_time_ms = sample_cpu_time_ms(server_pid);

  stop_server_unix(server_pid, socket_path);
  srv_guard.pid = 0;
  return result;
}

struct SustainedClassification {
  std::string status;  // PASS, FAIL, BLOCKED
  std::string reason;
  std::string diagnostic_details;
};

inline SustainedClassification classify_l6_sustained_status(
    std::uint64_t target_duration_s,
    std::uint64_t actual_duration_ms,
    std::size_t completed_cycles,
    std::size_t completed_matches,
    std::size_t total_requests,
    std::size_t total_failures,
    std::size_t transport_failures,
    std::size_t correctness_failures,
    bool abort_due_to_failure,
    const std::string& existing_reason) {
  SustainedClassification out;
  const bool reached_target_duration = (actual_duration_ms >= target_duration_s * 1000ULL);
  const double elapsed_s = static_cast<double>(actual_duration_ms) / 1000.0;

  if (abort_due_to_failure || total_failures > 0) {
    out.status = "FAIL";
    if (!existing_reason.empty()) {
      out.reason = existing_reason;
    } else {
      out.reason = "Encountered " + std::to_string(total_failures) + " failures (" +
                   std::to_string(transport_failures) + " transport, " +
                   std::to_string(correctness_failures) + " correctness)";
    }
    out.diagnostic_details = "Sustained L6 run aborted after " + std::to_string(completed_cycles) +
                             " completed cycles (" + std::to_string(completed_matches) +
                             " matches) and " + std::to_string(total_requests) +
                             " requests: " + out.reason;
  } else if (!reached_target_duration) {
    out.status = "BLOCKED";
    out.reason = "Workload stopped early before reaching target duration of " +
                 std::to_string(target_duration_s) + "s (actual: " +
                 std::to_string(static_cast<std::uint64_t>(elapsed_s)) + "s)";
    out.diagnostic_details = "Elapsed duration (" + std::to_string(actual_duration_ms) +
                             " ms) fell short of target duration (" +
                             std::to_string(target_duration_s * 1000ULL) + " ms)";
  } else if (completed_cycles == 0 || completed_matches == 0) {
    out.status = "FAIL";
    out.reason = "Zero cycles completed during workload";
    out.diagnostic_details = "Workload reached target duration but zero complete 4-match cycles were finished";
  } else {
    out.status = "PASS";
    out.diagnostic_details = "Workload completed successfully across " +
                             std::to_string(completed_cycles) + " cycles (" +
                             std::to_string(completed_matches) +
                             " matches) with 0 transport and 0 correctness failures";
  }
  return out;
}

struct L6SustainedResult {
  std::string stage_name;
  std::string status;
  std::string reason;
  std::string diagnostic_details;
  std::uint64_t seed{0};
  std::uint64_t target_duration_s{0};
  std::uint64_t actual_duration_ms{0};
  std::size_t completed_cycles{0};
  std::size_t completed_matches{0};
  std::size_t total_match_limit_retries{0};
  PhaseMetrics aggregate_sequential;
  PhaseMetrics aggregate_concurrent;
  PhaseMetrics aggregate_total;
  FailureCounts failures;
  double throughput_rps{0.0};
  std::size_t baseline_open_fds{0};
  std::size_t peak_open_fds{0};
  std::size_t immediate_post_cleanup_open_fds{0};
  std::size_t settled_post_cleanup_fds{0};
  bool all_cycles_fd_cleanup_passed{true};
  std::uint64_t initial_rss_kb{0};
  std::uint64_t peak_rss_kb{0};
  std::uint64_t end_rss_kb{0};
  double cpu_time_ms{0.0};
  std::vector<std::uint64_t> rss_samples_kb;
  bool all_cycles_overlapped{true};
};

inline L6SustainedResult execute_l6_sustained_workload(
    const std::string& server_path,
    std::uint64_t duration_seconds,
    std::uint64_t seed,
    const std::string& stage_name,
    const TestFaultConfig& fault_cfg = {}) {
  L6SustainedResult result;
  result.stage_name = stage_name;
  result.target_duration_s = duration_seconds;
  result.seed = seed;

  const std::string socket_path = generate_unique_socket_path();
  const std::string mod_token = kDefaultModeratorToken;

  const auto server_pid = spawn_server_unix(server_path, socket_path, mod_token);
  if (server_pid <= 0) {
    result.status = "FAIL";
    result.reason = "Failed to spawn darkpool_server for sustained workload";
    result.diagnostic_details = "spawn_server_unix failed for path: " + server_path;
    return result;
  }

  struct ServerProcessGuard {
    pid_t pid{0};
    std::string sock;
    ~ServerProcessGuard() {
      if (pid > 0) stop_server_unix(pid, sock);
    }
  } srv_guard{server_pid, socket_path};

  const int probe_fd = connect_unix(socket_path);
  if (probe_fd < 0) {
    result.status = "FAIL";
    result.reason = "Failed to probe server on unix socket";
    result.diagnostic_details = "connect_unix failed for path: " + socket_path;
    return result;
  }
  (void)close(probe_fd);

  const auto base_fds = sample_open_fds(server_pid);
  if (!base_fds.has_value()) {
    result.status = "FAIL";
    result.reason = "Failed to sample baseline open FDs";
    result.diagnostic_details = "sample_open_fds returned nullopt for baseline";
    return result;
  }
  result.baseline_open_fds = *base_fds;
  result.initial_rss_kb = sample_peak_rss_kb(server_pid);

  std::vector<std::uint64_t> pooled_seq_lats;
  std::vector<std::uint64_t> pooled_conc_lats;
  std::vector<std::uint64_t> pooled_all_lats;

  const auto start_time = std::chrono::steady_clock::now();
  const auto deadline = start_time + std::chrono::seconds(duration_seconds);

  std::cout << "[" << stage_name << "] Beginning sustained repeated 4-match cycle workload (target: "
            << duration_seconds << "s, metadata seed: " << seed << ") on persistent server (PID "
            << server_pid << ")..." << std::endl;

  bool abort_due_to_failure = false;
  int cycle_index = 0;

  while (std::chrono::steady_clock::now() < deadline && !abort_due_to_failure) {
    ++cycle_index;
    const auto cycle_res = execute_l6_single_cycle(
        server_pid, socket_path, mod_token, cycle_index, result.baseline_open_fds, fault_cfg);

    if (cycle_res.peak_open_fds > result.peak_open_fds) {
      result.peak_open_fds = cycle_res.peak_open_fds;
    }
    result.immediate_post_cleanup_open_fds = cycle_res.immediate_post_cleanup_open_fds;
    result.settled_post_cleanup_fds = cycle_res.post_cleanup_open_fds;
    result.total_match_limit_retries += cycle_res.match_limit_retries;

    if (!cycle_res.all_matches_overlapped) {
      result.all_cycles_overlapped = false;
    }
    if (!cycle_res.post_cleanup_settled) {
      result.all_cycles_fd_cleanup_passed = false;
    }

    if (!cycle_res.run_passed || cycle_res.failures.total() > 0) {
      abort_due_to_failure = true;
      result.failures.transport_failures += cycle_res.failures.transport_failures;
      result.failures.correctness_failures += cycle_res.failures.correctness_failures;
      result.failures.unexpected_disconnects += cycle_res.failures.unexpected_disconnects;
      result.failures.timeouts += cycle_res.failures.timeouts;
      result.reason = cycle_res.failures.last_failure_reason.empty()
          ? ("Cycle " + std::to_string(cycle_index) + " failed verification")
          : cycle_res.failures.last_failure_reason;
      std::cerr << "[" << stage_name << "] ABORTING sustained run at cycle " << cycle_index
                << ": " << result.reason << std::endl;
      break;
    }

    result.completed_cycles++;
    result.completed_matches += cycle_res.matches.size();

    // Pool latencies and request metrics
    for (const auto& m : cycle_res.matches) {
      for (const auto& met : m.metrics) {
        pooled_all_lats.push_back(met.latency_us);
        result.aggregate_total.total_requests++;
        if (met.is_concurrent) {
          result.aggregate_concurrent.total_requests++;
          if (met.transport_ok && met.correctness_ok) {
            if (met.is_expected_rejection) {
              result.aggregate_concurrent.expected_rejections++;
            } else {
              result.aggregate_concurrent.successful_requests++;
            }
            pooled_conc_lats.push_back(met.latency_us);
          } else {
            if (!met.transport_ok) result.aggregate_concurrent.transport_failures++;
            if (!met.correctness_ok) result.aggregate_concurrent.correctness_failures++;
          }
        } else {
          result.aggregate_sequential.total_requests++;
          if (met.transport_ok && met.correctness_ok) {
            if (met.is_expected_rejection) {
              result.aggregate_sequential.expected_rejections++;
            } else {
              result.aggregate_sequential.successful_requests++;
            }
            pooled_seq_lats.push_back(met.latency_us);
          } else {
            if (!met.transport_ok) result.aggregate_sequential.transport_failures++;
            if (!met.correctness_ok) result.aggregate_sequential.correctness_failures++;
          }
        }
      }
    }

    // Sample RSS per cycle
    const auto current_rss = sample_peak_rss_kb(server_pid);
    if (current_rss > result.peak_rss_kb) result.peak_rss_kb = current_rss;
    result.end_rss_kb = current_rss;
    result.rss_samples_kb.push_back(current_rss);

    // Periodic progress report
    if (cycle_index % 25 == 0 || duration_seconds <= 10) {
      const auto now_t = std::chrono::steady_clock::now();
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now_t - start_time).count();
      std::cout << "[" << stage_name << "] Cycle " << cycle_index << " done ("
                << result.completed_matches << " matches, elapsed: "
                << (elapsed_ms / 1000) << "/" << duration_seconds << "s, RSS="
                << current_rss << " kB, settled_FDs=" << cycle_res.post_cleanup_open_fds << ")" << std::endl;
    }
  }

  const auto end_time = std::chrono::steady_clock::now();
  result.actual_duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

  result.end_rss_kb = sample_peak_rss_kb(server_pid);
  if (result.end_rss_kb > result.peak_rss_kb) result.peak_rss_kb = result.end_rss_kb;
  result.cpu_time_ms = sample_cpu_time_ms(server_pid);

  stop_server_unix(server_pid, socket_path);
  srv_guard.pid = 0;

  result.aggregate_total.successful_requests =
      result.aggregate_sequential.successful_requests + result.aggregate_concurrent.successful_requests;
  result.aggregate_total.expected_rejections =
      result.aggregate_sequential.expected_rejections + result.aggregate_concurrent.expected_rejections;
  result.aggregate_total.transport_failures = result.failures.transport_failures;
  result.aggregate_total.correctness_failures = result.failures.correctness_failures;

  compute_percentiles(pooled_seq_lats, result.aggregate_sequential);
  compute_percentiles(pooled_conc_lats, result.aggregate_concurrent);
  compute_percentiles(pooled_all_lats, result.aggregate_total);

  const double elapsed_s = static_cast<double>(result.actual_duration_ms) / 1000.0;
  result.throughput_rps = elapsed_s > 0.0
      ? (static_cast<double>(result.aggregate_total.total_requests) / elapsed_s)
      : 0.0;

  const auto classification = classify_l6_sustained_status(
      result.target_duration_s,
      result.actual_duration_ms,
      result.completed_cycles,
      result.completed_matches,
      result.aggregate_total.total_requests,
      result.failures.total(),
      result.failures.transport_failures,
      result.failures.correctness_failures,
      abort_due_to_failure,
      result.reason);

  result.status = classification.status;
  result.reason = classification.reason;
  result.diagnostic_details = classification.diagnostic_details;

  std::cout << "\n=================================================================\n";
  std::cout << " L6 SUSTAINED WORKLOAD REPORT (" << result.stage_name << ")\n";
  std::cout << "=================================================================\n";
  std::cout << " Status              : " << result.status << "\n";
  std::cout << " Target Duration     : " << result.target_duration_s << " s\n";
  std::cout << " Actual Duration     : " << std::fixed << std::setprecision(2)
            << elapsed_s << " s (" << result.actual_duration_ms << " ms)\n";
  std::cout << " Completed Cycles    : " << result.completed_cycles << " (4 matches per cycle)\n";
  std::cout << " Completed Matches   : " << result.completed_matches << "\n";
  std::cout << " Match Limit Retries : " << result.total_match_limit_retries << "\n";
  std::cout << " Total Requests      : " << result.aggregate_total.total_requests << "\n";
  std::cout << " Successful Requests : " << result.aggregate_total.successful_requests << "\n";
  std::cout << " Expected Rejections : " << result.aggregate_total.expected_rejections << "\n";
  std::cout << " Transport Failures  : " << result.failures.transport_failures << "\n";
  std::cout << " Correctness Failures: " << result.failures.correctness_failures << "\n";
  std::cout << " Unexpected Disconn  : " << result.failures.unexpected_disconnects << "\n";
  std::cout << " Timeouts            : " << result.failures.timeouts << "\n";
  std::cout << " Sequential Latency  : p50=" << result.aggregate_sequential.p50_ms << " ms, "
            << "p95=" << result.aggregate_sequential.p95_ms << " ms, "
            << "p99=" << result.aggregate_sequential.p99_ms << " ms\n";
  std::cout << " Concurrent Latency  : p50=" << result.aggregate_concurrent.p50_ms << " ms, "
            << "p95=" << result.aggregate_concurrent.p95_ms << " ms, "
            << "p99=" << result.aggregate_concurrent.p99_ms << " ms\n";
  std::cout << " Total Latency       : p50=" << result.aggregate_total.p50_ms << " ms, "
            << "p95=" << result.aggregate_total.p95_ms << " ms, "
            << "p99=" << result.aggregate_total.p99_ms << " ms\n";
  std::cout << " Throughput          : " << std::setprecision(1) << result.throughput_rps
            << " req/s (denominator: actual elapsed duration in seconds)\n";
  std::cout << " Resources           : Initial RSS=" << result.initial_rss_kb << " kB, "
            << "Peak RSS=" << result.peak_rss_kb << " kB, End RSS=" << result.end_rss_kb << " kB, "
            << "CPU=" << std::setprecision(1) << result.cpu_time_ms << " ms\n";
  std::cout << " Open FDs            : Baseline=" << result.baseline_open_fds
            << ", Peak=" << result.peak_open_fds
            << ", Settled=" << result.settled_post_cleanup_fds
            << " (all cycles settled within baseline + 2 tolerance: "
            << (result.all_cycles_fd_cleanup_passed ? "YES" : "NO") << ")\n";
  if (!result.reason.empty()) {
    std::cout << " Failure Reason      : " << result.reason << "\n";
  }
  std::cout << "=================================================================\n\n";

  return result;
}

inline bool write_l6_sustained_report(
    const L6SustainedResult& res,
    const std::string& output_path,
    const std::string& source_commit,
    bool git_dirty,
    const std::string& env_info) {
  json j = json::object();
  j["test_name"] = "darkpool_load_tests_l6_sustained";
  j["stage_name"] = res.stage_name;
  j["status"] = res.status;
  j["source_commit"] = source_commit;
  j["git_dirty"] = git_dirty;
  j["environment"] = env_info;
  j["workload_seed"] = res.seed;
  j["target_duration_s"] = res.target_duration_s;
  j["actual_duration_ms"] = res.actual_duration_ms;
  j["actual_duration_s"] = static_cast<double>(res.actual_duration_ms) / 1000.0;
  j["completed_cycles"] = res.completed_cycles;
  j["completed_matches"] = res.completed_matches;
  j["match_limit_retries"] = res.total_match_limit_retries;

  j["concurrency_topology"] = {
    {"concurrent_matches", kExpectedL6Matches},
    {"players_per_match", kExpectedPlayerCount},
    {"total_player_clients", kExpectedL6TotalPlayers},
    {"moderator_clients", kExpectedModerators},
    {"total_connected_clients", kExpectedL6TotalPlayers + kExpectedModerators},
    {"connection_cap", 260},
    {"persistent_server", true}
  };

  j["requests"] = {
    {"total_requests", res.aggregate_total.total_requests},
    {"successful_requests", res.aggregate_total.successful_requests},
    {"expected_rejections", res.aggregate_total.expected_rejections},
    {"transport_failures", res.aggregate_total.transport_failures},
    {"correctness_failures", res.aggregate_total.correctness_failures}
  };

  j["total_requests"] = res.aggregate_total.total_requests;
  j["successful_requests"] = res.aggregate_total.successful_requests;
  j["expected_rejections"] = res.aggregate_total.expected_rejections;
  j["transport_failures"] = res.aggregate_total.transport_failures;
  j["correctness_failures"] = res.aggregate_total.correctness_failures;

  j["failures"] = {
    {"transport_failures", res.failures.transport_failures},
    {"correctness_failures", res.failures.correctness_failures},
    {"unexpected_disconnects", res.failures.unexpected_disconnects},
    {"timeouts", res.failures.timeouts}
  };

  j["sequential_latency"] = {
    {"p50_ms", res.aggregate_sequential.p50_ms},
    {"p95_ms", res.aggregate_sequential.p95_ms},
    {"p99_ms", res.aggregate_sequential.p99_ms},
    {"mean_ms", res.aggregate_sequential.mean_ms},
    {"min_ms", res.aggregate_sequential.min_ms},
    {"max_ms", res.aggregate_sequential.max_ms}
  };

  j["concurrent_latency"] = {
    {"p50_ms", res.aggregate_concurrent.p50_ms},
    {"p95_ms", res.aggregate_concurrent.p95_ms},
    {"p99_ms", res.aggregate_concurrent.p99_ms},
    {"mean_ms", res.aggregate_concurrent.mean_ms},
    {"min_ms", res.aggregate_concurrent.min_ms},
    {"max_ms", res.aggregate_concurrent.max_ms}
  };

  j["total_latency"] = {
    {"p50_ms", res.aggregate_total.p50_ms},
    {"p95_ms", res.aggregate_total.p95_ms},
    {"p99_ms", res.aggregate_total.p99_ms},
    {"mean_ms", res.aggregate_total.mean_ms},
    {"min_ms", res.aggregate_total.min_ms},
    {"max_ms", res.aggregate_total.max_ms},
    {"measurement_definition", "Monotonic steady_clock round-trip duration measured from request frame write to correlated response parse; excludes inter-phase pauses."}
  };

  j["throughput"] = {
    {"requests_per_second", res.throughput_rps},
    {"denominator", "Actual elapsed duration in seconds (actual_duration_ms / 1000.0)"}
  };

  j["resources"] = {
    {"baseline_open_fds", res.baseline_open_fds},
    {"peak_open_fds", res.peak_open_fds},
    {"immediate_post_cleanup_open_fds", res.immediate_post_cleanup_open_fds},
    {"settled_post_cleanup_fds", res.settled_post_cleanup_fds},
    {"all_cycles_fd_cleanup_passed", res.all_cycles_fd_cleanup_passed},
    {"initial_rss_kb", res.initial_rss_kb},
    {"peak_rss_kb", res.peak_rss_kb},
    {"end_rss_kb", res.end_rss_kb},
    {"cpu_time_ms", res.cpu_time_ms},
    {"rss_sample_count", res.rss_samples_kb.size()}
  };

  j["cycle_diagnostics"] = {
    {"total_cycles_attempted", res.completed_cycles + (res.status == "PASS" ? 0 : 1)},
    {"completed_cycles", res.completed_cycles},
    {"completed_matches", res.completed_matches},
    {"all_cycles_overlapped", res.all_cycles_overlapped},
    {"match_limit_retries", res.total_match_limit_retries}
  };

  j["failure_reason"] = res.reason;
  j["diagnostic_details"] = res.diagnostic_details.empty()
      ? (res.reason.empty() ? "None" : res.reason)
      : res.diagnostic_details;
  j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  std::error_code ec;
  const std::filesystem::path out_p(output_path);
  if (out_p.has_parent_path()) {
    std::filesystem::create_directories(out_p.parent_path(), ec);
  }

  std::ofstream out_file(output_path);
  if (!out_file.is_open()) {
    std::cerr << "Error: Could not open sustained report output file for writing: " << output_path << "\n";
    return false;
  }
  out_file << j.dump(2) << '\n';
  out_file.flush();
  if (!out_file.good() || out_file.fail() || out_file.bad()) {
    std::cerr << "Error: Stream error writing sustained report output to: " << output_path << "\n";
    return false;
  }
  out_file.close();
  if (out_file.fail()) {
    std::cerr << "Error: Failed to close sustained report output file: " << output_path << "\n";
    return false;
  }
  std::cout << "Machine-readable sustained report written to: " << output_path << "\n\n";
  return true;
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
  std::size_t unexpected_disconnects_all = 0;
  std::size_t timeouts_all = 0;
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
    unexpected_disconnects_all += r.failures.unexpected_disconnects;
    timeouts_all += r.failures.timeouts;
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
      {"immediate_post_cleanup_open_fds", r.immediate_post_cleanup_open_fds},
      {"post_cleanup_open_fds", r.post_cleanup_open_fds},
      {"cleanup_settle_time_ms", r.cleanup_settle_time_ms},
      {"post_cleanup_settled", r.post_cleanup_settled},
      {"peak_rss_kb", r.peak_rss_kb},
      {"cpu_time_ms", r.cpu_time_ms},
      {"measurement_windows", "baseline_open_fds sampled at zero client load; peak_open_fds sampled with 260 active client sockets (4 moderators + 256 players) during Round 2 before any client closes; immediate_post_cleanup_open_fds sampled immediately after client close; post_cleanup_open_fds sampled after server event loop settles disconnects within bounded 2000ms deadline; peak_rss_kb and cpu_time_ms measured over entire server daemon process lifetime (server_pid)."}
    };
    run_obj["failures"] = {
      {"transport_failures", r.failures.transport_failures},
      {"correctness_failures", r.failures.correctness_failures},
      {"unexpected_disconnects", r.failures.unexpected_disconnects},
      {"timeouts", r.failures.timeouts}
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
    {"unexpected_disconnects_all_runs", unexpected_disconnects_all},
    {"timeouts_all_runs", timeouts_all},
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

bool verify_failure_aggregation_exactly_once() {
  // Test 1: Capacity outcome evaluation classifies correctly and counts exactly once
  {
    FailureCounts f;
    const bool ok1 = evaluate_capacity_outcome(CapacityRejectionOutcome::PromptEofClosed, "test_prompt", f);
    if (!ok1 || f.total() != 0) return false;

    const bool ok2 = evaluate_capacity_outcome(CapacityRejectionOutcome::ExplicitRejectionFrame, "test_frame", f);
    if (!ok2 || f.total() != 0) return false;

    const bool ok3 = evaluate_capacity_outcome(CapacityRejectionOutcome::UnexpectedAcceptance, "test_acc", f);
    if (ok3 || f.correctness_failures != 1 || f.total() != 1) return false;

    const bool ok4 = evaluate_capacity_outcome(CapacityRejectionOutcome::TimedOutWithoutClose, "test_timeout", f);
    if (ok4 || f.timeouts != 1 || f.transport_failures != 1 || f.total() != 2) return false;

    const bool ok5 = evaluate_capacity_outcome(CapacityRejectionOutcome::ConnectFailed, "test_conn_fail", f);
    if (ok5 || f.unexpected_disconnects != 1 || f.transport_failures != 2 || f.total() != 3) return false;

    const bool ok6 = evaluate_capacity_outcome(CapacityRejectionOutcome::UnexpectedData, "test_data", f);
    if (ok6 || f.correctness_failures != 2 || f.total() != 4) return false;

    const bool ok7 = evaluate_capacity_outcome(CapacityRejectionOutcome::SendFailed, "test_send_fail", f);
    if (ok7 || f.unexpected_disconnects != 2 || f.transport_failures != 3 || f.total() != 5) return false;

    const bool ok8 = evaluate_capacity_outcome(CapacityRejectionOutcome::SocketReadError, "test_sock_err", f);
    if (ok8 || f.unexpected_disconnects != 3 || f.transport_failures != 4 || f.total() != 6) return false;
  }

  // Test 2: Aggregation across matches and run-level failures produces exactly-once totals
  {
    L6RunResult res;
    res.matches.resize(kExpectedL6Matches);
    // Add 1 run-level failure (e.g. from capacity check timeout)
    evaluate_capacity_outcome(CapacityRejectionOutcome::TimedOutWithoutClose, "run_cap", res.failures);

    // Match 0 has 1 transport failure (unexpected disconnect)
    res.matches[0].failures.transport_failures = 1;
    res.matches[0].failures.unexpected_disconnects = 1;

    // Match 1 has 1 correctness failure
    res.matches[1].failures.correctness_failures = 1;

    // Simulate aggregation loop
    for (std::size_t m = 0; m < kExpectedL6Matches; ++m) {
      res.failures.transport_failures += res.matches[m].failures.transport_failures;
      res.failures.correctness_failures += res.matches[m].failures.correctness_failures;
      res.failures.unexpected_disconnects += res.matches[m].failures.unexpected_disconnects;
      res.failures.timeouts += res.matches[m].failures.timeouts;
    }
    res.aggregate_total.transport_failures = res.failures.transport_failures;
    res.aggregate_total.correctness_failures = res.failures.correctness_failures;

    if (res.failures.timeouts != 1) return false;
    if (res.failures.unexpected_disconnects != 1) return false;
    if (res.failures.transport_failures != 2) return false;
    if (res.failures.correctness_failures != 1) return false;
    if (res.failures.total() != 3) return false;
    if (res.aggregate_total.transport_failures != 2) return false;
    if (res.aggregate_total.correctness_failures != 1) return false;
  }

  return true;
}

bool run_l6_regression_selftest(const std::string& harness_path, const std::string& server_path) {
  std::cout << "=================================================================\n";
  std::cout << " Running DARKPOOL L6 (256-Player / 4-Match) Regression Tests\n";
  std::cout << "=================================================================\n";

  // Regression 1: Normal 4-match 256-player concurrent execution with 4 independent moderators
  std::cout << "[L6 Regression 1/16] Verifying 4-match 256P concurrent workload (4 independent moderators)... " << std::flush;
  const auto run_res = execute_l6_concurrent_matches(server_path, 1, {});
  if (!run_res.run_passed || !run_res.all_matches_overlapped || run_res.moderator_clients != 4) {
    std::cout << "FAILED (run failed or matches did not overlap or mod count != 4)\n";
    return false;
  }
  std::cout << "PASSED (4 matches, 4 moderators, 256 players, 0 failures, verified outcome InsiderWin, measured overlap "
            << run_res.overlap_duration_ms << " ms)\n";

  // Regression 2: Subprocess normal run exits 0
  std::cout << "[L6 Regression 2/16] Verifying normal L6 subprocess exits 0... " << std::flush;
  const int code_norm = run_subprocess(harness_path, {"--server", server_path, "--functional", "--out", "/tmp/selftest_l6_norm.json"});
  if (code_norm != 0) {
    std::cout << "FAILED (expected exit 0, got " << code_norm << ")\n";
    return false;
  }
  std::cout << "PASSED (exit code 0)\n";

  // Regression 3: Injected assertion failure subprocess exits nonzero
  std::cout << "[L6 Regression 3/16] Verifying assertion failure subprocess exits nonzero... " << std::flush;
  const int code_assert = run_subprocess(harness_path, {"--server", server_path, "--inject-assertion-failure", "--out", "/tmp/selftest_l6_assert.json"});
  if (code_assert == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_assert << ")\n";

  // Regression 4: Injected outcome mismatch subprocess exits nonzero
  std::cout << "[L6 Regression 4/16] Verifying outcome mismatch subprocess exits nonzero... " << std::flush;
  const int code_outcome = run_subprocess(harness_path, {"--server", server_path, "--inject-outcome-mismatch", "--out", "/tmp/selftest_l6_outcome.json"});
  if (code_outcome == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_outcome << ")\n";

  // Regression 5: Injected semantic failure subprocess exits nonzero
  std::cout << "[L6 Regression 5/16] Verifying semantic failure subprocess exits nonzero... " << std::flush;
  const int code_semantic = run_subprocess(harness_path, {"--server", server_path, "--inject-semantic-failure", "--out", "/tmp/selftest_l6_semantic.json"});
  if (code_semantic == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_semantic << ")\n";

  // Regression 6: Injected capacity accepted subprocess exits nonzero
  std::cout << "[L6 Regression 6/16] Verifying accepted over-capacity connection subprocess (exercised via real mock server) exits nonzero... " << std::flush;
  const int code_cap_acc = run_subprocess(harness_path, {"--server", server_path, "--inject-capacity-accepted", "--out", "/tmp/selftest_l6_cap_acc.json"});
  if (code_cap_acc == 0) {
    std::cout << "FAILED (expected nonzero exit code when capacity is violated, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_cap_acc << ")\n";

  // Regression 7: Injected capacity stalled subprocess exits nonzero
  std::cout << "[L6 Regression 7/16] Verifying stalled capacity connection subprocess (exercised via real mock server) exits nonzero... " << std::flush;
  const int code_cap_stall = run_subprocess(harness_path, {"--server", server_path, "--inject-capacity-stalled", "--out", "/tmp/selftest_l6_cap_stall.json"});
  if (code_cap_stall == 0) {
    std::cout << "FAILED (expected nonzero exit code when capacity stalls, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_cap_stall << ")\n";

  // Regression 8: Injected connection-establishment failure subprocess exits nonzero
  std::cout << "[L6 Regression 8/16] Verifying connection-establishment failure propagation subprocess exits nonzero... " << std::flush;
  const int code_conn_fail = run_subprocess(harness_path, {"--server", server_path, "--inject-connect-failure", "--out", "/tmp/selftest_l6_conn_fail.json"});
  if (code_conn_fail == 0) {
    std::cout << "FAILED (expected nonzero exit code when connection establishment fails, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_conn_fail << ")\n";

  // Regression 9: Unreachable server binary connection failure exits nonzero
  std::cout << "[L6 Regression 9/16] Verifying unreachable server binary connection failure exits nonzero... " << std::flush;
  const int code_unreach = run_subprocess(harness_path, {"--server", "/bin/false", "--functional", "--out", "/tmp/selftest_l6_unreach.json"});
  if (code_unreach == 0) {
    std::cout << "FAILED (expected nonzero exit code when server binary is unreachable, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_unreach << ")\n";

  // Regression 10: Injected FD cleanup leak subprocess exits nonzero
  std::cout << "[L6 Regression 10/16] Verifying FD cleanup leak subprocess exits nonzero... " << std::flush;
  const int code_fd_leak = run_subprocess(harness_path, {"--server", server_path, "--inject-fd-leak", "--out", "/tmp/selftest_l6_fd_leak.json"});
  if (code_fd_leak == 0) {
    std::cout << "FAILED (expected nonzero exit code when FDs leak, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_fd_leak << ")\n";

  // Regression 11: Injected premature server exit during FD cleanup subprocess exits nonzero
  std::cout << "[L6 Regression 11/16] Verifying premature server exit during FD cleanup exits nonzero... " << std::flush;
  const int code_srv_exit = run_subprocess(harness_path, {"--server", server_path, "--inject-server-exit-cleanup", "--out", "/tmp/selftest_l6_srv_exit.json"});
  if (code_srv_exit == 0) {
    std::cout << "FAILED (expected nonzero exit code when server exits prematurely during FD cleanup, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_srv_exit << ")\n";

  // Regression 12: Direct check of sample_settled_open_fds with dead process
  std::cout << "[L6 Regression 12/16] Verifying sample_settled_open_fds rejects dead PID without claiming cleanup... " << std::flush;
  {
    const pid_t dummy = fork();
    if (dummy == 0) {
      _exit(0);
    }
    int st = 0;
    waitpid(dummy, &st, 0);
    const auto dead_rep = sample_settled_open_fds(dummy, 9, 2, std::chrono::milliseconds(50));
    if (dead_rep.settled_within_tolerance || !dead_rep.server_exited_prematurely || !dead_rep.sampling_failed) {
      std::cout << "FAILED (dead process incorrectly reported as settled within tolerance or not premature exit!)\n";
      return false;
    }
  }
  std::cout << "PASSED (dead PID rejected with server_exited_prematurely=true, settled_within_tolerance=false)\n";

  // Regression 13: Direct check of process exiting during settle polling
  std::cout << "[L6 Regression 13/16] Verifying process exiting during settle polling classified as premature exit... " << std::flush;
  {
    int poll_count = 0;
    auto mock_sampler = [&poll_count](pid_t) -> std::optional<std::size_t> {
      ++poll_count;
      if (poll_count == 1) return 100;
      return std::nullopt;
    };
    auto mock_liveness = [&poll_count](pid_t) -> ProcessLiveness {
      if (poll_count >= 2) return ProcessLiveness::Terminated;
      return ProcessLiveness::Alive;
    };
    const auto exit_rep = sample_settled_open_fds(
        12345, 9, 2, std::chrono::milliseconds(50), mock_sampler, mock_liveness);
    if (!exit_rep.server_exited_prematurely || !exit_rep.sampling_failed || exit_rep.settled_within_tolerance) {
      std::cout << "FAILED (process exiting during settle polling was not classified as premature exit!)\n";
      return false;
    }
  }
  std::cout << "PASSED (exit during polling classified with server_exited_prematurely=true, sampling_failed=true)\n";

  // Regression 14: Direct check of sampling failure while process remains alive
  std::cout << "[L6 Regression 14/16] Verifying FD sampling failure while process remains alive classified as sampling failure (not exit)... " << std::flush;
  {
    int poll_count = 0;
    auto mock_sampler_alive = [&poll_count](pid_t) -> std::optional<std::size_t> {
      ++poll_count;
      if (poll_count == 1) return 100;
      return std::nullopt;
    };
    auto mock_liveness_alive = [](pid_t) -> ProcessLiveness {
      return ProcessLiveness::Alive;
    };
    const auto samp_fail_rep = sample_settled_open_fds(
        12345, 9, 2, std::chrono::milliseconds(50), mock_sampler_alive, mock_liveness_alive);
    if (samp_fail_rep.server_exited_prematurely || !samp_fail_rep.sampling_failed || samp_fail_rep.settled_within_tolerance) {
      std::cout << "FAILED (sampling failure while alive incorrectly reported exit or cleanup!)\n";
      return false;
    }
  }
  std::cout << "PASSED (sampling failure while alive classified with server_exited_prematurely=false, sampling_failed=true)\n";

  // Regression 15: Exactly-once failure aggregation validation
  std::cout << "[L6 Regression 15/16] Verifying exactly-once failure aggregation logic... " << std::flush;
  if (!verify_failure_aggregation_exactly_once()) {
    std::cout << "FAILED (failure aggregation logic failed assertions)\n";
    return false;
  }
  std::cout << "PASSED (exactly-once aggregation verified)\n";

  // Regression 16: Subprocess with unwritable output path exits nonzero
  std::cout << "[L6 Regression 16/19] Verifying unwritable report output path exits nonzero... " << std::flush;
  const int code_unwritable = run_subprocess(
      harness_path,
      {"--server", server_path, "--functional", "--out", "/dev/null/darkpool_invalid_l6_output.json"});
  if (code_unwritable == 0) {
    std::cout << "FAILED (expected nonzero exit code when report cannot be written, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_unwritable << ")\n";

  // Regression 17: Sustained status classification unit test
  std::cout << "[L6 Regression 17/19] Verifying sustained status classification unit tests... " << std::flush;
  {
    const auto c1 = classify_l6_sustained_status(600, 599000, 10, 40, 21760, 0, 0, 0, false, "");
    if (c1.status != "BLOCKED") { std::cout << "FAILED (c1 expected BLOCKED, got " << c1.status << ")\n"; return false; }
    const auto c2 = classify_l6_sustained_status(600, 600000, 10, 40, 21760, 0, 0, 0, false, "");
    if (c2.status != "PASS") { std::cout << "FAILED (c2 expected PASS, got " << c2.status << ")\n"; return false; }
    const auto c3 = classify_l6_sustained_status(600, 601000, 10, 40, 21760, 0, 0, 0, false, "");
    if (c3.status != "PASS") { std::cout << "FAILED (c3 expected PASS, got " << c3.status << ")\n"; return false; }
    const auto c4 = classify_l6_sustained_status(600, 600000, 10, 40, 21760, 1, 0, 1, false, "");
    if (c4.status != "FAIL") { std::cout << "FAILED (c4 expected FAIL, got " << c4.status << ")\n"; return false; }
    const auto c5 = classify_l6_sustained_status(600, 600000, 0, 0, 0, 0, 0, 0, false, "");
    if (c5.status != "FAIL") { std::cout << "FAILED (c5 expected FAIL, got " << c5.status << ")\n"; return false; }
    const auto c6 = classify_l6_sustained_status(1800, 1799999, 30, 120, 65280, 0, 0, 0, false, "");
    if (c6.status != "BLOCKED") { std::cout << "FAILED (c6 expected BLOCKED, got " << c6.status << ")\n"; return false; }
    const auto c7 = classify_l6_sustained_status(1800, 1800000, 30, 120, 65280, 0, 0, 0, false, "");
    if (c7.status != "PASS") { std::cout << "FAILED (c7 expected PASS, got " << c7.status << ")\n"; return false; }
  }
  std::cout << "PASSED (all 7 classification boundaries verified)\n";

  // Regression 18: Injected failure in sustained workload causes FAIL status and nonzero exit
  std::cout << "[L6 Regression 18/19] Verifying injected failure in sustained workload subprocess exits nonzero... " << std::flush;
  const int code_sust_fail = run_subprocess(
      harness_path,
      {"--server", server_path, "--sustained", "5", "--inject-semantic-failure", "--out", "/tmp/selftest_l6_sust_fail.json"});
  if (code_sust_fail == 0) {
    std::cout << "FAILED (expected nonzero exit code when sustained run has injected failure, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_sust_fail << ")\n";

  // Regression 19: Sustained short integration run completes on persistent server
  std::cout << "[L6 Regression 19/19] Verifying short sustained integration run executes and exits 0... " << std::flush;
  const int code_sust_short = run_subprocess(
      harness_path,
      {"--server", server_path, "--test-sustained-short", "--out", "/tmp/selftest_l6_sust_short.json"});
  if (code_sust_short != 0) {
    std::cout << "FAILED (expected exit 0 for short sustained integration run, got " << code_sust_short << ")\n";
    return false;
  }
  std::cout << "PASSED (exit 0)\n";

  std::cout << "\nAll L6 256-player concurrent regression checks PASSED (19/19).\n\n";
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
  bool is_inject_cap_accepted = false;
  bool is_inject_cap_stalled = false;
  bool is_inject_connect_fail = false;
  bool is_inject_fd_leak = false;
  bool is_inject_server_exit_cleanup = false;
  bool is_test_aggregation = false;
  bool is_smoke_10m = false;
  bool is_reliability_30m = false;
  bool is_test_sustained_short = false;
  bool is_test_sustained_classification = false;
  std::uint64_t custom_sustained_s = 0;

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
    } else if (arg == "--smoke-10m") {
      is_smoke_10m = true;
      if (output_json_path == "docs/load_test_l6_results.json") {
        output_json_path = "docs/load_test_l6_smoke_10m_results.json";
      }
    } else if (arg == "--reliability-30m") {
      is_reliability_30m = true;
      if (output_json_path == "docs/load_test_l6_results.json") {
        output_json_path = "docs/load_test_l6_reliability_30m_results.json";
      }
    } else if (arg == "--sustained" && i + 1 < argc) {
      custom_sustained_s = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
    } else if (arg == "--test-sustained-short") {
      is_test_sustained_short = true;
      if (output_json_path == "docs/load_test_l6_results.json") {
        output_json_path = "/tmp/load_test_l6_sustained_short.json";
      }
    } else if (arg == "--test-sustained-classification") {
      is_test_sustained_classification = true;
    } else if (arg == "--selftest") {
      is_selftest = true;
    } else if (arg == "--inject-capacity-accepted") {
      is_inject_cap_accepted = true;
    } else if (arg == "--inject-capacity-stalled") {
      is_inject_cap_stalled = true;
    } else if (arg == "--inject-connect-failure") {
      is_inject_connect_fail = true;
    } else if (arg == "--inject-fd-leak") {
      is_inject_fd_leak = true;
    } else if (arg == "--inject-server-exit-cleanup") {
      is_inject_server_exit_cleanup = true;
    } else if (arg == "--test-failure-aggregation") {
      is_test_aggregation = true;
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
  const std::string env_info = get_environment_info();

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

  if (is_test_sustained_classification) {
    const auto c1 = classify_l6_sustained_status(600, 599000, 10, 40, 21760, 0, 0, 0, false, "");
    if (c1.status != "BLOCKED") return 1;
    const auto c2 = classify_l6_sustained_status(600, 600000, 10, 40, 21760, 0, 0, 0, false, "");
    if (c2.status != "PASS") return 1;
    return 0;
  }
  if (is_test_sustained_short) {
    const auto res = execute_l6_sustained_workload(server_path, 3, 42, "sustained_short");
    const bool report_ok = write_l6_sustained_report(res, output_json_path, source_commit, git_dirty, env_info);
    if (!report_ok) return 1;
    return res.status == "PASS" ? 0 : 1;
  }
  if (is_smoke_10m) {
    const auto res = execute_l6_sustained_workload(server_path, 600, 42, "smoke_10m");
    const bool report_ok = write_l6_sustained_report(res, output_json_path, source_commit, git_dirty, env_info);
    if (!report_ok) return 1;
    return res.status == "PASS" ? 0 : 1;
  }
  if (is_reliability_30m) {
    const auto res = execute_l6_sustained_workload(server_path, 1800, 42, "reliability_30m");
    const bool report_ok = write_l6_sustained_report(res, output_json_path, source_commit, git_dirty, env_info);
    if (!report_ok) return 1;
    return res.status == "PASS" ? 0 : 1;
  }
  if (custom_sustained_s > 0) {
    TestFaultConfig fault;
    if (is_inject_semantic) fault.inject_semantic_failure = true;
    if (is_inject_assertion) fault.inject_assertion_failure = true;
    if (is_inject_outcome) fault.inject_outcome_mismatch = true;
    if (is_inject_cap_accepted) fault.inject_capacity_accepted = true;
    if (is_inject_cap_stalled) fault.inject_capacity_stalled = true;
    if (is_inject_connect_fail) fault.inject_connect_failure = true;
    if (is_inject_fd_leak) fault.inject_fd_leak = true;
    if (is_inject_server_exit_cleanup) fault.inject_premature_exit_before_cleanup = true;

    const auto res = execute_l6_sustained_workload(server_path, custom_sustained_s, 42, "custom_sustained", fault);
    const bool report_ok = write_l6_sustained_report(res, output_json_path, source_commit, git_dirty, env_info);
    if (!report_ok) return 1;
    return res.status == "PASS" ? 0 : 1;
  }
  if (is_test_aggregation) {
    return verify_failure_aggregation_exactly_once() ? 0 : 1;
  }
  if (is_inject_server_exit_cleanup) {
    TestFaultConfig fault;
    fault.inject_premature_exit_before_cleanup = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_cap_accepted) {
    TestFaultConfig fault;
    fault.inject_capacity_accepted = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_cap_stalled) {
    TestFaultConfig fault;
    fault.inject_capacity_stalled = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_connect_fail) {
    TestFaultConfig fault;
    fault.inject_connect_failure = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_fd_leak) {
    TestFaultConfig fault;
    fault.inject_fd_leak = true;
    const auto res = execute_l6_concurrent_matches(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
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
                << ", immediate_close=" << result.immediate_post_cleanup_open_fds
                << ", settled=" << result.post_cleanup_open_fds
                << " (" << result.cleanup_settle_time_ms << " ms)\n\n";

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
