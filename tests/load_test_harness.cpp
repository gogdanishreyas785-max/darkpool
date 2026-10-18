#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests requires Linux (epoll/sockets)\n";
  return 1;
}

#else

#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::json;

#ifndef DARKPOOL_SOURCE_COMMIT
#define DARKPOOL_SOURCE_COMMIT "unknown"
#endif

struct FailureCounts {
  std::size_t transport_failures{0};
  std::size_t correctness_failures{0};

  [[nodiscard]] std::size_t total() const noexcept {
    return transport_failures + correctness_failures;
  }
};

struct RequestMetric {
  std::string type;
  std::uint64_t latency_us{0};
  bool is_concurrent{false};
  bool transport_ok{false};
  bool correctness_ok{false};
};

struct PhaseMetrics {
  std::size_t total_requests{0};
  std::size_t successful_requests{0};
  std::size_t transport_failures{0};
  std::size_t correctness_failures{0};
  double p50_ms{0.0};
  double p95_ms{0.0};
  double p99_ms{0.0};
  double mean_ms{0.0};
  double min_ms{0.0};
  double max_ms{0.0};
};

struct RunResult {
  int run_index{0};
  std::size_t player_count{0};
  std::uint64_t duration_ms{0};
  bool run_passed{false};
  FailureCounts failures;
  PhaseMetrics sequential;
  PhaseMetrics concurrent;
  PhaseMetrics total;
  double cpu_time_ms{0.0};
  std::uint64_t peak_rss_kb{0};
  std::string final_phase;
  std::string final_outcome;
  std::vector<RequestMetric> metrics;
};

