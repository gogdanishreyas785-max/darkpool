#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "darkpool_load_tests requires Linux\n";
  return 0;
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

struct RequestMetric {
  std::string type;
  std::uint64_t latency_us{0};
  bool success{false};
};

struct RunResult {
  int run_index{0};
  std::size_t player_count{0};
  std::uint64_t duration_ms{0};
  std::size_t total_requests{0};
  std::size_t successful_requests{0};
  std::size_t failed_requests{0};
  double p50_us{0.0};
  double p95_us{0.0};
  double p99_us{0.0};
  double mean_us{0.0};
  std::uint64_t min_us{0};
  std::uint64_t max_us{0};
  double cpu_time_ms{0.0};
  std::uint64_t peak_rss_kb{0};
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

std::optional<json> timed_request(int fd, const json& payload, std::vector<RequestMetric>& out_metrics) {
  const std::string req_type = payload.value("type", "unknown");
  const auto outgoing = Frame(payload).serialize();

  // Timing starts immediately before sending the serialized frame bytes
  const auto start_time = std::chrono::steady_clock::now();

  if (!send_all(fd, outgoing.data(), outgoing.size())) {
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    out_metrics.push_back({req_type, us, false});
    return std::nullopt;
  }

  char header[4];
  if (!receive_exact(fd, header, sizeof(header))) {
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    out_metrics.push_back({req_type, us, false});
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
    out_metrics.push_back({req_type, us, false});
    return std::nullopt;
  }

  std::string bytes(header, sizeof(header));
  bytes.resize(static_cast<std::size_t>(length) + sizeof(header));
  if (!receive_exact(fd, bytes.data() + sizeof(header), static_cast<std::size_t>(length))) {
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    out_metrics.push_back({req_type, us, false});
    return std::nullopt;
  }

  // Timing ends immediately after the complete correlated response frame is read
  const auto end_time = std::chrono::steady_clock::now();
  const auto us = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());

