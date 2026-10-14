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
#include <functional>
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
  std::string last_failure_reason;

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
  bool is_expected_rejection{false};
};

struct PhaseMetrics {
  std::size_t total_requests{0};
  std::size_t successful_requests{0};
  std::size_t expected_rejections{0};
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
  std::string diagnostic_details;
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
  bool inject_semantic_failure{false};
};

struct ValidationResult {
  bool correctness_ok{true};
  bool is_expected_rejection{false};
  std::string failure_reason;

  static ValidationResult Success() {
    return {true, false, ""};
  }

  static ValidationResult ExpectedRejection() {
    return {true, true, ""};
  }

  static ValidationResult Failure(std::string reason) {
    return {false, false, std::move(reason)};
  }
};

using ResponseValidator = std::function<ValidationResult(const json&)>;

inline ResponseValidator expect_connect(const std::string& role) {
  return [role](const json& j) -> ValidationResult {
    if (j.value("type", "") != "connect") {
      return ValidationResult::Failure("Expected type 'connect', got '" + j.value("type", "") + "'");
    }
    if (j.value("role", "") != role) {
      return ValidationResult::Failure("Expected role '" + role + "', got '" + j.value("role", "") + "'");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_match_created(
    std::size_t player_count,
    std::size_t insiders,
    std::size_t investigators,
    std::size_t employees) {
  return [=](const json& j) -> ValidationResult {
    if (j.value("type", "") != "match_created") {
      return ValidationResult::Failure("Expected type 'match_created', got '" + j.value("type", "") + "'");
    }
    if (j.value("player_count", 0ULL) != player_count) {
      return ValidationResult::Failure("Expected player_count " + std::to_string(player_count) +
                                       ", got " + std::to_string(j.value("player_count", 0ULL)));
    }
    if (j.value("insiders", 0ULL) != insiders) {
      return ValidationResult::Failure("Expected insiders " + std::to_string(insiders) +
                                       ", got " + std::to_string(j.value("insiders", 0ULL)));
    }
    if (j.value("investigators", 0ULL) != investigators) {
      return ValidationResult::Failure("Expected investigators " + std::to_string(investigators) +
                                       ", got " + std::to_string(j.value("investigators", 0ULL)));
    }
    if (j.value("employees", 0ULL) != employees) {
      return ValidationResult::Failure("Expected employees " + std::to_string(employees) +
                                       ", got " + std::to_string(j.value("employees", 0ULL)));
    }
    if (!j.contains("match_id") || j["match_id"].get<std::uint64_t>() == 0U) {
      return ValidationResult::Failure("Missing or zero match_id");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_joined_match(std::uint32_t expected_player_id) {
  return [expected_player_id](const json& j) -> ValidationResult {
    if (j.value("type", "") != "joined_match") {
      return ValidationResult::Failure("Expected type 'joined_match', got '" + j.value("type", "") + "'");
    }
    if (j.value("player_id", 0U) != expected_player_id) {
      return ValidationResult::Failure("Expected player_id " + std::to_string(expected_player_id) +
                                       ", got " + std::to_string(j.value("player_id", 0U)));
    }
    if (!j.contains("private_role") || !j["private_role"].is_string()) {
      return ValidationResult::Failure("Missing or invalid private_role");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_transition(const std::string& expected_phase) {
  return [expected_phase](const json& j) -> ValidationResult {
    if (j.value("type", "") != "transition_result") {
      return ValidationResult::Failure("Expected type 'transition_result', got '" + j.value("type", "") + "'");
    }
    if (j.value("phase", "") != expected_phase) {
      return ValidationResult::Failure("Expected phase '" + expected_phase + "', got '" + j.value("phase", "") + "'");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_insider_hint(int expected_impact) {
  return [expected_impact](const json& j) -> ValidationResult {
    if (j.value("type", "") != "hint_private") {
      return ValidationResult::Failure("Expected type 'hint_private', got '" + j.value("type", "") + "'");
    }
    if (!j.contains("event_impact") || j["event_impact"].get<int>() != expected_impact) {
      return ValidationResult::Failure("Event impact mismatch in hint");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_trade_accepted() {
  return [](const json& j) -> ValidationResult {
    if (j.value("type", "") != "trade_result") {
      return ValidationResult::Failure("Expected type 'trade_result', got '" + j.value("type", "") + "'");
    }
    if (j.value("code", "") != "Accepted") {
      return ValidationResult::Failure("Expected trade code 'Accepted', got '" + j.value("code", "") + "'");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_market_result() {
  return [](const json& j) -> ValidationResult {
    if (j.value("type", "") != "market_result") {
      return ValidationResult::Failure("Expected type 'market_result', got '" + j.value("type", "") + "'");
    }
    if (!j.value("ok", false)) {
      return ValidationResult::Failure("Market resolution ok is false");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_vote_accepted() {
  return [](const json& j) -> ValidationResult {
    if (j.value("type", "") != "vote_result") {
      return ValidationResult::Failure("Expected type 'vote_result', got '" + j.value("type", "") + "'");
    }
    if (!j.value("accepted", false)) {
      return ValidationResult::Failure("Vote accepted is false");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_vote_resolution() {
  return [](const json& j) -> ValidationResult {
    if (j.value("type", "") != "vote_resolution") {
      return ValidationResult::Failure("Expected type 'vote_resolution', got '" + j.value("type", "") + "'");
    }
    if (!j.value("ok", false)) {
      return ValidationResult::Failure("Vote resolution ok is false");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_match_status(const std::string& expected_phase, const std::string& expected_outcome) {
  return [expected_phase, expected_outcome](const json& j) -> ValidationResult {
    if (j.value("type", "") != "match_status") {
      return ValidationResult::Failure("Expected type 'match_status', got '" + j.value("type", "") + "'");
    }
    if (j.value("phase", "") != expected_phase) {
      return ValidationResult::Failure("Expected status phase '" + expected_phase + "', got '" + j.value("phase", "") + "'");
    }
    if (j.value("outcome", "") != expected_outcome) {
      return ValidationResult::Failure("Expected status outcome '" + expected_outcome + "', got '" + j.value("outcome", "") + "'");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_investigator_clue() {
  return [](const json& j) -> ValidationResult {
    if (j.value("type", "") != "clue_private") {
      return ValidationResult::Failure("Expected type 'clue_private', got '" + j.value("type", "") + "'");
    }
    if (!j.contains("is_insider")) {
      return ValidationResult::Failure("Missing is_insider field in clue response");
    }
    return ValidationResult::Success();
  };
}

inline ResponseValidator expect_error(const std::string& expected_error) {
  return [expected_error](const json& j) -> ValidationResult {
    if (j.value("type", "") != "error") {
      return ValidationResult::Failure("Expected error response, got '" + j.value("type", "") + "'");
    }
    if (j.value("error", "") != expected_error) {
      return ValidationResult::Failure("Expected error '" + expected_error + "', got '" + j.value("error", "") + "'");
    }
    return ValidationResult::ExpectedRejection();
  };
}

inline ResponseValidator expect_operation_rejected() {
  return [](const json& j) -> ValidationResult {
    if (j.value("type", "") != "operation_result") {
      return ValidationResult::Failure("Expected type 'operation_result', got '" + j.value("type", "") + "'");
    }
    if (j.value("ok", true)) {
      return ValidationResult::Failure("Expected operation ok=false (rejection), got ok=true");
    }
    return ValidationResult::ExpectedRejection();
  };
}

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
    const ResponseValidator& validator = nullptr,
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
    failures.last_failure_reason = "send_all failed for " + req_type;
    out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
    return std::nullopt;
  }

  char header[4];
  if (!receive_exact(fd, header, sizeof(header))) {
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    failures.transport_failures++;
    failures.last_failure_reason = "receive header failed for " + req_type;
    out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
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
    failures.last_failure_reason = "invalid frame length (" + std::to_string(length) + ") for " + req_type;
    out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
    return std::nullopt;
  }

  std::string bytes(header, sizeof(header));
  bytes.resize(static_cast<std::size_t>(length) + sizeof(header));
  if (!receive_exact(fd, bytes.data() + sizeof(header), static_cast<std::size_t>(length))) {
    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());
    failures.transport_failures++;
    failures.last_failure_reason = "receive payload bytes failed for " + req_type;
    out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
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
      failures.last_failure_reason = "Frame::parse failed for " + req_type;
      out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
      return std::nullopt;
    }

    const auto resp_payload = response->payload();
    const ValidationResult v = validator ? validator(resp_payload) : ValidationResult::Success();

    if (!v.correctness_ok) {
      failures.correctness_failures++;
      failures.last_failure_reason = v.failure_reason.empty() ? ("Validation failed for " + req_type) : v.failure_reason;
      out_metrics.push_back({req_type, us, is_concurrent, /*transport_ok=*/true, /*correctness_ok=*/false, /*is_expected_rejection=*/false});
      return std::nullopt;
    }

    out_metrics.push_back({req_type, us, is_concurrent, /*transport_ok=*/true, /*correctness_ok=*/true, v.is_expected_rejection});
    return resp_payload;
  } catch (const std::exception& e) {
    failures.transport_failures++;
    failures.last_failure_reason = std::string("Exception during frame parse for ") + req_type + ": " + e.what();
    out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
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

  #define VERIFY_REQUEST(resp_opt, err_msg) \
    do { \
      if (!(resp_opt).has_value()) { \
        std::cerr << "Run " << run_index << " REQUEST ERROR: " << (err_msg) \
                  << " (" << result.failures.last_failure_reason << ") at line " << __LINE__ << '\n'; \
      } \
    } while (0)

  #define VERIFY_STATE(cond, err_msg) \
    do { \
      if (!(cond)) { \
        std::cerr << "Run " << run_index << " STATE ERROR: " << (err_msg) \
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
      result.failures,
      expect_connect("moderator"));
  VERIFY_REQUEST(mod_hello, "Moderator handshake failed");

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
        result.failures,
        expect_connect("player"));
    VERIFY_REQUEST(player_hello, "Player connect handshake failed");
  }

  // 3. Create 64-Player Match
  const auto create_resp = timed_request(
      mod_fd,
      json{{"type", "create_match"}, {"player_count", kExpectedPlayerCount}},
      result.metrics,
      result.failures,
      expect_match_created(kExpectedPlayerCount, kExpectedInsiders, kExpectedInvestigators, kExpectedEmployees));
  VERIFY_REQUEST(create_resp, "Failed to create 64-player match or role count mismatch");

  const auto match_id = create_resp ? (*create_resp)["match_id"].get<std::uint64_t>() : 0U;
  VERIFY_STATE(match_id > 0U, "Invalid match_id received");

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
        result.failures,
        expect_joined_match(static_cast<std::uint32_t>(i + 1U)));
    VERIFY_REQUEST(join_resp, "Player failed to join match");

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
      result.failures,
      expect_error("all configured players must join before start"));
  VERIFY_REQUEST(premature_start, "Expected premature start_match to be rejected before complete roster");

  // 6. Join 64th player
  const auto join_64 = timed_request(
      player_fds[kExpectedPlayerCount - 1U],
      json{{"type", "join_match"}, {"match_id", match_id}},
      result.metrics,
      result.failures,
      expect_joined_match(static_cast<std::uint32_t>(kExpectedPlayerCount)));
  VERIFY_REQUEST(join_64, "64th player join failed");
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
    (void)timed_request(extra_pfd, json{{"type", "connect"}, {"role", "player"}}, result.metrics, result.failures, expect_connect("player"));
    const auto excess_join = timed_request(
        extra_pfd,
        json{{"type", "join_match"}, {"match_id", match_id}},
        result.metrics,
        result.failures,
        expect_error("match is full"));
    VERIFY_REQUEST(excess_join, "Expected excess 65th join to be rejected with 'match is full'");
    (void)close(extra_pfd);
  }

  // 8. Verify exact role distribution for N=64
  VERIFY_STATE(insider_fds.size() == kExpectedInsiders, "Expected exactly 8 Insiders for N=64");
  VERIFY_STATE(investigator_fds.size() == kExpectedInvestigators, "Expected exactly 4 Investigators for N=64");
  VERIFY_STATE(employee_fds.size() == kExpectedEmployees, "Expected exactly 52 Employees for N=64");

  // Injected fault test: simulate assertion failure if configured
  if (fault_cfg.inject_assertion_failure) {
    VERIFY_STATE(false, "Simulated injected assertion failure for regression testing");
  }

  // 9. Start Match (now valid with all 64 players joined)
  const auto start_resp = timed_request(
      mod_fd,
      json{{"type", "start_match"}, {"match_id", match_id}},
      result.metrics,
      result.failures,
      expect_transition("AwaitingEvent"));
  VERIFY_REQUEST(start_resp, "Failed to start match or phase is not AwaitingEvent");

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
        result.failures,
        expect_transition("Discussion"));
    VERIFY_REQUEST(event_resp, "Publish event failed or phase not Discussion");

    // 10b. Privacy Visibility Check: Employee attempting insider hint must be rejected
    if (round == 1U && !employee_fds.empty()) {
      const auto unauth_hint = timed_request(
          employee_fds[0],
          json{{"type", "insider_hint"}, {"match_id", match_id}},
          result.metrics,
          result.failures,
          expect_operation_rejected());
      VERIFY_REQUEST(unauth_hint, "Expected unauthorized insider_hint from Employee to be rejected");
    }

    // 10c. Authorized Insider Hints: All 8 Insiders receive private hint
    for (const auto ifd : insider_fds) {
      const auto hint_resp = timed_request(
          ifd,
          json{{"type", "insider_hint"}, {"match_id", match_id}},
          result.metrics,
          result.failures,
          expect_insider_hint(event_impact));
      VERIFY_REQUEST(hint_resp, "Insider hint response mismatch or incorrect impact");
    }

    // 10d. Open Trading
    const auto trade_phase = timed_request(
        mod_fd,
        json{{"type", "open_trading"}, {"match_id", match_id}},
        result.metrics,
        result.failures,
        expect_transition("Trading"));
    VERIFY_REQUEST(trade_phase, "Open trading failed or phase not Trading");

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
              expect_trade_accepted(),
              true);
          if (resp) {
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
        VERIFY_STATE(thread_passed[i] == 1, "Concurrent trade request failed or rejected");
      }
    } else {
      // Sequential trade submissions
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        json trade_req = json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}};
        if (fault_cfg.inject_semantic_failure && round == 1U && i == 0U) {
          trade_req["match_id"] = match_id + 999999U; // intentional mismatch to trigger server error
        }
        const auto trade_resp = timed_request(
            player_fds[i],
            trade_req,
            result.metrics,
            result.failures,
            expect_trade_accepted());
        VERIFY_REQUEST(trade_resp, "Sequential trade request failed or rejected");
      }
    }

    // 10f. Resolve Market
    const auto market_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_market"}, {"match_id", match_id}},
        result.metrics,
        result.failures,
        expect_market_result());
    VERIFY_REQUEST(market_resp, "Resolve market failed");

    // 10g. Clue Phase & Privacy Validation (Round 1 only)
    if (round == 1U) {
      if (!employee_fds.empty() && employee_target > 0U) {
        // Unauthorized clue check: Employee attempting clue request must be rejected
        const auto unauth_target = (employee_target == 1U) ? 2U : 1U;
        const auto unauth_clue = timed_request(
            employee_fds[0],
            json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", unauth_target}},
            result.metrics,
            result.failures,
            expect_operation_rejected());
        VERIFY_REQUEST(unauth_clue, "Expected unauthorized request_clue from Employee to be rejected");
      }

      if (!investigator_fds.empty() && employee_target > 0U) {
        // Authorized Investigator clue request (Investigator 1 asks about an Employee)
        const auto clue_resp = timed_request(
            investigator_fds[0],
            json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", employee_target}},
            result.metrics,
            result.failures,
            expect_investigator_clue());
        VERIFY_REQUEST(clue_resp, "Investigator clue request failed or missing is_insider field");
      }
    }

    // 10h. Open Voting
    const auto vote_phase = timed_request(
        mod_fd,
        json{{"type", "open_voting"}, {"match_id", match_id}},
        result.metrics,
        result.failures,
        expect_transition("Voting"));
    VERIFY_REQUEST(vote_phase, "Open voting failed or phase not Voting");

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
              expect_vote_accepted(),
              true);
          if (resp) {
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
        VERIFY_STATE(thread_passed[i] == 1, "Concurrent vote request failed or rejected");
      }
    } else {
      // Sequential vote submissions (all abstain)
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        const auto vote_resp = timed_request(
            player_fds[i],
            json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
            result.metrics,
            result.failures,
            expect_vote_accepted());
        VERIFY_REQUEST(vote_resp, "Sequential vote request failed or rejected");
      }
    }

    // 10j. Resolve Votes
    const auto resolve_resp = timed_request(
        mod_fd,
        json{{"type", "resolve_votes"}, {"match_id", match_id}},
        result.metrics,
        result.failures,
        expect_vote_resolution());
    VERIFY_REQUEST(resolve_resp, "Resolve votes failed");
  }

  // 11. Verify Final Terminal Outcome
  const auto status_resp = timed_request(
      mod_fd,
      json{{"type", "match_status"}, {"match_id", match_id}},
      result.metrics,
      result.failures,
      expect_match_status("Finished", "InsiderWin"));
  VERIFY_REQUEST(status_resp, "Failed to retrieve final match status");

  if (status_resp) {
    result.final_phase = status_resp->value("phase", "Unknown");
    result.final_outcome = status_resp->value("outcome", "Unknown");
  }

  VERIFY_STATE(result.final_phase == "Finished", "Expected final phase to be Finished");
  VERIFY_STATE(result.final_outcome == "InsiderWin", "Expected guaranteed outcome to be InsiderWin");

  // Injected fault test: simulate outcome mismatch if configured
  if (fault_cfg.inject_outcome_mismatch) {
    VERIFY_STATE(result.final_outcome == "EmployeesWin", "Simulated injected outcome mismatch for regression testing");
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
        if (m.is_expected_rejection) {
          result.concurrent.expected_rejections++;
        } else {
          result.concurrent.successful_requests++;
        }
        conc_latencies.push_back(m.latency_us);
      } else {
        if (!m.transport_ok) result.concurrent.transport_failures++;
        if (!m.correctness_ok) result.concurrent.correctness_failures++;
      }
    } else {
      result.sequential.total_requests++;
      if (m.transport_ok && m.correctness_ok) {
        if (m.is_expected_rejection) {
          result.sequential.expected_rejections++;
        } else {
          result.sequential.successful_requests++;
        }
        seq_latencies.push_back(m.latency_us);
      } else {
        if (!m.transport_ok) result.sequential.transport_failures++;
        if (!m.correctness_ok) result.sequential.correctness_failures++;
      }
    }
  }

  result.total.successful_requests = result.sequential.successful_requests + result.concurrent.successful_requests;
  result.total.expected_rejections = result.sequential.expected_rejections + result.concurrent.expected_rejections;
  result.total.transport_failures = result.failures.transport_failures;
  result.total.correctness_failures = result.failures.correctness_failures;

  compute_percentiles(seq_latencies, result.sequential);
  compute_percentiles(conc_latencies, result.concurrent);
  compute_percentiles(all_latencies, result.total);

  result.run_passed = (result.failures.total() == 0);
  return result;
}

struct SustainedClassification {
  std::string status;  // PASS, FAIL, BLOCKED
  std::string reason;
  std::string diagnostic_details;
};

SustainedClassification classify_sustained_status(
    std::uint64_t target_duration_s,
    std::uint64_t actual_duration_ms,
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
    out.diagnostic_details = "Workload aborted after " + std::to_string(completed_matches) +
                             " completed matches and " + std::to_string(total_requests) +
                             " requests: " + out.reason;
  } else if (!reached_target_duration) {
    out.status = "BLOCKED";
    out.reason = "Workload stopped early before reaching target duration of " +
                 std::to_string(target_duration_s) + "s (actual: " +
                 std::to_string(static_cast<std::uint64_t>(elapsed_s)) + "s)";
    out.diagnostic_details = "Elapsed duration (" + std::to_string(actual_duration_ms) +
                             " ms) fell short of target duration (" +
                             std::to_string(target_duration_s * 1000ULL) + " ms)";
  } else if (completed_matches == 0) {
    out.status = "FAIL";
    out.reason = "Zero matches completed during workload";
    out.diagnostic_details = "Workload reached target duration but zero complete matches were finished";
  } else {
    out.status = "PASS";
    out.diagnostic_details = "Workload completed successfully with zero transport or correctness failures across " +
                             std::to_string(completed_matches) + " matches";
  }
  return out;
}

// Sustained repeated-match workload (for 10m smoke and 30m reliability)
SustainedStageResult execute_sustained_workload(
    const std::string& server_path,
    std::uint64_t duration_seconds,
    std::uint64_t seed,
    const std::string& stage_name,
    const TestFaultConfig& fault_cfg = {}) {
  SustainedStageResult result;
  result.stage_name = stage_name;
  result.target_duration_s = duration_seconds;
  result.seed = seed;

  const std::string socket_path = generate_unique_socket_path();
  const std::string mod_token = kDefaultModeratorToken;

  const auto server_pid = spawn_server_unix(server_path, socket_path, mod_token);
  if (server_pid <= 0) {
    result.status = "FAIL";
    result.reason = "Failed to spawn darkpool_server";
    result.diagnostic_details = "spawn_server_unix failed for path: " + server_path;
    return result;
  }

  const int probe_fd = connect_unix(socket_path);
  if (probe_fd < 0) {
    result.status = "FAIL";
    result.reason = "Failed to probe server on unix socket";
    result.diagnostic_details = "connect_unix failed for path: " + socket_path;
    stop_server_unix(server_pid, socket_path);
    return result;
  }
  (void)close(probe_fd);

  std::vector<std::uint64_t> all_latencies;
  FailureCounts failures;
  std::vector<RequestMetric> metrics;

  const auto start_time = std::chrono::steady_clock::now();
  const auto deadline = start_time + std::chrono::seconds(duration_seconds);

  std::cout << "[" << stage_name << "] Beginning sustained repeated-match workload (target: "
            << duration_seconds << "s, metadata seed: " << seed << ")..." << std::endl;

  bool abort_due_to_failure = false;

  while (std::chrono::steady_clock::now() < deadline && !abort_due_to_failure) {
    // 1. Moderator Connect
    int mod_fd = connect_unix(socket_path);
    if (mod_fd < 0) {
      result.unexpected_disconnects++;
      failures.transport_failures++;
      failures.last_failure_reason = "Failed to connect moderator socket";
      result.reason = failures.last_failure_reason;
      abort_due_to_failure = true;
      break;
    }

    const auto mod_hello = timed_request(
        mod_fd,
        json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}},
        metrics, failures,
        expect_connect("moderator"));
    if (!mod_hello) {
      result.reason = "Moderator handshake failed: " + failures.last_failure_reason;
      abort_due_to_failure = true;
      (void)close(mod_fd);
      break;
    }

    // 2. 64 Players Connect
    std::vector<int> player_fds;
    player_fds.reserve(kExpectedPlayerCount);
    for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
      const int pfd = connect_unix(socket_path);
      if (pfd < 0) {
        result.unexpected_disconnects++;
        failures.transport_failures++;
        failures.last_failure_reason = "Failed to connect socket for player " + std::to_string(i);
        result.reason = failures.last_failure_reason;
        abort_due_to_failure = true;
        break;
      }
      player_fds.push_back(pfd);
      const auto p_hello = timed_request(
          pfd, json{{"type", "connect"}, {"role", "player"}},
          metrics, failures,
          expect_connect("player"));
      if (!p_hello) {
        result.reason = "Player " + std::to_string(i) + " connect failed: " + failures.last_failure_reason;
        abort_due_to_failure = true;
        break;
      }
    }
    if (abort_due_to_failure) {
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }

    // 3. Create Match
    const auto create_resp = timed_request(
        mod_fd,
        json{{"type", "create_match"}, {"player_count", kExpectedPlayerCount}},
        metrics, failures,
        expect_match_created(kExpectedPlayerCount, kExpectedInsiders, kExpectedInvestigators, kExpectedEmployees));
    if (!create_resp) {
      result.reason = "Create match failed: " + failures.last_failure_reason;
      abort_due_to_failure = true;
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }
    const auto match_id = (*create_resp)["match_id"].get<std::uint64_t>();

    // 4. Players Join
    std::vector<int> insider_fds;
    for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
      const auto join_resp = timed_request(
          player_fds[i],
          json{{"type", "join_match"}, {"match_id", match_id}},
          metrics, failures,
          expect_joined_match(static_cast<std::uint32_t>(i + 1U)));
      if (!join_resp) {
        result.reason = "Player " + std::to_string(i + 1U) + " join failed: " + failures.last_failure_reason;
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

    if (insider_fds.size() != kExpectedInsiders) {
      failures.correctness_failures++;
      failures.last_failure_reason = "Insider count mismatch in sustained workload: expected " +
                                     std::to_string(kExpectedInsiders) + ", got " +
                                     std::to_string(insider_fds.size());
      result.reason = failures.last_failure_reason;
      abort_due_to_failure = true;
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }

    // 5. Start Match
    const auto start_resp = timed_request(
        mod_fd,
        json{{"type", "start_match"}, {"match_id", match_id}},
        metrics, failures,
        expect_transition("AwaitingEvent"));
    if (!start_resp) {
      result.reason = "Start match failed: " + failures.last_failure_reason;
      abort_due_to_failure = true;
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }

    // 6. Run 3 Rounds
    for (std::uint32_t round = 1; round <= 3U && !abort_due_to_failure; ++round) {
      // 6a. Publish Event
      const int event_impact = (round == 1U) ? 1 : (round == 2U) ? 0 : -1;
      const auto event_resp = timed_request(
          mod_fd,
          json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", event_impact}},
          metrics, failures,
          expect_transition("Discussion"));
      if (!event_resp) {
        result.reason = "Publish event round " + std::to_string(round) + " failed: " + failures.last_failure_reason;
        abort_due_to_failure = true;
        break;
      }

      // 6b. Hints for all Insiders
      for (std::size_t idx = 0; idx < insider_fds.size(); ++idx) {
        const auto hint_resp = timed_request(
            insider_fds[idx],
            json{{"type", "insider_hint"}, {"match_id", match_id}},
            metrics, failures,
            expect_insider_hint(event_impact));
        if (!hint_resp) {
          result.reason = "Insider hint round " + std::to_string(round) + " failed for insider " +
                          std::to_string(idx) + ": " + failures.last_failure_reason;
          abort_due_to_failure = true;
          break;
        }
      }
      if (abort_due_to_failure) break;

      // 6c. Open Trading
      const auto trade_phase = timed_request(
          mod_fd,
          json{{"type", "open_trading"}, {"match_id", match_id}},
          metrics, failures,
          expect_transition("Trading"));
      if (!trade_phase) {
        result.reason = "Open trading round " + std::to_string(round) + " failed: " + failures.last_failure_reason;
        abort_due_to_failure = true;
        break;
      }

      // 6d. Player Trades
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        json trade_req = json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}};
        if (fault_cfg.inject_semantic_failure && round == 1U && i == 0U) {
          trade_req["match_id"] = match_id + 999999U; // intentional mismatch to trigger server error
        }
        const auto trade_resp = timed_request(
            player_fds[i],
            trade_req,
            metrics, failures,
            expect_trade_accepted());
        if (!trade_resp) {
          result.reason = "Trade round " + std::to_string(round) + " failed for player " +
                          std::to_string(i + 1U) + ": " + failures.last_failure_reason;
          abort_due_to_failure = true;
          break;
        }
      }
      if (abort_due_to_failure) break;

      // 6e. Resolve Market
      const auto market_resp = timed_request(
          mod_fd,
          json{{"type", "resolve_market"}, {"match_id", match_id}},
          metrics, failures,
          expect_market_result());
      if (!market_resp) {
        result.reason = "Resolve market round " + std::to_string(round) + " failed: " + failures.last_failure_reason;
        abort_due_to_failure = true;
        break;
      }

      // 6f. Open Voting
      const auto vote_phase = timed_request(
          mod_fd,
          json{{"type", "open_voting"}, {"match_id", match_id}},
          metrics, failures,
          expect_transition("Voting"));
      if (!vote_phase) {
        result.reason = "Open voting round " + std::to_string(round) + " failed: " + failures.last_failure_reason;
        abort_due_to_failure = true;
        break;
      }

      // 6g. Player Votes
      for (std::size_t i = 0; i < kExpectedPlayerCount; ++i) {
        const auto vote_resp = timed_request(
            player_fds[i],
            json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
            metrics, failures,
            expect_vote_accepted());
        if (!vote_resp) {
          result.reason = "Vote round " + std::to_string(round) + " failed for player " +
                          std::to_string(i + 1U) + ": " + failures.last_failure_reason;
          abort_due_to_failure = true;
          break;
        }
      }
      if (abort_due_to_failure) break;

      // 6h. Resolve Votes
      const auto resolve_resp = timed_request(
          mod_fd,
          json{{"type", "resolve_votes"}, {"match_id", match_id}},
          metrics, failures,
          expect_vote_resolution());
      if (!resolve_resp) {
        result.reason = "Resolve votes round " + std::to_string(round) + " failed: " + failures.last_failure_reason;
        abort_due_to_failure = true;
        break;
      }
    }
    if (abort_due_to_failure) {
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
    }

    // 7. Verify Terminal Match Status
    const auto status_resp = timed_request(
        mod_fd,
        json{{"type", "match_status"}, {"match_id", match_id}},
        metrics, failures,
        expect_match_status("Finished", "InsiderWin"));
    if (!status_resp) {
      result.reason = "Terminal match status check failed: " + failures.last_failure_reason;
      abort_due_to_failure = true;
      for (const auto pfd : player_fds) (void)close(pfd);
      (void)close(mod_fd);
      break;
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
      if (m.is_expected_rejection) {
        result.metrics.expected_rejections++;
      } else {
        result.metrics.successful_requests++;
      }
    } else {
      if (!m.transport_ok) result.metrics.transport_failures++;
      if (!m.correctness_ok) result.metrics.correctness_failures++;
    }
  }

  compute_percentiles(all_latencies, result.metrics);

  const double elapsed_s = static_cast<double>(result.actual_duration_ms) / 1000.0;
  result.throughput_rps = elapsed_s > 0.0 ? (static_cast<double>(result.metrics.total_requests) / elapsed_s) : 0.0;

  const auto classification = classify_sustained_status(
      result.target_duration_s,
      result.actual_duration_ms,
      result.completed_matches,
      result.metrics.total_requests,
      failures.total(),
      failures.transport_failures,
      failures.correctness_failures,
      abort_due_to_failure,
      result.reason);
  result.status = classification.status;
  result.reason = classification.reason;
  result.diagnostic_details = classification.diagnostic_details;

  std::cout << "\n=================================================================\n";
  std::cout << " SUSTAINED WORKLOAD REPORT (" << result.stage_name << ")\n";
  std::cout << "=================================================================\n";
  std::cout << " Status              : " << result.status << "\n";
  std::cout << " Target Duration     : " << result.target_duration_s << " s\n";
  std::cout << " Actual Duration     : " << std::fixed << std::setprecision(2)
            << elapsed_s << " s (" << result.actual_duration_ms << " ms)\n";
  std::cout << " Completed Matches   : " << result.completed_matches << "\n";
  std::cout << " Planned Resets      : " << result.planned_resets << "\n";
  std::cout << " Unexpected Disconn  : " << result.unexpected_disconnects << "\n";
  std::cout << " Total Requests      : " << result.metrics.total_requests << "\n";
  std::cout << " Successful Requests : " << result.metrics.successful_requests << "\n";
  std::cout << " Expected Rejections : " << result.metrics.expected_rejections << "\n";
  std::cout << " Transport Failures  : " << result.metrics.transport_failures << "\n";
  std::cout << " Correctness Failures: " << result.metrics.correctness_failures << "\n";
  std::cout << " Latency (p50/p95/p99: " << result.metrics.p50_ms << " / "
            << result.metrics.p95_ms << " / " << result.metrics.p99_ms << " ms\n";
  std::cout << " Throughput          : " << std::setprecision(1) << result.throughput_rps
            << " req/s (denominator: actual elapsed duration in seconds)\n";
  std::cout << " Resources           : Peak RSS=" << result.peak_rss_kb << " kB, CPU="
            << result.cpu_time_ms << " ms, FDs=" << result.open_fds << "\n";
  if (!result.reason.empty()) {
    std::cout << " Failure Reason      : " << result.reason << "\n";
  }
  std::cout << "=================================================================\n\n";

  return result;
}

bool write_sustained_report(
    const SustainedStageResult& res,
    const std::string& output_path,
    const std::string& source_commit,
    const std::string& env_info) {
  json j = json::object();
  j["test_name"] = "darkpool_load_tests_64_player_sustained";
  j["stage_name"] = res.stage_name;
  j["status"] = res.status;  // PASS, FAIL, NOT RUN, or BLOCKED
  j["source_commit"] = source_commit;
  j["environment"] = env_info;
  j["workload_seed"] = res.seed;
  j["workload_seed_description"] = "Workload seed recorded as execution metadata; workload executes deterministic 64-player reference scenario";
  j["target_duration_s"] = res.target_duration_s;
  j["actual_duration_ms"] = res.actual_duration_ms;
  j["actual_duration_s"] = static_cast<double>(res.actual_duration_ms) / 1000.0;
  j["completed_matches"] = res.completed_matches;
  j["planned_resets"] = res.planned_resets;
  j["unexpected_disconnects"] = res.unexpected_disconnects;

  j["requests"] = {
    {"total_requests", res.metrics.total_requests},
    {"successful_requests", res.metrics.successful_requests},
    {"expected_rejections", res.metrics.expected_rejections},
    {"transport_failures", res.metrics.transport_failures},
    {"correctness_failures", res.metrics.correctness_failures}
  };

  j["total_requests"] = res.metrics.total_requests;
  j["successful_requests"] = res.metrics.successful_requests;
  j["expected_rejections"] = res.metrics.expected_rejections;
  j["transport_failures"] = res.metrics.transport_failures;
  j["correctness_failures"] = res.metrics.correctness_failures;

  j["latency"] = {
    {"p50_ms", res.metrics.p50_ms},
    {"p95_ms", res.metrics.p95_ms},
    {"p99_ms", res.metrics.p99_ms},
    {"mean_ms", res.metrics.mean_ms},
    {"min_ms", res.metrics.min_ms},
    {"max_ms", res.metrics.max_ms},
    {"measurement_definition", "Monotonic round-trip duration measured with std::chrono::steady_clock starting immediately before request frame bytes are written to socket and ending immediately when complete correlated response frame is received and parsed; excludes inter-match and inter-phase pacing."}
  };

  j["throughput"] = {
    {"requests_per_second", res.throughput_rps},
    {"denominator", "Actual elapsed duration in seconds (actual_duration_ms / 1000.0)"}
  };

  j["resources"] = {
    {"peak_rss_kb", res.peak_rss_kb},
    {"cpu_time_ms", res.cpu_time_ms},
    {"open_fds", res.open_fds}
  };

  j["failure_reason"] = res.reason;
  j["diagnostic_details"] = res.diagnostic_details.empty() ? (res.reason.empty() ? "None" : res.reason) : res.diagnostic_details;
  j["timestamp"] = std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

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

bool run_semantic_response_regression_test(const std::string& harness_path, const std::string& server_path) {
  std::cout << "=================================================================\n";
  std::cout << " Running Semantic Response Validation Regression Tests\n";
  std::cout << "=================================================================\n";

  // Test 1: Socketpair test with valid JSON frame containing semantic error payload
  std::cout << "[Semantic 1/6] Verifying timed_request rejects parseable error frame... " << std::flush;
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
    std::cout << "FAILED (socketpair creation error)\n";
    return false;
  }

  json err_payload = {{"type", "error"}, {"error", "simulated protocol error"}};
  const auto frame_bytes = Frame(err_payload).serialize();
  if (!send_all(sv[1], frame_bytes.data(), frame_bytes.size())) {
    (void)close(sv[0]);
    (void)close(sv[1]);
    std::cout << "FAILED (cannot write simulated frame)\n";
    return false;
  }

  std::vector<RequestMetric> test_metrics;
  FailureCounts test_failures;
  // Request expects trade accepted, but will receive parseable error payload
  const auto resp = timed_request(
      sv[0],
      json{{"type", "trade"}, {"match_id", 1}, {"action", "HOLD"}},
      test_metrics,
      test_failures,
      expect_trade_accepted());

  (void)close(sv[0]);
  (void)close(sv[1]);

  if (resp.has_value()) {
    std::cout << "FAILED (timed_request returned value on semantic failure)\n";
    return false;
  }
  if (test_failures.transport_failures != 0) {
    std::cout << "FAILED (transport_failures should be 0 for valid JSON frame, got "
              << test_failures.transport_failures << ")\n";
    return false;
  }
  if (test_failures.correctness_failures != 1) {
    std::cout << "FAILED (correctness_failures should be 1, got "
              << test_failures.correctness_failures << ")\n";
    return false;
  }
  if (test_metrics.size() != 1) {
    std::cout << "FAILED (expected 1 metric, got " << test_metrics.size() << ")\n";
    return false;
  }
  if (!test_metrics[0].transport_ok) {
    std::cout << "FAILED (transport_ok should be true for valid frame)\n";
    return false;
  }
  if (test_metrics[0].correctness_ok) {
    std::cout << "FAILED (correctness_ok must be false for semantic error)\n";
    return false;
  }
  std::cout << "PASSED (transport_ok=true, correctness_ok=false, correctness_failures=1)\n";

  // Test 2: Verify sustained workload fails and accounts correctly on semantic error
  std::cout << "[Semantic 2/6] Verifying sustained run aborts and fails on semantic error... " << std::flush;
  TestFaultConfig fault;
  fault.inject_semantic_failure = true;
  const auto sustained_res = execute_sustained_workload(server_path, 5, 42, "semantic_test", fault);
  if (sustained_res.status != "FAIL") {
    std::cout << "FAILED (expected status FAIL, got " << sustained_res.status << ")\n";
    return false;
  }
  if (sustained_res.metrics.correctness_failures == 0) {
    std::cout << "FAILED (correctness_failures should be >= 1, got 0)\n";
    return false;
  }
  if (sustained_res.reason.empty()) {
    std::cout << "FAILED (expected non-empty failure reason)\n";
    return false;
  }
  std::cout << "PASSED (status=FAIL, correctness_failures="
            << sustained_res.metrics.correctness_failures << ", reason logged)\n";

  // Test 3: Subprocess execution with --inject-semantic-failure
  std::cout << "[Semantic 3/6] Verifying subprocess with semantic error exits nonzero... " << std::flush;
  const int code = run_subprocess(harness_path, {"--server", server_path, "--inject-semantic-failure"});
  if (code == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code << ")\n";

  // Test 4: Duration classification strictly enforces full target duration (no early-pass tolerance)
  std::cout << "[Semantic 4/6] Verifying duration classification rejects runs finishing early (< target_duration)... " << std::flush;
  {
    // 600s target (smoke 10m): 599000 ms must be BLOCKED
    const auto c1 = classify_sustained_status(600, 599000, 10, 100, 0, 0, 0, false, "");
    if (c1.status != "BLOCKED") {
      std::cout << "FAILED (expected BLOCKED for 599000 ms / 600s target, got " << c1.status << ")\n";
      return false;
    }

    // 600s target: 599999 ms must be BLOCKED (proving removal of 1-second early-pass tolerance)
    const auto c2 = classify_sustained_status(600, 599999, 10, 100, 0, 0, 0, false, "");
    if (c2.status != "BLOCKED") {
      std::cout << "FAILED (expected BLOCKED for 599999 ms / 600s target, got " << c2.status << ")\n";
      return false;
    }

    // 600s target: 600000 ms (exact) must be PASS
    const auto c3 = classify_sustained_status(600, 600000, 10, 100, 0, 0, 0, false, "");
    if (c3.status != "PASS") {
      std::cout << "FAILED (expected PASS for 600000 ms / 600s target, got " << c3.status << ")\n";
      return false;
    }

    // 600s target: 601000 ms must be PASS
    const auto c4 = classify_sustained_status(600, 601000, 10, 100, 0, 0, 0, false, "");
    if (c4.status != "PASS") {
      std::cout << "FAILED (expected PASS for 601000 ms / 600s target, got " << c4.status << ")\n";
      return false;
    }

    // 600s target: reached duration but with failures must be FAIL
    const auto c5 = classify_sustained_status(600, 600000, 10, 100, 1, 0, 1, false, "");
    if (c5.status != "FAIL") {
      std::cout << "FAILED (expected FAIL when failures > 0, got " << c5.status << ")\n";
      return false;
    }

    // 600s target: reached duration but 0 completed matches must be FAIL
    const auto c6 = classify_sustained_status(600, 600000, 0, 0, 0, 0, 0, false, "");
    if (c6.status != "FAIL") {
      std::cout << "FAILED (expected FAIL when completed matches == 0, got " << c6.status << ")\n";
      return false;
    }

    // 1800s target (reliability 30m): 1799999 ms must be BLOCKED
    const auto c7 = classify_sustained_status(1800, 1799999, 30, 300, 0, 0, 0, false, "");
    if (c7.status != "BLOCKED") {
      std::cout << "FAILED (expected BLOCKED for 1799999 ms / 1800s target, got " << c7.status << ")\n";
      return false;
    }

    // 1800s target: 1800000 ms (exact) must be PASS
    const auto c8 = classify_sustained_status(1800, 1800000, 30, 300, 0, 0, 0, false, "");
    if (c8.status != "PASS") {
      std::cout << "FAILED (expected PASS for 1800000 ms / 1800s target, got " << c8.status << ")\n";
      return false;
    }
  }
  std::cout << "PASSED (exact duration boundary enforced; early finishes marked BLOCKED)\n";

  // Test 5: Direct write_sustained_report failure check with unwritable path
  std::cout << "[Semantic 5/6] Verifying write_sustained_report returns false on unwritable path... " << std::flush;
  {
    SustainedStageResult dummy_res;
    dummy_res.stage_name = "test_stage";
    dummy_res.target_duration_s = 600;
    dummy_res.actual_duration_ms = 600000;
    dummy_res.status = "PASS";
    dummy_res.completed_matches = 10;
    const bool write_result = write_sustained_report(dummy_res, "/dev/null/darkpool_invalid_output.json", "dummy_commit", "dummy_env");
    if (write_result) {
      std::cout << "FAILED (write_sustained_report should return false for invalid path)\n";
      return false;
    }
  }
  std::cout << "PASSED (write_sustained_report returned false)\n";

  // Test 6: Subprocess execution with unwritable output path exits nonzero even if workload passes
  std::cout << "[Semantic 6/6] Verifying subprocess exits nonzero when sustained report cannot be written... " << std::flush;
  const int code_unwritable = run_subprocess(
      harness_path,
      {"--server", server_path, "--duration", "1", "--out", "/dev/null/darkpool_invalid_output.json"});
  if (code_unwritable == 0) {
    std::cout << "FAILED (expected nonzero exit code when report cannot be written, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_unwritable << ")\n";

  std::cout << "\nAll semantic response validation and duration/report regression tests PASSED.\n\n";
  return true;
}

bool run_regression_selftest(const std::string& harness_path, const std::string& server_path) {
  std::cout << "=================================================================\n";
  std::cout << " Running 64-Player Load Harness Subprocess Regression Tests\n";
  std::cout << "=================================================================\n";

  std::cout << "[Regression 1/5] Verifying normal 64P run subprocess exits 0... " << std::flush;
  const int code_normal = run_subprocess(harness_path, {"--server", server_path, "--smoke-run"});
  if (code_normal != 0) {
    std::cout << "FAILED (expected exit code 0, got " << code_normal << ")\n";
    return false;
  }
  std::cout << "PASSED (exit code 0)\n";

  std::cout << "[Regression 2/5] Verifying assertion failure subprocess exits nonzero... " << std::flush;
  const int code_assert = run_subprocess(harness_path, {"--server", server_path, "--inject-assertion-failure"});
  if (code_assert == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_assert << ")\n";

  std::cout << "[Regression 3/5] Verifying outcome mismatch subprocess exits nonzero... " << std::flush;
  const int code_outcome = run_subprocess(harness_path, {"--server", server_path, "--inject-outcome-mismatch"});
  if (code_outcome == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_outcome << ")\n";

  std::cout << "[Regression 4/5] Verifying semantic error subprocess exits nonzero... " << std::flush;
  const int code_semantic = run_subprocess(harness_path, {"--server", server_path, "--inject-semantic-failure"});
  if (code_semantic == 0) {
    std::cout << "FAILED (expected nonzero exit code, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_semantic << ")\n";

  std::cout << "[Regression 5/5] Verifying unwritable report output path subprocess exits nonzero... " << std::flush;
  const int code_unwritable = run_subprocess(
      harness_path,
      {"--server", server_path, "--duration", "1", "--out", "/dev/null/darkpool_invalid_output.json"});
  if (code_unwritable == 0) {
    std::cout << "FAILED (expected nonzero exit code when report cannot be written, got 0)\n";
    return false;
  }
  std::cout << "PASSED (nonzero exit code: " << code_unwritable << ")\n";

  std::cout << "\nAll 64-player regression checks PASSED: real subprocess exit statuses verified.\n\n";
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string server_path = "./darkpool_server";
  std::string output_json_path = "load_test_64p_results.json";
  bool out_explicitly_set = false;
  std::string source_commit = DARKPOOL_SOURCE_COMMIT;
  int repeat_runs = 3;
  std::uint64_t custom_duration_s = 0;
  std::uint64_t seed = 42;
  bool is_smoke_10m = false;
  bool is_reliability_30m = false;
  bool is_selftest = false;
  bool is_test_semantic = false;
  bool is_smoke_run = false;
  bool is_inject_assertion = false;
  bool is_inject_outcome = false;
  bool is_inject_semantic = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--server" && i + 1 < argc) {
      server_path = argv[++i];
    } else if (arg == "--out" && i + 1 < argc) {
      output_json_path = argv[++i];
      out_explicitly_set = true;
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
    } else if (arg == "--test-semantic-response") {
      is_test_semantic = true;
    } else if (arg == "--smoke-run") {
      is_smoke_run = true;
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
  if (is_inject_semantic) {
    TestFaultConfig fault;
    fault.inject_semantic_failure = true;
    const auto res = execute_64_player_match(server_path, 1, fault);
    return res.run_passed ? 0 : 1;
  }
  if (is_selftest) {
    const auto self_exe = get_self_executable_path(argv[0]);
    return run_regression_selftest(self_exe, server_path) ? 0 : 1;
  }
  if (is_test_semantic) {
    const auto self_exe = get_self_executable_path(argv[0]);
    return run_semantic_response_regression_test(self_exe, server_path) ? 0 : 1;
  }

  const std::string env_info = get_environment_info();

  // Handle sustained workload execution paths
  if (is_smoke_10m || is_reliability_30m || custom_duration_s > 0) {
    if (!out_explicitly_set) {
      output_json_path = "load_test_64p_sustained_results.json";
    }

    if (is_smoke_10m) {
      const auto res = execute_sustained_workload(server_path, 600, seed, "smoke_10m");
      const bool report_ok = write_sustained_report(res, output_json_path, source_commit, env_info);
      if (!report_ok) {
        std::cerr << "Error: Failed to persist sustained test report to '" << output_json_path << "'\n";
        return 1;
      }
      return res.status == "PASS" ? 0 : 1;
    }
    if (is_reliability_30m) {
      const auto res = execute_sustained_workload(server_path, 1800, seed, "reliability_30m");
      const bool report_ok = write_sustained_report(res, output_json_path, source_commit, env_info);
      if (!report_ok) {
        std::cerr << "Error: Failed to persist sustained test report to '" << output_json_path << "'\n";
        return 1;
      }
      return res.status == "PASS" ? 0 : 1;
    }
    if (custom_duration_s > 0) {
      const auto res = execute_sustained_workload(server_path, custom_duration_s, seed, "custom_sustained");
      const bool report_ok = write_sustained_report(res, output_json_path, source_commit, env_info);
      if (!report_ok) {
        std::cerr << "Error: Failed to persist sustained test report to '" << output_json_path << "'\n";
        return 1;
      }
      return res.status == "PASS" ? 0 : 1;
    }
  }

  // Default mode or --functional: Run 3-run 64-player functional match suite
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
      std::cout << "         Sequential : " << result.sequential.total_requests << " reqs ("
                << result.sequential.successful_requests << " success, "
                << result.sequential.expected_rejections << " rejections), "
                << "p50=" << std::fixed << std::setprecision(2) << result.sequential.p50_ms << " ms, "
                << "p95=" << result.sequential.p95_ms << " ms, "
                << "p99=" << result.sequential.p99_ms << " ms\n";
      std::cout << "         Concurrent : " << result.concurrent.total_requests << " reqs ("
                << result.concurrent.successful_requests << " success), "
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
          if (m.is_expected_rejection) {
            aggregate_conc.expected_rejections++;
          } else {
            aggregate_conc.successful_requests++;
          }
          aggregate_conc_latencies.push_back(m.latency_us);
        } else {
          if (!m.transport_ok) aggregate_conc.transport_failures++;
          if (!m.correctness_ok) aggregate_conc.correctness_failures++;
        }
      } else {
        aggregate_seq.total_requests++;
        if (m.transport_ok && m.correctness_ok) {
          if (m.is_expected_rejection) {
            aggregate_seq.expected_rejections++;
          } else {
            aggregate_seq.successful_requests++;
          }
          aggregate_seq_latencies.push_back(m.latency_us);
        } else {
          if (!m.transport_ok) aggregate_seq.transport_failures++;
          if (!m.correctness_ok) aggregate_seq.correctness_failures++;
        }
      }
    }
  }

  aggregate_total.successful_requests = aggregate_seq.successful_requests + aggregate_conc.successful_requests;
  aggregate_total.expected_rejections = aggregate_seq.expected_rejections + aggregate_conc.expected_rejections;
  aggregate_total.transport_failures = aggregate_seq.transport_failures + aggregate_conc.transport_failures;
  aggregate_total.correctness_failures = aggregate_seq.correctness_failures + aggregate_conc.correctness_failures;

  compute_percentiles(aggregate_seq_latencies, aggregate_seq);
  compute_percentiles(aggregate_conc_latencies, aggregate_conc);
  compute_percentiles(aggregate_all_latencies, aggregate_total);

  std::cout << "=================================================================\n";
  std::cout << " SUMMARY RESULTS (Three Repeatable 64-Player Runs / L4)\n";
  std::cout << "=================================================================\n";
  std::cout << "| Run | Clients | Duration | Sequential (p50/p95/p99) | Concurrent (p50/p95/p99) | Rejections | Failures (Trans/Corr) | Peak RSS | CPU | FDs |\n";
  std::cout << "|---|---|---|---|---|---|---|---|---|---|\n";
  for (const auto& r : run_results) {
    std::cout << "| Run " << r.run_index << " | " << r.client_count << " | "
              << r.duration_ms << " ms | "
              << std::fixed << std::setprecision(2)
              << r.sequential.p50_ms << " / " << r.sequential.p95_ms << " / " << r.sequential.p99_ms << " ms | "
              << r.concurrent.p50_ms << " / " << r.concurrent.p95_ms << " / " << r.concurrent.p99_ms << " ms | "
              << r.total.expected_rejections << " | "
              << r.failures.transport_failures << " / " << r.failures.correctness_failures << " | "
              << r.peak_rss_kb << " kB | " << std::setprecision(1) << r.cpu_time_ms << " ms | "
              << r.open_fds << " |\n";
  }
  std::cout << "| Aggregate | " << (kExpectedPlayerCount + 1U) << " | "
            << total_duration_ms << " ms | "
            << std::fixed << std::setprecision(2)
            << aggregate_seq.p50_ms << " / " << aggregate_seq.p95_ms << " / " << aggregate_seq.p99_ms << " ms | "
            << aggregate_conc.p50_ms << " / " << aggregate_conc.p95_ms << " / " << aggregate_conc.p99_ms << " ms | "
            << aggregate_total.expected_rejections << " | "
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
    run_obj["expected_rejections"] = r.total.expected_rejections;
    run_obj["peak_rss_kb"] = r.peak_rss_kb;
    run_obj["cpu_time_ms"] = r.cpu_time_ms;
    run_obj["open_fds"] = r.open_fds;
    run_obj["final_phase"] = r.final_phase;
    run_obj["final_outcome"] = r.final_outcome;

    run_obj["sequential"] = {
      {"total_requests", r.sequential.total_requests},
      {"successful_requests", r.sequential.successful_requests},
      {"expected_rejections", r.sequential.expected_rejections},
      {"p50_ms", r.sequential.p50_ms},
      {"p95_ms", r.sequential.p95_ms},
      {"p99_ms", r.sequential.p99_ms},
      {"mean_ms", r.sequential.mean_ms}
    };
    run_obj["concurrent"] = {
      {"total_requests", r.concurrent.total_requests},
      {"successful_requests", r.concurrent.successful_requests},
      {"expected_rejections", r.concurrent.expected_rejections},
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
    {"expected_rejections", aggregate_seq.expected_rejections},
    {"p50_ms", aggregate_seq.p50_ms},
    {"p95_ms", aggregate_seq.p95_ms},
    {"p99_ms", aggregate_seq.p99_ms},
    {"mean_ms", aggregate_seq.mean_ms}
  };
  report["concurrent_summary"] = {
    {"total_requests", aggregate_conc.total_requests},
    {"successful_requests", aggregate_conc.successful_requests},
    {"expected_rejections", aggregate_conc.expected_rejections},
    {"p50_ms", aggregate_conc.p50_ms},
    {"p95_ms", aggregate_conc.p95_ms},
    {"p99_ms", aggregate_conc.p99_ms},
    {"mean_ms", aggregate_conc.mean_ms}
  };
  report["total_summary"] = {
    {"total_requests", aggregate_total.total_requests},
    {"successful_requests", aggregate_total.successful_requests},
    {"expected_rejections", aggregate_total.expected_rejections},
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
  bool report_written = false;
  if (out_file.is_open()) {
    out_file << report.dump(2) << '\n';
    out_file.flush();
    if (out_file.good() && !out_file.fail() && !out_file.bad()) {
      out_file.close();
      if (!out_file.fail()) {
        report_written = true;
      }
    }
  }
  if (report_written) {
    std::cout << "Machine-readable report written to: " << output_json_path << "\n\n";
  } else {
    std::cerr << "Error: Could not open or write complete report output file: " << output_json_path << "\n";
    return 1;
  }

  return all_passed ? 0 : 1;
}

#endif  // !defined(__linux__)
