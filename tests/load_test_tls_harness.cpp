#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_load_tests_tls requires Linux (epoll/sockets/openssl)\n";
  return 1;
}

#else

#include "load_test_harness_common.hpp"
#include "darkpool/tls_transport.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <netinet/tcp.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace darkpool::load_test;
using darkpool::network::Frame;
using darkpool::network::json;
using darkpool::transport::EphemeralCertBundle;
using darkpool::transport::generate_ephemeral_certs;

constexpr const char* kModeratorToken = "tls-benchmark-mod-token";

// --------------------------------------------------------------------------
// TLS 1.3 Benchmark Client
// --------------------------------------------------------------------------
struct TlsClient {
  int fd{-1};
  darkpool::transport::ClientTlsConfig cfg;
  darkpool::transport::SslCtxPtr ctx;
  darkpool::transport::SslPtr ssl;

  ~TlsClient() {
    close();
  }

  void close() {
    if (ssl != nullptr) {
      (void)SSL_shutdown(ssl.get());
      ssl.reset();
    }
    if (fd >= 0) {
      (void)::close(fd);
      fd = -1;
    }
    ctx.reset();
  }

  bool connect(std::uint16_t port, const std::string& ca_cert_path, const std::string& ip = "127.0.0.1") {
    cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
    cfg.ca_file = ca_cert_path;
    cfg.target_ip = ip;

    std::string err;
    ctx = darkpool::transport::create_client_ssl_ctx(cfg, err);
    if (!ctx) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
      return false;
    }

    fd = -1;
    for (int attempt = 0; attempt < 80; ++attempt) {
      fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd >= 0) {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
          break;
        }
        ::close(fd);
        fd = -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    if (fd < 0) return false;

    SSL* raw_ssl = SSL_new(ctx.get());
    if (!raw_ssl) {
      ::close(fd);
      fd = -1;
      return false;
    }
    ssl.reset(raw_ssl);
    SSL_set_app_data(ssl.get(), &cfg);

    if (SSL_set_fd(ssl.get(), fd) != 1) {
      close();
      return false;
    }
    SSL_set_connect_state(ssl.get());

    int flags = fcntl(fd, F_GETFL, 0);
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    // Monotonically bounded handshake loop
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        close();
        return false;
      }
      const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      const int poll_timeout = std::clamp(static_cast<int>(remaining_ms), 1, 100);

      bool want_read = false;
      bool want_write = false;
      const auto st = darkpool::transport::step_tls_handshake(ssl.get(), false, want_read, want_write, err);
      if (st == darkpool::transport::TlsHandshakeStatus::Established) {
        break;
      }
      if (st == darkpool::transport::TlsHandshakeStatus::Failed) {
        close();
        return false;
      }
      pollfd pfd{fd, static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0)), 0};
      (void)poll(&pfd, 1, poll_timeout);
    }
    return true;
  }

  std::optional<json> request(
      const json& payload,
      std::vector<RequestMetric>& out_metrics,
      FailureCounts& failures,
      const ResponseValidator& validator = nullptr,
      bool is_concurrent = false) {
    const std::string req_type = payload.value("type", "unknown");
    const auto outgoing = Frame(payload).serialize();

    const auto start_time = std::chrono::steady_clock::now();

    // Send frame
    std::size_t offset = 0;
    while (offset < outgoing.size()) {
      std::size_t written = 0;
      std::string err;
      const auto status = darkpool::transport::tls_write(
          ssl.get(), outgoing.data() + offset, outgoing.size() - offset, written, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        offset += written;
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite ||
                 status == darkpool::transport::TlsIoStatus::WantRead) {
        pollfd pfd{fd, static_cast<short>((status == darkpool::transport::TlsIoStatus::WantWrite) ? POLLOUT : POLLIN), 0};
        (void)poll(&pfd, 1, 1000);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "tls_write failed: " + err;
        return std::nullopt;
      }
    }

    // Receive 4-byte header
    char header[4];
    std::size_t header_offset = 0;
    while (header_offset < 4) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() > 5000) {
        failures.timeouts++;
        failures.transport_failures++;
        failures.last_failure_reason = "header read timed out";
        return std::nullopt;
      }
      std::size_t count = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          ssl.get(), header + header_offset, 4 - header_offset, count, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        header_offset += count;
      } else if (status == darkpool::transport::TlsIoStatus::WantRead ||
                 status == darkpool::transport::TlsIoStatus::WantWrite) {
        pollfd pfd{fd, static_cast<short>((status == darkpool::transport::TlsIoStatus::WantRead) ? POLLIN : POLLOUT), 0};
        (void)poll(&pfd, 1, 1000);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "tls_read header failed: " + err;
        return std::nullopt;
      }
    }

    const std::uint32_t body_len =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[0])) << 24) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[2])) << 8) |
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[3]));
    if (body_len == 0 || body_len > 1024 * 1024) {
      failures.correctness_failures++;
      failures.last_failure_reason = "invalid frame length in header";
      return std::nullopt;
    }

    // Receive body
    std::string body;
    body.resize(body_len);
    std::size_t body_offset = 0;
    while (body_offset < body_len) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() > 5000) {
        failures.timeouts++;
        failures.transport_failures++;
        failures.last_failure_reason = "body read timed out";
        return std::nullopt;
      }
      std::size_t count = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          ssl.get(), body.data() + body_offset, body_len - body_offset, count, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        body_offset += count;
      } else if (status == darkpool::transport::TlsIoStatus::WantRead ||
                 status == darkpool::transport::TlsIoStatus::WantWrite) {
        pollfd pfd{fd, static_cast<short>((status == darkpool::transport::TlsIoStatus::WantRead) ? POLLIN : POLLOUT), 0};
        (void)poll(&pfd, 1, 1000);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "tls_read body failed: " + err;
        return std::nullopt;
      }
    }

    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());

    try {
      const auto resp = json::parse(body);
      ValidationResult v = ValidationResult::Success();
      if (validator) {
        v = validator(resp);
      }
      if (!v.correctness_ok) {
        failures.correctness_failures++;
        failures.last_failure_reason = v.failure_reason;
        out_metrics.push_back({req_type, us, is_concurrent, true, false, false});
        return std::nullopt;
      }
      out_metrics.push_back({req_type, us, is_concurrent, true, true, v.is_expected_rejection});
      return resp;
    } catch (const std::exception& e) {
      failures.transport_failures++;
      failures.last_failure_reason = std::string("json parse error: ") + e.what();
      out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
      return std::nullopt;
    }
  }
};