  std::size_t consumed = 0;
  try {
    const auto response = Frame::parse(bytes, consumed);
    if (!response || consumed != bytes.size()) {
      out_metrics.push_back({req_type, us, false});
      return std::nullopt;
    }
    out_metrics.push_back({req_type, us, true});
    return response->payload();
  } catch (const std::exception&) {
    out_metrics.push_back({req_type, us, false});
    return std::nullopt;
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
  FILE* fp = popen("git rev-parse HEAD 2>/dev/null", "r");
  if (!fp) return "unknown";
  char buf[64];
  if (fgets(buf, sizeof(buf), fp)) {
    pclose(fp);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
  }
  pclose(fp);
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

RunResult execute_16_player_match(
    const std::string& server_path,
    int run_index,
    std::size_t player_count = 16) {
  RunResult result;
  result.run_index = run_index;
  result.player_count = player_count;

  const std::string mod_token = "load-test-mod-token-" + std::to_string(run_index);
  const std::uint16_t port = reserve_ephemeral_port();
  if (port == 0) {
    std::cerr << "Run " << run_index << ": Failed to reserve ephemeral port\n";
    result.failed_requests++;
    return result;
  }

  const auto server_pid = spawn_server(server_path, port, mod_token);
  if (server_pid <= 0) {
    std::cerr << "Run " << run_index << ": Failed to spawn server\n";
    result.failed_requests++;
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

  // 1. Establish Moderator Connection
  mod_fd = connect_loopback(port);
  if (mod_fd < 0) {
    std::cerr << "Run " << run_index << ": Failed to connect moderator\n";
    cleanup();
    (void)kill(server_pid, SIGTERM);
    (void)waitpid(server_pid, nullptr, 0);
    result.failed_requests++;
    return result;
  }

  const auto mod_hello = timed_request(
      mod_fd,
      json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
      result.metrics);
  if (!mod_hello || (*mod_hello)["type"] != "connect" || (*mod_hello)["role"] != "moderator") {
    std::cerr << "Run " << run_index << ": Moderator handshake failed\n";
    cleanup();
    (void)kill(server_pid, SIGTERM);
    (void)waitpid(server_pid, nullptr, 0);
    return result;
  }

  // 2. Establish 16 Player Connections
  for (std::size_t i = 0; i < player_count; ++i) {
    const int pfd = connect_loopback(port);
    if (pfd < 0) {
      std::cerr << "Run " << run_index << ": Failed to connect player " << i << '\n';
      cleanup();
      (void)kill(server_pid, SIGTERM);
      (void)waitpid(server_pid, nullptr, 0);
      return result;
    }
    player_fds.push_back(pfd);
    const auto player_hello = timed_request(
        pfd,
        json{{"type", "connect"}, {"role", "player"}},
        result.metrics);
    if (!player_hello || (*player_hello)["type"] != "connect") {
      std::cerr << "Run " << run_index << ": Player " << i << " handshake failed\n";
      cleanup();
      (void)kill(server_pid, SIGTERM);
      (void)waitpid(server_pid, nullptr, 0);
      return result;
    }
  }

  // 3. Create 16-Player Match
  const auto create_resp = timed_request(
      mod_fd,
      json{{"type", "create_match"}, {"player_count", player_count}},
      result.metrics);
  if (!create_resp || (*create_resp)["type"] != "match_created" ||
      (*create_resp)["player_count"] != player_count) {
    std::cerr << "Run " << run_index << ": Failed to create match\n";
    cleanup();
    (void)kill(server_pid, SIGTERM);
    (void)waitpid(server_pid, nullptr, 0);
    return result;
  }
  const auto match_id = (*create_resp)["match_id"].get<std::uint64_t>();

  // Role expectations for N=16: Insiders=2, Investigators=1, Employees=13
  std::vector<int> insider_fds;
  int investigator_fd = -1;
  std::uint32_t employee_target = 0;

  // 4. Players Join Match
  for (std::size_t i = 0; i < player_fds.size(); ++i) {
    const auto join_resp = timed_request(
        player_fds[i],
        json{{"type", "join_match"}, {"match_id", match_id}},
        result.metrics);
    if (!join_resp || (*join_resp)["type"] != "joined_match") {
      std::cerr << "Run " << run_index << ": Player " << i << " failed to join match\n";
      cleanup();
      (void)kill(server_pid, SIGTERM);
      (void)waitpid(server_pid, nullptr, 0);
      return result;
    }
    if (join_resp->contains("private_role")) {
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

  if (insider_fds.size() != 2U || investigator_fd < 0 || employee_target == 0U) {
    std::cerr << "Run " << run_index << ": Unexpected role distribution at N=16\n";
    cleanup();
    (void)kill(server_pid, SIGTERM);
    (void)waitpid(server_pid, nullptr, 0);
    return result;
  }

  // 5. Start Match
  const auto start_resp = timed_request(
      mod_fd,
      json{{"type", "start_match"}, {"match_id", match_id}},
      result.metrics);
  if (!start_resp || (*start_resp)["type"] != "transition_result" ||
      (*start_resp)["phase"] != "AwaitingEvent") {
    std::cerr << "Run " << run_index << ": Failed to start match\n";
    cleanup();
    (void)kill(server_pid, SIGTERM);
    (void)waitpid(server_pid, nullptr, 0);
    return result;
  }

  // 6. Execute All 3 Rounds
  for (std::uint32_t round = 1; round <= 3U; ++round) {
    // 6a. Publish Event
    const int event_impact = (round == 1U) ? 1 : (round == 2U) ? 0 : -1;
    const auto event_resp = timed_request(
        mod_fd,
        json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", event_impact}},
        result.metrics);
    if (!event_resp || (*event_resp)["type"] != "transition_result" ||
        (*event_resp)["phase"] != "Discussion") {
      std::cerr << "Run " << run_index << ": Round " << round << " publish_event failed\n";
      break;
    }

    // 6b. Discussion & Insider Hints
    for (const auto ifd : insider_fds) {
      const auto hint_resp = timed_request(
          ifd,
          json{{"type", "insider_hint"}, {"match_id", match_id}},
          result.metrics);
      if (!hint_resp || (*hint_resp)["type"] != "hint_private" ||
          (*hint_resp)["event_impact"] != event_impact) {
        std::cerr << "Run " << run_index << ": Round " << round << " hint failed\n";
      }
    }

    // 6c. Open Trading
    const auto trade_phase = timed_request(
        mod_fd,
        json{{"type", "open_trading"}, {"match_id", match_id}},
        result.metrics);
    if (!trade_phase || (*trade_phase)["type"] != "transition_result" ||
        (*trade_phase)["phase"] != "Trading") {
      std::cerr << "Run " << run_index << ": Round " << round << " open_trading failed\n";
      break;
    }

    // 6d. Legal Player Trades
    for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
      std::string action = "HOLD";
      if (round == 1U) {
        action = (seat < 8U) ? "BUY" : "HOLD";
      } else if (round == 2U) {
        action = (seat < 4U) ? "SELL" : (seat < 8U) ? "BUY" : "HOLD";
      } else {
        action = "HOLD";
      }

      const auto tr = timed_request(
          player_fds[seat],
          json{{"type", "trade"}, {"match_id", match_id}, {"action", action}},
          result.metrics);
      if (!tr || (*tr)["type"] != "trade_result" || (*tr)["ok"] != true) {
        std::cerr << "Run " << run_index << ": Round " << round << " trade by seat "
                  << seat << " failed\n";
      }
    }

    // 6e. Resolve Market
    const auto market_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_market"}, {"match_id", match_id}},
        result.metrics);
    if (!market_resp || (*market_resp)["type"] != "market_result") {
      std::cerr << "Run " << run_index << ": Round " << round << " resolve_market failed\n";
      break;
    }

    // 6f. Clue Phase (Round 1)
    if (round == 1U && investigator_fd >= 0 && employee_target > 0U) {
      const auto clue_resp = timed_request(
          investigator_fd,
          json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", employee_target}},
          result.metrics);
      if (!clue_resp || (*clue_resp)["type"] != "clue_private" ||
          (*clue_resp)["is_insider"] != false) {
        std::cerr << "Run " << run_index << ": request_clue failed\n";
      }
    }

    // 6g. Open Voting
    const auto vote_phase = timed_request(
        mod_fd,
        json{{"type", "open_voting"}, {"match_id", match_id}},
        result.metrics);
    if (!vote_phase || (*vote_phase)["type"] != "transition_result" ||
        (*vote_phase)["phase"] != "Voting") {
      std::cerr << "Run " << run_index << ": Round " << round << " open_voting failed\n";
      break;
    }

    // 6h. Player Votes (Abstain)
    for (std::size_t seat = 0; seat < player_fds.size(); ++seat) {
      const auto vr = timed_request(
          player_fds[seat],
          json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
          result.metrics);
      if (!vr || (*vr)["type"] != "vote_result" || (*vr)["accepted"] != true) {
        std::cerr << "Run " << run_index << ": Round " << round << " vote by seat "
                  << seat << " failed\n";
      }
    }

    // 6i. Resolve Votes
    const auto resolve_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", match_id}},
        result.metrics);
    if (!resolve_resp || (*resolve_resp)["type"] != "vote_resolution") {
      std::cerr << "Run " << run_index << ": Round " << round << " resolve_votes failed\n";
      break;
    }
  }

  // 7. Verify Final Match State
  const auto status_resp = timed_request(
      player_fds[0],
      json{{"type", "match_status"}, {"match_id", match_id}},
      result.metrics);
  if (!status_resp || (*status_resp)["type"] != "match_status" ||
      (*status_resp)["phase"] != "Finished") {
    std::cerr << "Run " << run_index << ": Final match state verification failed\n";
  }

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

  // 9. Aggregate Latency Metrics
  std::vector<std::uint64_t> latencies;
  latencies.reserve(result.metrics.size());
  result.total_requests = result.metrics.size();
  result.successful_requests = 0;
  result.failed_requests = 0;

  for (const auto& m : result.metrics) {
    if (m.success) {
      result.successful_requests++;
      latencies.push_back(m.latency_us);
    } else {
      result.failed_requests++;
    }
  }

  if (!latencies.empty()) {
    std::sort(latencies.begin(), latencies.end());
    result.min_us = latencies.front();
    result.max_us = latencies.back();
    const double sum = static_cast<double>(
        std::accumulate(latencies.begin(), latencies.end(), std::uint64_t{0}));
    result.mean_us = sum / static_cast<double>(latencies.size());

    const auto p50_idx = static_cast<std::size_t>(static_cast<double>(latencies.size()) * 0.50);
    const auto p95_idx = static_cast<std::size_t>(static_cast<double>(latencies.size()) * 0.95);
    const auto p99_idx = static_cast<std::size_t>(static_cast<double>(latencies.size()) * 0.99);

    result.p50_us = static_cast<double>(latencies[std::min(p50_idx, latencies.size() - 1)]);
    result.p95_us = static_cast<double>(latencies[std::min(p95_idx, latencies.size() - 1)]);
    result.p99_us = static_cast<double>(latencies[std::min(p99_idx, latencies.size() - 1)]);
  }

  return result;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string server_path = "./darkpool_server";
  std::string output_json_path = "load_test_results.json";
  std::string source_commit = DARKPOOL_SOURCE_COMMIT;
  int repeat_runs = 3;
  std::size_t player_count = 16;

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
  std::cout << "=================================================================\n\n";

  std::vector<RunResult> results;
  results.reserve(static_cast<std::size_t>(repeat_runs));

  bool all_runs_ok = true;

  for (int r = 1; r <= repeat_runs; ++r) {
    std::cout << "[Run " << r << "/" << repeat_runs << "] Starting 16-player match... " << std::flush;
    const auto res = execute_16_player_match(server_path, r, player_count);
    results.push_back(res);

    if (res.failed_requests > 0 || res.successful_requests == 0) {
      all_runs_ok = false;
      std::cout << "FAILED (" << res.failed_requests << " failures)\n";
    } else {
      std::cout << "COMPLETED in " << res.duration_ms << " ms\n";
      std::cout << "         Requests: " << res.successful_requests << " ok, 0 failed\n";
      std::cout << "         Latency : p50=" << std::fixed << std::setprecision(2)
                << (res.p50_us / 1000.0) << " ms, p95="
                << (res.p95_us / 1000.0) << " ms, p99="
                << (res.p99_us / 1000.0) << " ms (mean="
                << (res.mean_us / 1000.0) << " ms)\n";
      std::cout << "         Resource: Peak RSS=" << res.peak_rss_kb << " kB, CPU="
                << std::fixed << std::setprecision(1) << res.cpu_time_ms << " ms\n\n";
    }
  }

  // Generate aggregate summary
  std::vector<std::uint64_t> all_latencies;
  std::size_t total_requests_all = 0;
  std::size_t successful_requests_all = 0;
  std::size_t failed_requests_all = 0;
  std::uint64_t total_duration_all = 0;

  for (const auto& res : results) {
    total_requests_all += res.total_requests;
    successful_requests_all += res.successful_requests;
    failed_requests_all += res.failed_requests;
    total_duration_all += res.duration_ms;
    for (const auto& m : res.metrics) {
      if (m.success) all_latencies.push_back(m.latency_us);
    }
  }

  double aggregate_p50_us = 0.0;
  double aggregate_p95_us = 0.0;
  double aggregate_p99_us = 0.0;
  double aggregate_mean_us = 0.0;

  if (!all_latencies.empty()) {
    std::sort(all_latencies.begin(), all_latencies.end());
    const double sum = static_cast<double>(
        std::accumulate(all_latencies.begin(), all_latencies.end(), std::uint64_t{0}));
    aggregate_mean_us = sum / static_cast<double>(all_latencies.size());
    const auto p50_idx = static_cast<std::size_t>(static_cast<double>(all_latencies.size()) * 0.50);
    const auto p95_idx = static_cast<std::size_t>(static_cast<double>(all_latencies.size()) * 0.95);
    const auto p99_idx = static_cast<std::size_t>(static_cast<double>(all_latencies.size()) * 0.99);
    aggregate_p50_us = static_cast<double>(all_latencies[std::min(p50_idx, all_latencies.size() - 1)]);
    aggregate_p95_us = static_cast<double>(all_latencies[std::min(p95_idx, all_latencies.size() - 1)]);
    aggregate_p99_us = static_cast<double>(all_latencies[std::min(p99_idx, all_latencies.size() - 1)]);
  }

  // Print Markdown Results Table
  std::cout << "=================================================================\n";
  std::cout << " SUMMARY RESULTS (Three Repeatable 16-Player Runs)\n";
  std::cout << "=================================================================\n";
  std::cout << "| Run | Clients | Duration | Requests (OK/Fail) | p50 (ms) | p95 (ms) | p99 (ms) | Peak RSS | CPU |\n";
  std::cout << "|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& r : results) {
    std::cout << "| Run " << r.run_index << " | " << (r.player_count + 1) << " | "
              << r.duration_ms << " ms | " << r.successful_requests << "/" << r.failed_requests << " | "
              << std::fixed << std::setprecision(2) << (r.p50_us / 1000.0) << " ms | "
              << (r.p95_us / 1000.0) << " ms | "
              << (r.p99_us / 1000.0) << " ms | "
              << r.peak_rss_kb << " kB | "
              << std::fixed << std::setprecision(1) << r.cpu_time_ms << " ms |\n";
  }
  std::cout << "| Aggregate | " << (player_count + 1) << " | "
            << total_duration_all << " ms | " << successful_requests_all << "/" << failed_requests_all << " | "
            << std::fixed << std::setprecision(2) << (aggregate_p50_us / 1000.0) << " ms | "
            << (aggregate_p95_us / 1000.0) << " ms | "
            << (aggregate_p99_us / 1000.0) << " ms | - | - |\n\n";

  // Write Machine-Readable JSON Report
  json report;
  report["test_name"] = "darkpool_load_tests_16_player";
  report["source_commit"] = source_commit;
  report["environment"] = env_info;
  report["player_count"] = player_count;
  report["client_count"] = player_count + 1;
  report["repeat_runs"] = repeat_runs;
  report["timestamp"] = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(
          std::chrono::system_clock::now().time_since_epoch()).count());
  report["summary"] = {
    {"total_duration_ms", total_duration_all},
    {"total_requests", total_requests_all},
    {"successful_requests", successful_requests_all},
    {"failed_requests", failed_requests_all},
    {"p50_ms", aggregate_p50_us / 1000.0},
    {"p95_ms", aggregate_p95_us / 1000.0},
    {"p99_ms", aggregate_p99_us / 1000.0},
    {"mean_ms", aggregate_mean_us / 1000.0}
  };

  json runs_json = json::array();
  for (const auto& r : results) {
    runs_json.push_back({
      {"run_index", r.run_index},
      {"duration_ms", r.duration_ms},
      {"total_requests", r.total_requests},
      {"successful_requests", r.successful_requests},
      {"failed_requests", r.failed_requests},
      {"p50_ms", r.p50_us / 1000.0},
      {"p95_ms", r.p95_us / 1000.0},
      {"p99_ms", r.p99_us / 1000.0},
      {"mean_ms", r.mean_us / 1000.0},
      {"min_ms", static_cast<double>(r.min_us) / 1000.0},
      {"max_ms", static_cast<double>(r.max_us) / 1000.0},
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

  return all_runs_ok ? 0 : 1;
}

#endif  // !defined(__linux__)
