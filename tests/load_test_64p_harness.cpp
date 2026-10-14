#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests_64p requires Linux (epoll/unix sockets)\n";
  return 1;
}

#else

#include "darkpool/network.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
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

constexpr std::size_t kExpectedPlayerCount = 64;
constexpr std::size_t kExpectedInsiders = 8;
constexpr std::size_t kExpectedInvestigators = 4;
constexpr std::size_t kExpectedEmployees = 52;
constexpr const char* kDefaultModeratorToken = "test-mod-token-64p";

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
  std::size_t player_count{kExpectedPlayerCount};
  std::size_t client_count{kExpectedPlayerCount + 1U};
  std::uint64_t duration_ms{0};
  bool run_passed{false};
  FailureCounts failures;
  PhaseMetrics sequential;
  PhaseMetrics concurrent;
  PhaseMetrics total;
  double cpu_time_ms{0.0};
  std::uint64_t peak_rss_kb{0};
  std::size_t open_fds{0};
  std::string final_phase;
  std::string final_outcome;
  std::vector<RequestMetric> metrics;
};

struct SustainedStageResult {
  std::string stage_name;
  std::string status{"NOT RUN"};  // PASS, FAIL, NOT RUN, BLOCKED
  std::string reason;
  std::uint64_t target_duration_s{0};
  std::uint64_t actual_duration_ms{0};
  std::size_t completed_matches{0};
  std::size_t planned_resets{0};
  std::size_t unexpected_disconnects{0};
  std::uint64_t seed{0};
  PhaseMetrics metrics;
  double throughput_rps{0.0};
  std::uint64_t peak_rss_kb{0};
  double cpu_time_ms{0.0};
  std::size_t open_fds{0};
};

struct TestFaultConfig {
  bool inject_assertion_failure{false};
  bool inject_outcome_mismatch{false};
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

  // Monotonic clock starts immediately before writing request frame to socket
  const auto start_time = std::chrono::steady_clock::now();

  if (!send_all(fd, outgoing.data(), outgoing.size())) {
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    failures.transport_failures++;
    out_metrics.push_back({req_type, us, is_concurrent, false, false});
    return std::nullopt;
  }

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

  // Monotonic clock ends immediately when complete response frame is received and parsed
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
    out_metrics.push_back({req_type, us, is_concurrent, true, true});
    return response->payload();
  } catch (const std::exception&) {
    failures.transport_failures++;
    out_metrics.push_back({req_type, us, is_concurrent, false, false});
    return std::nullopt;
  }
}

std::string generate_unique_socket_path() {
  static std::atomic<std::uint64_t> counter{0};
  const auto pid = getpid();
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return "/tmp/dp_l4_" + std::to_string(pid) + "_" + std::to_string(counter.fetch_add(1)) +
         "_" + std::to_string(now % 1000000) + ".sock";
}

int connect_unix(const std::string& path, int max_attempts = 100) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof(address.sun_path)) {
      (void)close(fd);
      return -1;
    }
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
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

pid_t spawn_server_unix(const std::string& server_path, const std::string& socket_path, const std::string& token) {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (!token.empty()) {
      setenv("DARKPOOL_MODERATOR_TOKEN", token.c_str(), 1);
    } else {
      unsetenv("DARKPOOL_MODERATOR_TOKEN");
    }
    const int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
      close(dev_null);
    }
    execl(server_path.c_str(), server_path.c_str(),
          "--socket", socket_path.c_str(),
          "--no-tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  return child;
}