bool send_all(int fd, const char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto sent = send(fd, data + offset, size - offset, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (sent == 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

bool receive_exact(int fd, char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto received = recv(fd, data + offset, size - offset, 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (received == 0) return false;
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

std::optional<json> timed_request(
    int fd,
    const json& payload,
    std::vector<RequestMetric>& out_metrics,
    FailureCounts& failures,
    bool is_concurrent = false) {
  const std::string req_type = payload.value("type", "unknown");
  const auto outgoing = Frame(payload).serialize();

  // Timing starts immediately before writing frame bytes to socket
  const auto start_time = std::chrono::steady_clock::now();

  if (!send_all(fd, outgoing.data(), outgoing.size())) {
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    failures.transport_failures++;
    out_metrics.push_back({req_type, us, is_concurrent, false, false});
    return std::nullopt;
  }

  while (true) {
    char header[4];
    if (!receive_exact(fd, header, sizeof(header))) {
      const auto end_time = std::chrono::steady_clock::now();
      const auto us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
      failures.transport_failures++;
      out_metrics.push_back({req_type, us, is_concurrent, false, false});
      return std::nullopt;
    }

    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
    if (length == 0U || length > darkpool::network::kMaxMessageSize) {
      const auto end_time = std::chrono::steady_clock::now();
      const auto us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
      failures.transport_failures++;
      out_metrics.push_back({req_type, us, is_concurrent, false, false});
      return std::nullopt;
    }

    std::string bytes(header, sizeof(header));
    bytes.resize(static_cast<std::size_t>(length) + sizeof(header));
    if (!receive_exact(fd, bytes.data() + sizeof(header), static_cast<std::size_t>(length))) {
      const auto end_time = std::chrono::steady_clock::now();
      const auto us = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
      failures.transport_failures++;
      out_metrics.push_back({req_type, us, is_concurrent, false, false});
      return std::nullopt;
    }

    // Timing ends immediately when complete response frame is received
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());

    std::size_t consumed = 0;
    try {
      const auto response = Frame::parse(bytes, consumed);
      if (!response || consumed != bytes.size()) {
        failures.transport_failures++;
        out_metrics.push_back({req_type, us, is_concurrent, false, false});
        return std::nullopt;
      }
      const auto resp_payload = response->payload();
      if (resp_payload.is_object()) {
        const auto t = resp_payload.value("type", "");
        if (t == "server_event" || t == "market_snapshot") {
          continue;
        }
      }
      // Successfully received and parsed frame over transport
      out_metrics.push_back({req_type, us, is_concurrent, true, true});
      return resp_payload;
    } catch (const std::exception&) {
      failures.transport_failures++;
      out_metrics.push_back({req_type, us, is_concurrent, false, false});
      return std::nullopt;
    }
  }
}

std::uint16_t reserve_ephemeral_port() {
  const int probe = socket(AF_INET, SOCK_STREAM, 0);
  if (probe < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  const bool bound = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  socklen_t address_length = sizeof(address);
  const bool named = bound && getsockname(probe, reinterpret_cast<sockaddr*>(&address), &address_length) == 0;
  (void)close(probe);
  if (!named) return 0U;
  return ntohs(address.sin_port);
}

int connect_loopback(std::uint16_t port, int max_attempts = 100) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
      timeval timeout{5, 0};
      (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
      return fd;
    }
    (void)close(fd);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return -1;
}

pid_t spawn_server(const std::string& server_path, std::uint16_t port, const std::string& token) {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    setenv("DARKPOOL_MODERATOR_TOKEN", token.c_str(), 1);
    const auto port_text = std::to_string(port);
    execl(server_path.c_str(), server_path.c_str(), port_text.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return child;
}

std::uint64_t sample_peak_rss_kb(pid_t pid) {
  std::ifstream status_file("/proc/" + std::to_string(pid) + "/status");
  if (!status_file.is_open()) return 0;
  std::string line;
  while (std::getline(status_file, line)) {
    if (line.rfind("VmHWM:", 0) == 0) {
      std::istringstream iss(line.substr(6));
      std::uint64_t val = 0;
      if (iss >> val) return val;
    }
  }
  return 0;
}

double sample_cpu_time_ms(pid_t pid) {
  std::ifstream stat_file("/proc/" + std::to_string(pid) + "/stat");
  if (!stat_file.is_open()) return 0.0;
  std::string line;
  if (!std::getline(stat_file, line)) return 0.0;
  const auto close_paren = line.rfind(')');
  if (close_paren == std::string::npos || close_paren + 2 >= line.size()) return 0.0;
  std::istringstream iss(line.substr(close_paren + 2));
  char state;
  int ppid, pgrp, session, tty_nr, tpgid;
  unsigned long flags, minflt, cminflt, majflt, cmajflt;
  unsigned long utime = 0, stime = 0;
  if (iss >> state >> ppid >> pgrp >> session >> tty_nr >> tpgid >> flags
          >> minflt >> cminflt >> majflt >> cmajflt >> utime >> stime) {
    const long ticks = sysconf(_SC_CLK_TCK);
    if (ticks > 0) {
      return (static_cast<double>(utime + stime) * 1000.0) / static_cast<double>(ticks);
    }
  }
  return 0.0;
}

std::string detect_git_commit() {
  const char* cmds[] = {
    "git rev-parse HEAD 2>/dev/null",
    "git -C /mnt/c/darkpool-antigravity/darkpool rev-parse HEAD 2>/dev/null",
    "git -C .. rev-parse HEAD 2>/dev/null"
  };
  for (const auto* cmd : cmds) {
    FILE* fp = popen(cmd, "r");
    if (fp) {
      char buf[64];
      if (fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        std::string s(buf);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (!s.empty()) return s;
      } else {
        pclose(fp);
      }
    }
  }
  return "unknown";
}

std::string get_environment_info() {
  struct utsname uts {};
  std::ostringstream oss;
  if (uname(&uts) == 0) {
    oss << uts.sysname << " " << uts.release << " (" << uts.machine << ")";
  } else {
    oss << "Linux (unknown release)";
  }
  std::ifstream cpuinfo("/proc/cpuinfo");
  if (cpuinfo.is_open()) {
    std::string line;
    while (std::getline(cpuinfo, line)) {
      if (line.rfind("model name", 0) == 0) {
        const auto colon = line.find(':');
        if (colon != std::string::npos && colon + 2 < line.size()) {
          oss << "; CPU: " << line.substr(colon + 2);
          break;
        }
      }
    }
  }
  return oss.str();
}

void compute_percentiles(const std::vector<std::uint64_t>& latencies_us, PhaseMetrics& out) {
  if (latencies_us.empty()) return;
  auto sorted = latencies_us;
  std::sort(sorted.begin(), sorted.end());
  out.min_ms = static_cast<double>(sorted.front()) / 1000.0;
  out.max_ms = static_cast<double>(sorted.back()) / 1000.0;
  const double sum = static_cast<double>(
      std::accumulate(sorted.begin(), sorted.end(), std::uint64_t{0}));
  out.mean_ms = (sum / static_cast<double>(sorted.size())) / 1000.0;

  const auto p50_idx = static_cast<std::size_t>(static_cast<double>(sorted.size()) * 0.50);
  const auto p95_idx = static_cast<std::size_t>(static_cast<double>(sorted.size()) * 0.95);
  const auto p99_idx = static_cast<std::size_t>(static_cast<double>(sorted.size()) * 0.99);

  out.p50_ms = static_cast<double>(sorted[std::min(p50_idx, sorted.size() - 1)]) / 1000.0;
  out.p95_ms = static_cast<double>(sorted[std::min(p95_idx, sorted.size() - 1)]) / 1000.0;
  out.p99_ms = static_cast<double>(sorted[std::min(p99_idx, sorted.size() - 1)]) / 1000.0;
}

// Configurable failure injection for regression testing
struct TestFaultConfig {
  bool inject_assertion_failure{false};
  bool inject_outcome_mismatch{false};
};

RunResult execute_16_player_match(
    const std::string& server_path,
    int run_index,
    std::size_t player_count = 16,
    const TestFaultConfig& fault_cfg = {}) {
  RunResult result;
  result.run_index = run_index;
  result.player_count = player_count;

  const std::string mod_token = "load-test-mod-token-" + std::to_string(run_index);
  const std::uint16_t port = reserve_ephemeral_port();
  if (port == 0) {
    std::cerr << "Run " << run_index << ": Failed to reserve ephemeral port\n";
    result.failures.transport_failures++;
    return result;
  }

  const auto server_pid = spawn_server(server_path, port, mod_token);
  if (server_pid <= 0) {
    std::cerr << "Run " << run_index << ": Failed to spawn server\n";
    result.failures.transport_failures++;
    return result;
  }

  std::vector<int> player_fds;
  int mod_fd = -1;

  auto cleanup = [&]() {
    for (const auto fd : player_fds) {
      if (fd >= 0) (void)close(fd);
    }
    player_fds.clear();
    if (mod_fd >= 0) {
      (void)close(mod_fd);
      mod_fd = -1;
    }
  };

  const auto run_start = std::chrono::steady_clock::now();

  // Helper macro to verify correctness assertions fatalling
  #define VERIFY(cond, err_msg) \
    do { \
      if (!(cond)) { \
        std::cerr << "Run " << run_index << " CORRECTNESS ERROR: " << (err_msg) \
                  << " at line " << __LINE__ << '\n'; \
        result.failures.correctness_failures++; \
      } \
    } while (0)

  // 1. Establish Moderator Connection
  mod_fd = connect_loopback(port);
  if (mod_fd < 0) {
    std::cerr << "Run " << run_index << " TRANSPORT ERROR: Failed to connect moderator\n";
    result.failures.transport_failures++;
    cleanup();
    (void)kill(server_pid, SIGTERM);
    (void)waitpid(server_pid, nullptr, 0);
    return result;
  }

  const auto mod_hello = timed_request(
      mod_fd,
      json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
      result.metrics,
      result.failures);
  VERIFY(mod_hello && (*mod_hello)["type"] == "connect" && (*mod_hello)["role"] == "moderator",
         "Moderator handshake failed");

  // 2. Establish 16 Player Connections
  for (std::size_t i = 0; i < player_count; ++i) {
    const int pfd = connect_loopback(port);
    if (pfd < 0) {
      std::cerr << "Run " << run_index << " TRANSPORT ERROR: Failed to connect player " << i << '\n';
      result.failures.transport_failures++;
      cleanup();
      (void)kill(server_pid, SIGTERM);
      (void)waitpid(server_pid, nullptr, 0);
      return result;
    }
    player_fds.push_back(pfd);
    const auto player_hello = timed_request(
        pfd,
        json{{"type", "connect"}, {"role", "player"}},
        result.metrics,
        result.failures);
    VERIFY(player_hello && (*player_hello)["type"] == "connect" && (*player_hello)["role"] == "player",
           "Player connect handshake response mismatch");
  }

  // 3. Create 16-Player Match
  const auto create_resp = timed_request(
      mod_fd,
      json{{"type", "create_match"}, {"player_count", player_count}},
      result.metrics,
      result.failures);
  VERIFY(create_resp && (*create_resp)["type"] == "match_created" &&
             (*create_resp)["player_count"] == player_count && create_resp->contains("match_id"),
         "Failed to create match");

  const auto match_id = (create_resp && create_resp->contains("match_id"))
                            ? (*create_resp)["match_id"].get<std::uint64_t>()
                            : 1U;

  // Verify Role Counts from match_created using DEC-02 formulas
  const std::size_t expected_insiders = (player_count + 7) / 8;
  const std::size_t expected_investigators = (player_count + 15) / 16;
  const std::size_t expected_employees = player_count - expected_insiders - expected_investigators;
  VERIFY(create_resp && (*create_resp)["insiders"] == expected_insiders &&
             (*create_resp)["investigators"] == expected_investigators && (*create_resp)["employees"] == expected_employees,
         "Roster role counts mismatch for player count in match_created");

  // Role tracking
  std::vector<int> insider_fds;
  int investigator_fd = -1;
  std::uint32_t employee_target = 0;

  // 4. Players Join Match (Sequential)
  for (std::size_t i = 0; i < player_fds.size(); ++i) {
    const auto join_resp = timed_request(
        player_fds[i],
        json{{"type", "join_match"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(join_resp && (*join_resp)["type"] == "joined_match" &&
               (*join_resp)["player_id"] == i + 1U,
           "Player failed to join match");

    if (join_resp && join_resp->contains("private_role")) {
      const auto role = (*join_resp)["private_role"].get<std::string>();
      if (role == "Insider") {
        insider_fds.push_back(player_fds[i]);
      } else if (role == "Investigator") {
        investigator_fd = player_fds[i];
      } else if (role == "Employee" && employee_target == 0U) {
        employee_target = static_cast<std::uint32_t>(i + 1U);
      }
    }
  }

  // Verify Joined Role Distribution
  VERIFY(insider_fds.size() == expected_insiders, "Expected correct number of Insiders");
  VERIFY(investigator_fd >= 0, "Expected at least 1 Investigator");
  VERIFY(employee_target > 0U, "Expected at least 1 Employee target");

  // Injected fault test: simulate assertion failure if configured
  if (fault_cfg.inject_assertion_failure) {
    VERIFY(false, "Simulated injected assertion failure for regression testing");
  }

  // 5. Start Match
  const auto start_resp = timed_request(
      mod_fd,
      json{{"type", "start_match"}, {"match_id", match_id}},
      result.metrics,
      result.failures);
  VERIFY(start_resp && (*start_resp)["type"] == "transition_result" &&
             (*start_resp)["phase"] == "AwaitingEvent",
         "Failed to start match or phase is not AwaitingEvent");

  // 6. Execute All 3 Rounds
  // Round 1: Sequential requests (Smoke test)
  // Round 2: Concurrent requests burst (Bounded concurrency: 16 simultaneous client threads)
  // Round 3: Sequential requests (Resolution and final state)
  for (std::uint32_t round = 1; round <= 3U; ++round) {
    const bool is_concurrent_round = (round == 2U);

    // 6a. Publish Event
    const int event_impact = (round == 1U) ? 1 : (round == 2U) ? 0 : -1;
    const auto event_resp = timed_request(
        mod_fd,
        json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", event_impact}},
        result.metrics,
        result.failures);
    VERIFY(event_resp && (*event_resp)["type"] == "transition_result" &&
               (*event_resp)["phase"] == "Discussion",
           "Publish event failed or phase not Discussion");

    // 6b. Discussion & Insider Hints
    for (const auto ifd : insider_fds) {
      const auto hint_resp = timed_request(
          ifd,
          json{{"type", "insider_hint"}, {"match_id", match_id}},
          result.metrics,
          result.failures);
      VERIFY(hint_resp && (*hint_resp)["type"] == "hint_private" &&
                 (*hint_resp)["event_impact"] == event_impact,
             "Insider hint response mismatch or incorrect impact");
    }

    // 6c. Open Trading
    const auto trade_phase = timed_request(
        mod_fd,
        json{{"type", "open_trading"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(trade_phase && (*trade_phase)["type"] == "transition_result" &&
               (*trade_phase)["phase"] == "Trading",
           "Open trading failed or phase not Trading");

    // 6d. Player Trades
    if (is_concurrent_round) {
      // CONCURRENT REQUEST PHASE: 16 simultaneous client threads sending legal trades
      std::vector<std::thread> threads;
      std::vector<RequestMetric> conc_metrics(player_fds.size());
      std::vector<int> conc_success(player_fds.size(), 0);
      std::vector<FailureCounts> thread_failures(player_fds.size());

      const std::size_t num_sells = std::max<std::size_t>(1U, player_count / 4);
      const std::size_t num_buys = std::max<std::size_t>(1U, player_count / 4);

      for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
        threads.emplace_back([&, seat]() {
          std::string action = (seat < num_sells) ? "SELL" : (seat < num_sells + num_buys) ? "BUY" : "HOLD";
          std::vector<RequestMetric> local_metrics;
          const auto tr = timed_request(
              player_fds[seat],
              json{{"type", "trade"}, {"match_id", match_id}, {"action", action}},
              local_metrics,
              thread_failures[seat],
              true);
          if (tr && (*tr)["type"] == "trade_result" && (*tr)["ok"] == true) {
            conc_success[seat] = 1;
          } else {
            std::cerr << "Seat " << seat << " trade failure: "
                      << (tr ? tr->dump() : "nullopt (transport failure)") << '\n';
          }
          if (!local_metrics.empty()) {
            conc_metrics[seat] = local_metrics[0];
          }
        });
      }

      for (auto& t : threads) {
        t.join();
      }

      for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
        result.metrics.push_back(conc_metrics[seat]);
        result.failures.transport_failures += thread_failures[seat].transport_failures;
        result.failures.correctness_failures += thread_failures[seat].correctness_failures;
        VERIFY(conc_success[seat] == 1, "Concurrent trade rejected or failed");
      }
    } else {
      // SEQUENTIAL TRADES
      for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
        std::string action = "HOLD";
        if (round == 1U) {
          action = (seat < player_count / 2) ? "BUY" : "HOLD";
        } else {
          action = "HOLD";
        }
        const auto tr = timed_request(
            player_fds[seat],
            json{{"type", "trade"}, {"match_id", match_id}, {"action", action}},
            result.metrics,
            result.failures);
        VERIFY(tr && (*tr)["type"] == "trade_result" && (*tr)["ok"] == true,
               "Sequential trade failed or was rejected");
      }
    }

    // 6e. Resolve Market
    const auto market_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_market"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(market_resp && (*market_resp)["type"] == "market_result" &&
               (*market_resp)["ok"] == true,
           "Resolve market failed");

    // 6f. Clue Phase (Round 1)
    if (round == 1U && investigator_fd >= 0 && employee_target > 0U) {
      const auto clue_resp = timed_request(
          investigator_fd,
          json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", employee_target}},
          result.metrics,
          result.failures);
      VERIFY(clue_resp && (*clue_resp)["type"] == "clue_private" &&
                 (*clue_resp)["is_insider"] == false,
             "Investigator clue request failed or is_insider is true for employee");
    }

    // 6g. Open Voting
    const auto vote_phase = timed_request(
        mod_fd,
        json{{"type", "open_voting"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(vote_phase && (*vote_phase)["type"] == "transition_result" &&
               (*vote_phase)["phase"] == "Voting",
           "Open voting failed or phase not Voting");

    // 6h. Player Votes
    if (is_concurrent_round) {
      // CONCURRENT VOTES: 16 simultaneous client threads sending legal abstain votes
      std::vector<std::thread> threads;
      std::vector<RequestMetric> conc_metrics(player_fds.size());
      std::vector<int> conc_success(player_fds.size(), 0);
      std::vector<FailureCounts> thread_failures(player_fds.size());

      for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
        threads.emplace_back([&, seat]() {
          std::vector<RequestMetric> local_metrics;
          const auto vr = timed_request(
              player_fds[seat],
              json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
              local_metrics,
              thread_failures[seat],
              true);
          if (vr && (*vr)["type"] == "vote_result" && (*vr)["accepted"] == true) {
            conc_success[seat] = 1;
          } else {
            std::cerr << "Seat " << seat << " vote failure: "
                      << (vr ? vr->dump() : "nullopt (transport failure)") << '\n';
          }
          if (!local_metrics.empty()) {
            conc_metrics[seat] = local_metrics[0];
          }
        });
      }

      for (auto& t : threads) {
        t.join();
      }

      for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
        result.metrics.push_back(conc_metrics[seat]);
        result.failures.transport_failures += thread_failures[seat].transport_failures;
        result.failures.correctness_failures += thread_failures[seat].correctness_failures;
        VERIFY(conc_success[seat] == 1, "Concurrent vote rejected or failed");
      }
    } else {
      // SEQUENTIAL VOTES
      for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
        const auto vr = timed_request(
            player_fds[seat],
            json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
            result.metrics,
            result.failures);
        VERIFY(vr && (*vr)["type"] == "vote_result" && (*vr)["accepted"] == true,
               "Sequential vote rejected or failed");
      }
    }

    // 6i. Resolve Votes
    const auto resolve_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(resolve_resp && (*resolve_resp)["type"] == "vote_resolution" &&
               (*resolve_resp)["ok"] == true,
           "Resolve votes failed");
  }

  // 7. Verify Final Match State & Guaranteed Winner
  const auto status_resp = timed_request(
      player_fds[0],
      json{{"type", "match_status"}, {"match_id", match_id}},
      result.metrics,
      result.failures);

  VERIFY(status_resp && (*status_resp)["type"] == "match_status",
         "Failed to retrieve final match status");

  if (status_resp) {
    result.final_phase = status_resp->value("phase", "unknown");
    result.final_outcome = status_resp->value("outcome", "unknown");
    const auto health = status_resp->value("health", 0);

    // Assert that the match legitimately finished
    VERIFY(result.final_phase == "Finished", "Expected match phase to be Finished");

    // For our deterministic workload:
    // Company health remains > 0 (final health = 6).
    // All players abstained, so no players were eliminated (both Insiders survive).
    // Round 3 ends -> Rule mandates Outcome::InsiderWin.
    const std::string expected_winner = fault_cfg.inject_outcome_mismatch
                                            ? "EmployeesWin"
                                            : "InsiderWin";
    VERIFY(result.final_outcome == expected_winner,
           "Final outcome mismatch: expected " + expected_winner + " but got " + result.final_outcome);
    VERIFY(health > 0, "Expected positive company health at match end");
  }

  #undef VERIFY

  const auto run_end = std::chrono::steady_clock::now();
  result.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(run_end - run_start).count());

  // Sample Peak RSS and CPU from /proc before stopping server
  result.peak_rss_kb = sample_peak_rss_kb(server_pid);
  result.cpu_time_ms = sample_cpu_time_ms(server_pid);

  // 8. Graceful Teardown
  cleanup();

  (void)kill(server_pid, SIGTERM);
  int wait_status = 0;
  (void)waitpid(server_pid, &wait_status, 0);

  // Fallback to rusage if proc wasn't available
  if (result.cpu_time_ms == 0.0) {
    struct rusage usage {};
    if (getrusage(RUSAGE_CHILDREN, &usage) == 0) {
      const double user_ms = static_cast<double>(usage.ru_utime.tv_sec) * 1000.0 +
                             static_cast<double>(usage.ru_utime.tv_usec) / 1000.0;
      const double sys_ms = static_cast<double>(usage.ru_stime.tv_sec) * 1000.0 +
                            static_cast<double>(usage.ru_stime.tv_usec) / 1000.0;
      result.cpu_time_ms = user_ms + sys_ms;
      if (result.peak_rss_kb == 0 && usage.ru_maxrss > 0) {
        result.peak_rss_kb = static_cast<std::uint64_t>(usage.ru_maxrss);
      }
    }
  }

  // 9. Aggregate Latency Metrics Separately for Sequential and Concurrent
  std::vector<std::uint64_t> seq_latencies;
  std::vector<std::uint64_t> conc_latencies;
  std::vector<std::uint64_t> all_latencies;

  for (const auto& m : result.metrics) {
    all_latencies.push_back(m.latency_us);
    result.total.total_requests++;
    if (m.is_concurrent) {
      result.concurrent.total_requests++;
      if (m.transport_ok && m.correctness_ok) {
        result.concurrent.successful_requests++;
        conc_latencies.push_back(m.latency_us);
      } else {
        if (!m.transport_ok) result.concurrent.transport_failures++;
        if (!m.correctness_ok) result.concurrent.correctness_failures++;
      }
    } else {
      result.sequential.total_requests++;
      if (m.transport_ok && m.correctness_ok) {
        result.sequential.successful_requests++;
        seq_latencies.push_back(m.latency_us);
      } else {
        if (!m.transport_ok) result.sequential.transport_failures++;
        if (!m.correctness_ok) result.sequential.correctness_failures++;
      }
    }
  }

  result.total.successful_requests = result.sequential.successful_requests + result.concurrent.successful_requests;
  result.total.transport_failures = result.failures.transport_failures;
  result.total.correctness_failures = result.failures.correctness_failures;

  compute_percentiles(seq_latencies, result.sequential);
  compute_percentiles(conc_latencies, result.concurrent);
  compute_percentiles(all_latencies, result.total);

  result.run_passed = (result.failures.total() == 0);
  return result;
}

// Execute harness in a subprocess and return its exit code
int run_subprocess(const std::string& binary, const std::vector<std::string>& args) {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    const int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
      close(dev_null);
    }
    std::vector<char*> c_args;
    c_args.push_back(const_cast<char*>(binary.c_str()));
    for (const auto& a : args) {
      c_args.push_back(const_cast<char*>(a.c_str()));
    }
    c_args.push_back(nullptr);
    execv(binary.c_str(), c_args.data());
    _exit(127);
  }
  int status = 0;
  if (waitpid(child, &status, 0) != child) return -1;
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  if (WIFSIGNALED(status)) {
    return 128 + WTERMSIG(status);
  }
  return -1;
}

std::string get_self_executable_path(const char* argv0) {
  char buf[4096];
  const auto len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    return std::string(buf);
  }
  return std::string(argv0 ? argv0 : "./darkpool_load_tests");
}

// Regression tests that prove assertion failures and invalid final outcomes result in a non-zero exit status
// via real subprocess execution
bool run_regression_selftest(const std::string& harness_path, const std::string& server_path) {
  std::cout << "=================================================================\n";
  std::cout << " Running Load Test Harness Subprocess Regression Tests\n";
  std::cout << "=================================================================\n";

  // Test 1: Normal 16-player run subprocess must exit 0
  std::cout << "[Regression 1/3] Verifying normal run subprocess exits 0... " << std::flush;
  const int code_normal = run_subprocess(harness_path, {"--server", server_path, "--smoke-run"});
  if (code_normal != 0) {
    std::cout << "FAILED (expected exit code 0, got " << code_normal << ")\n";
    return false;
  }
  std::cout << "PASSED (exit code 0)\n";

  // Test 2: Injected assertion failure subprocess must exit nonzero
  std::cout << "[Regression 2/3] Verifying assertion failure subprocess exits nonzero... " << std::flush;
  const int code_assert = run_subprocess(harness_path, {"--server", server_path, "--inject-assertion-failure"});
  if (code_assert == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_assert << ")\n";

  // Test 3: Injected outcome mismatch subprocess must exit nonzero
  std::cout << "[Regression 3/3] Verifying outcome mismatch subprocess exits nonzero... " << std::flush;
  const int code_outcome = run_subprocess(harness_path, {"--server", server_path, "--inject-outcome-mismatch"});
  if (code_outcome == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_outcome << ")\n";

  std::cout << "\nAll regression checks PASSED: real subprocess exit statuses verified.\n\n";
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string server_path = "./darkpool_server";
  std::string output_json_path = "load_test_results.json";
  std::string source_commit = DARKPOOL_SOURCE_COMMIT;
  int repeat_runs = 3;
  std::size_t player_count = 16;
  bool is_selftest = false;
  bool is_smoke_run = false;
  bool is_inject_assertion = false;
  bool is_inject_outcome = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--server" && i + 1 < argc) {
      server_path = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      output_json_path = argv[++i];
    } else if (arg == "--runs" && i + 1 < argc) {
      repeat_runs = std::max(1, std::atoi(argv[++i]));
    } else if (arg == "--players" && i + 1 < argc) {
      player_count = static_cast<std::size_t>(std::max(3, std::atoi(argv[++i])));
    } else if (arg == "--commit" && i + 1 < argc) {
      source_commit = argv[++i];
    } else if (arg == "--selftest") {
      is_selftest = true;
    } else if (arg == "--smoke-run") {
      is_smoke_run = true;
    } else if (arg == "--inject-assertion-failure") {
      is_inject_assertion = true;
    } else if (arg == "--inject-outcome-mismatch") {
      is_inject_outcome = true;
    } else if (!arg.starts_with("--") && i == 1) {
      server_path = argv[1];
    }
  }

  if (source_commit == "unknown") {
    const auto detected = detect_git_commit();
    if (detected != "unknown") {
      source_commit = detected;
    }
  }

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
    std::cerr << "Error: server executable not found or not executable at '"
              << server_path << "'\n";
    return 1;
  }

  if (is_smoke_run) {
    const auto res = execute_16_player_match(server_path, 1, player_count, {});
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_assertion) {
    TestFaultConfig fault;
    fault.inject_assertion_failure = true;
    const auto res = execute_16_player_match(server_path, 1, player_count, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_outcome) {
    TestFaultConfig fault;
    fault.inject_outcome_mismatch = true;
    const auto res = execute_16_player_match(server_path, 1, player_count, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_selftest) {
    const auto self_exe = get_self_executable_path(argv[0]);
    return run_regression_selftest(self_exe, server_path) ? 0 : 1;
  }

  const std::string env_info = get_environment_info();

  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL Real-Server Load Test Harness (16-Player Match)\n";
  std::cout << "=================================================================\n";
  std::cout << " Server Path  : " << server_path << '\n';
  std::cout << " Source Commit: " << source_commit << '\n';
  std::cout << " Environment  : " << env_info << '\n';
  std::cout << " Player Count : " << player_count << " (+ 1 moderator = " << (player_count + 1) << " clients)\n";
  std::cout << " Planned Runs : " << repeat_runs << '\n';
  std::cout << " Output JSON  : " << output_json_path << '\n';
  std::cout << " Timing Basis : Monotonic clock (std::chrono::steady_clock)\n";
  std::cout << " ACK Boundary : Frame send initiation -> complete response parse\n";
  std::cout << " Phases       : Sequential 3-round smoke + 16-client concurrent burst (Round 2)\n";
  std::cout << " Disclaimer   : Establishes client concurrency; does NOT claim sustained capacity\n";
  std::cout << "=================================================================\n\n";

  std::vector<RunResult> results;
  results.reserve(static_cast<std::size_t>(repeat_runs));

  bool all_runs_passed = true;

  for (int r = 1; r <= repeat_runs; ++r) {
    std::cout << "[Run " << r << "/" << repeat_runs << "] Executing " << player_count << "-player match... " << std::flush;
    const auto res = execute_16_player_match(server_path, r, player_count);
    results.push_back(res);

    if (!res.run_passed) {
      all_runs_passed = false;
      std::cout << "FAILED (Transport: " << res.failures.transport_failures
                << ", Correctness: " << res.failures.correctness_failures << ")\n";
    } else {
      std::cout << "PASSED in " << res.duration_ms << " ms (Winner: " << res.final_outcome << ")\n";
      std::cout << "         Sequential : " << res.sequential.successful_requests << " reqs, "
                << "p50=" << std::fixed << std::setprecision(2) << res.sequential.p50_ms << " ms, "
                << "p95=" << res.sequential.p95_ms << " ms, "
                << "p99=" << res.sequential.p99_ms << " ms\n";
      std::cout << "         Concurrent : " << res.concurrent.successful_requests << " reqs, "
                << "p50=" << std::fixed << std::setprecision(2) << res.concurrent.p50_ms << " ms, "
                << "p95=" << res.concurrent.p95_ms << " ms, "
                << "p99=" << res.concurrent.p99_ms << " ms\n";
      std::cout << "         Resource   : Peak RSS=" << res.peak_rss_kb << " kB, CPU="
                << std::fixed << std::setprecision(1) << res.cpu_time_ms << " ms\n\n";
    }
  }

  // Generate aggregate metrics
  std::vector<std::uint64_t> all_seq_latencies;
  std::vector<std::uint64_t> all_conc_latencies;
  std::vector<std::uint64_t> all_total_latencies;
  std::size_t total_trans_fails = 0;
  std::size_t total_corr_fails = 0;
  std::uint64_t total_wall_time = 0;

  for (const auto& r : results) {
    total_trans_fails += r.failures.transport_failures;
    total_corr_fails += r.failures.correctness_failures;
    total_wall_time += r.duration_ms;
    for (const auto& m : r.metrics) {
      all_total_latencies.push_back(m.latency_us);
      if (m.is_concurrent) {
        if (m.transport_ok && m.correctness_ok) all_conc_latencies.push_back(m.latency_us);
      } else {
        if (m.transport_ok && m.correctness_ok) all_seq_latencies.push_back(m.latency_us);
      }
    }
  }

  PhaseMetrics agg_seq{};
  PhaseMetrics agg_conc{};
  PhaseMetrics agg_total{};
  compute_percentiles(all_seq_latencies, agg_seq);
  compute_percentiles(all_conc_latencies, agg_conc);
  compute_percentiles(all_total_latencies, agg_total);

  agg_seq.total_requests = all_seq_latencies.size();
  agg_seq.successful_requests = all_seq_latencies.size();
  agg_conc.total_requests = all_conc_latencies.size();
  agg_conc.successful_requests = all_conc_latencies.size();
  agg_total.total_requests = all_total_latencies.size();
  agg_total.successful_requests = agg_seq.successful_requests + agg_conc.successful_requests;
  agg_total.transport_failures = total_trans_fails;
  agg_total.correctness_failures = total_corr_fails;

  // Print Markdown Results Table
  std::cout << "=================================================================\n";
  std::cout << " SUMMARY RESULTS (Three Repeatable " << player_count << "-Player Runs)\n";
  std::cout << "=================================================================\n";
  std::cout << "| Run | Clients | Duration | Sequential (p50/p95/p99) | Concurrent (p50/p95/p99) | Failures (Trans/Corr) | Peak RSS | CPU |\n";
  std::cout << "|---|---|---|---|---|---|---|---|\n";
  for (const auto& r : results) {
    std::cout << "| Run " << r.run_index << " | " << (r.player_count + 1) << " | "
              << r.duration_ms << " ms | "
              << std::fixed << std::setprecision(2) << r.sequential.p50_ms << " / " << r.sequential.p95_ms << " / " << r.sequential.p99_ms << " ms | "
              << r.concurrent.p50_ms << " / " << r.concurrent.p95_ms << " / " << r.concurrent.p99_ms << " ms | "
              << r.failures.transport_failures << " / " << r.failures.correctness_failures << " | "
              << r.peak_rss_kb << " kB | "
              << std::fixed << std::setprecision(1) << r.cpu_time_ms << " ms |\n";
  }
  std::cout << "| Aggregate | " << (player_count + 1) << " | "
            << total_wall_time << " ms | "
            << std::fixed << std::setprecision(2) << agg_seq.p50_ms << " / " << agg_seq.p95_ms << " / " << agg_seq.p99_ms << " ms | "
            << agg_conc.p50_ms << " / " << agg_conc.p95_ms << " / " << agg_conc.p99_ms << " ms | "
            << total_trans_fails << " / " << total_corr_fails << " | - | - |\n\n";

  // Write Machine-Readable JSON Report
  json report;
  report["test_name"] = "darkpool_load_tests_" + std::to_string(player_count) + "p";
  report["status"] = all_runs_passed ? "PASS" : "FAIL";
  report["source_commit"] = source_commit;
  report["environment"] = env_info;
  report["player_count"] = player_count;
  report["client_count"] = player_count + 1;
  report["repeat_runs"] = repeat_runs;
  report["all_runs_passed"] = all_runs_passed;
  report["timestamp"] = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());

  report["sequential_summary"] = {
    {"total_requests", agg_seq.total_requests},
    {"successful_requests", agg_seq.successful_requests},
    {"p50_ms", agg_seq.p50_ms},
    {"p95_ms", agg_seq.p95_ms},
    {"p99_ms", agg_seq.p99_ms},
    {"mean_ms", agg_seq.mean_ms}
  };

  report["concurrent_summary"] = {
    {"total_requests", agg_conc.total_requests},
    {"successful_requests", agg_conc.successful_requests},
    {"p50_ms", agg_conc.p50_ms},
    {"p95_ms", agg_conc.p95_ms},
    {"p99_ms", agg_conc.p99_ms},
    {"mean_ms", agg_conc.mean_ms}
  };

  report["total_summary"] = {
    {"total_duration_ms", total_wall_time},
    {"total_requests", agg_total.total_requests},
    {"successful_requests", agg_total.successful_requests},
    {"transport_failures", total_trans_fails},
    {"correctness_failures", total_corr_fails},
    {"p50_ms", agg_total.p50_ms},
    {"p95_ms", agg_total.p95_ms},
    {"p99_ms", agg_total.p99_ms},
    {"mean_ms", agg_total.mean_ms}
  };

  json runs_json = json::array();
  for (const auto& r : results) {
    runs_json.push_back({
      {"run_index", r.run_index},
      {"duration_ms", r.duration_ms},
      {"run_passed", r.run_passed},
      {"transport_failures", r.failures.transport_failures},
      {"correctness_failures", r.failures.correctness_failures},
      {"final_phase", r.final_phase},
      {"final_outcome", r.final_outcome},
      {"sequential", {
        {"total_requests", r.sequential.total_requests},
        {"successful_requests", r.sequential.successful_requests},
        {"p50_ms", r.sequential.p50_ms},
        {"p95_ms", r.sequential.p95_ms},
        {"p99_ms", r.sequential.p99_ms},
        {"mean_ms", r.sequential.mean_ms}
      }},
      {"concurrent", {
        {"total_requests", r.concurrent.total_requests},
        {"successful_requests", r.concurrent.successful_requests},
        {"p50_ms", r.concurrent.p50_ms},
        {"p95_ms", r.concurrent.p95_ms},
        {"p99_ms", r.concurrent.p99_ms},
        {"mean_ms", r.concurrent.mean_ms}
      }},
      {"peak_rss_kb", r.peak_rss_kb},
      {"cpu_time_ms", r.cpu_time_ms}
    });
  }
  report["runs"] = runs_json;

  std::ofstream out_file(output_json_path);
  if (out_file.is_open()) {
    out_file << report.dump(2) << '\n';
    std::cout << "Machine-readable report written to: " << output_json_path << "\n\n";
  } else {
    std::cerr << "Warning: Could not open output file: " << output_json_path << '\n';
  }

  return all_runs_passed ? 0 : 1;
}

#endif  // !defined(__linux__)