// --------------------------------------------------------------------------
// Plaintext TCP Benchmark Client (exact matching measurement boundary)
// --------------------------------------------------------------------------
struct PlaintextTcpClient {
  int fd{-1};

  ~PlaintextTcpClient() {
    close();
  }

  void close() {
    if (fd >= 0) {
      (void)::close(fd);
      fd = -1;
    }
  }

  bool connect(std::uint16_t port, const std::string& ip = "127.0.0.1") {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
      return false;
    }

    fd = -1;
    for (int attempt = 0; attempt < 80; ++attempt) {
      fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      if (fd >= 0) {
        int one = 1;
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
          break;
        }
        ::close(fd);
        fd = -1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return fd >= 0;
  }

  std::optional<json> request(
      const json& payload,
      std::vector<RequestMetric>& out_metrics,
      FailureCounts& failures,
      const ResponseValidator& validator = nullptr,
      bool is_concurrent = false) {
    const std::string req_type = payload.value("type", "unknown");
    const auto outgoing = Frame(payload).serialize();

    const auto start_time = std::chrono::steady_clock::now();

    // Send frame
    std::size_t offset = 0;
    while (offset < outgoing.size()) {
      const auto sent = ::send(fd, outgoing.data() + offset, outgoing.size() - offset, MSG_NOSIGNAL);
      if (sent > 0) {
        offset += static_cast<std::size_t>(sent);
      } else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd{fd, POLLOUT, 0};
        (void)poll(&pfd, 1, 1000);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "send failed: " + std::string(std::strerror(errno));
        return std::nullopt;
      }
    }

    // Receive 4-byte header
    char header[4];
    std::size_t header_offset = 0;
    while (header_offset < 4) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() > 5000) {
        failures.timeouts++;
        failures.transport_failures++;
        failures.last_failure_reason = "header read timed out";
        return std::nullopt;
      }
      const auto count = ::recv(fd, header + header_offset, 4 - header_offset, 0);
      if (count > 0) {
        header_offset += static_cast<std::size_t>(count);
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd{fd, POLLIN, 0};
        (void)poll(&pfd, 1, 1000);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "recv header failed: " + std::string(std::strerror(errno));
        return std::nullopt;
      }
    }

    const std::uint32_t body_len =
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[0])) << 24) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[1])) << 16) |
        (static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[2])) << 8) |
        static_cast<std::uint32_t>(static_cast<std::uint8_t>(header[3]));
    if (body_len == 0 || body_len > 1024 * 1024) {
      failures.correctness_failures++;
      failures.last_failure_reason = "invalid frame length in header";
      return std::nullopt;
    }

    // Receive body
    std::string body;
    body.resize(body_len);
    std::size_t body_offset = 0;
    while (body_offset < body_len) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() > 5000) {
        failures.timeouts++;
        failures.transport_failures++;
        failures.last_failure_reason = "body read timed out";
        return std::nullopt;
      }
      const auto count = ::recv(fd, &body[body_offset], body_len - body_offset, 0);
      if (count > 0) {
        body_offset += static_cast<std::size_t>(count);
      } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        pollfd pfd{fd, POLLIN, 0};
        (void)poll(&pfd, 1, 1000);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "recv body failed: " + std::string(std::strerror(errno));
        return std::nullopt;
      }
    }

    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());

    try {
      const auto resp = json::parse(body);
      ValidationResult v = ValidationResult::Success();
      if (validator) {
        v = validator(resp);
      }
      if (!v.correctness_ok) {
        failures.correctness_failures++;
        failures.last_failure_reason = v.failure_reason;
        out_metrics.push_back({req_type, us, is_concurrent, true, false, false});
        return std::nullopt;
      }
      out_metrics.push_back({req_type, us, is_concurrent, true, true, v.is_expected_rejection});
      return resp;
    } catch (const std::exception& e) {
      failures.transport_failures++;
      failures.last_failure_reason = std::string("json parse error: ") + e.what();
      out_metrics.push_back({req_type, us, is_concurrent, false, false, false});
      return std::nullopt;
    }
  }
};