void stop_server_unix(pid_t server_pid, const std::string& socket_path) {
  if (server_pid > 0) {
    (void)kill(server_pid, SIGTERM);
    int status = 0;
    (void)waitpid(server_pid, &status, 0);
  }
  if (!socket_path.empty()) {
    struct stat st {};
    if (::lstat(socket_path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
      (void)::unlink(socket_path.c_str());
    }
  }
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

std::size_t sample_open_fds(pid_t pid) {
  const std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
  DIR* dir = opendir(fd_dir.c_str());
  if (!dir) return 0;
  std::size_t count = 0;
  while (struct dirent* entry = readdir(dir)) {
    if (entry->d_name[0] != '.') ++count;
  }
  closedir(dir);
  return count;
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
  std::ostringstream oss;
  utsname un{};
  if (uname(&un) == 0) {
    oss << un.sysname << " " << un.release << " (" << un.machine << ")";
  } else {
    oss << "Linux (x86_64)";
  }
  std::ifstream cpuinfo("/proc/cpuinfo");
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
#if defined(__GNUC__)
  oss << "; GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
#endif
  return oss.str();
}

void compute_percentiles(std::vector<std::uint64_t>& latencies_us, PhaseMetrics& out) {
  if (latencies_us.empty()) return;
  std::sort(latencies_us.begin(), latencies_us.end());
  const auto n = latencies_us.size();
  const auto p50_idx = std::min(n - 1, static_cast<std::size_t>(static_cast<double>(n) * 0.50));
  const auto p95_idx = std::min(n - 1, static_cast<std::size_t>(static_cast<double>(n) * 0.95));
  const auto p99_idx = std::min(n - 1, static_cast<std::size_t>(static_cast<double>(n) * 0.99));

  out.p50_ms = static_cast<double>(latencies_us[p50_idx]) / 1000.0;
  out.p95_ms = static_cast<double>(latencies_us[p95_idx]) / 1000.0;
  out.p99_ms = static_cast<double>(latencies_us[p99_idx]) / 1000.0;
  out.min_ms = static_cast<double>(latencies_us.front()) / 1000.0;
  out.max_ms = static_cast<double>(latencies_us.back()) / 1000.0;

  const double sum = std::accumulate(latencies_us.begin(), latencies_us.end(), 0.0);
  out.mean_ms = (sum / static_cast<double>(n)) / 1000.0;
}

// Execute 1 complete 64-player match over UNIX-domain socket transport
RunResult execute_64_player_match(
    const std::string& server_path,
    int run_index,
    const TestFaultConfig& fault_cfg = {}) {
  RunResult result;
  result.run_index = run_index;
  result.player_count = kExpectedPlayerCount;
  result.client_count = kExpectedPlayerCount + 1U;

  const std::string socket_path = generate_unique_socket_path();
  const std::string mod_token = kDefaultModeratorToken;

  const auto server_pid = spawn_server_unix(server_path, socket_path, mod_token);
  if (server_pid <= 0) {
    std::cerr << "Run " << run_index << " ERROR: Failed to spawn darkpool_server\n";
    result.failures.transport_failures++;
    return result;
  }

  // Ensure server is responding on UNIX socket
  const int probe_fd = connect_unix(socket_path);
  if (probe_fd < 0) {
    std::cerr << "Run " << run_index << " ERROR: Failed to connect probe socket\n";
    result.failures.transport_failures++;
    stop_server_unix(server_pid, socket_path);
    return result;
  }
  (void)close(probe_fd);

  std::vector<int> player_fds;
  int mod_fd = -1;

  auto cleanup_sockets = [&]() {
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

  #define VERIFY(cond, err_msg) \
    do { \
      if (!(cond)) { \
        std::cerr << "Run " << run_index << " CORRECTNESS ERROR: " << (err_msg) \
                  << " at line " << __LINE__ << '\n'; \
        result.failures.correctness_failures++; \
      } \
    } while (0)

  // 1. Establish Moderator Connection
  mod_fd = connect_unix(socket_path);
  if (mod_fd < 0) {
    std::cerr << "Run " << run_index << " TRANSPORT ERROR: Failed to connect moderator\n";
    result.failures.transport_failures++;
    cleanup_sockets();
    stop_server_unix(server_pid, socket_path);
    return result;
  }

  const auto mod_hello = timed_request(
      mod_fd,
      json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
      result.metrics,
      result.failures);
  VERIFY(mod_hello && (*mod_hello)["type"] == "connect" && (*mod_hello)["role"] == "moderator",
         "Moderator handshake failed");

  // 2. Establish 64 Player Connections over UNIX Socket
  for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
    const int pfd = connect_unix(socket_path);
    if (pfd < 0) {
      std::cerr << "Run " << run_index << " TRANSPORT ERROR: Failed to connect player " << i << '\n';
      result.failures.transport_failures++;
      cleanup_sockets();
      stop_server_unix(server_pid, socket_path);
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

  // 3. Create 64-Player Match
  const auto create_resp = timed_request(
      mod_fd,
      json{{"type", "create_match"}, {"player_count", kExpectedPlayerCount}},
      result.metrics,
      result.failures);
  VERIFY(create_resp && (*create_resp)["type"] == "match_created" &&
             (*create_resp)["player_count"] == kExpectedPlayerCount &&
             (*create_resp)["insiders"] == kExpectedInsiders &&
             (*create_resp)["investigators"] == kExpectedInvestigators &&
             (*create_resp)["employees"] == kExpectedEmployees,
         "Failed to create 64-player match or role count mismatch");

  const auto match_id = create_resp ? (*create_resp)["match_id"].get<std::uint64_t>() : 0U;
  VERIFY(match_id > 0U, "Invalid match_id received");

  // 4. Have first 63 players join
  std::vector<int> insider_fds;
  std::vector<int> investigator_fds;
  std::vector<int> employee_fds;
  std::uint32_t employee_target = 0;

  for (std::size_t i = 0; i < kExpectedPlayerCount - 1U; ++i) {
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
        investigator_fds.push_back(player_fds[i]);
      } else if (role == "Employee") {
        employee_fds.push_back(player_fds[i]);
        if (employee_target == 0U) employee_target = static_cast<std::uint32_t>(i + 1U);
      }
    }
  }

  // 5. Pre-start roster validation: Verify match cannot start before roster is complete (63/64 joined)
  const auto premature_start = timed_request(
      mod_fd,
      json{{"type", "start_match"}, {"match_id", match_id}},
      result.metrics,
      result.failures);
  VERIFY(premature_start && (*premature_start)["type"] == "error" &&
             (*premature_start)["error"] == "all configured players must join before start",
         "Expected premature start_match to be rejected before complete roster");

  // 6. Join 64th player
  const auto join_64 = timed_request(
      player_fds[kExpectedPlayerCount - 1U],
      json{{"type", "join_match"}, {"match_id", match_id}},
      result.metrics,
      result.failures);
  VERIFY(join_64 && (*join_64)["type"] == "joined_match" &&
             (*join_64)["player_id"] == kExpectedPlayerCount,
         "64th player join failed");
  if (join_64 && join_64->contains("private_role")) {
    const auto role = (*join_64)["private_role"].get<std::string>();
    if (role == "Insider") {
      insider_fds.push_back(player_fds[kExpectedPlayerCount - 1U]);
    } else if (role == "Investigator") {
      investigator_fds.push_back(player_fds[kExpectedPlayerCount - 1U]);
    } else if (role == "Employee") {
      employee_fds.push_back(player_fds[kExpectedPlayerCount - 1U]);
      if (employee_target == 0U) employee_target = static_cast<std::uint32_t>(kExpectedPlayerCount);
    }
  }

  // 7. Excess join validation: Verify 65th player join is rejected (match is full)
  const int extra_pfd = connect_unix(socket_path);
  if (extra_pfd >= 0) {
    (void)timed_request(extra_pfd, json{{"type", "connect"}, {"role", "player"}}, result.metrics, result.failures);
    const auto excess_join = timed_request(
        extra_pfd,
        json{{"type", "join_match"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(excess_join && (*excess_join)["type"] == "error" &&
               (*excess_join)["error"] == "match is full",
           "Expected excess 65th join to be rejected with 'match is full'");
    (void)close(extra_pfd);
  }

  // 8. Verify exact role distribution for N=64
  VERIFY(insider_fds.size() == kExpectedInsiders, "Expected exactly 8 Insiders for N=64");
  VERIFY(investigator_fds.size() == kExpectedInvestigators, "Expected exactly 4 Investigators for N=64");
  VERIFY(employee_fds.size() == kExpectedEmployees, "Expected exactly 52 Employees for N=64");

  // Injected fault test: simulate assertion failure if configured
  if (fault_cfg.inject_assertion_failure) {
    VERIFY(false, "Simulated injected assertion failure for regression testing");
  }

  // 9. Start Match (now valid with all 64 players joined)
  const auto start_resp = timed_request(
      mod_fd,
      json{{"type", "start_match"}, {"match_id", match_id}},
      result.metrics,
      result.failures);
  VERIFY(start_resp && (*start_resp)["type"] == "transition_result" &&
             (*start_resp)["phase"] == "AwaitingEvent",
         "Failed to start match or phase is not AwaitingEvent");

  // 10. Execute All 3 Rounds
  // Round 1: Sequential requests + Privacy visibility checks
  // Round 2: Concurrent requests burst (Bounded concurrency: 64 simultaneous client threads)
  // Round 3: Sequential requests + Final resolution
  for (std::uint32_t round = 1; round <= 3U; ++round) {
    const bool is_concurrent_round = (round == 2U);

    // 10a. Publish Event
    const int event_impact = (round == 1U) ? 1 : (round == 2U) ? 0 : -1;
    const auto event_resp = timed_request(
        mod_fd,
        json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", event_impact}},
        result.metrics,
        result.failures);
    VERIFY(event_resp && (*event_resp)["type"] == "transition_result" &&
               (*event_resp)["phase"] == "Discussion",
           "Publish event failed or phase not Discussion");

    // 10b. Privacy Visibility Check: Employee attempting insider hint must be rejected
    if (round == 1U && !employee_fds.empty()) {
      const auto unauth_hint = timed_request(
          employee_fds[0],
          json{{"type", "insider_hint"}, {"match_id", match_id}},
          result.metrics,
          result.failures);
      VERIFY(unauth_hint && (*unauth_hint)["type"] == "operation_result" &&
                 (*unauth_hint)["ok"] == false,
             "Expected unauthorized insider_hint from Employee to be rejected");
    }

    // 10c. Authorized Insider Hints: All 8 Insiders receive private hint
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

    // 10d. Open Trading
    const auto trade_phase = timed_request(
        mod_fd,
        json{{"type", "open_trading"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(trade_phase && (*trade_phase)["type"] == "transition_result" &&
               (*trade_phase)["phase"] == "Trading",
           "Open trading failed or phase not Trading");

    // 10e. Player Trades
    if (is_concurrent_round) {
      // CONCURRENT REQUEST PHASE: 64 simultaneous client threads sending legal trades
      std::vector<std::thread> threads;
      threads.reserve(kExpectedPlayerCount);
      std::vector<std::vector<RequestMetric>> thread_metrics(kExpectedPlayerCount);
      std::vector<FailureCounts> thread_failures(kExpectedPlayerCount);
      std::vector<int> thread_passed(kExpectedPlayerCount, 0);

      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        threads.emplace_back([&, i]() {
          const auto resp = timed_request(
              player_fds[i],
              json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}},
              thread_metrics[i],
              thread_failures[i],
              true);
          if (resp && (*resp)["type"] == "trade_result" && (*resp)["code"] == "Accepted") {
            thread_passed[i] = 1;
          }
        });
      }

      for (auto& th : threads) {
        if (th.joinable()) th.join();
      }

      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        for (const auto& m : thread_metrics[i]) result.metrics.push_back(m);
        result.failures.transport_failures += thread_failures[i].transport_failures;
        result.failures.correctness_failures += thread_failures[i].correctness_failures;
        VERIFY(thread_passed[i] == 1, "Concurrent trade request failed or rejected");
      }
    } else {
      // Sequential trade submissions
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        const auto trade_resp = timed_request(
            player_fds[i],
            json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}},
            result.metrics,
            result.failures);
        VERIFY(trade_resp && (*trade_resp)["type"] == "trade_result" &&
                   (*trade_resp)["code"] == "Accepted",
               "Sequential trade request failed or rejected");
      }
    }

    // 10f. Resolve Market
    const auto market_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_market"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(market_resp && (*market_resp)["type"] == "market_result" &&
               (*market_resp)["ok"] == true,
           "Resolve market failed");

    // 10g. Clue Phase & Privacy Validation (Round 1 only)
    if (round == 1U) {
      if (!employee_fds.empty() && employee_target > 0U) {
        // Unauthorized clue check: Employee attempting clue request must be rejected
        const auto unauth_target = (employee_target == 1U) ? 2U : 1U;
        const auto unauth_clue = timed_request(
            employee_fds[0],
            json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", unauth_target}},
            result.metrics,
            result.failures);
        VERIFY(unauth_clue && (*unauth_clue)["type"] == "operation_result" &&
                   (*unauth_clue)["ok"] == false,
               "Expected unauthorized request_clue from Employee to be rejected");
      }

      if (!investigator_fds.empty() && employee_target > 0U) {
        // Authorized Investigator clue request (Investigator 1 asks about an Employee)
        const auto clue_resp = timed_request(
            investigator_fds[0],
            json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", employee_target}},
            result.metrics,
            result.failures);
        VERIFY(clue_resp && (*clue_resp)["type"] == "clue_private" && clue_resp->contains("is_insider"),
               "Investigator clue request failed or missing is_insider field");
      }
    }

    // 10h. Open Voting
    const auto vote_phase = timed_request(
        mod_fd,
        json{{"type", "open_voting"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(vote_phase && (*vote_phase)["type"] == "transition_result" &&
               (*vote_phase)["phase"] == "Voting",
           "Open voting failed or phase not Voting");

    // 10i. Player Votes
    if (is_concurrent_round) {
      // CONCURRENT REQUEST PHASE: 64 simultaneous client threads sending votes (abstain)
      std::vector<std::thread> threads;
      threads.reserve(kExpectedPlayerCount);
      std::vector<std::vector<RequestMetric>> thread_metrics(kExpectedPlayerCount);
      std::vector<FailureCounts> thread_failures(kExpectedPlayerCount);
      std::vector<int> thread_passed(kExpectedPlayerCount, 0);

      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        threads.emplace_back([&, i]() {
          const auto resp = timed_request(
              player_fds[i],
              json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
              thread_metrics[i],
              thread_failures[i],
              true);
          if (resp && (*resp)["type"] == "vote_result" && (*resp)["accepted"] == true) {
            thread_passed[i] = 1;
          }
        });
      }

      for (auto& th : threads) {
        if (th.joinable()) th.join();
      }

      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        for (const auto& m : thread_metrics[i]) result.metrics.push_back(m);
        result.failures.transport_failures += thread_failures[i].transport_failures;
        result.failures.correctness_failures += thread_failures[i].correctness_failures;
        VERIFY(thread_passed[i] == 1, "Concurrent vote request failed or rejected");
      }
    } else {
      // Sequential vote submissions (all abstain)
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        const auto vote_resp = timed_request(
            player_fds[i],
            json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
            result.metrics,
            result.failures);
        VERIFY(vote_resp && (*vote_resp)["type"] == "vote_result" &&
                   (*vote_resp)["accepted"] == true,
               "Sequential vote request failed or rejected");
      }
    }

    // 10j. Resolve Votes
    const auto resolve_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", match_id}},
        result.metrics,
        result.failures);
    VERIFY(resolve_resp && (*resolve_resp)["type"] == "vote_resolution" &&
               (*resolve_resp)["ok"] == true,
           "Resolve votes failed");
  }

  // 11. Verify Final Terminal Outcome
  const auto status_resp = timed_request(
      mod_fd,
      json{{"type", "match_status"}, {"match_id", match_id}},
      result.metrics,
      result.failures);
  VERIFY(status_resp && (*status_resp)["type"] == "match_status", "Failed to retrieve final match status");

  if (status_resp) {
    result.final_phase = status_resp->value("phase", "Unknown");
    result.final_outcome = status_resp->value("outcome", "Unknown");
  }

  VERIFY(result.final_phase == "Finished", "Expected final phase to be Finished");
  VERIFY(result.final_outcome == "InsiderWin", "Expected guaranteed outcome to be InsiderWin");

  // Injected fault test: simulate outcome mismatch if configured
  if (fault_cfg.inject_outcome_mismatch) {
    VERIFY(result.final_outcome == "EmployeesWin", "Simulated injected outcome mismatch for regression testing");
  }

  const auto run_end = std::chrono::steady_clock::now();
  result.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(run_end - run_start).count());

  // Sample Peak RSS, CPU, and open FDs
  result.peak_rss_kb = sample_peak_rss_kb(server_pid);
  result.cpu_time_ms = sample_cpu_time_ms(server_pid);
  result.open_fds = sample_open_fds(server_pid);

  // 12. Graceful Teardown
  cleanup_sockets();
  stop_server_unix(server_pid, socket_path);

  // 13. Aggregate Latencies
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

