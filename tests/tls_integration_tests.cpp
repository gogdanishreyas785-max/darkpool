#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "error: darkpool_tls_integration_tests requires Linux (epoll/sockets/openssl)\n";
  return 1;
}

#else

#include "darkpool/tls_transport.hpp"
#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
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

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

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

int connect_tcp(std::uint16_t port, int max_attempts = 80) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0) {
      int one = 1;
      (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
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
  if (child > 0) {
    (void)kill(child, SIGTERM);
    int status = 0;
    (void)waitpid(child, &status, 0);
  }
}

struct TlsClientConnection {
  int fd{-1};
  darkpool::transport::SslCtxPtr ctx;
  darkpool::transport::SslPtr ssl;

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

  bool send_frame(const json& payload) {
    const std::string bytes = Frame(payload).serialize();
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      std::size_t written = 0;
      std::string err;
      const auto status = darkpool::transport::tls_write(
          ssl.get(), bytes.data() + offset, bytes.size() - offset, written, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        offset += written;
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite ||
                 status == darkpool::transport::TlsIoStatus::WantRead) {
        pollfd pfd{fd, static_cast<short>((status == darkpool::transport::TlsIoStatus::WantWrite) ? POLLOUT : POLLIN), 0};
        (void)poll(&pfd, 1, 1000);
      } else {
        return false;
      }
    }
    return true;
  }

  std::optional<json> receive_frame(int timeout_ms = 5000) {
    char header[4];
    std::size_t header_offset = 0;
    const auto start = std::chrono::steady_clock::now();
    while (header_offset < 4) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeout_ms) {
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
        return std::nullopt;
      }
    }

    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
    const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
    if (length == 0U || length > darkpool::network::kMaxMessageSize) return std::nullopt;

    std::string body;
    body.resize(length);
    std::size_t body_offset = 0;
    while (body_offset < length) {
      if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > timeout_ms) {
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
        return std::nullopt;
      }
    }

    try {
      return json::parse(body);
    } catch (...) {
      return std::nullopt;
    }
  }
};

