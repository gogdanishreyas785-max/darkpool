#include "darkpool/client_runtime.hpp"

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace darkpool::network {

ClientConnection::ClientConnection() = default;

ClientConnection::~ClientConnection() {
  close();
}

ClientConnection::ClientConnection(ClientConnection&& other) noexcept
    : fd_(other.fd_),
      ssl_ctx_(std::move(other.ssl_ctx_)),
      ssl_(std::move(other.ssl_)),
      response_stream_(std::move(other.response_stream_)),
      is_tls_(other.is_tls_) {
  other.fd_ = -1;
  other.is_tls_ = false;
}

ClientConnection& ClientConnection::operator=(ClientConnection&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    ssl_ctx_ = std::move(other.ssl_ctx_);
    ssl_ = std::move(other.ssl_);
    response_stream_ = std::move(other.response_stream_);
    is_tls_ = other.is_tls_;
    other.fd_ = -1;
    other.is_tls_ = false;
  }
  return *this;
}

void ClientConnection::close() noexcept {
  if (ssl_ != nullptr) {
    (void)SSL_shutdown(ssl_.get());
    ssl_.reset();
  }
  ssl_ctx_.reset();
  if (fd_ >= 0) {
    (void)::close(fd_);
    fd_ = -1;
  }
  is_tls_ = false;
}

bool ClientConnection::connect_tcp(const std::string& host, std::uint16_t port, std::string& error_msg) {
  close();
  fd_ = darkpool::network::connect_tcp(host, port, error_msg);
  if (fd_ < 0) {
    return false;
  }
  is_tls_ = false;
  return true;
}

bool ClientConnection::connect_unix(const std::string& path, std::string& error_msg) {
  close();
  fd_ = darkpool::network::connect_unix(path, error_msg);
  if (fd_ < 0) {
    return false;
  }
  is_tls_ = false;
  return true;
}

bool ClientConnection::perform_tls_handshake(std::chrono::seconds timeout, std::string& error_msg) {
  const int flags = fcntl(fd_, F_GETFL, 0);
  (void)fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      error_msg = "TLS handshake timed out after " + std::to_string(timeout.count()) + " seconds";
      (void)fcntl(fd_, F_SETFL, flags);
      return false;
    }
    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    const int poll_timeout = std::clamp(static_cast<int>(remaining_ms), 1, 500);

    bool want_read = false;
    bool want_write = false;
    std::string err;
    const auto status = darkpool::transport::step_tls_handshake(
        ssl_.get(), false, want_read, want_write, err);

    if (status == darkpool::transport::TlsHandshakeStatus::Established) {
      break;
    }
    if (status == darkpool::transport::TlsHandshakeStatus::Failed) {
      error_msg = "TLS handshake failed: " + err;
      (void)fcntl(fd_, F_SETFL, flags);
      return false;
    }

    pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = (want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0);
    const int pret = poll(&pfd, 1, poll_timeout);
    if (pret < 0 && errno != EINTR) {
      error_msg = std::string("poll during TLS handshake: ") + std::strerror(errno);
      (void)fcntl(fd_, F_SETFL, flags);
      return false;
    }
  }

  (void)fcntl(fd_, F_SETFL, flags);
  return true;
}

bool ClientConnection::verify_server_certificate(
    const std::string& host,
    const darkpool::transport::ClientTlsConfig& config,
    std::string& error_msg) {
  X509* peer_cert = SSL_get_peer_certificate(ssl_.get());
  if (peer_cert == nullptr) {
    error_msg = "server presented no certificate";
    return false;
  }
  darkpool::transport::X509Ptr cert_guard(peer_cert);

  std::string err;
  if (!darkpool::transport::verify_cert_time_validity(peer_cert, err)) {
    error_msg = "certificate validity check failed: " + err;
    return false;
  }
  if (!darkpool::transport::verify_cert_server_purpose(peer_cert, err)) {
    error_msg = "certificate server purpose check failed: " + err;
    return false;
  }
  if (!darkpool::transport::verify_cert_ip_san(peer_cert, host, err)) {
    error_msg = "IP SAN verification failed: " + err;
    return false;
  }

  if (config.mode == darkpool::transport::ClientVerifyMode::FingerprintPinning) {
    const std::string actual_fp = darkpool::transport::compute_cert_fingerprint_sha256(peer_cert);
    std::array<std::uint8_t, 32> exp_digest{};
    if (!darkpool::transport::parse_fingerprint_sha256(config.pinned_fingerprint_hex, exp_digest, err)) {
      error_msg = "invalid pinned fingerprint: " + err;
      return false;
    }
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const auto b : exp_digest) {
      oss << std::setw(2) << static_cast<int>(b);
    }
    if (actual_fp != oss.str()) {
      error_msg = "server certificate fingerprint mismatch (pinned " + oss.str() + ", presented " + actual_fp + ")";
      return false;
    }
  } else if (config.mode == darkpool::transport::ClientVerifyMode::LocalCa) {
    const long verify_res = SSL_get_verify_result(ssl_.get());
    if (verify_res != X509_V_OK) {
      error_msg = std::string("certificate verification failed: ") + X509_verify_cert_error_string(verify_res);
      return false;
    }
  }

  return true;
}