// Sustained repeated-match workload (for 10m smoke and 30m reliability)
SustainedStageResult execute_sustained_workload(
    const std::string& server_path,
    std::uint64_t duration_seconds,
    std::uint64_t seed,
    const std::string& stage_name) {
  SustainedStageResult result;
  result.stage_name = stage_name;
  result.target_duration_s = duration_seconds;
  result.seed = seed;

  const std::string socket_path = generate_unique_socket_path();
  const std::string mod_token = kDefaultModeratorToken;

  const auto server_pid = spawn_server_unix(server_path, socket_path, mod_token);
  if (server_pid <= 0) {
    result.status = "FAIL";
    result.reason = "Failed to spawn server";
    return result;
  }

  const int probe_fd = connect_unix(socket_path);
  if (probe_fd < 0) {
    result.status = "FAIL";
    result.reason = "Failed to probe server on unix socket";
    stop_server_unix(server_pid, socket_path);
    return result;
  }
  (void)close(probe_fd);

  std::mt19937_64 rng(seed);
  std::vector<std::uint64_t> all_latencies;
  FailureCounts failures;
  std::vector<RequestMetric> metrics;

  const auto start_time = std::chrono::steady_clock::now();
  const auto deadline = start_time + std::chrono::seconds(duration_seconds);

  std::cout << "[" << stage_name << "] Beginning sustained repeated-match workload (target: "
            << duration_seconds << "s, seed: " << seed << ")..." << std::endl;

  bool abort_due_to_failure = false;

  while (std::chrono::steady_clock::now() < deadline && !abort_due_to_failure) {
    // Connect moderator and 64 players for this match (planned reset)
    int mod_fd = connect_unix(socket_path);
    if (mod_fd < 0) {
      result.unexpected_disconnects++;
      failures.transport_failures++;
      abort_due_to_failure = true;
      break;
    }

    const auto mod_hello = timed_request(
        mod_fd,
        json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
        metrics, failures);
    if (!mod_hello || (*mod_hello)["type"] != "connect") {
      failures.correctness_failures++;
      abort_due_to_failure = true;
      (void)close(mod_fd);
      break;
    }

    std::vector<int> player_fds;
    player_fds.reserve(kExpectedPlayerCount);
    for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
      const int pfd = connect_unix(socket_path);
      if (pfd < 0) {
        result.unexpected_disconnects++;
        failures.transport_failures++;
        abort_due_to_failure = true;
        break;
      }
      player_fds.push_back(pfd);
      const auto p_hello = timed_request(
          pfd, json{{"type", "connect"}, {"role", "player"}}, metrics, failures);
      if (!p_hello || (*p_hello)["type"] != "connect") {
        failures.correctness_failures++;
        abort_due_to_failure = true;
        break;
      }
    }
    if (abort_due_to_failure) {
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }

    // Create match
    const auto create_resp = timed_request(
        mod_fd, json{{"type", "create_match"}, {"player_count", kExpectedPlayerCount}}, metrics, failures);
    if (!create_resp || (*create_resp)["type"] != "match_created") {
      failures.correctness_failures++;
      abort_due_to_failure = true;
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }
    const auto match_id = (*create_resp)["match_id"].get<std::uint64_t>();

    // Players join
    std::vector<int> insider_fds;
    for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
      const auto join_resp = timed_request(
          player_fds[i], json{{"type", "join_match"}, {"match_id", match_id}}, metrics, failures);
      if (!join_resp || (*join_resp)["type"] != "joined_match") {
        failures.correctness_failures++;
        abort_due_to_failure = true;
        break;
      }
      if (join_resp->value("private_role", "") == "Insider") {
        insider_fds.push_back(player_fds[i]);
      }
    }
    if (abort_due_to_failure) {
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }

    // Start match
    const auto start_resp = timed_request(
        mod_fd, json{{"type", "start_match"}, {"match_id", match_id}}, metrics, failures);
    if (!start_resp || (*start_resp)["type"] != "transition_result") {
      failures.correctness_failures++;
      abort_due_to_failure = true;
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }

    // Run 3 rounds
    for (std::uint32_t round = 1; round <= 3U && !abort_due_to_failure; ++round) {
      const int event_impact = (round == 1U) ? 1 : (round == 2U) ? 0 : -1;
      (void)timed_request(mod_fd, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", event_impact}}, metrics, failures);

      // Hints
      for (const auto ifd : insider_fds) {
        (void)timed_request(ifd, json{{"type", "insider_hint"}, {"match_id", match_id}}, metrics, failures);
      }

      // Trading
      (void)timed_request(mod_fd, json{{"type", "open_trading"}, {"match_id", match_id}}, metrics, failures);
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        (void)timed_request(player_fds[i], json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}}, metrics, failures);
      }
      (void)timed_request(mod_fd, json{{"type", "resolve_market"}, {"match_id", match_id}}, metrics, failures);

      // Voting
      (void)timed_request(mod_fd, json{{"type", "open_voting"}, {"match_id", match_id}}, metrics, failures);
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        (void)timed_request(player_fds[i], json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}}, metrics, failures);
      }
      (void)timed_request(mod_fd, json{{"type", "resolve_votes"}, {"match_id", match_id}}, metrics, failures);
    }

    // Verify terminal outcome
    const auto status_resp = timed_request(mod_fd, json{{"type", "match_status"}, {"match_id", match_id}}, metrics, failures);
    if (!status_resp || (*status_resp)["phase"] != "Finished" || (*status_resp)["outcome"] != "InsiderWin") {
      failures.correctness_failures++;
      abort_due_to_failure = true;
    }

    // Clean disconnect of this match's clients (planned reset)
    for (const auto pfd : player_fds) (void)close(pfd);
    (void)close(mod_fd);
    result.planned_resets++;
    result.completed_matches++;

    // Small yield / pacing between matches
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  const auto end_time = std::chrono::steady_clock::now();
  result.actual_duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

  result.peak_rss_kb = sample_peak_rss_kb(server_pid);
  result.cpu_time_ms = sample_cpu_time_ms(server_pid);
  result.open_fds = sample_open_fds(server_pid);

  stop_server_unix(server_pid, socket_path);

  for (const auto& m : metrics) {
    all_latencies.push_back(m.latency_us);
    result.metrics.total_requests++;
    if (m.transport_ok && m.correctness_ok) {
      result.metrics.successful_requests++;
    } else {
      if (!m.transport_ok) result.metrics.transport_failures++;
      if (!m.correctness_ok) result.metrics.correctness_failures++;
    }
  }

  compute_percentiles(all_latencies, result.metrics);

  const double elapsed_s = static_cast<double>(result.actual_duration_ms) / 1000.0;
  result.throughput_rps = elapsed_s > 0.0 ? (static_cast<double>(result.metrics.total_requests) / elapsed_s) : 0.0;

  if (failures.total() == 0 && !abort_due_to_failure && result.completed_matches > 0) {
    result.status = "PASS";
  } else {
    result.status = "FAIL";
    result.reason = "Encountered " + std::to_string(failures.total()) + " failures during workload";
  }

  std::cout << "[" << stage_name << "] Complete. Status: " << result.status
            << " (" << result.completed_matches << " matches, "
            << result.metrics.total_requests << " reqs in "
            << (result.actual_duration_ms / 1000) << "s, throughput: "
            << std::fixed << std::setprecision(1) << result.throughput_rps << " req/s)\n";

  return result;
}

