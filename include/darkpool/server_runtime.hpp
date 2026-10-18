#pragma once

#include "darkpool/network.hpp"
#include "darkpool/socket_primitives.hpp"

#if defined(__linux__)
#include "darkpool/reactor_dispatch.hpp"
#include "darkpool/tls_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace darkpool::network {

struct TokenBucketRateLimiter {
  static constexpr double kRate = 20.0;     // 20 requests per second (DEC-11)
  static constexpr double kBurst = 40.0;    // burst capacity 40 (DEC-11)
  double tokens{kBurst};
  std::chrono::steady_clock::time_point last_refill{std::chrono::steady_clock::now()};

  bool try_acquire() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(now - last_refill).count();
    last_refill = now;
    tokens = std::min(kBurst, tokens + elapsed * kRate);
    if (tokens >= 1.0) {
      tokens -= 1.0;
      return true;
    }
    return false;
  }
};

struct ServerRuntimeConfig {
  bool enable_tcp{false};
  std::uint16_t tcp_port{4782};
  std::string bind_address{"127.0.0.1"};

  bool enable_unix{false};
  std::string unix_path;

  bool enable_tls{false};
  std::uint16_t tls_port{4783};
  std::string tls_bind_address{"127.0.0.1"};
  std::string tls_cert_path;
  std::string tls_key_path;

  std::size_t max_connections{36};
  std::chrono::milliseconds handshake_timeout{5000};
  int epoll_timeout_ms{100};
  bool enable_rate_limiting{true};
  bool handle_signals{true};
};

using MessageHandler = std::function<bool(std::uint64_t session_id, const Frame& frame)>;
using DisconnectHandler = std::function<void(std::uint64_t session_id)>;
using TickHandler = std::function<void()>;
using ConnectHandler = std::function<void(std::uint64_t session_id)>;

class ServerRuntime {
 public:
  explicit ServerRuntime(ServerRuntimeConfig config);
  ~ServerRuntime();

  ServerRuntime(const ServerRuntime&) = delete;
  ServerRuntime& operator=(const ServerRuntime&) = delete;
  ServerRuntime(ServerRuntime&&) noexcept;
  ServerRuntime& operator=(ServerRuntime&&) noexcept;

  void set_message_handler(MessageHandler handler) { message_handler_ = std::move(handler); }
  void set_disconnect_handler(DisconnectHandler handler) { disconnect_handler_ = std::move(handler); }
  void set_tick_handler(TickHandler handler) { tick_handler_ = std::move(handler); }
  void set_connect_handler(ConnectHandler handler) { connect_handler_ = std::move(handler); }

  bool start(std::string& error_msg);
  int run();
  bool step(int timeout_ms = -1);
  void stop() noexcept;
  [[nodiscard]] bool is_running() const noexcept { return is_running_; }

  bool send_message(std::uint64_t session_id, const Frame& frame);
  void send_multicast(std::span<const std::uint64_t> session_ids, const Frame& frame);
  void send_broadcast(const Frame& frame);
  void close_session(std::uint64_t session_id);

  void set_authenticated(std::uint64_t session_id, bool auth = true);
  [[nodiscard]] bool is_authenticated(std::uint64_t session_id) const;
  [[nodiscard]] bool try_acquire_rate_limit(std::uint64_t session_id);
  [[nodiscard]] std::size_t connection_count() const noexcept;
  [[nodiscard]] bool has_session(std::uint64_t session_id) const noexcept;

 private:
  enum class TransportType { Plaintext, Tls };

  struct Connection {
    std::uint64_t session_id{0};
    int fd{-1};
    FrameDecoder decoder;
    OutputQueue output;
    TokenBucketRateLimiter rate_limiter;
    bool is_authenticated{false};
    bool peer_closed{false};
    std::chrono::steady_clock::time_point connected_at{std::chrono::steady_clock::now()};

    TransportType transport{TransportType::Plaintext};
    darkpool::transport::SslPtr ssl{nullptr};
    bool tls_handshake_done{true};
    bool tls_handshake_want_read{false};
    bool tls_handshake_want_write{false};

    bool tls_read_want_read{false};
    bool tls_read_want_write{false};

    bool tls_write_want_read{false};
    bool tls_write_want_write{false};
  };

  [[nodiscard]] static darkpool::transport::ConnectionDispatchState to_dispatch_state(const Connection& conn) noexcept;
  bool update_connection_events(int fd, Connection& conn);
  bool set_epoll_events(int fd, bool reading, bool writing);
  bool enqueue_frame(int fd, Connection& conn, const Frame& frame);
  void drop_connection(int fd);
  bool handle_client_read(int fd, Connection& conn);
  bool handle_client_write(int fd, Connection& conn);
  void accept_pending_clients(int listener_fd, bool is_tls);

  ServerRuntimeConfig config_;
  int epoll_fd_{-1};
  int tcp_listener_{-1};
  int tls_listener_{-1};
  int unix_listener_{-1};
  int signal_fd_{-1};
  SocketFileGuard socket_guard_;
  darkpool::transport::SslCtxPtr server_ssl_ctx_{nullptr};

  std::map<int, Connection> clients_;
  bool is_running_{false};
  bool should_terminate_{false};

  MessageHandler message_handler_;
  DisconnectHandler disconnect_handler_;
  TickHandler tick_handler_;
  ConnectHandler connect_handler_;
};

}  // namespace darkpool::network

#endif  // defined(__linux__)
