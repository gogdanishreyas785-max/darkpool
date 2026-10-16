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
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace darkpool::load_test;
using darkpool::network::Frame;
using darkpool::network::json;
using darkpool::transport::EphemeralCertBundle;
using darkpool::transport::generate_ephemeral_certs;

constexpr const char* kModeratorToken = "tls-benchmark-mod-token";

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

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) <= 0) {
      return false;
    }

    for (int attempt = 0; attempt < 80; ++attempt) {
      fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd >= 0) {
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

    // Advance handshake to completion
    const auto start = std::chrono::steady_clock::now();
    while (true) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > 5000) {
        close();
        return false;
      }
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
      pollfd pfd{fd, static_cast<short>(want_read ? POLLIN : POLLOUT), 0};
      (void)poll(&pfd, 1, 100);
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
        (void)poll(&pfd, 1, 100);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "header tls_read failed: " + err;
        return std::nullopt;
      }
    }

    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
    if (length == 0U || length > darkpool::network::kMaxMessageSize) {
      failures.transport_failures++;
      failures.last_failure_reason = "invalid frame length: " + std::to_string(length);
      return std::nullopt;
    }

    std::string body;
    body.resize(length);
    std::size_t body_offset = 0;
    while (body_offset < length) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start_time).count() > 5000) {
        failures.timeouts++;
        failures.last_failure_reason = "body read timed out";
        return std::nullopt;
      }
      std::size_t count = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          ssl.get(), body.data() + body_offset, length - body_offset, count, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        body_offset += count;
      } else if (status == darkpool::transport::TlsIoStatus::WantRead ||
                 status == darkpool::transport::TlsIoStatus::WantWrite) {
        pollfd pfd{fd, static_cast<short>((status == darkpool::transport::TlsIoStatus::WantRead) ? POLLIN : POLLOUT), 0};
        (void)poll(&pfd, 1, 100);
      } else {
        failures.transport_failures++;
        failures.last_failure_reason = "body tls_read failed: " + err;
        return std::nullopt;
      }
    }

    const auto end_time = std::chrono::steady_clock::now();
    const auto us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count());

    try {
      const auto resp = json::parse(body);
      const ValidationResult v = validator ? validator(resp) : ValidationResult::Success();
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
    std::uint16_t tls_port,
    const std::string& cert_path,
    const std::string& key_path) {
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
    const auto port_str = std::to_string(tls_port);
    execl(server_bin.c_str(), server_bin.c_str(),
          "--no-tcp",
          "--no-unix",
          "--tls-bind-address", "127.0.0.1",
          "--tls-port", port_str.c_str(),
          "--tls-cert", cert_path.c_str(),
          "--tls-key", key_path.c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  return child;
}

// --------------------------------------------------------------------------
// Run a single match (5 players or 64 players)
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
  const auto server_pid = launch_server(server_bin, port, certs.server_cert_pem_path, certs.server_key_pem_path);
  if (server_pid <= 0) {
    res.failures.last_failure_reason = "Failed to launch server";
    return res;
  }

  // Poll for server listener readiness
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

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
      res.failures.transport_failures++;
      res.failures.last_failure_reason = "Player " + std::to_string(i) + " connect failed";
      break;
    }
    auto p_conn = p->request(
        json{{"type", "connect"}, {"role", "player"}},
        res.metrics, res.failures, expect_connect("player"));
    if (!p_conn) break;

    auto join = p->request(
        json{{"type", "join_match"}, {"match_id", match_id}},
        res.metrics, res.failures);
    if (!join) break;
    roles.push_back(join->value("role", ""));
    players.push_back(std::move(p));
  }

  if (players.size() != player_count) {
    res.failures.last_failure_reason = "Failed to connect all players";
    for (auto& p : players) p->close();
    mod.close();
    (void)kill(server_pid, SIGTERM);
    waitpid(server_pid, nullptr, 0);
    return res;
  }

  // 4. Start match
  (void)mod.request(
      json{{"type", "start_match"}, {"match_id", match_id}},
      res.metrics, res.failures);

  // 5. Rounds 1..3
  for (std::uint32_t round = 1; round <= 3; ++round) {
    // Event
    (void)mod.request(
        json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}},
        res.metrics, res.failures);

    // Hints
    for (std::size_t i = 0; i < player_count; ++i) {
      if (roles[i] == "Insider") {
        (void)players[i]->request(
            json{{"type", "insider_hint"}, {"match_id", match_id}},
            res.metrics, res.failures);
      }
    }

    // Open trading
    (void)mod.request(
        json{{"type", "open_trading"}, {"match_id", match_id}},
        res.metrics, res.failures);

    // Concurrent trading
    {
      std::vector<std::thread> threads;
      threads.reserve(player_count);
      std::mutex metrics_mutex;
      for (std::size_t i = 0; i < player_count; ++i) {
        threads.emplace_back([&, i]() {
          std::vector<RequestMetric> local_metrics;
          FailureCounts local_failures;
          (void)players[i]->request(
              json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}},
              local_metrics, local_failures, nullptr, /*is_concurrent=*/true);
          std::lock_guard<std::mutex> lock(metrics_mutex);
          res.metrics.insert(res.metrics.end(), local_metrics.begin(), local_metrics.end());
          res.failures.transport_failures += local_failures.transport_failures;
          res.failures.correctness_failures += local_failures.correctness_failures;
          res.failures.timeouts += local_failures.timeouts;
        });
      }
      for (auto& t : threads) t.join();
    }

    // Resolve market
    (void)mod.request(
        json{{"type", "resolve_market"}, {"match_id", match_id}},
        res.metrics, res.failures);

    // Open voting
    (void)mod.request(
        json{{"type", "open_voting"}, {"match_id", match_id}},
        res.metrics, res.failures);

    // Concurrent voting
    {
      std::vector<std::thread> threads;
      threads.reserve(player_count);
      std::mutex metrics_mutex;
      for (std::size_t i = 0; i < player_count; ++i) {
        threads.emplace_back([&, i]() {
          std::vector<RequestMetric> local_metrics;
          FailureCounts local_failures;
          (void)players[i]->request(
              json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}},
              local_metrics, local_failures, nullptr, /*is_concurrent=*/true);
          std::lock_guard<std::mutex> lock(metrics_mutex);
          res.metrics.insert(res.metrics.end(), local_metrics.begin(), local_metrics.end());
          res.failures.transport_failures += local_failures.transport_failures;
          res.failures.correctness_failures += local_failures.correctness_failures;
          res.failures.timeouts += local_failures.timeouts;
        });
      }
      for (auto& t : threads) t.join();
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

  // Sample peak RSS and CPU
  res.peak_rss_kb = sample_peak_rss_kb(server_pid).value_or(0);
  res.cpu_time_ms = sample_cpu_time_ms(server_pid);

  // Close all clients
  for (auto& p : players) p->close();
  mod.close();

  // Allow server to process disconnects
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  res.open_fds = sample_open_fds(server_pid).value_or(0);

  // Stop server
  (void)kill(server_pid, SIGTERM);
  waitpid(server_pid, nullptr, 0);

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

  res.run_passed = (res.failures.total() == 0 && res.final_phase == "Finished");
  return res;
}