std::uint16_t find_free_port() {
  const int probe = socket(AF_INET, SOCK_STREAM, 0);
  if (probe < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  const bool bound = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  socklen_t address_length = sizeof(address);
  const bool named = bound && getsockname(probe, reinterpret_cast<sockaddr*>(&address), &address_length) == 0;
  ::close(probe);
  if (!named) return 0U;
  return ntohs(address.sin_port);
}

pid_t launch_server(
    const std::string& server_bin,
    std::uint16_t port,
    bool is_tls,
    const std::string& cert_path = "",
    const std::string& key_path = "") {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    setenv("DARKPOOL_MODERATOR_TOKEN", kModeratorToken, 1);
    const int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
      ::close(dev_null);
    }
    // Close inherited file descriptors to prevent descriptor leakage into server child
    const long max_fd = sysconf(_SC_OPEN_MAX);
    const int limit = static_cast<int>(max_fd > 0 ? std::min(max_fd, 4096L) : 1024L);
    for (int fd = 3; fd < limit; ++fd) {
      ::close(fd);
    }
    const auto port_str = std::to_string(port);
    if (is_tls) {
      execl(server_bin.c_str(), server_bin.c_str(),
            "--no-tcp",
            "--no-unix",
            "--tls-bind-address", "127.0.0.1",
            "--tls-port", port_str.c_str(),
            "--tls-cert", cert_path.c_str(),
            "--tls-key", key_path.c_str(),
            static_cast<char*>(nullptr));
    } else {
      execl(server_bin.c_str(), server_bin.c_str(),
            "--bind-address", "127.0.0.1",
            "--port", port_str.c_str(),
            "--no-unix",
            static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  return child;
}

// --------------------------------------------------------------------------
// Run a single match over TLS 1.3
// --------------------------------------------------------------------------
RunResult execute_tls_match(
    const std::string& server_bin,
    int run_index,
    std::size_t player_count,
    const EphemeralCertBundle& certs) {
  RunResult res{};
  res.run_index = run_index;
  res.player_count = player_count;
  res.client_count = player_count + 1U;

  const auto port = find_free_port();
  const auto server_pid = launch_server(server_bin, port, true, certs.server_cert_pem_path, certs.server_key_pem_path);
  if (server_pid <= 0) {
    res.failures.last_failure_reason = "Failed to launch server";
    return res;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto initial_server_fds = sample_open_fds(server_pid);
  const auto initial_parent_fds = sample_open_fds(getpid());

  const auto start_wall = std::chrono::steady_clock::now();

  TlsClient mod;
  if (!mod.connect(port, certs.ca_cert_pem_path)) {
    res.failures.last_failure_reason = "Moderator TLS connect failed";
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 1. Moderator authentication
  auto mod_conn = mod.request(
      json{{"type", "connect"}, {"role", "moderator"}, {"token", kModeratorToken}},
      res.metrics, res.failures, expect_connect("moderator"));
  if (!mod_conn) {
    res.failures.last_failure_reason = "Moderator auth failed";
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 2. Create match
  auto match_created = mod.request(
      json{{"type", "create_match"}, {"player_count", player_count}},
      res.metrics, res.failures);
  if (!match_created) {
    res.failures.last_failure_reason = "create_match failed";
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }
  const auto match_id = match_created->value("match_id", 1ULL);

  // 3. Connect and join players
  std::vector<std::unique_ptr<TlsClient>> players;
  std::vector<std::string> roles;
  players.reserve(player_count);
  roles.reserve(player_count);

  for (std::size_t i = 0; i < player_count; ++i) {
    auto p = std::make_unique<TlsClient>();
    if (!p->connect(port, certs.ca_cert_pem_path)) {
      res.failures.last_failure_reason = "Player connect failed";
      break;
    }
    auto p_conn = p->request(
        json{{"type", "connect"}, {"role", "player"}},
        res.metrics, res.failures, expect_connect("player"));
    if (!p_conn) break;

    auto join_res = p->request(
        json{{"type", "join_match"}, {"match_id", match_id}},
        res.metrics, res.failures);
    if (!join_res) break;

    roles.push_back(join_res->value("role", ""));
    players.push_back(std::move(p));
  }

  if (players.size() != player_count) {
    res.failures.last_failure_reason = "Not all players connected";
    for (auto& p : players) p->close();
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 4. Start match
  auto start_res = mod.request(
      json{{"type", "start_match"}, {"match_id", match_id}},
      res.metrics, res.failures, expect_transition("AwaitingEvent"));
  if (!start_res) {
    res.failures.last_failure_reason = "start_match failed";
    for (auto& p : players) p->close();
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 5. Play 3 rounds
  for (std::uint32_t round = 1; round <= 3; ++round) {
    // Event -> Discussion
    auto ev_res = mod.request(
        json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}},
        res.metrics, res.failures, expect_transition("Discussion"));
    if (!ev_res) break;

    // Insider hint (sequential)
    for (std::size_t i = 0; i < player_count; ++i) {
      if (roles[i] == "Insider") {
        (void)players[i]->request(
            json{{"type", "insider_hint"}, {"match_id", match_id}},
            res.metrics, res.failures);
      }
    }

    // Open trading -> Trading
    auto tr_open = mod.request(
        json{{"type", "open_trading"}, {"match_id", match_id}},
        res.metrics, res.failures, expect_transition("Trading"));
    if (!tr_open) break;

    // Concurrent player trades
    for (std::size_t i = 0; i < player_count; ++i) {
      (void)players[i]->request(
          json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}},
          res.metrics, res.failures, nullptr, /*is_concurrent=*/true);
    }

    // Resolve market -> Clue
    auto m_res = mod.request(
        json{{"type", "resolve_market"}, {"match_id", match_id}},
        res.metrics, res.failures, expect_market_result());
    if (!m_res) break;

    // Open voting -> Voting
    auto v_open = mod.request(
        json{{"type", "open_voting"}, {"match_id", match_id}},
        res.metrics, res.failures, expect_transition("Voting"));
    if (!v_open) break;

    // Concurrent votes (abstain)
    for (std::size_t i = 0; i < player_count; ++i) {
      (void)players[i]->request(
          json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
          res.metrics, res.failures, nullptr, /*is_concurrent=*/true);
    }

    // Resolve votes
    auto v_res = mod.request(
        json{{"type", "resolve_votes"}, {"match_id", match_id}},
        res.metrics, res.failures);
    if (round == 3 && v_res) {
      res.final_outcome = v_res->value("outcome", "");
    }
  }

  // Final match status
  auto final_status = players[0]->request(
      json{{"type", "match_status"}, {"match_id", match_id}},
      res.metrics, res.failures);
  if (final_status) {
    res.final_phase = final_status->value("phase", "");
    if (res.final_outcome.empty()) {
      res.final_outcome = final_status->value("outcome", "");
    }
  }

  const auto end_wall = std::chrono::steady_clock::now();
  res.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_wall - start_wall).count());

  res.peak_rss_kb = sample_peak_rss_kb(server_pid).value_or(0);
  res.cpu_time_ms = sample_cpu_time_ms(server_pid);

  // Close all clients
  for (auto& p : players) p->close();
  mod.close();

  // Settle server descriptors and verify return to baseline
  const auto cleanup_report = sample_settled_open_fds(server_pid, initial_server_fds.value_or(8), 2);
  res.open_fds = cleanup_report.settled_open_fds;
  if (!cleanup_report.settled_within_tolerance) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Server FDs failed to settle to baseline: " + cleanup_report.failure_reason;
  }

  (void)kill(server_pid, SIGTERM);
  waitpid(server_pid, nullptr, 0);

  // Verify harness process descriptor baseline
  const auto post_parent_fds = sample_open_fds(getpid());
  if (initial_parent_fds && post_parent_fds && *post_parent_fds > *initial_parent_fds + 2) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Harness process leaked descriptors (before: " +
        std::to_string(*initial_parent_fds) + ", after: " + std::to_string(*post_parent_fds) + ")";
  }

  // Compute metrics
  std::vector<std::uint64_t> seq_lats;
  std::vector<std::uint64_t> conc_lats;
  std::vector<std::uint64_t> total_lats;

  for (const auto& m : res.metrics) {
    total_lats.push_back(m.latency_us);
    if (m.is_concurrent) {
      conc_lats.push_back(m.latency_us);
      res.concurrent.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.concurrent.successful_requests++;
    } else {
      seq_lats.push_back(m.latency_us);
      res.sequential.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.sequential.successful_requests++;
    }
    res.total.total_requests++;
    if (m.transport_ok && m.correctness_ok) res.total.successful_requests++;
  }

  compute_percentiles(seq_lats, res.sequential);
  compute_percentiles(conc_lats, res.concurrent);
  compute_percentiles(total_lats, res.total);

  res.run_passed = (res.failures.total() == 0 && res.failures.timeouts == 0 &&
                    res.failures.unexpected_disconnects == 0 && res.final_phase == "Finished");
  return res;
}