bool ClientConnection::connect_tls(
    const std::string& host,
    std::uint16_t port,
    const darkpool::transport::ClientTlsConfig& tls_config,
    std::string& error_msg,
    std::chrono::seconds handshake_timeout) {
  close();

  std::string tls_init_err;
  ssl_ctx_ = darkpool::transport::create_client_ssl_ctx(tls_config, tls_init_err);
  if (ssl_ctx_ == nullptr) {
    error_msg = "client TLS initialization failed: " + tls_init_err;
    return false;
  }

  fd_ = darkpool::network::connect_tcp(host, port, error_msg);
  if (fd_ < 0) {
    ssl_ctx_.reset();
    return false;
  }

  SSL* raw_ssl = SSL_new(ssl_ctx_.get());
  if (raw_ssl == nullptr) {
    error_msg = "failed to allocate client SSL session";
    close();
    return false;
  }
  ssl_.reset(raw_ssl);
  SSL_set_app_data(ssl_.get(), const_cast<darkpool::transport::ClientTlsConfig*>(&tls_config));

  if (SSL_set_fd(ssl_.get(), fd_) != 1) {
    error_msg = "failed to attach socket to SSL session";
    close();
    return false;
  }

  if (!perform_tls_handshake(handshake_timeout, error_msg)) {
    close();
    return false;
  }

  if (!verify_server_certificate(host, tls_config, error_msg)) {
    close();
    return false;
  }

  is_tls_ = true;
  return true;
}

bool ClientConnection::write_all_bytes(const std::string& buffer) {
  if (fd_ < 0) {
    return false;
  }

  if (is_tls_) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
      std::size_t written = 0;
      std::string err;
      const auto status = darkpool::transport::tls_write(
          ssl_.get(), buffer.data() + offset, buffer.size() - offset, written, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        offset += written;
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite ||
                 status == darkpool::transport::TlsIoStatus::WantRead) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = (status == darkpool::transport::TlsIoStatus::WantWrite) ? POLLOUT : POLLIN;
        (void)poll(&pfd, 1, 1000);
      } else {
        return false;
      }
    }
    return true;
  }

  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const auto sent = send(fd_, buffer.data() + offset, buffer.size() - offset, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (sent == 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

bool ClientConnection::send_frame(const Frame& frame) {
  return write_all_bytes(frame.serialize());
}

bool ClientConnection::receive_frame(Frame& out_frame) {
  if (response_stream_.next(out_frame)) {
    return true;
  }
  if (fd_ < 0) {
    return false;
  }

  char buffer[4096];
  while (true) {
    if (is_tls_) {
      std::size_t bytes_read = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          ssl_.get(), buffer, sizeof(buffer), bytes_read, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        try {
          response_stream_.feed(std::span<const char>(buffer, bytes_read));
        } catch (const std::exception& error) {
          std::cerr << "protocol error: invalid server frame: " << error.what() << '\n';
          return false;
        }
        if (response_stream_.next(out_frame)) return true;
      } else if (status == darkpool::transport::TlsIoStatus::WantRead ||
                 status == darkpool::transport::TlsIoStatus::WantWrite) {
        pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = (status == darkpool::transport::TlsIoStatus::WantRead) ? POLLIN : POLLOUT;
        (void)poll(&pfd, 1, 1000);
      } else {
        return false;
      }
    } else {
      const auto count = recv(fd_, buffer, sizeof(buffer), 0);
      if (count <= 0) {
        if (count < 0 && errno == EINTR) continue;
        return false;
      }
      try {
        response_stream_.feed(std::span<const char>(buffer, static_cast<std::size_t>(count)));
      } catch (const std::exception& error) {
        std::cerr << "protocol error: invalid server frame: " << error.what() << '\n';
        return false;
      }
      if (response_stream_.next(out_frame)) return true;
    }
  }
}

bool ClientConnection::has_pending_data() const noexcept {
  if (response_stream_.pending_frames() > 0) return true;
#if defined(__linux__)
  if (is_tls_ && ssl_ && SSL_pending(ssl_.get()) > 0) return true;
#endif
  return false;
}

bool ClientConnection::poll_frame(Frame& out_frame, std::chrono::milliseconds timeout) {
  if (response_stream_.next(out_frame)) {
    return true;
  }
  if (fd_ < 0) {
    return false;
  }

#if defined(__linux__)
  if (is_tls_ && ssl_ && SSL_pending(ssl_.get()) > 0) {
    char buffer[4096];
    std::size_t bytes_read = 0;
    std::string err;
    const auto status = darkpool::transport::tls_read(
        ssl_.get(), buffer, sizeof(buffer), bytes_read, err);
    if (status == darkpool::transport::TlsIoStatus::Ok) {
      try {
        response_stream_.feed(std::span<const char>(buffer, bytes_read));
      } catch (const std::exception&) {
        return false;
      }
      if (response_stream_.next(out_frame)) return true;
    }
  }
#endif

  pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;
  const int ret = poll(&pfd, 1, static_cast<int>(timeout.count()));
  if (ret <= 0) {
    return false;
  }

  char buffer[4096];
  if (is_tls_) {
    std::size_t bytes_read = 0;
    std::string err;
    const auto status = darkpool::transport::tls_read(
        ssl_.get(), buffer, sizeof(buffer), bytes_read, err);
    if (status == darkpool::transport::TlsIoStatus::Ok) {
      try {
        response_stream_.feed(std::span<const char>(buffer, bytes_read));
      } catch (const std::exception&) {
        return false;
      }
      return response_stream_.next(out_frame);
    }
    return false;
  }

  const auto count = recv(fd_, buffer, sizeof(buffer), 0);
  if (count <= 0) {
    return false;
  }
  try {
    response_stream_.feed(std::span<const char>(buffer, static_cast<std::size_t>(count)));
  } catch (const std::exception&) {
    return false;
  }
  return response_stream_.next(out_frame);
}

}  // namespace darkpool::network

#endif  // defined(__linux__)
