#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_tls_integration_tests requires Linux (epoll/sockets/openssl)\n";
  return 1;
}

#else

#include "darkpool/reactor_dispatch.hpp"
#include "darkpool/tls_transport.hpp"
#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <atomic>
#include <chrono>
#include <cstring>
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
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::json;
using darkpool::transport::EphemeralCertBundle;
using darkpool::transport::generate_ephemeral_certs;

#include <signal.h>

std::atomic<int> g_failures{0};
std::atomic<int> g_current_scenario_id{0};
std::atomic<const char*> g_current_scenario_name{"none"};
std::atomic<const char*> g_current_operation{"idle"};

void set_operation(const char* op) noexcept {
  g_current_operation.store(op, std::memory_order_relaxed);
}

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "FAIL [Scenario " << g_current_scenario_id.load(std::memory_order_relaxed)
              << " (" << g_current_scenario_name.load(std::memory_order_relaxed)
              << ") | op: " << g_current_operation.load(std::memory_order_relaxed)
              << "] line " << line << ": " << expression << std::endl;
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

template <typename Func>
void run_scenario(int id, const char* name, const char* desc, Func&& func) {
  g_current_scenario_id.store(id, std::memory_order_relaxed);
  g_current_scenario_name.store(name, std::memory_order_relaxed);
  set_operation("start");
  std::cerr << "[SCENARIO " << id << " START] " << name << " - " << desc << std::endl;
  const auto start = std::chrono::steady_clock::now();
  func();
  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start).count();
  set_operation("completed");
  std::cerr << "[SCENARIO " << id << " PASS] " << name << " (" << elapsed_ms << " ms)" << std::endl;
}

std::size_t count_open_fds() {
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/self/fd")) {
    (void)entry;
    ++count;
  }
  return count;
}

std::optional<std::size_t> count_open_fds_pid(pid_t pid) {
  const std::string fd_dir = "/proc/" + std::to_string(pid) + "/fd";
  std::error_code ec;
  std::size_t count = 0;
  for (const auto& entry : std::filesystem::directory_iterator(fd_dir, ec)) {
    (void)entry;
    ++count;
  }
  if (ec) return std::nullopt;
  return count;
}

std::uint16_t reserve_port() {
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

int connect_tcp(std::uint16_t port, int max_attempts = 80, int rcvbuf = 0) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      int one = 1;
      (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (rcvbuf > 0) {
        (void)setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
      }
      sockaddr_in addr{};
      addr.sin_family = AF_INET;
      addr.sin_port = htons(port);
      addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        return fd;
      }
      (void)close(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return -1;
}

pid_t start_tls_server(
    const char* server_path,
    std::uint16_t tls_port,
    const std::string& cert_path,
    const std::string& key_path,
    const char* token_env = "test-moderator-token",
    const std::string& bind_ip = "127.0.0.1",
    std::string* server_stdout_path = nullptr) {
  set_operation("server startup");
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (token_env != nullptr) {
      setenv("DARKPOOL_MODERATOR_TOKEN", token_env, 1);
    } else {
      unsetenv("DARKPOOL_MODERATOR_TOKEN");
    }
    int out_fd = -1;
    if (server_stdout_path != nullptr && !server_stdout_path->empty()) {
      out_fd = ::open(server_stdout_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (out_fd >= 0) {
        dup2(out_fd, STDOUT_FILENO);
        dup2(out_fd, STDERR_FILENO);
      }
    } else {
      const int dev_null = ::open("/dev/null", O_WRONLY);
      if (dev_null >= 0) {
        dup2(dev_null, STDOUT_FILENO);
        dup2(dev_null, STDERR_FILENO);
        (void)::close(dev_null);
      }
    }
    // Close inherited descriptors in child
    const long max_fd_l = sysconf(_SC_OPEN_MAX);
    const int max_fd = max_fd_l > 0 ? static_cast<int>(std::min(max_fd_l, 4096L)) : 1024;
    for (int fd = 3; fd < max_fd; ++fd) {
      if (fd != out_fd) (void)::close(fd);
    }
    const auto port_text = std::to_string(tls_port);
    execl(server_path, server_path,
          "--no-tcp",
          "--tls-bind-address", bind_ip.c_str(),
          "--tls-port", port_text.c_str(),
          "--tls-cert", cert_path.c_str(),
          "--tls-key", key_path.c_str(),
          static_cast<char*>(nullptr));
    _exit(127);
  }
  return child;
}

void stop_server(pid_t child) {
  set_operation("server cleanup");
  if (child > 0) {
    if (kill(child, SIGTERM) != 0) {
      if (errno == ESRCH) {
        return;
      }
    }
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool terminated = false;
    while (std::chrono::steady_clock::now() < deadline) {
      const pid_t w = waitpid(child, &status, WNOHANG);
      if (w == child || (w < 0 && errno == ECHILD)) {
        terminated = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!terminated) {
      (void)kill(child, SIGKILL);
      (void)waitpid(child, &status, 0);
    }
  }
}

struct ServerSubprocessGuard {
  pid_t pid{-1};
  ~ServerSubprocessGuard() {
    if (pid > 0) {
      stop_server(pid);
    }
  }
  void release() noexcept { pid = -1; }
};

enum class FrameReadStatus {
  Success,
  Timeout,
  CleanClose,
  TransportError,
  ProtocolError
};

struct FrameResult {
  FrameReadStatus status{FrameReadStatus::Timeout};
  std::optional<json> frame{std::nullopt};
  std::string error_message{};

  bool ok() const noexcept { return status == FrameReadStatus::Success && frame.has_value(); }
};

enum class FrameWriteStatus {
  Sent,
  Timeout,
  PeerClosed,
  TransportError,
  TlsError
};

struct FrameWriteResult {
  FrameWriteStatus status{FrameWriteStatus::Timeout};
  std::size_t bytes_written{0};
  std::string error_message{};

  bool ok() const noexcept { return status == FrameWriteStatus::Sent; }
};

struct FrameWriteCursor {
  std::string frame_bytes{};
  std::size_t offset{0};
  bool in_progress{false};
  bool abandoned{false};

  [[nodiscard]] bool empty() const noexcept { return !in_progress; }
  [[nodiscard]] std::size_t remaining() const noexcept {
    return in_progress ? (frame_bytes.size() - offset) : 0;
  }
};

struct TlsClientConnection {
  int fd{-1};
  darkpool::transport::SslCtxPtr ctx;
  darkpool::transport::SslPtr ssl;
  FrameWriteCursor write_cursor{};

  ~TlsClientConnection() {
    if (ssl != nullptr) {
      (void)SSL_shutdown(ssl.get());
      ssl.reset();
    }
    if (fd >= 0) {
      (void)close(fd);
      fd = -1;
    }
  }

  [[nodiscard]] bool has_pending_frame() const noexcept {
    return write_cursor.in_progress;
  }

  [[nodiscard]] std::size_t pending_frame_offset() const noexcept {
    return write_cursor.offset;
  }

  [[nodiscard]] std::size_t pending_frame_remaining() const noexcept {
    return write_cursor.remaining();
  }

  FrameWriteResult resume_frame(int timeout_ms = 5000) {
    set_operation("frame transmission");
    if (write_cursor.abandoned) {
      return {FrameWriteStatus::TransportError, write_cursor.offset,
              "Connection stream was abandoned due to prior unrecoverable error"};
    }
    if (!write_cursor.in_progress) {
      return {FrameWriteStatus::Sent, 0, ""};
    }

    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::milliseconds(timeout_ms);
    while (write_cursor.offset < write_cursor.frame_bytes.size()) {
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) {
        return {FrameWriteStatus::Timeout, write_cursor.offset,
                "Write deadline expired under socket backpressure"};
      }
      const auto remaining_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
      std::size_t written = 0;
      std::string err;
      const char* data_ptr = write_cursor.frame_bytes.data() + write_cursor.offset;
      const std::size_t remaining_bytes = write_cursor.frame_bytes.size() - write_cursor.offset;
      const auto status = darkpool::transport::tls_write(
          ssl.get(), data_ptr, remaining_bytes, written, err);

      if (status == darkpool::transport::TlsIoStatus::Ok) {
        write_cursor.offset += written;
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite ||
                 status == darkpool::transport::TlsIoStatus::WantRead) {
        const short poll_events = static_cast<short>(
            ((status == darkpool::transport::TlsIoStatus::WantWrite) ? POLLOUT : POLLIN)
#ifdef POLLRDHUP
            | POLLRDHUP
#endif
        );
        pollfd pfd{fd, poll_events, 0};
        const int wait_ms = static_cast<int>(std::clamp<long long>(remaining_ms, 1, 100));
        const int p = poll(&pfd, 1, wait_ms);
        if (p < 0) {
          if (errno != EINTR) {
            write_cursor.abandoned = true;
            return {FrameWriteStatus::TransportError, write_cursor.offset,
                    std::string("Socket poll system call error during write: ") + std::strerror(errno)};
          }
        } else if (p > 0) {
#ifdef POLLRDHUP
          if ((pfd.revents & (POLLHUP | POLLRDHUP)) != 0) {
#else
          if ((pfd.revents & POLLHUP) != 0) {
#endif
            write_cursor.abandoned = true;
            return {FrameWriteStatus::PeerClosed, write_cursor.offset,
                    "Socket peer closure detected during write poll"};
          }
          if ((pfd.revents & (POLLERR | POLLNVAL)) != 0) {
            write_cursor.abandoned = true;
            return {FrameWriteStatus::TransportError, write_cursor.offset,
                    "Socket POLLERR/POLLNVAL detected during write poll"};
          }
        }
      } else if (status == darkpool::transport::TlsIoStatus::PeerClosed) {
        write_cursor.abandoned = true;
        return {FrameWriteStatus::PeerClosed, write_cursor.offset,
                "Clean TLS close_notify received during write"};
      } else {
        write_cursor.abandoned = true;
        return {FrameWriteStatus::TransportError, write_cursor.offset,
                err.empty() ? "Transport error during write" : err};
      }
    }

    const std::size_t total_written = write_cursor.frame_bytes.size();
    write_cursor.in_progress = false;
    write_cursor.frame_bytes.clear();
    write_cursor.offset = 0;
    return {FrameWriteStatus::Sent, total_written, ""};
  }

  FrameWriteResult send_frame_detailed(const json& payload, int timeout_ms = 5000) {
    if (write_cursor.abandoned) {
      return {FrameWriteStatus::TransportError, write_cursor.offset,
              "Connection stream was abandoned due to prior unrecoverable error"};
    }
    // If a frame write is already in progress, resume that pending frame!
    // Do NOT serialize or begin another frame while bytes from the previous frame remain outstanding.
    if (write_cursor.in_progress) {
      return resume_frame(timeout_ms);
    }

    // Now start the new frame: serialize exactly once into write_cursor
    write_cursor.frame_bytes = Frame(payload).serialize();
    write_cursor.offset = 0;
    write_cursor.in_progress = true;

    return resume_frame(timeout_ms);
  }

  bool send_frame(const json& payload, int timeout_ms = 5000) {
    return send_frame_detailed(payload, timeout_ms).ok();
  }

  FrameResult receive_frame_detailed(int timeout_ms = 5000) {
    set_operation("response receipt");
    char header[4];
    std::size_t header_offset = 0;
    const auto start = std::chrono::steady_clock::now();
    while (header_offset < 4) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if (elapsed > timeout_ms) {
        return {FrameReadStatus::Timeout, std::nullopt, "Read timed out waiting for frame header"};
      }
      std::size_t count = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          ssl.get(), header + header_offset, 4 - header_offset, count, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        header_offset += count;
      } else if (status == darkpool::transport::TlsIoStatus::WantRead ||
                 status == darkpool::transport::TlsIoStatus::WantWrite) {
        const int remaining = std::clamp(static_cast<int>(timeout_ms - elapsed), 1, 100);
        pollfd pfd{fd, static_cast<short>((status == darkpool::transport::TlsIoStatus::WantRead) ? POLLIN : POLLOUT), 0};
        const int p = poll(&pfd, 1, remaining);
        if (p < 0 && errno != EINTR) {
          return {FrameReadStatus::TransportError, std::nullopt,
                  std::string("Socket poll system call error during header read: ") + std::strerror(errno)};
        }
      } else if (status == darkpool::transport::TlsIoStatus::PeerClosed) {
        return {FrameReadStatus::CleanClose, std::nullopt, "Clean TLS close_notify received"};
      } else {
        return {FrameReadStatus::TransportError, std::nullopt, err.empty() ? "Transport error reading header" : err};
      }
    }

    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
    if (length == 0U || length > darkpool::network::kMaxMessageSize) {
      return {FrameReadStatus::ProtocolError, std::nullopt, "Invalid frame length in header"};
    }

    std::string body;
    body.resize(length);
    std::size_t body_offset = 0;
    while (body_offset < length) {
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
      if (elapsed > timeout_ms) {
        return {FrameReadStatus::Timeout, std::nullopt, "Read timed out waiting for frame body"};
      }
      std::size_t count = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          ssl.get(), body.data() + body_offset, length - body_offset, count, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        body_offset += count;
      } else if (status == darkpool::transport::TlsIoStatus::WantRead ||
                 status == darkpool::transport::TlsIoStatus::WantWrite) {
        const int remaining = std::clamp(static_cast<int>(timeout_ms - elapsed), 1, 100);
        pollfd pfd{fd, static_cast<short>((status == darkpool::transport::TlsIoStatus::WantRead) ? POLLIN : POLLOUT), 0};
        const int p = poll(&pfd, 1, remaining);
        if (p < 0 && errno != EINTR) {
          return {FrameReadStatus::TransportError, std::nullopt,
                  std::string("Socket poll system call error during body read: ") + std::strerror(errno)};
        }
      } else if (status == darkpool::transport::TlsIoStatus::PeerClosed) {
        return {FrameReadStatus::CleanClose, std::nullopt, "Clean TLS close_notify received during body read"};
      } else {
        return {FrameReadStatus::TransportError, std::nullopt, err.empty() ? "Transport error reading body" : err};
      }
    }

    try {
      return {FrameReadStatus::Success, json::parse(body), ""};
    } catch (const std::exception& e) {
      return {FrameReadStatus::ProtocolError, std::nullopt, std::string("JSON parse error: ") + e.what()};
    }
  }

  std::optional<json> receive_frame(int timeout_ms = 5000, bool skip_notifications = true) {
    while (true) {
      auto res = receive_frame_detailed(timeout_ms);
      if (!res.frame.has_value()) return std::nullopt;
      if (skip_notifications && res.frame->is_object()) {
        const auto t = res.frame->value("type", "");
        if (t == "server_event" || t == "market_snapshot") {
          continue;
        }
      }
      return res.frame;
    }
  }
};