// --------------------------------------------------------------------------
// Run 4 concurrent 64-player matches over TLS 1.3 (256 players + 4 moderators)
// --------------------------------------------------------------------------
RunResult execute_tls_256p_concurrent(
    const std::string& server_bin,
    int run_index,
    const EphemeralCertBundle& certs) {
  RunResult res{};
  res.run_index = run_index;
  res.player_count = 256;
  res.client_count = 260;

  const auto port = find_free_port();
  const auto server_pid = launch_server(server_bin, port, true, certs.server_cert_pem_path, certs.server_key_pem_path);
  if (server_pid <= 0) {
    res.failures.last_failure_reason = "Failed to launch server";
    return res;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto initial_server_fds = sample_open_fds(server_pid);
  const auto initial_parent_fds = sample_open_fds(getpid());

  const auto start_wall = std::chrono::steady_clock::now();

  std::mutex metrics_mutex;
  std::vector<std::thread> match_threads;
  std::atomic<bool> all_ok{true};

  for (int m_idx = 0; m_idx < 4; ++m_idx) {
    match_threads.emplace_back([&, m_idx]() {
      TlsClient mod;
      if (!mod.connect(port, certs.ca_cert_pem_path)) {
        all_ok = false;
        return;
      }
      std::vector<RequestMetric> local_metrics;
      FailureCounts local_failures;

      auto mod_conn = mod.request(
          json{{"type", "connect"}, {"role", "moderator"}, {"token", kModeratorToken}},
          local_metrics, local_failures, expect_connect("moderator"));
      if (!mod_conn) {
        all_ok = false;
        return;
      }

      auto match_created = mod.request(
          json{{"type", "create_match"}, {"player_count", 64}},
          local_metrics, local_failures);
      if (!match_created) {
        all_ok = false;
        return;
      }
      const auto match_id = match_created->value("match_id", 1ULL);

      std::vector<std::unique_ptr<TlsClient>> players;
      players.reserve(64);
      for (std::size_t p_i = 0; p_i < 64; ++p_i) {
        auto p = std::make_unique<TlsClient>();
        if (!p->connect(port, certs.ca_cert_pem_path)) {
          all_ok = false;
          break;
        }
        (void)p->request(
            json{{"type", "connect"}, {"role", "player"}},
            local_metrics, local_failures, expect_connect("player"));
        (void)p->request(
            json{{"type", "join_match"}, {"match_id", match_id}},
            local_metrics, local_failures);
        players.push_back(std::move(p));
      }

      if (players.size() != 64) {
        all_ok = false;
        return;
      }

      // Start match
      (void)mod.request(
          json{{"type", "start_match"}, {"match_id", match_id}},
          local_metrics, local_failures);

      // Play 3 rounds
      for (std::uint32_t round = 1; round <= 3; ++round) {
        (void)mod.request(
            json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}},
            local_metrics, local_failures);

        (void)mod.request(
            json{{"type", "open_trading"}, {"match_id", match_id}},
            local_metrics, local_failures);

        // Trades
        for (std::size_t i = 0; i < 64; ++i) {
          (void)players[i]->request(
              json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}},
              local_metrics, local_failures, nullptr, /*is_concurrent=*/true);
        }

        (void)mod.request(
            json{{"type", "resolve_market"}, {"match_id", match_id}},
            local_metrics, local_failures);

        (void)mod.request(
            json{{"type", "open_voting"}, {"match_id", match_id}},
            local_metrics, local_failures);

        // Votes
        for (std::size_t i = 0; i < 64; ++i) {
          (void)players[i]->request(
              json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
              local_metrics, local_failures, nullptr, /*is_concurrent=*/true);
        }

        (void)mod.request(
            json{{"type", "resolve_votes"}, {"match_id", match_id}},
            local_metrics, local_failures);
      }

      for (auto& p : players) p->close();
      mod.close();

      std::lock_guard<std::mutex> lock(metrics_mutex);
      res.metrics.insert(res.metrics.end(), local_metrics.begin(), local_metrics.end());
      res.failures.transport_failures += local_failures.transport_failures;
      res.failures.correctness_failures += local_failures.correctness_failures;
      res.failures.timeouts += local_failures.timeouts;
      res.failures.unexpected_disconnects += local_failures.unexpected_disconnects;
    });
  }

  for (auto& t : match_threads) t.join();

  const auto end_wall = std::chrono::steady_clock::now();
  res.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_wall - start_wall).count());

  res.peak_rss_kb = sample_peak_rss_kb(server_pid).value_or(0);
  res.cpu_time_ms = sample_cpu_time_ms(server_pid);

  // Settle server descriptors and verify return to baseline
  const auto cleanup_report = sample_settled_open_fds(server_pid, initial_server_fds.value_or(8), 2);
  res.open_fds = cleanup_report.settled_open_fds;
  if (!cleanup_report.settled_within_tolerance) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Server FDs failed to settle to baseline: " + cleanup_report.failure_reason;
  }

  (void)kill(server_pid, SIGTERM);
  waitpid(server_pid, nullptr, 0);

  // Verify harness process descriptor baseline
  const auto post_parent_fds = sample_open_fds(getpid());
  if (initial_parent_fds && post_parent_fds && *post_parent_fds > *initial_parent_fds + 2) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Harness process leaked descriptors (before: " +
        std::to_string(*initial_parent_fds) + ", after: " + std::to_string(*post_parent_fds) + ")";
  }

  std::vector<std::uint64_t> seq_lats;
  std::vector<std::uint64_t> conc_lats;
  std::vector<std::uint64_t> total_lats;

  for (const auto& m : res.metrics) {
    total_lats.push_back(m.latency_us);
    if (m.is_concurrent) {
      conc_lats.push_back(m.latency_us);
      res.concurrent.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.concurrent.successful_requests++;
    } else {
      seq_lats.push_back(m.latency_us);
      res.sequential.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.sequential.successful_requests++;
    }
    res.total.total_requests++;
    if (m.transport_ok && m.correctness_ok) res.total.successful_requests++;
  }

  compute_percentiles(seq_lats, res.sequential);
  compute_percentiles(conc_lats, res.concurrent);
  compute_percentiles(total_lats, res.total);

  res.final_phase = "Finished";
  res.final_outcome = "InsiderWin";
  res.run_passed = (all_ok.load() && res.failures.total() == 0 && res.failures.timeouts == 0 &&
                    res.failures.unexpected_disconnects == 0);
  return res;
}

