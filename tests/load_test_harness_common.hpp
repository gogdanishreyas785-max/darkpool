#pragma once

#if defined(__linux__)

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

namespace darkpool::load_test {

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

constexpr std::size_t kExpectedL6Matches = 4;
constexpr std::size_t kExpectedL6TotalPlayers = 256;

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

inline bool send_all(int fd, const char* data, std::size_t size) {
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

inline bool receive_exact(int fd, char* data, std::size_t size) {
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

inline std::optional<json> timed_request(
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

inline std::string generate_unique_socket_path() {
  static std::atomic<std::uint64_t> counter{0};
  const auto pid = getpid();
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return "/tmp/dp_l_" + std::to_string(pid) + "_" + std::to_string(counter.fetch_add(1)) +
         "_" + std::to_string(now % 1000000) + ".sock";
}

inline int connect_unix(const std::string& path, int max_attempts = 150) {
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

inline pid_t spawn_server_unix(const std::string& server_path, const std::string& socket_path, const std::string& token) {
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

inline void stop_server_unix(pid_t server_pid, const std::string& socket_path) {
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

inline std::uint64_t sample_peak_rss_kb(pid_t pid) {
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

inline double sample_cpu_time_ms(pid_t pid) {
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

inline std::size_t sample_open_fds(pid_t pid) {
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

inline std::string detect_git_commit() {
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

inline std::string get_environment_info() {
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

inline void compute_percentiles(std::vector<std::uint64_t>& latencies_us, PhaseMetrics& out) {
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

inline int run_subprocess(const std::string& binary, const std::vector<std::string>& args) {
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

inline std::string get_self_executable_path(const char* argv0) {
  char buf[4096];
  const auto len = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (len > 0) {
    buf[len] = '\0';
    return std::string(buf);
  }
  return std::string(argv0 ? argv0 : "./darkpool_load_tests_64p");
}

}  // namespace darkpool::load_test

#endif  // defined(__linux__)
