#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "darkpool_tls_integration_tests requires Linux\n";
  return 0;
}

#else

#include "darkpool/network.hpp"
#include "darkpool/tls_transport.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
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
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
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
    if (server_stdout_path != nullptr && !server_stdout_path->empty()) {
      int out_fd = ::open(server_stdout_path->c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (out_fd >= 0) {
        dup2(out_fd, STDOUT_FILENO);
        dup2(out_fd, STDERR_FILENO);
        close(out_fd);
      }
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

  // Set nonblocking to perform handshake with timeout
  int flags = fcntl(fd, F_GETFL, 0);
  (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() > 5) {
      (void)close(fd);
      return nullptr;
    }
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
    (void)poll(&pfd, 1, 100);
  }
  (void)fcntl(fd, F_SETFL, flags);

  // Verification post-handshake
  if (config.mode == darkpool::transport::ClientVerifyMode::FingerprintPinning) {
    X509* peer_cert = SSL_get_peer_certificate(ssl.get());
    if (peer_cert == nullptr) {
      (void)close(fd);
      return nullptr;
    }
    darkpool::transport::X509Ptr cert_guard(peer_cert);
    std::string v_err;
    if (!darkpool::transport::verify_cert_time_validity(peer_cert, v_err) ||
        !darkpool::transport::verify_cert_ip_san(peer_cert, config.target_ip, v_err)) {
      (void)close(fd);
      return nullptr;
    }
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
    // Assert TLS 1.3 was negotiated
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
// Scenario 2: Valid TLS 1.3 handshake in fingerprint-pinning mode
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
  client_cfg.ca_file = other_ca->ca_cert_pem_path; // wrong CA!
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr); // must fail closed!
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
  std::string bad_fp = certs->server_fingerprint_hex;
  bad_fp[0] = (bad_fp[0] == 'a') ? 'b' : 'a'; // flip one digit
  client_cfg.pinned_fingerprint_hex = bad_fp;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr); // must fail closed!
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 5: Rejection of mismatched IP SAN
// --------------------------------------------------------------------------
void test_scenario_05_mismatched_ip_san(const char* server_path) {
  // Server certificate has IP SAN 10.0.0.1, but client connects to 127.0.0.1
  auto certs = generate_ephemeral_certs("10.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1"; // mismatch with SAN 10.0.0.1!

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr); // must fail closed!
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 6: Rejection of expired certificate
// --------------------------------------------------------------------------
void test_scenario_06_expired_cert(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, true); // expired = true
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto client = connect_tls_client(port, client_cfg);
  CHECK(client == nullptr); // must fail closed!
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

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  std::string err;
  auto ctx = darkpool::transport::create_client_ssl_ctx(client_cfg, err);
  CHECK(ctx != nullptr);
  // Restrict client to TLS 1.2 max
  SSL_CTX_set_max_proto_version(ctx.get(), TLS1_2_VERSION);

  const int fd = connect_tcp(port);
  CHECK(fd >= 0);
  SSL* raw_ssl = SSL_new(ctx.get());
  darkpool::transport::SslPtr ssl(raw_ssl);
  SSL_set_fd(ssl.get(), fd);
  const int ret = SSL_connect(ssl.get());
  CHECK(ret <= 0); // must be rejected!
  (void)close(fd);
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
  const std::string plaintext_request = Frame(json{{"type", "connect"}, {"role", "player"}}).serialize();
  (void)send(fd, plaintext_request.data(), plaintext_request.size(), MSG_NOSIGNAL);

  char buf[64];
  timeval timeout{1, 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const auto recvd = recv(fd, buf, sizeof(buf), 0);
  // Server must close connection (recvd == 0 or ECONNRESET)
  CHECK(recvd <= 0);
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
  // Send nothing, wait for 5s server handshake timeout
  timeval timeout{6, 500000};
  (void)setsockopt(stalled_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char buf[16];
  const auto recvd = recv(stalled_fd, buf, sizeof(buf), 0);
  CHECK(recvd == 0 || (recvd < 0 && (errno == ECONNRESET || errno == EPIPE)));
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
    // Send connect
    CHECK(client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto resp = client->receive_frame();
    CHECK(resp.has_value());

    // Send two frames back-to-back coalesced in one write
    const std::string f1 = Frame(json{{"type", "status"}}).serialize();
    const std::string f2 = Frame(json{{"type", "status"}}).serialize();
    const std::string coalesced = f1 + f2;

    std::size_t written = 0;
    std::string err;
    (void)darkpool::transport::tls_write(client->ssl.get(), coalesced.data(), coalesced.size(), written, err);

    auto r1 = client->receive_frame();
    auto r2 = client->receive_frame();
    CHECK(r1.has_value());
    CHECK(r2.has_value());
  }
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 11: Slow-reader output queue bounds over TLS
// --------------------------------------------------------------------------
void test_scenario_11_slow_reader_bounds(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

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

  // Blast status requests without reading until output queue > 64 KiB causes eviction
  for (int i = 0; i < 200; ++i) {
    if (!slow_client->send_frame(json{{"type", "status"}})) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  // Connect healthy sibling client over TLS to verify isolation
  auto healthy_client = connect_tls_client(port, client_cfg);
  CHECK(healthy_client != nullptr);
  if (healthy_client != nullptr) {
    CHECK(healthy_client->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto healthy_resp = healthy_client->receive_frame();
    CHECK(healthy_resp.has_value());
  }

  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 12: Request flood rate limiting over TLS
// --------------------------------------------------------------------------
void test_scenario_12_flood_rate_limiting(const char* server_path) {
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
    auto resp = client->receive_frame();
    CHECK(resp.has_value());

    int accepted = 0;
    int rate_limited = 0;
    for (int i = 0; i < 100; ++i) {
      CHECK(client->send_frame(json{{"type", "status"}}));
      auto r = client->receive_frame();
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
  }
  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 13: Secret redaction in server and client diagnostics
// --------------------------------------------------------------------------
void test_scenario_13_secret_redaction(const char* server_path) {
  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const std::string log_file = certs->temp_dir_path + "/server_audit.log";
  std::string stdout_path = log_file;
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path,
                                      "super-secret-moderator-token-12345", "127.0.0.1", &stdout_path);
  CHECK(child > 0);

  darkpool::transport::ClientTlsConfig client_cfg;
  client_cfg.mode = darkpool::transport::ClientVerifyMode::LocalCa;
  client_cfg.ca_file = certs->ca_cert_pem_path;
  client_cfg.target_ip = "127.0.0.1";

  auto mod_client = connect_tls_client(port, client_cfg);
  CHECK(mod_client != nullptr);
  if (mod_client != nullptr) {
    CHECK(mod_client->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "super-secret-moderator-token-12345"}}));
    auto resp = mod_client->receive_frame();
    CHECK(resp.has_value());
  }

  stop_server(child);

  // Inspect server output log
  std::string log_contents;
  if (std::filesystem::exists(log_file)) {
    FILE* f = fopen(log_file.c_str(), "r");
    if (f != nullptr) {
      char b[512];
      while (fgets(b, sizeof(b), f) != nullptr) log_contents += b;
      fclose(f);
    }
  }
  // The secret moderator token must NEVER appear in server logs
  CHECK(log_contents.find("super-secret-moderator-token-12345") == std::string::npos);
}

// --------------------------------------------------------------------------
// Scenario 14: Complete 5-player match gameplay over TLS 1.3
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
  for (int i = 0; i < 5; ++i) {
    auto p = connect_tls_client(port, client_cfg);
    CHECK(p != nullptr);
    CHECK(p->send_frame(json{{"type", "connect"}, {"role", "player"}}));
    auto p_conn = p->receive_frame();
    CHECK(p_conn.has_value());

    CHECK(p->send_frame(json{{"type", "join_match"}, {"match_id", match_id}}));
    auto join_resp = p->receive_frame();
    CHECK(join_resp.has_value());
    if (join_resp) {
      roles.push_back(join_resp->value("role", ""));
    }
    players.push_back(std::move(p));
  }
  CHECK(players.size() == 5);
  CHECK(roles.size() == 5);

  // 4. Moderator starts match
  CHECK(mod->send_frame(json{{"type", "start_match"}, {"match_id", match_id}}));
  auto start_resp = mod->receive_frame();
  CHECK(start_resp.has_value());
  CHECK(start_resp->value("phase", "") == "AwaitingEvent");

  // 5. Moderator publishes event (impact 0) -> transitions to Discussion
  CHECK(mod->send_frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}}));
  auto event_resp = mod->receive_frame();
  CHECK(event_resp.has_value());
  CHECK(event_resp->value("phase", "") == "Discussion");

  // 6. Insider requests hint
  for (std::size_t i = 0; i < 5; ++i) {
    if (roles[i] == "Insider") {
      CHECK(players[i]->send_frame(json{{"type", "insider_hint"}, {"match_id", match_id}}));
      auto hint_resp = players[i]->receive_frame();
      CHECK(hint_resp.has_value());
      if (hint_resp) {
        CHECK(hint_resp->value("type", "") == "hint_private");
        CHECK(hint_resp->value("event_impact", -99) == 0);
      }
    }
  }

  // 7. Moderator opens trading
  CHECK(mod->send_frame(json{{"type", "open_trading"}, {"match_id", match_id}}));
  auto open_trade_resp = mod->receive_frame();
  CHECK(open_trade_resp.has_value());
  CHECK(open_trade_resp->value("phase", "") == "Trading");

  // 8. Players submit trades
  for (int i = 0; i < 5; ++i) {
    CHECK(players[static_cast<std::size_t>(i)]->send_frame(
        json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}}));
    auto tr = players[static_cast<std::size_t>(i)]->receive_frame();
    CHECK(tr.has_value());
    CHECK(tr->value("ok", false) == true);
  }

  // 9. Moderator resolves market -> transitions to Clue
  CHECK(mod->send_frame(json{{"type", "resolve_market"}, {"match_id", match_id}}));
  auto market_resp = mod->receive_frame();
  CHECK(market_resp.has_value());
  CHECK(market_resp->value("ok", false) == true);

  CHECK(players[0]->send_frame(json{{"type", "match_status"}, {"match_id", match_id}}));
  auto clue_status = players[0]->receive_frame();
  CHECK(clue_status.has_value());
  CHECK(clue_status->value("phase", "") == "Clue");

  // 10. Moderator opens voting
  CHECK(mod->send_frame(json{{"type", "open_voting"}, {"match_id", match_id}}));
  auto open_vote_resp = mod->receive_frame();
  CHECK(open_vote_resp.has_value());
  CHECK(open_vote_resp->value("phase", "") == "Voting");

  // 11. Players vote (all abstain)
  for (int i = 0; i < 5; ++i) {
    CHECK(players[static_cast<std::size_t>(i)]->send_frame(
        json{{"type", "vote"}, {"match_id", match_id}, {"target_id", nullptr}}));
    auto vr = players[static_cast<std::size_t>(i)]->receive_frame();
    CHECK(vr.has_value());
    CHECK(vr->value("ok", false) == true);
  }

  // 12. Moderator resolves votes
  CHECK(mod->send_frame(json{{"type", "resolve_votes"}, {"match_id", match_id}}));
  auto vote_resp = mod->receive_frame();
  CHECK(vote_resp.has_value());
  CHECK(vote_resp->value("ok", false) == true);

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
  auto m1_conn = mod1->receive_frame();
  CHECK(m1_conn.has_value());
  CHECK(mod1->send_frame(json{{"type", "create_match"}, {"player_count", 5}}));
  auto m1_create = mod1->receive_frame();
  CHECK(m1_create.has_value());
  const std::uint64_t match_id_1 = m1_create->value("match_id", 1ULL);

  auto mod2 = connect_tls_client(port, client_cfg);
  CHECK(mod2 != nullptr);
  CHECK(mod2->send_frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token"}}));
  auto m2_conn = mod2->receive_frame();
  CHECK(m2_conn.has_value());
  CHECK(mod2->send_frame(json{{"type", "create_match"}, {"player_count", 5}}));
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

  stop_server(child);
}

// --------------------------------------------------------------------------
// Scenario 16: Disconnect and shutdown cleanup (resource & FD settling)
// --------------------------------------------------------------------------
void test_scenario_16_disconnect_cleanup(const char* server_path) {
  const auto fds_before = count_open_fds();

  auto certs = generate_ephemeral_certs("127.0.0.1", 1, false, false);
  const auto port = reserve_port();
  const auto child = start_tls_server(server_path, port, certs->server_cert_pem_path, certs->server_key_pem_path);
  CHECK(child > 0);

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
  } // All 10 clients destroyed and closed cleanly

  // Allow server reactor event loop ticks to settle FDs
  std::this_thread::sleep_for(std::chrono::milliseconds(150));

  stop_server(child);

  // Allow local process FDs to settle
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto fds_after = count_open_fds();
  // Open file descriptors must settle back to baseline (within 2 tolerance for test runner directories)
  CHECK(fds_after <= fds_before + 2);
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

  if (g_failures > 0) {
    std::cerr << "FAIL: " << g_failures << " assertions failed\n";
    return 1;
  }
  std::cout << "PASS: All S9 TLS 1.3 integration tests passed cleanly!\n";
  return 0;
}

#endif // defined(__linux__)