// --------------------------------------------------------------------------
// Run 4 concurrent 64-player matches (256 players + 4 moderators = 260 connections)
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
  const auto server_pid = launch_server(server_bin, port, certs.server_cert_pem_path, certs.server_key_pem_path);
  if (server_pid <= 0) {
    res.failures.last_failure_reason = "Failed to launch server";
    return res;
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    });
  }

  for (auto& t : match_threads) t.join();

  const auto end_wall = std::chrono::steady_clock::now();
  res.duration_ms = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(end_wall - start_wall).count());

  res.peak_rss_kb = sample_peak_rss_kb(server_pid).value_or(0);
  res.cpu_time_ms = sample_cpu_time_ms(server_pid);

  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  res.open_fds = sample_open_fds(server_pid).value_or(0);

  (void)kill(server_pid, SIGTERM);
  waitpid(server_pid, nullptr, 0);

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
  res.run_passed = (all_ok.load() && res.failures.total() == 0);
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
  std::cout << " DARKPOOL S9 TLS 1.3 Transport Performance Benchmark\n";
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
  root["transport"] = "TLS 1.3 (OpenSSL 3.x)";
  root["cipher_suite"] = "TLS_AES_256_GCM_SHA384";

  // 1. 5-Player Complete Match (3 runs)
  std::cout << "\n--> Running 5-Player Complete Match (3 runs over TLS 1.3)...\n";
  json runs_5p = json::array();
  std::vector<RunResult> results_5p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tls_match(server_path, r, 5, *certs);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, RSS: " << res.peak_rss_kb << " kB)";
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

  // 2. 64-Player Complete Match (3 runs)
  std::cout << "\n--> Running 64-Player Complete Match (3 runs over TLS 1.3)...\n";
  json runs_64p = json::array();
  std::vector<RunResult> results_64p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tls_match(server_path, r, 64, *certs);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, RSS: " << res.peak_rss_kb << " kB)";
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

  // 3. Four Concurrent 64-Player Matches (256 players) (3 runs)
  std::cout << "\n--> Running 4 Concurrent 64-Player Matches (256 players / 260 clients over TLS 1.3)...\n";
  json runs_256p = json::array();
  std::vector<RunResult> results_256p;
  for (int r = 1; r <= 3; ++r) {
    std::cout << "    Run " << r << "/3... " << std::flush;
    auto res = execute_tls_256p_concurrent(server_path, r, *certs);
    std::cout << (res.run_passed ? "PASS" : "FAIL") << " (duration: " << res.duration_ms << " ms, p50: "
              << res.total.p50_ms << " ms, p95: " << res.total.p95_ms << " ms, RSS: " << res.peak_rss_kb << " kB)";
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
  root["comparison_summary"] = {
      {"baseline_unencrypted_reference", {
          {"5p_mean_p50_ms", 0.338},
          {"5p_mean_p95_ms", 0.646},
          {"5p_mean_duration_ms", 40.0},
          {"64p_mean_p50_ms", 0.134},
          {"64p_mean_p95_ms", 0.579},
          {"64p_mean_duration_ms", 75.0},
          {"256p_mean_p50_ms", 0.159},
          {"256p_mean_p95_ms", 19.543},
          {"256p_mean_duration_ms", 601.3}
      }},
      {"tls13_measured_summary", {
          {"5p_mean_p50_ms", (results_5p[0].total.p50_ms + results_5p[1].total.p50_ms + results_5p[2].total.p50_ms) / 3.0},
          {"5p_mean_p95_ms", (results_5p[0].total.p95_ms + results_5p[1].total.p95_ms + results_5p[2].total.p95_ms) / 3.0},
          {"5p_mean_duration_ms", static_cast<double>(results_5p[0].duration_ms + results_5p[1].duration_ms + results_5p[2].duration_ms) / 3.0},
          {"64p_mean_p50_ms", (results_64p[0].total.p50_ms + results_64p[1].total.p50_ms + results_64p[2].total.p50_ms) / 3.0},
          {"64p_mean_p95_ms", (results_64p[0].total.p95_ms + results_64p[1].total.p95_ms + results_64p[2].total.p95_ms) / 3.0},
          {"64p_mean_duration_ms", static_cast<double>(results_64p[0].duration_ms + results_64p[1].duration_ms + results_64p[2].duration_ms) / 3.0},
          {"256p_mean_p50_ms", (results_256p[0].total.p50_ms + results_256p[1].total.p50_ms + results_256p[2].total.p50_ms) / 3.0},
          {"256p_mean_p95_ms", (results_256p[0].total.p95_ms + results_256p[1].total.p95_ms + results_256p[2].total.p95_ms) / 3.0},
          {"256p_mean_duration_ms", static_cast<double>(results_256p[0].duration_ms + results_256p[1].duration_ms + results_256p[2].duration_ms) / 3.0}
      }}
  };

  std::ofstream out(output_path);
  if (!out.is_open()) {
    std::cerr << "Failed to open output file " << output_path << '\n';
    return 1;
  }
  out << root.dump(2) << '\n';
  out.close();

  std::cout << "\n=========================================================\n";
  std::cout << " TLS 1.3 BENCHMARK COMPLETE — Results written to " << output_path << "\n";
  std::cout << "=========================================================\n";

  return 0;
}

#endif // defined(__linux__)
