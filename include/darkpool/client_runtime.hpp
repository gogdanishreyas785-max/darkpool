#pragma once

#include "darkpool/network.hpp"
#include "darkpool/socket_primitives.hpp"

#if defined(__linux__)
#include "darkpool/tls_transport.hpp"

#include <chrono>
#include <cstdint>
#include <string>

namespace darkpool::network {

class ClientConnection {
 public:
  ClientConnection();
  ~ClientConnection();

  ClientConnection(const ClientConnection&) = delete;
  ClientConnection& operator=(const ClientConnection&) = delete;
  ClientConnection(ClientConnection&& other) noexcept;
  ClientConnection& operator=(ClientConnection&& other) noexcept;

  // Connects to a remote TCP server
  bool connect_tcp(const std::string& host, std::uint16_t port, std::string& error_msg);

  // Connects to a local UNIX domain socket
  bool connect_unix(const std::string& path, std::string& error_msg);

  // Connects to a remote TLS 1.3 server with certificate verification
  bool connect_tls(
      const std::string& host,
      std::uint16_t port,
      const darkpool::transport::ClientTlsConfig& tls_config,
      std::string& error_msg,
      std::chrono::seconds handshake_timeout = std::chrono::seconds(5));

  // Sends a length-prefixed JSON frame (blocks until all bytes sent or error)
  bool send_frame(const Frame& frame);

  // Receives a length-prefixed JSON frame (blocks until full frame or error)
  bool receive_frame(Frame& out_frame);

  // Polls for an incoming frame with a timeout (supports asynchronous notifications)
  bool poll_frame(Frame& out_frame, std::chrono::milliseconds timeout = std::chrono::milliseconds(0));

  [[nodiscard]] bool has_pending_frame() const noexcept { return response_stream_.pending_frames() > 0; }
  [[nodiscard]] bool has_pending_data() const noexcept;
  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int native_handle() const noexcept { return fd_; }

  void close() noexcept;

 private:
  bool write_all_bytes(const std::string& buffer);
  bool perform_tls_handshake(std::chrono::seconds timeout, std::string& error_msg);
  bool verify_server_certificate(const std::string& host, const darkpool::transport::ClientTlsConfig& config, std::string& error_msg);

  int fd_{-1};
  darkpool::transport::SslCtxPtr ssl_ctx_{nullptr};
  darkpool::transport::SslPtr ssl_{nullptr};
  FrameStream response_stream_;
  bool is_tls_{false};
};

}  // namespace darkpool::network

#endif  // defined(__linux__)