// Subprocess execution for regression self-test
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
  return std::string(argv0 ? argv0 : "./darkpool_load_tests_64p");
}

bool run_regression_selftest(const std::string& harness_path, const std::string& server_path) {
  std::cout << "=================================================================\n";
  std::cout << " Running 64-Player Load Harness Subprocess Regression Tests\n";
  std::cout << "=================================================================\n";

  std::cout << "[Regression 1/3] Verifying normal 64P run subprocess exits 0... " << std::flush;
  const int code_normal = run_subprocess(harness_path, {"--server", server_path, "--smoke-run"});
  if (code_normal != 0) {
    std::cout << "FAILED (expected exit code 0, got " << code_normal << ")\n";
    return false;
  }
  std::cout << "PASSED (exit code 0)\n";

  std::cout << "[Regression 2/3] Verifying assertion failure subprocess exits nonzero... " << std::flush;
  const int code_assert = run_subprocess(harness_path, {"--server", server_path, "--inject-assertion-failure"});
  if (code_assert == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_assert << ")\n";

  std::cout << "[Regression 3/3] Verifying outcome mismatch subprocess exits nonzero... " << std::flush;
  const int code_outcome = run_subprocess(harness_path, {"--server", server_path, "--inject-outcome-mismatch"});
  if (code_outcome == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_outcome << ")\n";

  std::cout << "\nAll 64-player regression checks PASSED: real subprocess exit statuses verified.\n\n";
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string server_path = "./darkpool_server";
  std::string output_json_path = "load_test_64p_results.json";
  std::string source_commit = DARKPOOL_SOURCE_COMMIT;
  int repeat_runs = 3;
  std::uint64_t custom_duration_s = 0;
  std::uint64_t seed = 42;
  bool is_smoke_10m = false;
  bool is_reliability_30m = false;
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
    } else if (arg == "--duration" && i + 1 < argc) {
      custom_duration_s = static_cast<std::uint64_t>(std::max(1, std::atoi(argv[++i])));
    } else if (arg == "--seed" && i + 1 < argc) {
      seed = static_cast<std::uint64_t>(std::atoll(argv[++i]));
    } else if (arg == "--commit" && i + 1 < argc) {
      source_commit = argv[++i];
    } else if (arg == "--functional") {
      // Default execution mode
    } else if (arg == "--smoke-10m") {
      is_smoke_10m = true;
    } else if (arg == "--reliability-30m") {
      is_reliability_30m = true;
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
    const auto res = execute_64_player_match(server_path, 1, {});
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_assertion) {
    TestFaultConfig fault;
    fault.inject_assertion_failure = true;
    const auto res = execute_64_player_match(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_inject_outcome) {
    TestFaultConfig fault;
    fault.inject_outcome_mismatch = true;
    const auto res = execute_64_player_match(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_selftest) {
    const auto self_exe = get_self_executable_path(argv[0]);
    return run_regression_selftest(self_exe, server_path) ? 0 : 1;
  }

  if (is_smoke_10m) {
    const auto res = execute_sustained_workload(server_path, 600, seed, "smoke_10m");
    return res.status == "PASS" ? 0 : 1;
  }
  if (is_reliability_30m) {
    const auto res = execute_sustained_workload(server_path, 1800, seed, "reliability_30m");
    return res.status == "PASS" ? 0 : 1;
  }
  if (custom_duration_s > 0) {
    const auto res = execute_sustained_workload(server_path, custom_duration_s, seed, "custom_sustained");
    return res.status == "PASS" ? 0 : 1;
  }

  // Default mode or --functional: Run 3-run 64-player functional match suite
  const std::string env_info = get_environment_info();

  std::cout << "=================================================================\n";
  std::cout << " DARKPOOL Real-Server Load Test Harness (64-Player Match / L4)\n";
  std::cout << "=================================================================\n";
  std::cout << " Server Path  : " << server_path << "\n";
  std::cout << " Source Commit: " << source_commit << "\n";
  std::cout << " Environment  : " << env_info << "\n";
  std::cout << " Player Count : " << kExpectedPlayerCount << " (+ 1 moderator = " << (kExpectedPlayerCount + 1U) << " clients)\n";
  std::cout << " Transport    : UNIX-Domain Socket (SOCK_STREAM)\n";
  std::cout << " Planned Runs : " << repeat_runs << "\n";
  std::cout << " Output JSON  : " << output_json_path << "\n";
  std::cout << " Timing Basis : Monotonic clock (std::chrono::steady_clock)\n";
  std::cout << " ACK Boundary : Frame send initiation -> complete response parse\n";
  std::cout << " Phases       : Sequential Rounds 1 & 3 + 64-client concurrent burst (Round 2)\n";
  std::cout << " Roles        : 8 Insiders, 4 Investigators, 52 Employees\n";
  std::cout << "=================================================================\n\n";

  std::vector<RunResult> run_results;
  bool all_passed = true;

  for (int r = 1; r <= repeat_runs; ++r) {
    std::cout << "[Run " << r << "/" << repeat_runs << "] Executing 64-player match over UNIX socket... " << std::flush;
    auto result = execute_64_player_match(server_path, r, {});
    run_results.push_back(result);

    if (result.run_passed) {
      std::cout << "PASSED in " << result.duration_ms << " ms (Winner: " << result.final_outcome << ")\n";
      std::cout << "         Sequential : " << result.sequential.total_requests << " reqs, "
                << "p50=" << std::fixed << std::setprecision(2) << result.sequential.p50_ms << " ms, "
                << "p95=" << result.sequential.p95_ms << " ms, "
                << "p99=" << result.sequential.p99_ms << " ms\n";
      std::cout << "         Concurrent : " << result.concurrent.total_requests << " reqs, "
                << "p50=" << result.concurrent.p50_ms << " ms, "
                << "p95=" << result.concurrent.p95_ms << " ms, "
                << "p99=" << result.concurrent.p99_ms << " ms\n";
      std::cout << "         Resource   : Peak RSS=" << result.peak_rss_kb << " kB, "
                << "CPU=" << std::setprecision(1) << result.cpu_time_ms << " ms, "
                << "Open FDs=" << result.open_fds << "\n\n";
    } else {
      all_passed = false;
      std::cout << "FAILED with " << result.failures.total() << " total failures ("
                << result.failures.transport_failures << " transport, "
                << result.failures.correctness_failures << " correctness)\n\n";
    }
  }

  // Aggregate latencies across all runs
  std::vector<std::uint64_t> aggregate_seq_latencies;
  std::vector<std::uint64_t> aggregate_conc_latencies;
  std::vector<std::uint64_t> aggregate_all_latencies;
  PhaseMetrics aggregate_seq;
  PhaseMetrics aggregate_conc;
  PhaseMetrics aggregate_total;
  std::uint64_t max_peak_rss = 0;
  double total_cpu_time = 0.0;
  std::size_t max_open_fds = 0;
  std::uint64_t total_duration_ms = 0;

  for (const auto& res : run_results) {
    total_duration_ms += res.duration_ms;
    if (res.peak_rss_kb > max_peak_rss) max_peak_rss = res.peak_rss_kb;
    total_cpu_time += res.cpu_time_ms;
    if (res.open_fds > max_open_fds) max_open_fds = res.open_fds;

    for (const auto& m : res.metrics) {
      aggregate_all_latencies.push_back(m.latency_us);
      aggregate_total.total_requests++;
      if (m.is_concurrent) {
        aggregate_conc.total_requests++;
        if (m.transport_ok && m.correctness_ok) {
          aggregate_conc.successful_requests++;
          aggregate_conc_latencies.push_back(m.latency_us);
        } else {
          if (!m.transport_ok) aggregate_conc.transport_failures++;
          if (!m.correctness_ok) aggregate_conc.correctness_failures++;
        }
      } else {
        aggregate_seq.total_requests++;
        if (m.transport_ok && m.correctness_ok) {
          aggregate_seq.successful_requests++;
          aggregate_seq_latencies.push_back(m.latency_us);
        } else {
          if (!m.transport_ok) aggregate_seq.transport_failures++;
          if (!m.correctness_ok) aggregate_seq.correctness_failures++;
        }
      }
    }
  }

  aggregate_total.successful_requests = aggregate_seq.successful_requests + aggregate_conc.successful_requests;
  aggregate_total.transport_failures = aggregate_seq.transport_failures + aggregate_conc.transport_failures;
  aggregate_total.correctness_failures = aggregate_seq.correctness_failures + aggregate_conc.correctness_failures;

  compute_percentiles(aggregate_seq_latencies, aggregate_seq);
  compute_percentiles(aggregate_conc_latencies, aggregate_conc);
  compute_percentiles(aggregate_all_latencies, aggregate_total);

  std::cout << "=================================================================\n";
  std::cout << " SUMMARY RESULTS (Three Repeatable 64-Player Runs / L4)\n";
  std::cout << "=================================================================\n";
  std::cout << "| Run | Clients | Duration | Sequential (p50/p95/p99) | Concurrent (p50/p95/p99) | Failures (Trans/Corr) | Peak RSS | CPU | FDs |\n";
  std::cout << "|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& r : run_results) {
    std::cout << "| Run " << r.run_index << " | " << r.client_count << " | "
              << r.duration_ms << " ms | "
              << std::fixed << std::setprecision(2)
              << r.sequential.p50_ms << " / " << r.sequential.p95_ms << " / " << r.sequential.p99_ms << " ms | "
              << r.concurrent.p50_ms << " / " << r.concurrent.p95_ms << " / " << r.concurrent.p99_ms << " ms | "
              << r.failures.transport_failures << " / " << r.failures.correctness_failures << " | "
              << r.peak_rss_kb << " kB | " << std::setprecision(1) << r.cpu_time_ms << " ms | "
              << r.open_fds << " |\n";
  }
  std::cout << "| Aggregate | " << (kExpectedPlayerCount + 1U) << " | "
            << total_duration_ms << " ms | "
            << std::fixed << std::setprecision(2)
            << aggregate_seq.p50_ms << " / " << aggregate_seq.p95_ms << " / " << aggregate_seq.p99_ms << " ms | "
            << aggregate_conc.p50_ms << " / " << aggregate_conc.p95_ms << " / " << aggregate_conc.p99_ms << " ms | "
            << aggregate_total.transport_failures << " / " << aggregate_total.correctness_failures << " | "
            << max_peak_rss << " kB max | " << std::setprecision(1) << total_cpu_time << " ms total | "
            << max_open_fds << " max |\n\n";

  // Generate JSON report
  json report = json::object();
  report["test_name"] = "darkpool_load_tests_64_player";
  report["source_commit"] = source_commit;
  report["environment"] = env_info;
  report["player_count"] = kExpectedPlayerCount;
  report["client_count"] = kExpectedPlayerCount + 1U;
  report["transport"] = "unix_domain_socket";
  report["repeat_runs"] = repeat_runs;
  report["all_runs_passed"] = all_passed;
  report["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  json runs_arr = json::array();
  for (const auto& r : run_results) {
    json run_obj = json::object();
    run_obj["run_index"] = r.run_index;
    run_obj["duration_ms"] = r.duration_ms;
    run_obj["run_passed"] = r.run_passed;
    run_obj["transport_failures"] = r.failures.transport_failures;
    run_obj["correctness_failures"] = r.failures.correctness_failures;
    run_obj["peak_rss_kb"] = r.peak_rss_kb;
    run_obj["cpu_time_ms"] = r.cpu_time_ms;
    run_obj["open_fds"] = r.open_fds;
    run_obj["final_phase"] = r.final_phase;
    run_obj["final_outcome"] = r.final_outcome;

    run_obj["sequential"] = {
      {"total_requests", r.sequential.total_requests},
      {"successful_requests", r.sequential.successful_requests},
      {"p50_ms", r.sequential.p50_ms},
      {"p95_ms", r.sequential.p95_ms},
      {"p99_ms", r.sequential.p99_ms},
      {"mean_ms", r.sequential.mean_ms}
    };
    run_obj["concurrent"] = {
      {"total_requests", r.concurrent.total_requests},
      {"successful_requests", r.concurrent.successful_requests},
      {"p50_ms", r.concurrent.p50_ms},
      {"p95_ms", r.concurrent.p95_ms},
      {"p99_ms", r.concurrent.p99_ms},
      {"mean_ms", r.concurrent.mean_ms}
    };
    runs_arr.push_back(run_obj);
  }
  report["runs"] = runs_arr;

  report["sequential_summary"] = {
    {"total_requests", aggregate_seq.total_requests},
    {"successful_requests", aggregate_seq.successful_requests},
    {"p50_ms", aggregate_seq.p50_ms},
    {"p95_ms", aggregate_seq.p95_ms},
    {"p99_ms", aggregate_seq.p99_ms},
    {"mean_ms", aggregate_seq.mean_ms}
  };
  report["concurrent_summary"] = {
    {"total_requests", aggregate_conc.total_requests},
    {"successful_requests", aggregate_conc.successful_requests},
    {"p50_ms", aggregate_conc.p50_ms},
    {"p95_ms", aggregate_conc.p95_ms},
    {"p99_ms", aggregate_conc.p99_ms},
    {"mean_ms", aggregate_conc.mean_ms}
  };
  report["total_summary"] = {
    {"total_requests", aggregate_total.total_requests},
    {"successful_requests", aggregate_total.successful_requests},
    {"transport_failures", aggregate_total.transport_failures},
    {"correctness_failures", aggregate_total.correctness_failures},
    {"p50_ms", aggregate_total.p50_ms},
    {"p95_ms", aggregate_total.p95_ms},
    {"p99_ms", aggregate_total.p99_ms},
    {"mean_ms", aggregate_total.mean_ms},
    {"total_duration_ms", total_duration_ms}
  };

  // Record status of sustained stages
  report["stages"] = {
    {"functional_64p", {
      {"status", all_passed ? "PASS" : "FAIL"},
      {"runs", repeat_runs},
      {"description", "Single 64-player match over UNIX sockets, 3 repeatable runs with sequential and concurrent phases"}
    }},
    {"sustained_smoke_10m", {
      {"status", "NOT RUN"},
      {"description", "10-minute repeated-match bounded smoke test over UNIX sockets (executable via --smoke-10m)"}
    }},
    {"reliability_30m", {
      {"status", "NOT RUN"},
      {"description", "30-minute repeated-match reliability test over UNIX sockets (executable via --reliability-30m)"}
    }},
    {"four_match_256p_l6", {
      {"status", "NOT RUN"},
      {"description", "Four-match 256-player capacity remains unverified"}
    }}
  };

  std::ofstream out_file(output_json_path);
  if (out_file.is_open()) {
    out_file << report.dump(2) << '\n';
    std::cout << "Machine-readable report written to: " << output_json_path << "\n\n";
  } else {
    std::cerr << "Warning: Could not open output file: " << output_json_path << "\n";
  }

  return all_passed ? 0 : 1;
}

#endif  // !defined(__linux__)