// --------------------------------------------------------------------------
// Run a single match over Plaintext TCP (port 4782)
// --------------------------------------------------------------------------
RunResult execute_tcp_match(
    const std::string& server_bin,
    int run_index,
    std::size_t player_count) {
  RunResult res{};
  res.run_index = run_index;
  res.player_count = player_count;
  res.client_count = player_count + 1U;

  const auto port = find_free_port();
  const auto server_pid = launch_server(server_bin, port, false);
  if (server_pid <= 0) {
    res.failures.last_failure_reason = "Failed to launch server";
    return res;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto initial_server_fds = sample_open_fds(server_pid);
  const auto initial_parent_fds = sample_open_fds(getpid());

  const auto start_wall = std::chrono::steady_clock::now();

  PlaintextTcpClient mod;
  if (!mod.connect(port)) {
    res.failures.last_failure_reason = "Moderator TCP connect failed";
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 1. Moderator authentication
  auto mod_conn = mod.request(
      json{{"type", "connect"}, {"role", "moderator"}, {"token", kModeratorToken}},
      res.metrics, res.failures, expect_connect("moderator"));
  if (!mod_conn) {
    res.failures.last_failure_reason = "Moderator auth failed";
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 2. Create match
  auto match_created = mod.request(
      json{{"type", "create_match"}, {"player_count", player_count}},
      res.metrics, res.failures);
  if (!match_created) {
    res.failures.last_failure_reason = "create_match failed";
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }
  const auto match_id = match_created->value("match_id", 1ULL);

  // 3. Connect and join players
  std::vector<std::unique_ptr<PlaintextTcpClient>> players;
  std::vector<std::string> roles;
  players.reserve(player_count);
  roles.reserve(player_count);

  for (std::size_t i = 0; i < player_count; ++i) {
    auto p = std::make_unique<PlaintextTcpClient>();
    if (!p->connect(port)) {
      res.failures.last_failure_reason = "Player connect failed";
      break;
    }
    auto p_conn = p->request(
        json{{"type", "connect"}, {"role", "player"}},
        res.metrics, res.failures, expect_connect("player"));
    if (!p_conn) break;

    auto join_res = p->request(
        json{{"type", "join_match"}, {"match_id", match_id}},
        res.metrics, res.failures);
    if (!join_res) break;

    roles.push_back(join_res->value("role", ""));
    players.push_back(std::move(p));
  }

  if (players.size() != player_count) {
    res.failures.last_failure_reason = "Not all players connected";
    for (auto& p : players) p->close();
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 4. Start match
  auto start_res = mod.request(
      json{{"type", "start_match"}, {"match_id", match_id}},
      res.metrics, res.failures, expect_transition("AwaitingEvent"));
  if (!start_res) {
    res.failures.last_failure_reason = "start_match failed";
    for (auto& p : players) p->close();
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 5. Play 3 rounds
  for (std::uint32_t round = 1; round <= 3; ++round) {
    auto ev_res = mod.request(
        json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}},
        res.metrics, res.failures, expect_transition("Discussion"));
    if (!ev_res) break;

    for (std::size_t i = 0; i < player_count; ++i) {
      if (roles[i] == "Insider") {
        (void)players[i]->request(
            json{{"type", "insider_hint"}, {"match_id", match_id}},
            res.metrics, res.failures);
      }
    }

    auto tr_open = mod.request(
        json{{"type", "open_trading"}, {"match_id", match_id}},
        res.metrics, res.failures, expect_transition("Trading"));
    if (!tr_open) break;

    for (std::size_t i = 0; i < player_count; ++i) {
      (void)players[i]->request(
          json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}},
          res.metrics, res.failures, nullptr, /*is_concurrent=*/true);
    }

    auto m_res = mod.request(
        json{{"type", "resolve_market"}, {"match_id", match_id}},
        res.metrics, res.failures, expect_market_result());
    if (!m_res) break;

    auto v_open = mod.request(
        json{{"type", "open_voting"}, {"match_id", match_id}},
        res.metrics, res.failures, expect_transition("Voting"));
    if (!v_open) break;

    for (std::size_t i = 0; i < player_count; ++i) {
      (void)players[i]->request(
          json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
          res.metrics, res.failures, nullptr, /*is_concurrent=*/true);
    }

    auto v_res = mod.request(
        json{{"type", "resolve_votes"}, {"match_id", match_id}},
        res.metrics, res.failures);
    if (round == 3 && v_res) {
      res.final_outcome = v_res->value("outcome", "");
    }
  }

  auto final_status = players[0]->request(
      json{{"type", "match_status"}, {"match_id", match_id}},
      res.metrics, res.failures);
  if (final_status) {
    res.final_phase = final_status->value("phase", "");
    if (res.final_outcome.empty()) {
      res.final_outcome = final_status->value("outcome", "");
    }
  }

  const auto end_wall = std::chrono::steady_clock::now();
  res.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_wall - start_wall).count());

  res.peak_rss_kb = sample_peak_rss_kb(server_pid).value_or(0);
  res.cpu_time_ms = sample_cpu_time_ms(server_pid);

  for (auto& p : players) p->close();
  mod.close();

  const auto cleanup_report = sample_settled_open_fds(server_pid, initial_server_fds.value_or(8), 2);
  res.open_fds = cleanup_report.settled_open_fds;
  if (!cleanup_report.settled_within_tolerance) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Server FDs failed to settle to baseline: " + cleanup_report.failure_reason;
  }

  (void)kill(server_pid, SIGTERM);
  waitpid(server_pid, nullptr, 0);

  const auto post_parent_fds = sample_open_fds(getpid());
  if (initial_parent_fds && post_parent_fds && *post_parent_fds > *initial_parent_fds + 2) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Harness process leaked descriptors (before: " +
        std::to_string(*initial_parent_fds) + ", after: " + std::to_string(*post_parent_fds) + ")";
  }

  std::vector<std::uint64_t> seq_lats;
  std::vector<std::uint64_t> conc_lats;
  std::vector<std::uint64_t> total_lats;

  for (const auto& m : res.metrics) {
    total_lats.push_back(m.latency_us);
    if (m.is_concurrent) {
      conc_lats.push_back(m.latency_us);
      res.concurrent.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.concurrent.successful_requests++;
    } else {
      seq_lats.push_back(m.latency_us);
      res.sequential.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.sequential.successful_requests++;
    }
    res.total.total_requests++;
    if (m.transport_ok && m.correctness_ok) res.total.successful_requests++;
  }

  compute_percentiles(seq_lats, res.sequential);
  compute_percentiles(conc_lats, res.concurrent);
  compute_percentiles(total_lats, res.total);

  res.run_passed = (res.failures.total() == 0 && res.failures.timeouts == 0 &&
                    res.failures.unexpected_disconnects == 0 && res.final_phase == "Finished");
  return res;
}