std::unique_ptr<TlsClientConnection> connect_tls_client(
    std::uint16_t port,
    const darkpool::transport::ClientTlsConfig& config,
    int rcvbuf = 0) {
  set_operation("tls client connect/handshake");
  std::string err;
  auto ctx = darkpool::transport::create_client_ssl_ctx(config, err);
  if (ctx == nullptr) {
    return nullptr;
  }
  const int fd = connect_tcp(port, 80, rcvbuf);
  if (fd < 0) {
    return nullptr;
  }

  SSL* raw_ssl = SSL_new(ctx.get());
  if (raw_ssl == nullptr) {
    (void)close(fd);
    return nullptr;
  }
  darkpool::transport::SslPtr ssl(raw_ssl);
  SSL_set_app_data(ssl.get(), const_cast<darkpool::transport::ClientTlsConfig*>(&config));
  if (SSL_set_fd(ssl.get(), fd) != 1) {
    (void)close(fd);
    return nullptr;
  }

  const int flags = fcntl(fd, F_GETFL, 0);
  CHECK(flags >= 0);
  if (flags >= 0) {
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      (void)close(fd);
      return nullptr;
    }
    const auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int p_timeout = std::clamp(static_cast<int>(rem_ms), 1, 100);

    bool want_read = false, want_write = false;
    std::string hs_err;
    const auto status = darkpool::transport::step_tls_handshake(
        ssl.get(), false, want_read, want_write, hs_err);
    if (status == darkpool::transport::TlsHandshakeStatus::Established) {
      break;
    }
    if (status == darkpool::transport::TlsHandshakeStatus::Failed) {
      (void)close(fd);
      return nullptr;
    }
    pollfd pfd{fd, static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0)), 0};
    const int p = poll(&pfd, 1, p_timeout);
    if (p < 0 && errno != EINTR) {
      (void)close(fd);
      return nullptr;
    }
  }
  // Keep socket in nonblocking mode for accurate timeout loops and WANT_READ/WANT_WRITE handling
  if (flags >= 0) {
    (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }
  const int post_hs_flags = fcntl(fd, F_GETFL, 0);
  CHECK(post_hs_flags >= 0 && (post_hs_flags & O_NONBLOCK) != 0);

  // Strict post-handshake verification
  X509* peer_cert = SSL_get_peer_certificate(ssl.get());
  if (peer_cert == nullptr) {
    (void)close(fd);
    return nullptr;
  }
  darkpool::transport::X509Ptr cert_guard(peer_cert);
  std::string v_err;
  if (!darkpool::transport::verify_cert_time_validity(peer_cert, v_err) ||
      !darkpool::transport::verify_cert_server_purpose(peer_cert, v_err) ||
      !darkpool::transport::verify_cert_ip_san(peer_cert, config.target_ip, v_err)) {
    (void)close(fd);
    return nullptr;
  }

  if (config.mode == darkpool::transport::ClientVerifyMode::FingerprintPinning) {
    const std::string actual_fp = darkpool::transport::compute_cert_fingerprint_sha256(peer_cert);
    std::array<std::uint8_t, 32> exp_digest{};
    if (!darkpool::transport::parse_fingerprint_sha256(config.pinned_fingerprint_hex, exp_digest, v_err)) {
      (void)close(fd);
      return nullptr;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const auto b : exp_digest) oss << std::setw(2) << static_cast<int>(b);
    if (actual_fp != oss.str()) {
      (void)close(fd);
      return nullptr;
    }
  } else if (config.mode == darkpool::transport::ClientVerifyMode::LocalCa) {
    if (SSL_get_verify_result(ssl.get()) != X509_V_OK) {
      (void)close(fd);
      return nullptr;
    }
  }

  auto conn = std::make_unique<TlsClientConnection>();
  conn->fd = fd;
  conn->ctx = std::move(ctx);
  conn->ssl = std::move(ssl);
  return conn;
}