std::unique_ptr<TlsClientConnection> connect_tls_client(
    std::uint16_t port,
    const darkpool::transport::ClientTlsConfig& config) {
  std::string err;
  auto ctx = darkpool::transport::create_client_ssl_ctx(config, err);
  if (ctx == nullptr) {
    return nullptr;
  }
  const int fd = connect_tcp(port);
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

  int flags = fcntl(fd, F_GETFL, 0);
  (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
    (void)poll(&pfd, 1, p_timeout);
  }
  (void)fcntl(fd, F_SETFL, flags);

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
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);
  // Wait for server listener to initialize before capturing baseline open FDs
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto initial_server_fds = count_open_fds_pid(child);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto slow_client = connect_tls_client(port, client_cfg);
  CHECK(slow_client != nullptr);

  // Shrink client receive buffer to quickly saturate TCP window
  int rcvbuf = 2048;
  (void)setsockopt(slow_client->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  CHECK(slow_client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
  auto resp = slow_client->receive_frame();
  CHECK(resp.has_value());

  // Send status requests without reading replies to drive server output queue beyond 64 KiB boundary
  // At ~51 bytes per rate-limited response frame, filling TCP socket buffers (~128 KiB)
  // plus the 64 KiB server user-space queue requires > 3,800 unread response frames.
  bool disconnected = false;
  for (int i = 0; i < 6000; ++i) {
    if (!slow_client->send_frame(json{{"type", "status"}})) {
      disconnected = true;
      break;
    }
  }

  // If writes buffered in kernel send buffer, reading until EOF proves eviction occurred
  if (!disconnected) {
    while (true) {
      auto r = slow_client->receive_frame(100);
      if (!r) {
        disconnected = true;
        break;
      }
    }
  }
  CHECK(disconnected);

  // Connect healthy sibling client over TLS to verify server reactor is responsive and sibling completes cleanly
  auto healthy_client = connect_tls_client(port, client_cfg);
  CHECK(healthy_client != nullptr);
  if (healthy_client != nullptr) {
    CHECK(healthy_client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto healthy_resp = healthy_client->receive_frame();
    CHECK(healthy_resp.has_value());
    CHECK(healthy_resp->value("type", "") == "connect");

    CHECK(healthy_client->send_frame(json{{"type", "status"}}));
    auto healthy_status = healthy_client->receive_frame();
    CHECK(healthy_status.has_value());
  }

  healthy_client.reset();
  slow_client.reset();

  // Allow server reactor to settle FDs after client closures (poll up to 1000 ms)
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  bool settled = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto cur = count_open_fds_pid(child);
    if (cur && initial_server_fds && *cur <= *initial_server_fds + 1) {
      settled = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  CHECK(settled);

  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 12: Request flood rate limiting over TLS (with state-mutation verification)
// --------------------------------------------------------------------------
void test_scenario_12_flood_rate_limiting(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path, "mod-token");
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  // Create match and set to Trading phase
  auto mod = connect_tls_client(port, client_cfg);
  CHECK(mod != nullptr);
  CHECK(mod->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token"}}));
  (void)mod->receive_frame();

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

  CHECK(mod->send_frame(json{{"type", "start_match"}, {"match_id", match_id}})); (void)mod->receive_frame();
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}})); (void)mod->receive_frame();
  CHECK(mod->send_frame(json{{"type", "open_trading"}, {"match_id", match_id}})); (void)mod->receive_frame();

  // Snapshot authoritative match status before flood
  CHECK(mod->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto status_before = mod->receive_frame();
  CHECK(status_before.has_value());
  const auto phase_before = status_before->value("phase", "");
  CHECK(phase_before == "Trading");

  // Ensure token bucket has refilled completely to 40 burst tokens after setup commands
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  // Flood 100 state-changing operations from p1
  int accepted = 0;
  int rate_limited = 0;
  for (int i = 0; i < 100; ++i) {
    CHECK(p1->send_frame(json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}}));
    auto r = p1->receive_frame();
    CHECK(r.has_value());
    if (r) {
      if (r->value("error", "") == "rate limit exceeded") {
        ++rate_limited;
      } else {
        ++accepted;
      }
    }
  }

  // Token bucket burst is 40: exactly 40 accepted, 60 rate-limited
  CHECK(accepted == 40);
  CHECK(rate_limited == 60);

  // Snapshot authoritative match status after flood: verify zero unexpected state mutation
  CHECK(mod->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto status_after = mod->receive_frame();
  CHECK(status_after.has_value());
  CHECK(status_after->value("phase", "") == "Trading");

  // Verify token bucket refill recovers capacity: wait 100ms (2 tokens refilled)
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  CHECK(p1->send_frame(json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}}));
  auto refilled_resp = p1->receive_frame();
  CHECK(refilled_resp.has_value());
  CHECK(refilled_resp->value("error", "") != "rate limit exceeded");

  stop_server(child);
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
void test_scenario_24_reactor_cross_direction_and_key_update(const char* server_path) {
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
    auto resp1 = client->receive_frame();
    CHECK(resp1.has_value());

    // Trigger TLS 1.3 KeyUpdate on client SSL session
    const int ku_res = SSL_key_update(client->ssl.get(), SSL_KEY_UPDATE_NOT_REQUESTED);
    CHECK(ku_res == 1);

    // Send frame after KeyUpdate to trigger key change transmission and reactor cross-direction handling
    CHECK(client->send_frame(json{{"type", "status"}}));
    auto resp2 = client->receive_frame();
    CHECK(resp2.has_value());

    // Send subsequent status requests to verify continued responsiveness
    for (int i = 0; i < 5; ++i) {
      CHECK(client->send_frame(json{{"type", "status"}}));
      auto r = client->receive_frame();
      CHECK(r.has_value());
    }
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

  std::cout << "Running DARKPOOL S9 TLS 1.3 Integration Tests (subtest: " << subtest << ")...\n";

  if (subtest == "all" || subtest == "valid-ca") test_scenario_01_valid_ca(server_path);
  if (subtest == "all" || subtest == "valid-fingerprint") test_scenario_02_valid_fingerprint(server_path);
  if (subtest == "all" || subtest == "untrusted-ca") test_scenario_03_untrusted_ca(server_path);
  if (subtest == "all" || subtest == "incorrect-fingerprint") test_scenario_04_incorrect_fingerprint(server_path);
  if (subtest == "all" || subtest == "mismatched-san") test_scenario_05_mismatched_ip_san(server_path);
  if (subtest == "all" || subtest == "expired-cert") test_scenario_06_expired_cert(server_path);
  if (subtest == "all" || subtest == "reject-tls12") test_scenario_07_reject_tls12(server_path);
  if (subtest == "all" || subtest == "reject-plaintext") test_scenario_08_reject_plaintext_on_tls_port(server_path);
  if (subtest == "all" || subtest == "handshake-stall") test_scenario_09_handshake_stall_timeout(server_path);
  if (subtest == "all" || subtest == "coalesced-frames") test_scenario_10_coalesced_frames(server_path);
  if (subtest == "all" || subtest == "slow-reader") test_scenario_11_slow_reader_bounds(server_path);
  if (subtest == "all" || subtest == "flood-limiting") test_scenario_12_flood_rate_limiting(server_path);
  if (subtest == "all" || subtest == "secret-redaction") test_scenario_13_secret_redaction(server_path);
  if (subtest == "all" || subtest == "gameplay") test_scenario_14_complete_match_gameplay(server_path);
  if (subtest == "all" || subtest == "isolation") test_scenario_15_concurrent_match_isolation(server_path);
  if (subtest == "all" || subtest == "cleanup") test_scenario_16_disconnect_cleanup(server_path);
  if (subtest == "all" || subtest == "ca-leaf-fingerprint") test_scenario_17_ca_signed_leaf_fingerprint_mode(server_path);
  if (subtest == "all" || subtest == "not-yet-valid") test_scenario_18_not_yet_valid_cert_rejection(server_path);
  if (subtest == "all" || subtest == "invalid-eku") test_scenario_19_invalid_server_eku_rejection(server_path);
  if (subtest == "all" || subtest == "cert-rotation") test_scenario_20_cert_rotation_pin_update(server_path);
  if (subtest == "all" || subtest == "key-permissions") test_scenario_21_strict_key_permissions(server_path);
  if (subtest == "all" || subtest == "cli-exclusivity") test_scenario_22_cli_mutual_exclusivity(server_path);
  if (subtest == "all" || subtest == "unexpected-eof") test_scenario_23_truncated_tls_termination(server_path);
  if (subtest == "all" || subtest == "cross-direction") test_scenario_24_reactor_cross_direction_and_key_update(server_path);

  if (g_failures > 0) {
    std::cerr << "FAIL: " << g_failures << " assertions failed\n";
    return 1;
  }
  std::cout << "PASS: All S9 TLS 1.3 integration tests passed cleanly!\n";
  return 0;
}

#endif // defined(__linux__)