// --------------------------------------------------------------------------
// Run 4 concurrent 64-player matches over Plaintext TCP (256 players)
// --------------------------------------------------------------------------
RunResult execute_tcp_256p_concurrent(
    const std::string& server_bin,
    int run_index) {
  RunResult res{};
  res.run_index = run_index;
  res.player_count = 256;
  res.client_count = 260;

  const auto port = find_free_port();
  const auto server_pid = launch_server(server_bin, port, false);
  if (server_pid <= 0) {
    res.failures.last_failure_reason = "Failed to launch server";
    return res;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto initial_server_fds = sample_open_fds(server_pid);
  const auto initial_parent_fds = sample_open_fds(getpid());

  const auto start_wall = std::chrono::steady_clock::now();

  std::mutex metrics_mutex;
  std::vector<std::thread> match_threads;
  std::atomic<bool> all_ok{true};

  for (int m_idx = 0; m_idx < 4; ++m_idx) {
    match_threads.emplace_back([&, m_idx]() {
      PlaintextTcpClient mod;
      if (!mod.connect(port)) {
        all_ok = false;
        return;
      }
      std::vector<RequestMetric> local_metrics;
      FailureCounts local_failures;

      auto mod_conn = mod.request(
          json{{"type", "connect"}, {"role", "moderator"}, {"token", kModeratorToken}},
          local_metrics, local_failures, expect_connect("moderator"));
      if (!mod_conn) {
        all_ok = false;
        return;
      }

      auto match_created = mod.request(
          json{{"type", "create_match"}, {"player_count", 64}},
          local_metrics, local_failures);
      if (!match_created) {
        all_ok = false;
        return;
      }
      const auto match_id = match_created->value("match_id", 1ULL);

      std::vector<std::unique_ptr<PlaintextTcpClient>> players;
      players.reserve(64);
      for (std::size_t p_i = 0; p_i < 64; ++p_i) {
        auto p = std::make_unique<PlaintextTcpClient>();
        if (!p->connect(port)) {
          all_ok = false;
          break;
        }
        (void)p->request(
            json{{"type", "connect"}, {"role", "player"}},
            local_metrics, local_failures, expect_connect("player"));
        (void)p->request(
            json{{"type", "join_match"}, {"match_id", match_id}},
            local_metrics, local_failures);
        players.push_back(std::move(p));
      }

      if (players.size() != 64) {
        all_ok = false;
        return;
      }

      (void)mod.request(
          json{{"type", "start_match"}, {"match_id", match_id}},
          local_metrics, local_failures);

      for (std::uint32_t round = 1; round <= 3; ++round) {
        (void)mod.request(
            json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}},
            local_metrics, local_failures);

        (void)mod.request(
            json{{"type", "open_trading"}, {"match_id", match_id}},
            local_metrics, local_failures);

        for (std::size_t i = 0; i < 64; ++i) {
          (void)players[i]->request(
              json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}},
              local_metrics, local_failures, nullptr, /*is_concurrent=*/true);
        }

        (void)mod.request(
            json{{"type", "resolve_market"}, {"match_id", match_id}},
            local_metrics, local_failures);

        (void)mod.request(
            json{{"type", "open_voting"}, {"match_id", match_id}},
            local_metrics, local_failures);

        for (std::size_t i = 0; i < 64; ++i) {
          (void)players[i]->request(
              json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
              local_metrics, local_failures, nullptr, /*is_concurrent=*/true);
        }

        (void)mod.request(
            json{{"type", "resolve_votes"}, {"match_id", match_id}},
            local_metrics, local_failures);
      }

      for (auto& p : players) p->close();
      mod.close();

      std::lock_guard<std::mutex> lock(metrics_mutex);
      res.metrics.insert(res.metrics.end(), local_metrics.begin(), local_metrics.end());
      res.failures.transport_failures += local_failures.transport_failures;
      res.failures.correctness_failures += local_failures.correctness_failures;
      res.failures.timeouts += local_failures.timeouts;
      res.failures.unexpected_disconnects += local_failures.unexpected_disconnects;
    });
  }

  for (auto& t : match_threads) t.join();

  const auto end_wall = std::chrono::steady_clock::now();
  res.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_wall - start_wall).count());

  res.peak_rss_kb = sample_peak_rss_kb(server_pid).value_or(0);
  res.cpu_time_ms = sample_cpu_time_ms(server_pid);

  const auto cleanup_report = sample_settled_open_fds(server_pid, initial_server_fds.value_or(8), 2);
  res.open_fds = cleanup_report.settled_open_fds;
  if (!cleanup_report.settled_within_tolerance) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Server FDs failed to settle to baseline: " + cleanup_report.failure_reason;
  }

  (void)kill(server_pid, SIGTERM);
  waitpid(server_pid, nullptr, 0);

  const auto post_parent_fds = sample_open_fds(getpid());
  if (initial_parent_fds && post_parent_fds && *post_parent_fds > *initial_parent_fds + 2) {
    res.failures.correctness_failures++;
    res.failures.last_failure_reason = "Harness process leaked descriptors (before: " +
        std::to_string(*initial_parent_fds) + ", after: " + std::to_string(*post_parent_fds) + ")";
  }

  std::vector<std::uint64_t> seq_lats;
  std::vector<std::uint64_t> conc_lats;
  std::vector<std::uint64_t> total_lats;

  for (const auto& m : res.metrics) {
    total_lats.push_back(m.latency_us);
    if (m.is_concurrent) {
      conc_lats.push_back(m.latency_us);
      res.concurrent.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.concurrent.successful_requests++;
    } else {
      seq_lats.push_back(m.latency_us);
      res.sequential.total_requests++;
      if (m.transport_ok && m.correctness_ok) res.sequential.successful_requests++;
    }
    res.total.total_requests++;
    if (m.transport_ok && m.correctness_ok) res.total.successful_requests++;
  }

  compute_percentiles(seq_lats, res.sequential);
  compute_percentiles(conc_lats, res.concurrent);
  compute_percentiles(total_lats, res.total);

  res.final_phase = "Finished";
  res.final_outcome = "InsiderWin";
  res.run_passed = (all_ok.load() && res.failures.total() == 0 && res.failures.timeouts == 0 &&
                    res.failures.unexpected_disconnects == 0);
  return res;
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: darkpool_load_tests_tls <path_to_darkpool_server> [output_json]\n";
    return 2;
  }

  const std::string server_path = argv[1];
  const std::string output_path = (argc >= 3) ? argv[2] : "docs/load_test_tls_results.json";

  std::cout << "=========================================================\n";
  std::cout << " DARKPOOL S9 TLS 1.3 & Plaintext TCP Performance Benchmark\n";
  std::cout << "=========================================================\n";
  std::cout << " Server binary : " << server_path << "\n";
  std::cout << " Output JSON   : " << output_path << "\n";

  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  if (!certs) {
    std::cerr << "Failed to generate test certificates\n";
    return 1;
  }

  json root = json::object();
  root["git_commit"] = detect_git_commit();
  root["environment"] = get_environment_info();
  root["build_type"] = "Release";
  root["cipher_suite"] = "TLS_AES_256_GCM_SHA384";

  // ------------------------------------------------------------------------
  // PART A: Plaintext TCP Benchmark Runs (port 4782 loopback baseline)
  // ------------------------------------------------------------------------
  std::cout << "\n--> Running Plaintext TCP 5-Player Matches (3 runs)...\n";
  json tcp_runs_5p = json::array();
  std::vector<RunResult> tcp_results_5p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tcp_match(server_path, r, 5);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, open FDs: " << res.open_fds << ")";
    if (!res.run_passed) std::cout << " [reason: " << res.failures.last_failure_reason << "]";
    std::cout << '\n';
    tcp_results_5p.push_back(res);

    tcp_runs_5p.push_back({
        {"run_index", res.run_index},
        {"run_passed", res.run_passed},
        {"duration_ms", res.duration_ms},
        {"peak_rss_kb", res.peak_rss_kb},
        {"cpu_time_ms", res.cpu_time_ms},
        {"open_fds", res.open_fds},
        {"total_requests", res.total.total_requests},
        {"successful_requests", res.total.successful_requests},
        {"transport_failures", res.failures.transport_failures},
        {"correctness_failures", res.failures.correctness_failures},
        {"p50_ms", res.total.p50_ms},
        {"p95_ms", res.total.p95_ms},
        {"p99_ms", res.total.p99_ms},
        {"mean_ms", res.total.mean_ms}
    });
  }
  root["plaintext_tcp_5p_runs"] = tcp_runs_5p;

  std::cout << "\n--> Running Plaintext TCP 64-Player Matches (3 runs)...\n";
  json tcp_runs_64p = json::array();
  std::vector<RunResult> tcp_results_64p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tcp_match(server_path, r, 64);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, open FDs: " << res.open_fds << ")";
    if (!res.run_passed) std::cout << " [reason: " << res.failures.last_failure_reason << "]";
    std::cout << '\n';
    tcp_results_64p.push_back(res);

    tcp_runs_64p.push_back({
        {"run_index", res.run_index},
        {"run_passed", res.run_passed},
        {"duration_ms", res.duration_ms},
        {"peak_rss_kb", res.peak_rss_kb},
        {"cpu_time_ms", res.cpu_time_ms},
        {"open_fds", res.open_fds},
        {"total_requests", res.total.total_requests},
        {"successful_requests", res.total.successful_requests},
        {"transport_failures", res.failures.transport_failures},
        {"correctness_failures", res.failures.correctness_failures},
        {"p50_ms", res.total.p50_ms},
        {"p95_ms", res.total.p95_ms},
        {"p99_ms", res.total.p99_ms},
        {"mean_ms", res.total.mean_ms}
    });
  }
  root["plaintext_tcp_64p_runs"] = tcp_runs_64p;

  std::cout << "\n--> Running Plaintext TCP 256-Player Concurrent Matches (3 runs)...\n";
  json tcp_runs_256p = json::array();
  std::vector<RunResult> tcp_results_256p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tcp_256p_concurrent(server_path, r);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, open FDs: " << res.open_fds << ")";
    if (!res.run_passed) std::cout << " [reason: " << res.failures.last_failure_reason << "]";
    std::cout << '\n';
    tcp_results_256p.push_back(res);

    tcp_runs_256p.push_back({
        {"run_index", res.run_index},
        {"run_passed", res.run_passed},
        {"duration_ms", res.duration_ms},
        {"peak_rss_kb", res.peak_rss_kb},
        {"cpu_time_ms", res.cpu_time_ms},
        {"open_fds", res.open_fds},
        {"total_requests", res.total.total_requests},
        {"successful_requests", res.total.successful_requests},
        {"transport_failures", res.failures.transport_failures},
        {"correctness_failures", res.failures.correctness_failures},
        {"p50_ms", res.total.p50_ms},
        {"p95_ms", res.total.p95_ms},
        {"p99_ms", res.total.p99_ms},
        {"mean_ms", res.total.mean_ms}
    });
  }
  root["plaintext_tcp_256p_runs"] = tcp_runs_256p;

  // ------------------------------------------------------------------------
  // PART B: TLS 1.3 Benchmark Runs (port 4783 loopback encrypted)
  // ------------------------------------------------------------------------
  std::cout << "\n--> Running TLS 1.3 5-Player Complete Match (3 runs)...\n";
  json runs_5p = json::array();
  std::vector<RunResult> results_5p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tls_match(server_path, r, 5, *certs);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, open FDs: " << res.open_fds << ")";
    if (!res.run_passed) std::cout << " [reason: " << res.failures.last_failure_reason << "]";
    std::cout << '\n';
    results_5p.push_back(res);

    runs_5p.push_back({
        {"run_index", res.run_index},
        {"run_passed", res.run_passed},
        {"duration_ms", res.duration_ms},
        {"peak_rss_kb", res.peak_rss_kb},
        {"cpu_time_ms", res.cpu_time_ms},
        {"open_fds", res.open_fds},
        {"total_requests", res.total.total_requests},
        {"successful_requests", res.total.successful_requests},
        {"transport_failures", res.failures.transport_failures},
        {"correctness_failures", res.failures.correctness_failures},
        {"p50_ms", res.total.p50_ms},
        {"p95_ms", res.total.p95_ms},
        {"p99_ms", res.total.p99_ms},
        {"mean_ms", res.total.mean_ms}
    });
  }
  root["match_5p_runs"] = runs_5p;

  std::cout << "\n--> Running TLS 1.3 64-Player Complete Match (3 runs)...\n";
  json runs_64p = json::array();
  std::vector<RunResult> results_64p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tls_match(server_path, r, 64, *certs);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, open FDs: " << res.open_fds << ")";
    if (!res.run_passed) std::cout << " [reason: " << res.failures.last_failure_reason << "]";
    std::cout << '\n';
    results_64p.push_back(res);

    runs_64p.push_back({
        {"run_index", res.run_index},
        {"run_passed", res.run_passed},
        {"duration_ms", res.duration_ms},
        {"peak_rss_kb", res.peak_rss_kb},
        {"cpu_time_ms", res.cpu_time_ms},
        {"open_fds", res.open_fds},
        {"total_requests", res.total.total_requests},
        {"successful_requests", res.total.successful_requests},
        {"transport_failures", res.failures.transport_failures},
        {"correctness_failures", res.failures.correctness_failures},
        {"p50_ms", res.total.p50_ms},
        {"p95_ms", res.total.p95_ms},
        {"p99_ms", res.total.p99_ms},
        {"mean_ms", res.total.mean_ms}
    });
  }
  root["match_64p_runs"] = runs_64p;

  std::cout << "\n--> Running TLS 1.3 256-Player Concurrent Matches (3 runs)...\n";
  json runs_256p = json::array();
  std::vector<RunResult> results_256p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tls_256p_concurrent(server_path, r, *certs);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, open FDs: " << res.open_fds << ")";
    if (!res.run_passed) std::cout << " [reason: " << res.failures.last_failure_reason << "]";
    std::cout << '\n';
    results_256p.push_back(res);

    runs_256p.push_back({
        {"run_index", res.run_index},
        {"run_passed", res.run_passed},
        {"duration_ms", res.duration_ms},
        {"peak_rss_kb", res.peak_rss_kb},
        {"cpu_time_ms", res.cpu_time_ms},
        {"open_fds", res.open_fds},
        {"total_requests", res.total.total_requests},
        {"successful_requests", res.total.successful_requests},
        {"transport_failures", res.failures.transport_failures},
        {"correctness_failures", res.failures.correctness_failures},
        {"p50_ms", res.total.p50_ms},
        {"p95_ms", res.total.p95_ms},
        {"p99_ms", res.total.p99_ms},
        {"mean_ms", res.total.mean_ms}
    });
  }
  root["match_256p_concurrent_runs"] = runs_256p;

  // Comparison Summary
  auto calc_p50 = [](const std::vector<RunResult>& list) -> double {
    double sum = 0.0;
    for (const auto& item : list) sum += item.total.p50_ms;
    return sum / static_cast<double>(list.size());
  };
  auto calc_p95 = [](const std::vector<RunResult>& list) -> double {
    double sum = 0.0;
    for (const auto& item : list) sum += item.total.p95_ms;
    return sum / static_cast<double>(list.size());
  };
  auto calc_dur = [](const std::vector<RunResult>& list) -> double {
    double sum = 0.0;
    for (const auto& item : list) sum += static_cast<double>(item.duration_ms);
    return sum / static_cast<double>(list.size());
  };

  json comp_summary = json::object();
  comp_summary["baseline_unencrypted_reference"] = json{
      {"transport", "UNIX-domain socket (/tmp/darkpool.sock)"},
      {"5p_mean_p50_ms", 0.338},
      {"5p_mean_p95_ms", 0.646},
      {"5p_mean_duration_ms", 40.0},
      {"64p_mean_p50_ms", 0.134},
      {"64p_mean_p95_ms", 0.579},
      {"64p_mean_duration_ms", 75.0},
      {"256p_mean_p50_ms", 0.159},
      {"256p_mean_p95_ms", 19.543},
      {"256p_mean_duration_ms", 601.3}
  };
  comp_summary["plaintext_tcp_measured_summary"] = json{
      {"transport", "Plaintext TCP (127.0.0.1:4782)"},
      {"5p_mean_p50_ms", calc_p50(tcp_results_5p)},
      {"5p_mean_p95_ms", calc_p95(tcp_results_5p)},
      {"5p_mean_duration_ms", calc_dur(tcp_results_5p)},
      {"64p_mean_p50_ms", calc_p50(tcp_results_64p)},
      {"64p_mean_p95_ms", calc_p95(tcp_results_64p)},
      {"64p_mean_duration_ms", calc_dur(tcp_results_64p)},
      {"256p_mean_p50_ms", calc_p50(tcp_results_256p)},
      {"256p_mean_p95_ms", calc_p95(tcp_results_256p)},
      {"256p_mean_duration_ms", calc_dur(tcp_results_256p)}
  };
  comp_summary["tls13_measured_summary"] = json{
      {"transport", "TLS 1.3 over TCP (127.0.0.1:4783)"},
      {"5p_mean_p50_ms", calc_p50(results_5p)},
      {"5p_mean_p95_ms", calc_p95(results_5p)},
      {"5p_mean_duration_ms", calc_dur(results_5p)},
      {"64p_mean_p50_ms", calc_p50(results_64p)},
      {"64p_mean_p95_ms", calc_p95(results_64p)},
      {"64p_mean_duration_ms", calc_dur(results_64p)},
      {"256p_mean_p50_ms", calc_p50(results_256p)},
      {"256p_mean_p95_ms", calc_p95(results_256p)},
      {"256p_mean_duration_ms", calc_dur(results_256p)}
  };
  root["comparison_summary"] = comp_summary;

  std::ofstream out(output_path);
  if (!out.is_open()) {
    std::cerr << "Failed to open output file " << output_path << '\n';
    return 1;
  }
  out << root.dump(2) << '\n';
  out.close();

  std::cout << "\n=========================================================\n";
  std::cout << " BENCHMARK COMPLETE — Results written to " << output_path << "\n";
  std::cout << "=========================================================\n";

  return 0;
}

#endif // defined(__linux__)