// --------------------------------------------------------------------------
// Scenario 1: Valid TLS 1.3 handshake with trusted local CA and matching IP SAN
// --------------------------------------------------------------------------
void test_scenario_01_valid_ca(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  CHECK(certs != nullptr);
  const auto port = reserve_port();
  CHECK(port != 0U);
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);
  if (client != nullptr) {
    const char* version = SSL_get_version(client->ssl.get());
    CHECK(std::string(version) == "TLSv1.3");
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto resp = client->receive_frame();
    CHECK(resp.has_value());
    if (resp) {
      CHECK(resp->value("type", "") == "connect");
      CHECK(resp->value("role", "") == "player");
    }
  }
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 2: Valid TLS 1.3 handshake in fingerprint mode with self-signed leaf
// --------------------------------------------------------------------------
void test_scenario_02_valid_fingerprint(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, true, false); // self-signed
  CHECK(certs != nullptr);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
  client_cfg.pinned_fingerprint_hex = certs->server_fingerprint_hex;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);
  if (client != nullptr) {
    const char* version = SSL_get_version(client->ssl.get());
    CHECK(std::string(version) == "TLSv1.3");
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto resp = client->receive_frame();
    CHECK(resp.has_value());
    if (resp) {
      CHECK(resp->value("type", "") == "connect");
    }
  }
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 3: Rejection of an untrusted CA root
// --------------------------------------------------------------------------
void test_scenario_03_untrusted_ca(const char* server_path) {
  auto server_certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  auto other_ca = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, server_certs->server_cert_pem_path, server_certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = other_ca->ca_cert_pem_path; // Untrusted CA
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr); // Handshake must fail
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 4: Rejection of an incorrect fingerprint
// --------------------------------------------------------------------------
void test_scenario_04_incorrect_fingerprint(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, true, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
  // All zeroes fingerprint: guaranteed mismatch
  client_cfg.pinned_fingerprint_hex = "0000000000000000000000000000000000000000000000000000000000000000";
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr);
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 5: Rejection of mismatched IP SAN
// --------------------------------------------------------------------------
void test_scenario_05_mismatched_ip_san(const char* server_path) {
  auto certs = generate_ephemeral_certs("10.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1"; // Differs from 10.0.0.1

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr);
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 6: Rejection of expired certificate
// --------------------------------------------------------------------------
void test_scenario_06_expired_cert(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, true); // expired
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr);
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 7: Rejection of TLS 1.2 and earlier
// --------------------------------------------------------------------------
void test_scenario_07_reject_tls12(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  SSL_CTX* legacy_ctx = SSL_CTX_new(TLS_client_method());
  CHECK(legacy_ctx != nullptr);
  (void)SSL_CTX_set_max_proto_version(legacy_ctx, TLS1_2_VERSION);
  (void)SSL_CTX_load_verify_locations(legacy_ctx, certs->ca_cert_pem_path.c_str(), nullptr);

  const int fd = connect_tcp(port);
  CHECK(fd >= 0);

  SSL* ssl = SSL_new(legacy_ctx);
  (void)SSL_set_fd(ssl, fd);

  const int res = SSL_connect(ssl);
  CHECK(res <= 0); // Server must reject TLS 1.2

  SSL_free(ssl);
  (void)close(fd);
  SSL_CTX_free(legacy_ctx);
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 8: Rejection of plaintext traffic sent to TLS listener
// --------------------------------------------------------------------------
void test_scenario_08_reject_plaintext_on_tls_port(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  const int fd = connect_tcp(port);
  CHECK(fd >= 0);

  const std::string plaintext_req = Frame(json{{"type", "connect"}, {"role", "player"}}).serialize();
  const auto sent = ::send(fd, plaintext_req.data(), plaintext_req.size(), 0);
  CHECK(sent == static_cast<ssize_t>(plaintext_req.size()));

  char buf[64];
  const auto recvd = ::recv(fd, buf, sizeof(buf), 0);
  CHECK(recvd <= 0); // Server terminates connection on handshake error

  (void)close(fd);
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 9: Handshake stall timeout and cleanup
// --------------------------------------------------------------------------
void test_scenario_09_handshake_stall_timeout(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  const int stalled_fd = connect_tcp(port);
  CHECK(stalled_fd >= 0);

  // Sleep 5.5s to exceed the 5.0s handshake timeout
  std::this_thread::sleep_for(std::chrono::milliseconds(5500));

  char buf[16];
  const auto recvd = ::recv(stalled_fd, buf, sizeof(buf), 0);
  CHECK(recvd == 0); // Server closed connection after timeout

  (void)close(stalled_fd);
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 10: Fragmented TLS records and multiple coalesced application frames
// --------------------------------------------------------------------------
void test_scenario_10_coalesced_frames(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);
  if (client != nullptr) {
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto conn_resp = client->receive_frame();
    CHECK(conn_resp.has_value());

    const std::string f1 = Frame(json{{"type", "status"}}).serialize();
    const std::string f2 = Frame(json{{"type", "status"}}).serialize();
    const std::string combined = f1 + f2;

    // Send in two 3-byte fragments then remaining
    std::size_t written = 0;
    std::string err;
    (void)darkpool::transport::tls_write(client->ssl.get(), combined.data(), 3, written, err);
    (void)darkpool::transport::tls_write(client->ssl.get(), combined.data() + 3, 3, written, err);
    (void)darkpool::transport::tls_write(client->ssl.get(), combined.data() + 6, combined.size() - 6, written, err);

    auto r1 = client->receive_frame();
    auto r2 = client->receive_frame();
    CHECK(r1.has_value());
    CHECK(r2.has_value());
  }
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 11: Slow-reader output queue bounds over TLS (drives queue >64 KiB)
// --------------------------------------------------------------------------
void test_scenario_11_slow_reader_bounds(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();

  const std::string server_log_path = certs->temp_dir_path + "/server_scenario11.log";
  std::string log_arg = server_log_path;

  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path,
                                      certs->server_key_pem_path, "mod-token", "127.0.0.1", &log_arg);
  CHECK(child > 0);
  ServerSubprocessGuard server_guard{child};

  // 1. Establish deterministic baseline: verify server is listening via a TCP probe, then capture baseline open FDs
  const int probe_fd = connect_tcp(port, 80);
  CHECK(probe_fd >= 0);
  (void)::close(probe_fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto initial_server_fds = count_open_fds_pid(child);
  CHECK(initial_server_fds.has_value());

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  // 2. Setup healthy sibling match participants
  auto mod = connect_tls_client(port, client_cfg);
  CHECK(mod != nullptr);
  CHECK(mod->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token"}}));
  auto mod_conn = mod->receive_frame();
  CHECK(mod_conn.has_value());

  CHECK(mod->send_frame(json{{"type", "create_match"}, {"player_count", 3}}));
  auto create_resp = mod->receive_frame();
  CHECK(create_resp.has_value());
  const std::uint64_t match_id = create_resp->value("match_id", 1ULL);

  auto p1 = connect_tls_client(port, client_cfg);
  auto p2 = connect_tls_client(port, client_cfg);
  auto p3 = connect_tls_client(port, client_cfg);
  CHECK(p1 != nullptr && p2 != nullptr && p3 != nullptr);
  CHECK(p1->send_frame(json{{"type", "connect"}, {"role", "player"}})); (void)p1->receive_frame();
  CHECK(p1->send_frame(json{{"type", "join_match"}, {"match_id", match_id}})); (void)p1->receive_frame();
  CHECK(p2->send_frame(json{{"type", "connect"}, {"role", "player"}})); (void)p2->receive_frame();
  CHECK(p2->send_frame(json{{"type", "join_match"}, {"match_id", match_id}})); (void)p2->receive_frame();
  CHECK(p3->send_frame(json{{"type", "connect"}, {"role", "player"}})); (void)p3->receive_frame();
  CHECK(p3->send_frame(json{{"type", "join_match"}, {"match_id", match_id}})); (void)p3->receive_frame();

  // 3. Slow client connects as an independent connection over TLS with constrained receive buffer
  auto slow_client = connect_tls_client(port, client_cfg, 2048);
  CHECK(slow_client != nullptr);
  int rcvbuf = 2048;
  (void)setsockopt(slow_client->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  CHECK(slow_client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
  auto slow_conn = slow_client->receive_frame();
  CHECK(slow_conn.has_value());

  // 4. Concurrency & Flood worker synchronization primitives
  std::atomic<bool> stop_flood{false};
  std::atomic<bool> phase_b_active{false};
  std::atomic<bool> worker_active_barrier{false};
  std::atomic<bool> worker_saw_eviction{false};
  std::atomic<int> completed_frames{0};
  std::string worker_eviction_reason;
  std::mutex worker_mutex;

  struct ThreadJoiner {
    std::thread& t;
    ~ThreadJoiner() {
      if (t.joinable()) {
        t.join();
      }
    }
  };

  const json flood_payload = json{{"type", "match_status"}, {"match_id", match_id}};

  std::thread flood_worker([&]() {
    const auto flood_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!stop_flood.load() && std::chrono::steady_clock::now() < flood_deadline) {
      const auto write_res = slow_client->send_frame_detailed(flood_payload, 200);
      if (write_res.status == FrameWriteStatus::Sent) {
        const int cnt = completed_frames.fetch_add(1) + 1;
        if (cnt >= 2) {
          worker_active_barrier.store(true);
        }
        if (!phase_b_active.load()) {
          // Pacing during Phase A to ensure match progression runs concurrently without premature queue overflow
          std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
      } else if (write_res.status == FrameWriteStatus::Timeout) {
        // Socket backpressure: write deadline expired without socket closure.
        // Under flow control, backpressure does NOT indicate disconnection.
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      } else if (write_res.status == FrameWriteStatus::PeerClosed ||
                 write_res.status == FrameWriteStatus::TransportError) {
        // Genuine peer disconnect or transport error established
        std::lock_guard<std::mutex> lock(worker_mutex);
        worker_saw_eviction.store(true);
        worker_eviction_reason = write_res.error_message;
        break;
      } else {
        std::lock_guard<std::mutex> lock(worker_mutex);
        worker_eviction_reason = write_res.error_message;
        break;
      }
    }
  });
  ThreadJoiner joiner{flood_worker};

  // Phase A: Concurrent sibling-progress verification
  // Wait on synchronization barrier until worker has completed whole valid frames
  const auto barrier_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!worker_active_barrier.load() && std::chrono::steady_clock::now() < barrier_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  CHECK(worker_active_barrier.load());

  // Capture baseline completed frames immediately before moderator operations begin
  const int baseline_frames = completed_frames.load();

  // Concurrently, while flood_worker is actively sending queries:
  CHECK(mod->send_frame(json{{"type", "start_match"}, {"match_id", match_id}}));
  auto start_resp = mod->receive_frame(3000);
  CHECK(start_resp.has_value());
  CHECK(start_resp->value("ok", false) == true);
  CHECK(start_resp->value("phase", "") == "AwaitingEvent");

  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}}));
  auto pub_resp = mod->receive_frame(3000);
  CHECK(pub_resp.has_value());
  CHECK(pub_resp->value("ok", false) == true);
  CHECK(pub_resp->value("phase", "") == "Discussion");

  CHECK(p1->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto p1_status = p1->receive_frame(3000);
  CHECK(p1_status.has_value());
  CHECK(p1_status->value("phase", "") == "Discussion");

  // Verify that flood traffic was actively occurring during this sequence
  const auto phase_a_progress_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  while (completed_frames.load() <= baseline_frames && std::chrono::steady_clock::now() < phase_a_progress_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  const int post_phase_a_frames = completed_frames.load();
  CHECK(post_phase_a_frames > baseline_frames);
  CHECK(!worker_saw_eviction.load());

  // Phase B: Queue-pressure and eviction verification
  // Signal worker to flood at sustained full speed to exhaust server's 64 KiB queue
  phase_b_active.store(true);

  const auto wait_eviction_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (!worker_saw_eviction.load() && std::chrono::steady_clock::now() < wait_eviction_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  stop_flood.store(true);
  if (flood_worker.joinable()) {
    flood_worker.join();
  }

  bool evicted = worker_saw_eviction.load();
  if (!evicted) {
    const auto drain_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < drain_deadline) {
      auto res = slow_client->receive_frame_detailed(100);
      if (res.status == FrameReadStatus::CleanClose ||
          res.status == FrameReadStatus::TransportError) {
        evicted = true;
        break;
      }
      if (res.status == FrameReadStatus::Timeout) {
        const auto w_res = slow_client->send_frame_detailed(flood_payload, 100);
        if (w_res.status == FrameWriteStatus::PeerClosed ||
            w_res.status == FrameWriteStatus::TransportError) {
          evicted = true;
          break;
        }
      }
    }
  }
  CHECK(evicted);
  // Verify defensible traffic volume: at least 500 completed frames sent to exhaust 64 KiB queue
  CHECK(completed_frames.load() >= 500);

  // 5. Verify server-side affirmative eviction diagnostic in server log
  std::string server_log_contents;
  const auto log_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < log_deadline) {
    std::ifstream log_file(server_log_path);
    if (log_file.is_open()) {
      std::stringstream ss;
      ss << log_file.rdbuf();
      server_log_contents = ss.str();
      if (server_log_contents.find("evicted: output queue overflow") != std::string::npos) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(server_log_contents.find("evicted: output queue overflow") != std::string::npos);
  CHECK(server_log_contents.find("framing error") == std::string::npos);

  // 6. Cleanup client connections and verify server descriptors settle back to baseline
  slow_client.reset();
  p1.reset();
  p2.reset();
  p3.reset();
  mod.reset();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2500);
  bool settled = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto cur = count_open_fds_pid(child);
    if (cur && initial_server_fds && *cur <= *initial_server_fds) {
      settled = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(settled);
}

// --------------------------------------------------------------------------
// Scenario 12: Request flood rate limiting over TLS (with state-mutation verification)
// --------------------------------------------------------------------------
void test_scenario_12_flood_rate_limiting(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path, "mod-token");
  CHECK(child > 0);
  ServerSubprocessGuard server_guard{child};

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  // Create match and initialize to Event phase
  auto mod = connect_tls_client(port, client_cfg);
  CHECK(mod != nullptr);
  CHECK(mod->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token"}}));
  auto mod_conn = mod->receive_frame();
  CHECK(mod_conn.has_value());

  CHECK(mod->send_frame(json{{"type", "create_match"}, {"player_count", 3}}));
  auto create_resp = mod->receive_frame();
  CHECK(create_resp.has_value());
  const std::uint64_t match_id = create_resp->value("match_id", 1ULL);

  auto p1 = connect_tls_client(port, client_cfg);
  auto p2 = connect_tls_client(port, client_cfg);
  auto p3 = connect_tls_client(port, client_cfg);
  CHECK(p1 != nullptr && p2 != nullptr && p3 != nullptr);

  CHECK(p1->send_frame(json{{"type", "connect"}, {"role", "player"}})); (void)p1->receive_frame();
  CHECK(p1->send_frame(json{{"type", "join_match"}, {"match_id", match_id}})); (void)p1->receive_frame();
  CHECK(p2->send_frame(json{{"type", "connect"}, {"role", "player"}})); (void)p2->receive_frame();
  CHECK(p2->send_frame(json{{"type", "join_match"}, {"match_id", match_id}})); (void)p2->receive_frame();
  CHECK(p3->send_frame(json{{"type", "connect"}, {"role", "player"}})); (void)p3->receive_frame();
  CHECK(p3->send_frame(json{{"type", "join_match"}, {"match_id", match_id}})); (void)p3->receive_frame();

  CHECK(mod->send_frame(json{{"type", "start_match"}, {"match_id", match_id}}));
  auto start_resp = mod->receive_frame();
  CHECK(start_resp.has_value());
  CHECK(start_resp->value("ok", false) == true);
  CHECK(start_resp->value("phase", "") == "AwaitingEvent");

  // Snapshot complete observable match status from independent player connection (p1) before throttled mutation
  CHECK(p1->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto status_before = p1->receive_frame();
  CHECK(status_before.has_value());
  const std::string phase_before = status_before->value("phase", "");
  const int health_before = status_before->value("health", 0);
  const int round_before = status_before->value("round", 0);
  CHECK(phase_before == "AwaitingEvent");
  CHECK(health_before == 5);
  CHECK(round_before == 1);

  // Ensure token bucket has refilled completely to 40 burst tokens after setup commands
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  // Exhaust moderator's token bucket rate limiter (burst capacity 40 tokens)
  int accepted_status = 0;
  for (int i = 0; i < 40; ++i) {
    CHECK(mod->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
    auto r = mod->receive_frame();
    CHECK(r.has_value());
    if (r && r->value("type", "") == "match_status") {
      accepted_status++;
    }
  }
  CHECK(accepted_status == 40);

  // Immediately send a valid state-changing mutation command while throttled:
  // In AwaitingEvent phase, publishing an event (impact 1) is a valid mutation that advances to Discussion phase.
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}}));
  auto throttled_resp = mod->receive_frame();
  CHECK(throttled_resp.has_value());
  CHECK(throttled_resp->value("type", "") == "error");
  CHECK(throttled_resp->value("error", "") == "rate limit exceeded");

  // Query observable match status via independent connection (p1)
  // Verify that the complete observable match-status snapshot remained completely unchanged:
  CHECK(p1->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto status_after_throttled = p1->receive_frame();
  CHECK(status_after_throttled.has_value());
  // Complete JSON equality comparison
  CHECK(*status_after_throttled == *status_before);
  CHECK(status_after_throttled->value("match_id", 0ULL) == status_before->value("match_id", 0ULL));
  CHECK(status_after_throttled->value("phase", "") == phase_before);
  CHECK(status_after_throttled->value("health", 0) == health_before);
  CHECK(status_after_throttled->value("round", 0) == round_before);
  CHECK(status_after_throttled->value("price", 0) == status_before->value("price", 0));
  CHECK(status_after_throttled->value("outcome", "") == status_before->value("outcome", ""));
  CHECK(status_after_throttled->value("player_count", 0) == status_before->value("player_count", 0));
  CHECK(status_after_throttled->value("joined_players", 0) == status_before->value("joined_players", 0));
  if (status_before->contains("last_market")) {
    CHECK(status_after_throttled->contains("last_market"));
    CHECK((*status_after_throttled)["last_market"] == (*status_before)["last_market"]);
  }
  if (status_before->contains("last_vote")) {
    CHECK(status_after_throttled->contains("last_vote"));
    CHECK((*status_after_throttled)["last_vote"] == (*status_before)["last_vote"]);
  }

  // Test token refill: rate is 20 tokens/sec (1 token per 50 ms).
  // Wait 250 ms for token bucket to refill at least 5 tokens.
  std::this_thread::sleep_for(std::chrono::milliseconds(250));

  // Now submit the EXACT SAME valid mutation command that was previously throttled:
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}}));
  auto refilled_resp = mod->receive_frame();
  CHECK(refilled_resp.has_value());
  CHECK(refilled_resp->value("type", "") == "transition_result");
  CHECK(refilled_resp->value("ok", false) == true);
  CHECK(refilled_resp->value("phase", "") == "Discussion");

  // Query observable match status via p1 again: verify that state mutation has now succeeded!
  CHECK(p1->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto status_after_refilled = p1->receive_frame();
  CHECK(status_after_refilled.has_value());
  CHECK(status_after_refilled->value("phase", "") == "Discussion");
  CHECK(status_after_refilled->value("health", 0) == health_before);
  CHECK(*status_after_refilled != *status_before);
}

// --------------------------------------------------------------------------
// Scenario 13: Secret redaction in server and client diagnostics
// --------------------------------------------------------------------------
void test_scenario_13_secret_redaction(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();

  std::string stdout_log = certs->temp_dir_path + "/server_stdout.log";
  const std::string sensitive_token = "ultra-secret-test-token-xyz123";
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path, sensitive_token.c_str(), "127.0.0.1", &stdout_log);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);
  if (client != nullptr) {
    // Attempt authentication with wrong token
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "wrong-secret-abc"}}));
    (void)client->receive_frame();
  }

  stop_server(child);

  // Audit server stdout log: sensitive token must never appear in log output
  std::ifstream log_file(stdout_log);
  if (log_file.is_open()) {
    std::string content((std::istreambuf_iterator<char>(log_file)), std::istreambuf_iterator<char>());
    CHECK(content.find(sensitive_token) == std::string::npos);
    CHECK(content.find("wrong-secret-abc") == std::string::npos);
  }
}

// --------------------------------------------------------------------------
// Scenario 14: Complete 5-player match gameplay over TLS 1.3 (All 3 rounds to Finished)
// --------------------------------------------------------------------------
void test_scenario_14_complete_match_gameplay(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path, "mod-secret");
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  // 1. Moderator connects
  auto mod = connect_tls_client(port, client_cfg);
  CHECK(mod != nullptr);
  CHECK(mod->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-secret"}}));
  auto mod_conn_resp = mod->receive_frame();
  CHECK(mod_conn_resp.has_value());

  // 2. Moderator creates match for 5 players
  CHECK(mod->send_frame(json{{"type", "create_match"}, {"player_count", 5}}));
  auto create_resp = mod->receive_frame();
  CHECK(create_resp.has_value());
  const std::uint64_t match_id = create_resp->value("match_id", 1ULL);

  // 3. Connect 5 players over TLS
  std::vector<std::unique_ptr<TlsClientConnection>> players;
  std::vector<std::string> roles;
  std::size_t insider_idx = 999;
  std::size_t investigator_idx = 999;
  std::size_t employee_idx = 999;

  for (std::size_t i = 0; i < 5; ++i) {
    auto p = connect_tls_client(port, client_cfg);
    CHECK(p != nullptr);
    CHECK(p->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto p_conn = p->receive_frame();
    CHECK(p_conn.has_value());

    CHECK(p->send_frame(json{{"type", "join_match"}, {"match_id", match_id}}));
    auto join_resp = p->receive_frame();
    CHECK(join_resp.has_value());
    if (join_resp) {
      const std::string r = join_resp->value("private_role", "");
      roles.push_back(r);
      if (r == "Insider") insider_idx = i;
      else if (r == "Investigator") investigator_idx = i;
      else if (employee_idx == 999) employee_idx = i;
    }
    players.push_back(std::move(p));
  }
  CHECK(players.size() == 5);
  CHECK(insider_idx < 5);
  CHECK(investigator_idx < 5);
  CHECK(employee_idx < 5);

  // 4. Moderator starts match
  CHECK(mod->send_frame(json{{"type", "start_match"}, {"match_id", match_id}}));
  auto start_resp = mod->receive_frame();
  CHECK(start_resp.has_value());
  CHECK(start_resp->value("phase", "") == "AwaitingEvent");

  // 5. Round 1
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}}));
  auto ev1_resp = mod->receive_frame();
  CHECK(ev1_resp.has_value());
  CHECK(ev1_resp->value("phase", "") == "Discussion");

  // Insider requests private hint
  CHECK(players[insider_idx]->send_frame(json{{"type", "insider_hint"}, {"match_id", match_id}}));
  auto hint_resp = players[insider_idx]->receive_frame();
  CHECK(hint_resp.has_value());
  CHECK(hint_resp->value("type", "") == "hint_private");
  CHECK(hint_resp->value("event_impact", -99) == 0);

  // Open trading & submit trades
  CHECK(mod->send_frame(json{{"type", "open_trading"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(players[i]->send_frame(json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}}));
    auto tr = players[i]->receive_frame();
    CHECK(tr.has_value());
  }

  // Resolve market -> Clue phase
  CHECK(mod->send_frame(json{{"type", "resolve_market"}, {"match_id", match_id}}));
  (void)mod->receive_frame();

  // Investigator submits clue request targeting Employee (player ID = employee_idx + 1)
  const std::uint64_t target_employee_id = static_cast<std::uint64_t>(employee_idx + 1);
  CHECK(players[investigator_idx]->send_frame(json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", target_employee_id}}));
  auto clue_resp = players[investigator_idx]->receive_frame();
  CHECK(clue_resp.has_value());
  CHECK(clue_resp->value("type", "") == "clue_private");
  CHECK(clue_resp->value("is_insider", true) == false); // Target employee is not an insider!

  // Employee cannot submit clue request (permission rejected)
  CHECK(players[employee_idx]->send_frame(json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", 1ULL}}));
  auto emp_clue_resp = players[employee_idx]->receive_frame();
  CHECK(emp_clue_resp.has_value());
  CHECK(emp_clue_resp->value("ok", true) == false);

  // Open voting -> Voting
  CHECK(mod->send_frame(json{{"type", "open_voting"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(players[i]->send_frame(json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}}));
    (void)players[i]->receive_frame();
  }
  CHECK(mod->send_frame(json{{"type", "resolve_votes"}, {"match_id", match_id}}));
  auto v1_res = mod->receive_frame();
  CHECK(v1_res.has_value());

  // 6. Round 2
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}}));
  (void)mod->receive_frame();
  CHECK(mod->send_frame(json{{"type", "open_trading"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(players[i]->send_frame(json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}}));
    (void)players[i]->receive_frame();
  }
  CHECK(mod->send_frame(json{{"type", "resolve_market"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  CHECK(mod->send_frame(json{{"type", "open_voting"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(players[i]->send_frame(json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}}));
    (void)players[i]->receive_frame();
  }
  CHECK(mod->send_frame(json{{"type", "resolve_votes"}, {"match_id", match_id}}));
  auto v2_res = mod->receive_frame();
  CHECK(v2_res.has_value());

  // 7. Round 3
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", -1}}));
  (void)mod->receive_frame();
  CHECK(mod->send_frame(json{{"type", "open_trading"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(players[i]->send_frame(json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}}));
    (void)players[i]->receive_frame();
  }
  CHECK(mod->send_frame(json{{"type", "resolve_market"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  CHECK(mod->send_frame(json{{"type", "open_voting"}, {"match_id", match_id}}));
  (void)mod->receive_frame();
  for (std::size_t i = 0; i < 5; ++i) {
    CHECK(players[i]->send_frame(json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}}));
    (void)players[i]->receive_frame();
  }
  CHECK(mod->send_frame(json{{"type", "resolve_votes"}, {"match_id", match_id}}));
  auto v3_res = mod->receive_frame();
  CHECK(v3_res.has_value());

  // Terminal state verification: phase must be Finished!
  CHECK(players[0]->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto terminal_status = players[0]->receive_frame();
  CHECK(terminal_status.has_value());
  CHECK(terminal_status->value("phase", "") == "Finished");
  CHECK(!terminal_status->value("outcome", "").empty());

  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 15: Multiple TLS clients and concurrent match isolation
// --------------------------------------------------------------------------
void test_scenario_15_concurrent_match_isolation(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path, "mod-token");
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto mod1 = connect_tls_client(port, client_cfg);
  CHECK(mod1 != nullptr);
  CHECK(mod1->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token"}}));
  (void)mod1->receive_frame();
  CHECK(mod1->send_frame(json{{"type", "create_match"}, {"player_count", 3}}));
  auto m1_create = mod1->receive_frame();
  CHECK(m1_create.has_value());
  const std::uint64_t match_id_1 = m1_create->value("match_id", 1ULL);

  auto mod2 = connect_tls_client(port, client_cfg);
  CHECK(mod2 != nullptr);
  CHECK(mod2->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token"}}));
  (void)mod2->receive_frame();
  CHECK(mod2->send_frame(json{{"type", "create_match"}, {"player_count", 3}}));
  auto m2_create = mod2->receive_frame();
  CHECK(m2_create.has_value());
  const std::uint64_t match_id_2 = m2_create->value("match_id", 2ULL);

  CHECK(match_id_1 != match_id_2);

  auto p1 = connect_tls_client(port, client_cfg);
  CHECK(p1 != nullptr);
  CHECK(p1->send_frame(json{{"type", "connect"}, {"role", "player"}}));
  (void)p1->receive_frame();
  CHECK(p1->send_frame(json{{"type", "join_match"}, {"match_id", match_id_1}}));
  auto p1_join = p1->receive_frame();
  CHECK(p1_join.has_value());

  auto p2 = connect_tls_client(port, client_cfg);
  CHECK(p2 != nullptr);
  CHECK(p2->send_frame(json{{"type", "connect"}, {"role", "player"}}));
  (void)p2->receive_frame();
  CHECK(p2->send_frame(json{{"type", "join_match"}, {"match_id", match_id_2}}));
  auto p2_join = p2->receive_frame();
  CHECK(p2_join.has_value());

  // Cross-match probe 1: Player 1 (joined in Match 1) attempts action on Match 2 -> must be rejected
  CHECK(p1->send_frame(json{{"type", "trade"}, {"match_id", match_id_2}, {"action", "HOLD"}}));
  auto cross_trade = p1->receive_frame();
  CHECK(cross_trade.has_value());
  CHECK(cross_trade->value("type", "") == "error");

  // Cross-match probe 2: Player 2 (joined in Match 2) attempts action on Match 1 -> must be rejected
  CHECK(p2->send_frame(json{{"type", "vote"}, {"match_id", match_id_1}, {"target_id", nullptr}}));
  auto cross_vote = p2->receive_frame();
  CHECK(cross_vote.has_value());
  CHECK(cross_vote->value("type", "") == "error");

  // Cross-match probe 3: Moderator 1 attempts to control Match 2 -> must be rejected
  CHECK(mod1->send_frame(json{{"type", "start_match"}, {"match_id", match_id_2}}));
  auto cross_mod = mod1->receive_frame();
  CHECK(cross_mod.has_value());
  CHECK(cross_mod->value("type", "") == "error");

  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 16: Disconnect and shutdown cleanup (monitoring server PID)
// --------------------------------------------------------------------------
void test_scenario_16_disconnect_cleanup(const char* server_path) {
  const auto fds_before_parent = count_open_fds();

  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  // Poll for child listener readiness and capture server baseline open FDs
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto server_baseline_fds = count_open_fds_pid(child);
  CHECK(server_baseline_fds.has_value());

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  {
    std::vector<std::unique_ptr<TlsClientConnection>> clients;
    for (int i = 0; i < 10; ++i) {
      auto c = connect_tls_client(port, client_cfg);
      CHECK(c != nullptr);
      if (c != nullptr) {
        CHECK(c->send_frame(json{{"type", "connect"}, {"role", "player"}}));
        auto r = c->receive_frame();
        CHECK(r.has_value());
        clients.push_back(std::move(c));
      }
    }
    // Verify server child open FDs increased with 10 clients connected
    const auto active_server_fds = count_open_fds_pid(child);
    CHECK(active_server_fds.has_value());
    CHECK(*active_server_fds >= *server_baseline_fds + 10);
  } // All 10 clients destroyed and closed cleanly

  // Monitor server child process /proc/<child>/fd to verify settling back to baseline within 2000 ms
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2000);
  bool settled = false;
  std::size_t settled_server_fds = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto current = count_open_fds_pid(child);
    if (current && *current <= *server_baseline_fds + 1) {
      settled = true;
      settled_server_fds = *current;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(settled);
  (void)settled_server_fds;

  stop_server(child);

  // Verify test process parent FDs returned to baseline
  const auto fds_after_parent = count_open_fds();
  CHECK(fds_after_parent <= fds_before_parent + 2);
}

// --------------------------------------------------------------------------
// Scenario 17: Valid TLS 1.3 in fingerprint mode with a CA-signed leaf
// --------------------------------------------------------------------------
void test_scenario_17_ca_signed_leaf_fingerprint_mode(const char* server_path) {
  // Generate CA-issued server cert
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, /*self_signed=*/false, false);
  CHECK(certs != nullptr);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  // Client uses FingerprintPinning mode with the leaf certificate's SHA-256 fingerprint.
  // Note: Client does NOT load or trust the CA root!
  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
  client_cfg.pinned_fingerprint_hex = certs->server_fingerprint_hex;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);
  if (client != nullptr) {
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto resp = client->receive_frame();
    CHECK(resp.has_value());
    CHECK(resp->value("type", "") == "connect");
  }
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 18: Fingerprint mode with not-yet-valid certificate rejection
// --------------------------------------------------------------------------
void test_scenario_18_not_yet_valid_cert_rejection(const char* server_path) {
  // Generate certificate with notBefore in future
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, true, false, /*not_yet_valid=*/true);
  CHECK(certs != nullptr);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
  client_cfg.pinned_fingerprint_hex = certs->server_fingerprint_hex;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr); // Must be rejected due to notBefore in future!
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 19: Invalid server certificate usage / EKU rejection
// --------------------------------------------------------------------------
void test_scenario_19_invalid_server_eku_rejection(const char* server_path) {
  // Generate certificate with clientAuth only (missing serverAuth)
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false, false, /*client_auth_only_eku=*/true);
  CHECK(certs != nullptr);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr); // Must be rejected because certificate does not permit serverAuth!
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 20: Certificate rotation (old pin fails until updated)
// --------------------------------------------------------------------------
void test_scenario_20_cert_rotation_pin_update(const char* server_path) {
  auto certs_old = generate_ephemeral_certs("127.0.0.1", 1, true, false);
  auto certs_new = generate_ephemeral_certs("127.0.0.1", 1, true, false);
  const auto port = reserve_port();

  // Server starts with rotated (new) certificate
  const auto child = start_tls_server(server_path, port, certs_new->server_cert_pem_path, certs_new->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig old_cfg;
  old_cfg.mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
  old_cfg.pinned_fingerprint_hex = certs_old->server_fingerprint_hex; // Stale pin
  old_cfg.target_ip = "127.0.0.1";

  auto client_old = connect_tls_client(port, old_cfg);
  CHECK(client_old == nullptr); // Old pin must be rejected!

  darkpool::transport::ClientTlsConfig new_cfg;
  new_cfg.mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
  new_cfg.pinned_fingerprint_hex = certs_new->server_fingerprint_hex; // Updated pin
  new_cfg.target_ip = "127.0.0.1";

  auto client_new = connect_tls_client(port, new_cfg);
  CHECK(client_new != nullptr); // Updated pin succeeds!

  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 21: Strict private key file permissions (0600 succeeds, 0644 fails closed)
// --------------------------------------------------------------------------
void test_scenario_21_strict_key_permissions(const char* server_path) {
  (void)server_path;
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  CHECK(certs != nullptr);

  // 1. Verify 0600 permissions succeed
  (void)::chmod(certs->server_key_pem_path.c_str(), 0600);
  std::string err_msg;
  darkpool::transport::ServerTlsConfig cfg_ok;
  cfg_ok.cert_file = certs->server_cert_pem_path;
  cfg_ok.key_file = certs->server_key_pem_path;
  auto ctx_ok = darkpool::transport::create_server_ssl_ctx(cfg_ok, err_msg);
  CHECK(ctx_ok != nullptr);

  // 2. Make permissions overly permissive (0644 - group and world readable)
  (void)::chmod(certs->server_key_pem_path.c_str(), 0644);
  err_msg.clear();
  auto ctx_fail = darkpool::transport::create_server_ssl_ctx(cfg_ok, err_msg);
  CHECK(ctx_fail == nullptr); // Must fail closed!
  CHECK(err_msg.find("chmod 0600 or 0400") != std::string::npos); // Must include actionable message!
}

// --------------------------------------------------------------------------
// Scenario 22: Mutually exclusive CLI verification options
// --------------------------------------------------------------------------
void test_scenario_22_cli_mutual_exclusivity(const char* server_path) {
  // Execute client with both --ca-cert and --server-fingerprint
  std::string client_bin = std::string(server_path);
  const auto pos = client_bin.rfind("darkpool_server");
  if (pos != std::string::npos) {
    client_bin.replace(pos, 15, "darkpool_client");
  }

  const std::string cmd = client_bin + " --tls --ca-cert /dev/null --server-fingerprint 0000000000000000000000000000000000000000000000000000000000000000 127.0.0.1 4783 player 2>&1";
  FILE* pipe = ::popen(cmd.c_str(), "r");
  CHECK(pipe != nullptr);
  if (pipe != nullptr) {
    char buf[256];
    std::string output;
    while (::fgets(buf, sizeof(buf), pipe) != nullptr) {
      output += buf;
    }
    const int code = ::pclose(pipe);
    const int exit_status = WEXITSTATUS(code);
    CHECK(exit_status == 2); // Argument error exit code
    CHECK(output.find("mutually exclusive") != std::string::npos);
  }
}

// --------------------------------------------------------------------------
// Scenario 23: Truncated / unexpected TLS termination (EOF without close_notify)
// --------------------------------------------------------------------------
void test_scenario_23_truncated_tls_termination(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);
  if (client != nullptr) {
    // Send valid frame
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto resp = client->receive_frame();
    CHECK(resp.has_value());

    // Abruptly close TCP socket with SO_LINGER timeout 0 (RST packet, no TLS close_notify alert)
    linger sl{};
    sl.l_onoff = 1;
    sl.l_linger = 0;
    (void)setsockopt(client->fd, SOL_SOCKET, SO_LINGER, &sl, sizeof(sl));
    (void)::close(client->fd);
    client->fd = -1;
  }

  // Allow server reactor to process abrupt reset
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Connect healthy sibling client to ensure server remained completely responsive
  auto healthy = connect_tls_client(port, client_cfg);
  CHECK(healthy != nullptr);
  if (healthy != nullptr) {
    CHECK(healthy->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto r = healthy->receive_frame();
    CHECK(r.has_value());
  }

  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 24: Reactor state machine cross-direction WANT_READ/WANT_WRITE & KeyUpdate
// --------------------------------------------------------------------------
static bool g_seam_read_want_write = false;
static bool g_seam_write_want_read = false;

static int cross_direction_seam_write(BIO* b, const char* in, int inl) {
  BIO_clear_retry_flags(b);
  if (g_seam_write_want_read) {
    BIO_set_retry_read(b);
    return -1;
  }
  BIO* next = BIO_next(b);
  if (!next) return -1;
  int ret = BIO_write(next, in, inl);
  BIO_copy_next_retry(b);
  return ret;
}

static int cross_direction_seam_read(BIO* b, char* out, int outl) {
  BIO_clear_retry_flags(b);
  if (g_seam_read_want_write) {
    BIO_set_retry_write(b);
    return -1;
  }
  BIO* next = BIO_next(b);
  if (!next) return -1;
  int ret = BIO_read(next, out, outl);
  BIO_copy_next_retry(b);
  return ret;
}

static long cross_direction_seam_ctrl(BIO* b, int cmd, long num, void* ptr) {
  BIO* next = BIO_next(b);
  if (!next) return 0;
  return BIO_ctrl(next, cmd, num, ptr);
}

void test_transport_level_cross_direction_io(const darkpool::transport::EphemeralCertBundle& certs) {
  darkpool::transport::ServerTlsConfig s_cfg;
  s_cfg.cert_file = certs.server_cert_pem_path;
  s_cfg.key_file = certs.server_key_pem_path;
  std::string err;
  auto s_ctx = darkpool::transport::create_server_ssl_ctx(s_cfg, err);
  CHECK(s_ctx != nullptr);

  darkpool::transport::ClientTlsConfig c_cfg;
  c_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  c_cfg.ca_file = certs.ca_cert_pem_path;
  c_cfg.target_ip = "127.0.0.1";
  auto c_ctx = darkpool::transport::create_client_ssl_ctx(c_cfg, err);
  CHECK(c_ctx != nullptr);

  auto server_ssl = darkpool::transport::SslPtr(SSL_new(s_ctx.get()));
  auto client_ssl = darkpool::transport::SslPtr(SSL_new(c_ctx.get()));
  CHECK(server_ssl != nullptr && client_ssl != nullptr);

  SSL_set_app_data(client_ssl.get(), &c_cfg);

  // Setup BIO pairs with a test seam filter on server BIO to verify exact WantRead and WantWrite transitions
  BIO *s_in = nullptr, *s_net = nullptr;
  BIO_new_bio_pair(&s_in, 4096, &s_net, 4096);

  static BIO_METHOD* seam_method = nullptr;
  if (!seam_method) {
    seam_method = BIO_meth_new(BIO_TYPE_FILTER, "cross_direction_seam");
    BIO_meth_set_write(seam_method, cross_direction_seam_write);
    BIO_meth_set_read(seam_method, cross_direction_seam_read);
    BIO_meth_set_ctrl(seam_method, cross_direction_seam_ctrl);
  }
  BIO* seam_bio = BIO_new(seam_method);
  s_in = BIO_push(seam_bio, s_in);

  SSL_set_bio(server_ssl.get(), s_in, s_in);
  SSL_set_accept_state(server_ssl.get());

  BIO *c_in = nullptr, *c_net = nullptr;
  BIO_new_bio_pair(&c_in, 4096, &c_net, 4096);
  SSL_set_bio(client_ssl.get(), c_in, c_in);
  SSL_set_connect_state(client_ssl.get());

  auto pump = [](BIO* from, BIO* to) -> std::size_t {
    char buf[1024];
    std::size_t total = 0;
    while (true) {
      int r = BIO_read(from, buf, sizeof(buf));
      if (r <= 0) break;
      BIO_write(to, buf, r);
      total += static_cast<std::size_t>(r);
    }
    return total;
  };

  // Complete TLS handshake
  for (int i = 0; i < 20; ++i) {
    (void)SSL_do_handshake(client_ssl.get());
    pump(c_net, s_net);
    (void)SSL_do_handshake(server_ssl.get());
    pump(s_net, c_net);
    if (SSL_is_init_finished(client_ssl.get()) && SSL_is_init_finished(server_ssl.get())) {
      break;
    }
  }
  CHECK(SSL_is_init_finished(client_ssl.get()));
  CHECK(SSL_is_init_finished(server_ssl.get()));

  // 1. Ordinary WANT_READ when no network data is available
  char read_buf[256];
  std::size_t count = 0;
  auto st_read = darkpool::transport::tls_read(server_ssl.get(), read_buf, sizeof(read_buf), count, err);
  CHECK(st_read == darkpool::transport::TlsIoStatus::WantRead);

  // 2. Deterministic cross-direction: SSL_read returning WANT_WRITE
  // Client sends framed request bytes
  const std::string test_req = Frame(json{{"type", "cross_req"}, {"id", 101}}).serialize();
  std::size_t c_written = 0;
  auto st_cw = darkpool::transport::tls_write(client_ssl.get(), test_req.data(), test_req.size(), c_written, err);
  CHECK(st_cw == darkpool::transport::TlsIoStatus::Ok);
  CHECK(c_written == test_req.size());
  pump(c_net, s_net);

  // Force transport read seam to signal write readiness required (WantWrite)
  g_seam_read_want_write = true;
  st_read = darkpool::transport::tls_read(server_ssl.get(), read_buf, sizeof(read_buf), count, err);
  CHECK(st_read == darkpool::transport::TlsIoStatus::WantWrite);

  // Simulate reactor event handling for tls_read_want_write:
  // Write readiness arrives (seam cleared), reactor retries the EXACT pending read operation
  g_seam_read_want_write = false;
  st_read = darkpool::transport::tls_read(server_ssl.get(), read_buf, sizeof(read_buf), count, err);
  CHECK(st_read == darkpool::transport::TlsIoStatus::Ok);
  CHECK(count > 0);
  darkpool::network::FrameDecoder decoder;
  auto decoded_frames = decoder.feed(std::span<const char>(read_buf, count));
  CHECK(decoded_frames.size() == 1);
  CHECK(decoded_frames[0].payload().value("type", "") == "cross_req");
  CHECK(decoded_frames[0].payload().value("id", 0) == 101);

  // 3. Deterministic cross-direction: SSL_write returning WANT_READ
  // Server prepares framed response
  const std::string test_resp = Frame(json{{"type", "cross_resp"}, {"id", 101}}).serialize();
  std::size_t s_written = 0;

  // Force transport write seam to signal read readiness required (WantRead)
  g_seam_write_want_read = true;
  auto st_write = darkpool::transport::tls_write(server_ssl.get(), test_resp.data(), test_resp.size(), s_written, err);
  CHECK(st_write == darkpool::transport::TlsIoStatus::WantRead);

  // Simulate reactor event handling for tls_write_want_read:
  // Read readiness arrives (seam cleared), reactor retries the EXACT pending write operation
  g_seam_write_want_read = false;
  st_write = darkpool::transport::tls_write(server_ssl.get(), test_resp.data(), test_resp.size(), s_written, err);
  CHECK(st_write == darkpool::transport::TlsIoStatus::Ok);
  CHECK(s_written == test_resp.size());
  pump(s_net, c_net);

  // Client reads and verifies the response frame intact without loss or corruption
  char c_read_buf[256];
  std::size_t c_read_count = 0;
  auto st_cr = darkpool::transport::tls_read(client_ssl.get(), c_read_buf, sizeof(c_read_buf), c_read_count, err);
  CHECK(st_cr == darkpool::transport::TlsIoStatus::Ok);
  darkpool::network::FrameDecoder client_decoder;
  auto client_frames = client_decoder.feed(std::span<const char>(c_read_buf, c_read_count));
  CHECK(client_frames.size() == 1);
  CHECK(client_frames[0].payload().value("type", "") == "cross_resp");
  CHECK(client_frames[0].payload().value("id", 0) == 101);

  // Cleanup network BIOs
  BIO_free(s_net);
  BIO_free(c_net);
}

void test_reactor_dispatch_state_and_decisions() {
  using darkpool::transport::ConnectionDispatchState;
  using darkpool::transport::evaluate_reactor_dispatch;
  using darkpool::transport::compute_reactor_epoll_interest;

  // 1. Pending write requires read readiness (tls_write_want_read):
  // Reactor must resume write on EPOLLIN, suppress normal read and normal write,
  // and register interest for read ONLY (no busy-spin on EPOLLOUT).
  ConnectionDispatchState w_conn{};
  w_conn.is_tls = true;
  w_conn.tls_write_want_read = true;
  w_conn.tls_read_want_write = false;
  w_conn.peer_closed = false;
  w_conn.has_pending_output = true;

  auto w_interest = compute_reactor_epoll_interest(w_conn);
  CHECK(w_interest.want_read == true);
  CHECK(w_interest.want_write == false); // Guarantees zero spinloop on EPOLLOUT

  auto w_decisions = evaluate_reactor_dispatch(w_conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(w_decisions.do_resume_tls_write == true);
  CHECK(w_decisions.do_normal_read == false);
  CHECK(w_decisions.do_normal_write == false);
  CHECK(w_decisions.do_resume_tls_read == false);

  // 2. Pending read requires write readiness (tls_read_want_write):
  // Reactor must resume read on EPOLLOUT, suppress normal write and normal read,
  // and register interest for write ONLY.
  ConnectionDispatchState r_conn{};
  r_conn.is_tls = true;
  r_conn.tls_write_want_read = false;
  r_conn.tls_read_want_write = true;
  r_conn.peer_closed = false;
  r_conn.has_pending_output = false;

  auto r_interest = compute_reactor_epoll_interest(r_conn);
  CHECK(r_interest.want_read == false);
  CHECK(r_interest.want_write == true);

  auto r_decisions = evaluate_reactor_dispatch(r_conn, static_cast<std::uint32_t>(EPOLLOUT));
  CHECK(r_decisions.do_resume_tls_read == true);
  CHECK(r_decisions.do_normal_read == false);
  CHECK(r_decisions.do_normal_write == false);
  CHECK(r_decisions.do_resume_tls_write == false);

  // 3. Normal idle and busy interest registrations
  ConnectionDispatchState n_conn{};
  n_conn.is_tls = true;
  n_conn.has_pending_output = false;
  auto n_interest = compute_reactor_epoll_interest(n_conn);
  CHECK(n_interest.want_read == true);
  CHECK(n_interest.want_write == false);

  n_conn.has_pending_output = true;
  n_interest = compute_reactor_epoll_interest(n_conn);
  CHECK(n_interest.want_read == true);
  CHECK(n_interest.want_write == true);
}

void test_scenario_24_reactor_cross_direction_and_key_update(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  CHECK(certs != nullptr);

  // 1. Direct unit verification of production reactor dispatch decisions and epoll interest
  test_reactor_dispatch_state_and_decisions();

  // 2. Run transport-level deterministic verification of WantRead, WantWrite, and cross-direction retry
  test_transport_level_cross_direction_io(*certs);

  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);
  ServerSubprocessGuard server_guard{child};

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  // Integration smoke test: verify TLS 1.3 KeyUpdate on live reactor socket
  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);
  if (client != nullptr) {
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto resp1 = client->receive_frame();
    CHECK(resp1.has_value());

    // Trigger TLS 1.3 KeyUpdate on client SSL session with response requested
    const int ku_res = SSL_key_update(client->ssl.get(), SSL_KEY_UPDATE_REQUESTED);
    CHECK(ku_res == 1);

    // Send frame after KeyUpdate to trigger key change transmission and reactor cross-direction handling
    CHECK(client->send_frame(json{{"type", "status"}}));
    auto resp2 = client->receive_frame();
    CHECK(resp2.has_value());

    // Measure time bounded execution to prove reactor is not spinning in continuous EPOLLOUT loop
    const auto t_start = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) {
      CHECK(client->send_frame(json{{"type", "status"}}));
      auto r = client->receive_frame();
      CHECK(r.has_value());
      CHECK(r->value("type", "") == "error" || r->value("type", "") == "status");
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t_start).count();
    // 20 requests over local loopback TLS should execute within 1500 ms (no spinloop)
    CHECK(elapsed < 1500);
  }
}

// --------------------------------------------------------------------------
// Scenario 25: Persistent TLS FrameWriteCursor and Resumable Partial Writes
// --------------------------------------------------------------------------
void test_scenario_25_write_cursor_semantics(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);
  ServerSubprocessGuard server_guard{child};

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client != nullptr);

  // 1. Fully successful frame completes with exact byte count
  const json f1 = json{{"type", "connect"}, {"role", "player"}};
  const std::string s1 = Frame(f1).serialize();
  const auto res1 = client->send_frame_detailed(f1, 5000);
  CHECK(res1.status == FrameWriteStatus::Sent);
  CHECK(res1.bytes_written == s1.size());
  CHECK(!client->has_pending_frame());
  CHECK(client->pending_frame_remaining() == 0);
  auto r1 = client->receive_frame(2000);
  CHECK(r1.has_value());

  // 2. Test persistent write cursor under synthetic partial progress
  // Manually arm client's write_cursor to simulate a partially transmitted frame
  const json f2 = json{{"type", "status"}, {"padding", std::string(300, 'z')}};
  const std::string s2 = Frame(f2).serialize();
  client->write_cursor.frame_bytes = s2;
  client->write_cursor.offset = 0; // ready to transmit
  client->write_cursor.in_progress = true;

  CHECK(client->has_pending_frame());
  CHECK(client->pending_frame_offset() == 0);
  CHECK(client->pending_frame_remaining() == s2.size());

  // A call to send_frame_detailed with a DIFFERENT payload f3:
  // MUST NOT send f3, MUST resume and complete the in-progress frame f2 first!
  const json f3 = json{{"type", "should_not_interrupt_f2"}};
  const auto res2 = client->send_frame_detailed(f3, 5000);
  CHECK(res2.status == FrameWriteStatus::Sent);
  CHECK(res2.bytes_written == s2.size());
  CHECK(!client->has_pending_frame());
  auto r2 = client->receive_frame(2000);
  CHECK(r2.has_value());

  // 3. Test abandoned stream: fatal connection error rejects subsequent writes
  client->write_cursor.abandoned = true;
  const auto res_err = client->send_frame_detailed(f1, 100);
  CHECK(res_err.status == FrameWriteStatus::TransportError);
  CHECK(res_err.error_message.find("abandoned") != std::string::npos);
}

// --------------------------------------------------------------------------
// Scenario 26: TLS Push Notification Delivery & Hybrid Moderator Controls
// --------------------------------------------------------------------------
void test_scenario_26_tls_push_notifications_and_moderator_controls(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path, "mod-secret");
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  // 1. Moderator connects
  auto mod = connect_tls_client(port, client_cfg);
  CHECK(mod != nullptr);
  CHECK(mod->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-secret"}}));
  auto mod_conn_resp = mod->receive_frame(5000, true);
  CHECK(mod_conn_resp.has_value());

  // 2. Moderator creates match for 5 players
  CHECK(mod->send_frame(json{{"type", "create_match"}, {"player_count", 5}}));
  auto create_resp = mod->receive_frame(5000, true);
  CHECK(create_resp.has_value() && (*create_resp)["type"] == "match_created");
  const auto match_id = (*create_resp)["match_id"].get<std::uint64_t>();

  // 3. Connect 5 players
  std::vector<std::unique_ptr<TlsClientConnection>> players;
  players.reserve(5);
  for (std::size_t i = 0; i < 5; ++i) {
    auto p = connect_tls_client(port, client_cfg);
    CHECK(p != nullptr);
    CHECK(p->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto p_conn = p->receive_frame(5000, true);
    CHECK(p_conn.has_value());
    CHECK(p->send_frame(json{{"type", "join_match"}, {"match_id", match_id}}));
    auto join_resp = p->receive_frame(5000, true);
    CHECK(join_resp.has_value() && (*join_resp)["type"] == "joined_match");
    // Player also received initial market snapshot pushed over TLS!
    auto init_snap = p->receive_frame(5000, false);
    CHECK(init_snap.has_value() && (*init_snap)["type"] == "market_snapshot");
    players.push_back(std::move(p));
  }

  // 4. Moderator starts match
  CHECK(mod->send_frame(json{{"type", "start_match"}, {"match_id", match_id}}));
  auto start_resp = mod->receive_frame(5000, true);
  CHECK(start_resp.has_value() && (*start_resp)["ok"]);

  // Moderator receives match_started and market_snapshot push over TLS
  auto mod_ev = mod->receive_frame(5000, false);
  CHECK(mod_ev.has_value() && (*mod_ev)["type"] == "server_event" && (*mod_ev)["event"] == "match_started");
  auto mod_snap = mod->receive_frame(5000, false);
  CHECK(mod_snap.has_value() && (*mod_snap)["type"] == "market_snapshot");

  // All 5 players receive match_started and market_snapshot pushed over TLS
  for (auto& p : players) {
    auto p_ev = p->receive_frame(5000, false);
    CHECK(p_ev.has_value() && (*p_ev)["type"] == "server_event" && (*p_ev)["event"] == "match_started");
    auto p_sn = p->receive_frame(5000, false);
    CHECK(p_sn.has_value() && (*p_sn)["type"] == "market_snapshot");
  }

  // 5. Moderator publishes event (impact 1) -> enters Discussion
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}}));
  auto pub_resp = mod->receive_frame(5000, true);
  CHECK(pub_resp.has_value() && (*pub_resp)["ok"]);
  (void)mod->receive_frame(5000, false); // mod phase_started
  (void)mod->receive_frame(5000, false); // mod market_snapshot

  for (auto& p : players) {
    auto p_ev = p->receive_frame(5000, false);
    CHECK(p_ev.has_value() && (*p_ev)["type"] == "server_event" && (*p_ev)["event"] == "phase_started");
    CHECK((*p_ev)["phase"] == "Discussion");
    auto p_sn = p->receive_frame(5000, false);
    CHECK(p_sn.has_value() && (*p_sn)["type"] == "market_snapshot");
  }

  // 6. Test pause over TLS
  CHECK(mod->send_frame(json{{"type", "pause"}, {"match_id", match_id}}));
  auto pause_resp = mod->receive_frame(5000, true);
  CHECK(pause_resp.has_value() && (*pause_resp)["type"] == "pause_result" && (*pause_resp)["ok"]);
  (void)mod->receive_frame(5000, false); // mod match_paused
  (void)mod->receive_frame(5000, false); // mod market_snapshot

  for (auto& p : players) {
    auto p_ev = p->receive_frame(5000, false);
    CHECK(p_ev.has_value() && (*p_ev)["type"] == "server_event" && (*p_ev)["event"] == "match_paused");
    auto p_sn = p->receive_frame(5000, false);
    CHECK(p_sn.has_value() && (*p_sn)["type"] == "market_snapshot");
    CHECK((*p_sn)["is_paused"] == true);
  }

  // 7. Test resume over TLS
  CHECK(mod->send_frame(json{{"type", "resume"}, {"match_id", match_id}}));
  auto resume_resp = mod->receive_frame(5000, true);
  CHECK(resume_resp.has_value() && (*resume_resp)["type"] == "resume_result" && (*resume_resp)["ok"]);
  (void)mod->receive_frame(5000, false); // mod match_resumed
  (void)mod->receive_frame(5000, false); // mod market_snapshot

  for (auto& p : players) {
    auto p_ev = p->receive_frame(5000, false);
    CHECK(p_ev.has_value() && (*p_ev)["type"] == "server_event" && (*p_ev)["event"] == "match_resumed");
    auto p_sn = p->receive_frame(5000, false);
    CHECK(p_sn.has_value() && (*p_sn)["type"] == "market_snapshot");
    CHECK((*p_sn)["is_paused"] == false);
  }

  // 8. Test abort over TLS
  CHECK(mod->send_frame(json{{"type", "abort"}, {"match_id", match_id}}));
  auto abort_resp = mod->receive_frame(5000, true);
  CHECK(abort_resp.has_value() && (*abort_resp)["type"] == "abort_result" && (*abort_resp)["ok"]);
  (void)mod->receive_frame(5000, false); // mod match_aborted
  (void)mod->receive_frame(5000, false); // mod market_snapshot

  for (auto& p : players) {
    auto p_ev = p->receive_frame(5000, false);
    CHECK(p_ev.has_value() && (*p_ev)["type"] == "server_event" && (*p_ev)["event"] == "match_aborted");
    CHECK((*p_ev)["phase"] == "Finished");
    CHECK((*p_ev)["outcome"] == "Aborted");
    auto p_sn = p->receive_frame(5000, false);
    CHECK(p_sn.has_value() && (*p_sn)["type"] == "market_snapshot");
    CHECK((*p_sn)["phase"] == "Finished");
    CHECK((*p_sn)["outcome"] == "Aborted");
  }

  stop_server(child);
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "usage: darkpool_tls_integration_tests <path_to_darkpool_server> [subtest]\n";
    return 2;
  }
  const char* server_path = argv[1];
  const std::string subtest = (argc >= 3) ? argv[2] : "all";

  std::cout << "Running DARKPOOL S9 TLS 1.3 Integration Tests (subtest: " << subtest << ")...\n" << std::flush;

  auto maybe_run = [&](int id, const char* key, const char* name, auto&& fn) {
    if (subtest == "all" || subtest == key) {
      run_scenario(id, key, name, [&]() { fn(server_path); });
    }
  };

  maybe_run(1, "valid-ca", "Valid TLS 1.3 handshake with trusted local CA and matching IP SAN", test_scenario_01_valid_ca);
  maybe_run(2, "valid-fingerprint", "Valid TLS 1.3 handshake in fingerprint mode with self-signed leaf", test_scenario_02_valid_fingerprint);
  maybe_run(3, "untrusted-ca", "Rejection of untrusted CA root", test_scenario_03_untrusted_ca);
  maybe_run(4, "incorrect-fingerprint", "Rejection of incorrect leaf fingerprint", test_scenario_04_incorrect_fingerprint);
  maybe_run(5, "mismatched-san", "Rejection of mismatched IP SAN", test_scenario_05_mismatched_ip_san);
  maybe_run(6, "expired-cert", "Rejection of expired certificate", test_scenario_06_expired_cert);
  maybe_run(7, "reject-tls12", "Rejection of TLS 1.2 and earlier protocols", test_scenario_07_reject_tls12);
  maybe_run(8, "reject-plaintext", "Rejection of plaintext traffic on TLS port", test_scenario_08_reject_plaintext_on_tls_port);
  maybe_run(9, "handshake-stall", "Handshake stall timeout and cleanup", test_scenario_09_handshake_stall_timeout);
  maybe_run(10, "coalesced-frames", "Fragmented TLS records and coalesced application frames", test_scenario_10_coalesced_frames);
  maybe_run(11, "slow-reader", "Slow-reader output queue bounding and concurrent match progression", test_scenario_11_slow_reader_bounds);
  maybe_run(12, "flood-limiting", "Flood rate limiting over TLS with state-mutation verification", test_scenario_12_flood_rate_limiting);
  maybe_run(13, "secret-redaction", "Secret redaction in server and client diagnostics", test_scenario_13_secret_redaction);
  maybe_run(14, "gameplay", "Complete 5-player match gameplay over TLS", test_scenario_14_complete_match_gameplay);
  maybe_run(15, "isolation", "Multiple TLS clients and concurrent match isolation", test_scenario_15_concurrent_match_isolation);
  maybe_run(16, "cleanup", "Disconnect and shutdown cleanup", test_scenario_16_disconnect_cleanup);
  maybe_run(17, "ca-leaf-fingerprint", "Valid TLS 1.3 in fingerprint mode with CA-signed leaf", test_scenario_17_ca_signed_leaf_fingerprint_mode);
  maybe_run(18, "not-yet-valid", "Fingerprint mode with not-yet-valid certificate rejection", test_scenario_18_not_yet_valid_cert_rejection);
  maybe_run(19, "invalid-eku", "Invalid server certificate usage / EKU rejection", test_scenario_19_invalid_server_eku_rejection);
  maybe_run(20, "cert-rotation", "Certificate rotation and pin update", test_scenario_20_cert_rotation_pin_update);
  maybe_run(21, "key-permissions", "Strict private key file permissions", test_scenario_21_strict_key_permissions);
  maybe_run(22, "cli-exclusivity", "Mutually exclusive CLI verification options", test_scenario_22_cli_mutual_exclusivity);
  maybe_run(23, "unexpected-eof", "Truncated / unexpected TLS termination (EOF without close_notify)", test_scenario_23_truncated_tls_termination);
  maybe_run(24, "cross-direction", "Reactor state machine cross-direction readiness & KeyUpdate", test_scenario_24_reactor_cross_direction_and_key_update);
  maybe_run(25, "write-cursor", "Persistent TLS FrameWriteCursor and resumable partial writes", test_scenario_25_write_cursor_semantics);
  maybe_run(26, "push-notifications", "Push notification delivery and moderator controls over TLS", test_scenario_26_tls_push_notifications_and_moderator_controls);

  if (g_failures.load() > 0) {
    std::cerr << "FAIL: " << g_failures.load() << " assertions failed\n" << std::flush;
    return 1;
  }
  std::cout << "PASS: All S9 TLS 1.3 integration tests passed cleanly!\n" << std::flush;
  return 0;
}

#endif // defined(__linux__)
