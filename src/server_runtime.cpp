#include "darkpool/server_runtime.hpp"

#if defined(__linux__)

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <iostream>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace darkpool::network {

ServerRuntime::ServerRuntime(ServerRuntimeConfig config)
    : config_(std::move(config)) {}

ServerRuntime::~ServerRuntime() {
  stop();
}

ServerRuntime::ServerRuntime(ServerRuntime&&) noexcept = default;
ServerRuntime& ServerRuntime::operator=(ServerRuntime&&) noexcept = default;

darkpool::transport::ConnectionDispatchState ServerRuntime::to_dispatch_state(
    const Connection& conn) noexcept {
  return darkpool::transport::ConnectionDispatchState{
      .is_tls = (conn.transport == TransportType::Tls),
      .tls_write_want_read = conn.tls_write_want_read,
      .tls_write_want_write = conn.tls_write_want_write,
      .tls_read_want_write = conn.tls_read_want_write,
      .peer_closed = conn.peer_closed,
      .has_pending_output = !conn.output.pending().empty()};
}

bool ServerRuntime::set_epoll_events(int fd, bool reading, bool writing) {
  epoll_event event{};
  event.events = 0U;
  if (reading) {
    event.events |= static_cast<std::uint32_t>(EPOLLIN) |
                    static_cast<std::uint32_t>(EPOLLRDHUP);
  }
  if (writing) {
    event.events |= static_cast<std::uint32_t>(EPOLLOUT);
  }
  event.data.fd = fd;
  return epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &event) == 0;
}

bool ServerRuntime::update_connection_events(int fd, Connection& conn) {
  if (conn.transport == TransportType::Plaintext) {
    const bool reading = !conn.peer_closed;
    const bool writing = !conn.output.pending().empty();
    return set_epoll_events(fd, reading, writing);
  }

  // Handshake phase
  if (!conn.tls_handshake_done) {
    return set_epoll_events(fd, conn.tls_handshake_want_read,
                            conn.tls_handshake_want_write);
  }

  const auto state = to_dispatch_state(conn);
  const auto interest = darkpool::transport::compute_reactor_epoll_interest(state);
  return set_epoll_events(fd, interest.want_read, interest.want_write);
}

void ServerRuntime::drop_connection(int fd) {
  const auto found = clients_.find(fd);
  if (found == clients_.end()) {
    return;
  }
  const std::uint64_t session_id = found->second.session_id;
  (void)epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  if (found->second.ssl != nullptr) {
    (void)SSL_shutdown(found->second.ssl.get());
    found->second.ssl.reset();
  }
  (void)::close(fd);
  clients_.erase(found);

  if (disconnect_handler_) {
    disconnect_handler_(session_id);
  }
}

bool ServerRuntime::enqueue_frame(int fd, Connection& conn, const Frame& frame) {
  try {
    if (!conn.output.append(frame.serialize())) {
      std::cerr << "[server] connection " << fd << " evicted: output queue overflow ("
                << conn.output.pending().size() << " bytes queued, limit "
                << darkpool::network::kMaxQueuedBytes << ")\n";
      return false;
    }
    return update_connection_events(fd, conn);
  } catch (const std::exception&) {
    return false;
  }
}

bool ServerRuntime::start(std::string& error_msg) {
  if (is_running_) {
    return true;
  }

  if (config_.enable_tcp) {
    tcp_listener_ = create_tcp_listener(config_.bind_address, config_.tcp_port, error_msg);
    if (tcp_listener_ < 0) {
      return false;
    }
  }

  if (config_.enable_tls) {
    darkpool::transport::ServerTlsConfig tls_cfg;
    tls_cfg.cert_file = config_.tls_cert_path;
    tls_cfg.key_file = config_.tls_key_path;
    tls_cfg.bind_address = config_.tls_bind_address;
    tls_cfg.port = config_.tls_port;

    std::string tls_init_err;
    server_ssl_ctx_ = darkpool::transport::create_server_ssl_ctx(tls_cfg, tls_init_err);
    if (server_ssl_ctx_ == nullptr) {
      error_msg = "TLS initialization failed: " + tls_init_err;
      stop();
      return false;
    }

    tls_listener_ = create_tcp_listener(config_.tls_bind_address, config_.tls_port, error_msg);
    if (tls_listener_ < 0) {
      stop();
      return false;
    }
  }

  if (config_.enable_unix) {
    unix_listener_ = create_unix_listener(config_.unix_path, error_msg);
    if (unix_listener_ < 0) {
      stop();
      return false;
    }
    socket_guard_.path = config_.unix_path;
    socket_guard_.active = true;
  }

  epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd_ < 0) {
    error_msg = std::string("epoll_create1: ") + std::strerror(errno);
    stop();
    return false;
  }

  if (tcp_listener_ >= 0) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = tcp_listener_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tcp_listener_, &event) < 0) {
      error_msg = std::string("epoll_ctl tcp listener: ") + std::strerror(errno);
      stop();
      return false;
    }
  }

  if (tls_listener_ >= 0) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = tls_listener_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tls_listener_, &event) < 0) {
      error_msg = std::string("epoll_ctl tls listener: ") + std::strerror(errno);
      stop();
      return false;
    }
  }

  if (unix_listener_ >= 0) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = unix_listener_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, unix_listener_, &event) < 0) {
      error_msg = std::string("epoll_ctl unix listener: ") + std::strerror(errno);
      stop();
      return false;
    }
  }

  if (config_.handle_signals) {
    std::signal(SIGPIPE, SIG_IGN);
    sigset_t sig_mask;
    sigemptyset(&sig_mask);
    sigaddset(&sig_mask, SIGINT);
    sigaddset(&sig_mask, SIGTERM);
    sigaddset(&sig_mask, SIGPIPE);
    (void)sigprocmask(SIG_BLOCK, &sig_mask, nullptr);
    signal_fd_ = signalfd(-1, &sig_mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (signal_fd_ >= 0) {
      epoll_event event{};
      event.events = static_cast<std::uint32_t>(EPOLLIN);
      event.data.fd = signal_fd_;
      (void)epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, signal_fd_, &event);
    }
  }

  is_running_ = true;
  should_terminate_ = false;
  return true;
}

void ServerRuntime::accept_pending_clients(int listener_fd, bool is_tls) {
  while (true) {
    const auto client =
        accept4(listener_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (client < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (clients_.size() >= config_.max_connections) {
      (void)::close(client);
      continue;
    }
    int sndbuf = 16384;
    (void)setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int one = 1;
    (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    const auto session_id = static_cast<std::uint64_t>(client);

    if (is_tls) {
      if (server_ssl_ctx_ == nullptr) {
        (void)::close(client);
        continue;
      }
      SSL* raw_ssl = SSL_new(server_ssl_ctx_.get());
      if (raw_ssl == nullptr) {
        (void)::close(client);
        continue;
      }
      darkpool::transport::SslPtr ssl(raw_ssl);
      if (SSL_set_fd(ssl.get(), client) != 1) {
        (void)::close(client);
        continue;
      }
      SSL_set_accept_state(ssl.get());

      epoll_event client_event{};
      client_event.events = static_cast<std::uint32_t>(EPOLLIN) |
                            static_cast<std::uint32_t>(EPOLLRDHUP);
      client_event.data.fd = client;
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client, &client_event) == 0) {
        Connection conn;
        conn.session_id = session_id;
        conn.fd = client;
        conn.transport = TransportType::Tls;
        conn.ssl = std::move(ssl);
        conn.tls_handshake_done = false;
        conn.tls_handshake_want_read = true;
        conn.tls_handshake_want_write = false;
        clients_.emplace(client, std::move(conn));
        if (connect_handler_) {
          connect_handler_(session_id);
        }
      } else {
        (void)::close(client);
      }
    } else {
      epoll_event client_event{};
      client_event.events = static_cast<std::uint32_t>(EPOLLIN) |
                            static_cast<std::uint32_t>(EPOLLRDHUP);
      client_event.data.fd = client;
      if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client, &client_event) == 0) {
        Connection conn;
        conn.session_id = session_id;
        conn.fd = client;
        clients_.emplace(client, std::move(conn));
        if (connect_handler_) {
          connect_handler_(session_id);
        }
      } else {
        (void)::close(client);
      }
    }
  }
}

bool ServerRuntime::handle_client_read(int fd, Connection& conn) {
  if (conn.transport == TransportType::Tls) {
    char buffer[4096];
    while (true) {
      std::size_t bytes_read = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          conn.ssl.get(), buffer, sizeof(buffer), bytes_read, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        conn.tls_read_want_read = false;
        conn.tls_read_want_write = false;
        try {
          const auto frames = conn.decoder.feed(
              std::span<const char>(buffer, bytes_read));
          for (const auto& frame : frames) {
            if (!conn.is_authenticated) {
              if (message_handler_ && !message_handler_(conn.session_id, frame)) {
                return false;
              }
            } else {
              if (config_.enable_rate_limiting && !conn.rate_limiter.try_acquire()) {
                if (!enqueue_frame(fd, conn, Frame(json{{"type", "error"}, {"error", "rate limit exceeded"}}))) {
                  return false;
                }
              } else {
                if (message_handler_ && !message_handler_(conn.session_id, frame)) {
                  return false;
                }
              }
            }
          }
        } catch (const std::exception& e) {
          std::cerr << "[server] connection " << fd << " closed: framing error: " << e.what() << "\n";
          return false;
        }
      } else if (status == darkpool::transport::TlsIoStatus::PeerClosed) {
        conn.peer_closed = true;
        conn.tls_read_want_read = false;
        conn.tls_read_want_write = false;
        return update_connection_events(fd, conn);
      } else if (status == darkpool::transport::TlsIoStatus::WantRead) {
        conn.tls_read_want_read = true;
        conn.tls_read_want_write = false;
        return update_connection_events(fd, conn);
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite) {
        conn.tls_read_want_read = false;
        conn.tls_read_want_write = true;
        return update_connection_events(fd, conn);
      } else {
        return false;
      }
    }
  }

  char buffer[4096];
  while (true) {
    const auto count = recv(fd, buffer, sizeof(buffer), 0);
    if (count == 0) {
      conn.peer_closed = true;
      return update_connection_events(fd, conn);
    }
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      return false;
    }
    try {
      const auto frames = conn.decoder.feed(
          std::span<const char>(buffer, static_cast<std::size_t>(count)));
      for (const auto& frame : frames) {
        if (!conn.is_authenticated) {
          if (message_handler_ && !message_handler_(conn.session_id, frame)) {
            return false;
          }
        } else {
          if (config_.enable_rate_limiting && !conn.rate_limiter.try_acquire()) {
            if (!enqueue_frame(fd, conn, Frame(json{{"type", "error"}, {"error", "rate limit exceeded"}}))) {
              return false;
            }
          } else {
            if (message_handler_ && !message_handler_(conn.session_id, frame)) {
              return false;
            }
          }
        }
      }
    } catch (const std::exception&) {
      return false;
    }
  }
}

bool ServerRuntime::handle_client_write(int fd, Connection& conn) {
  if (conn.transport == TransportType::Tls) {
    while (!conn.output.pending().empty()) {
      const auto pending = conn.output.pending();
      std::size_t bytes_written = 0;
      std::string err;
      const auto status = darkpool::transport::tls_write(
          conn.ssl.get(), pending.data(), pending.size(), bytes_written, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        conn.output.consume(bytes_written);
        conn.tls_write_want_read = false;
        conn.tls_write_want_write = false;
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite) {
        conn.tls_write_want_write = true;
        conn.tls_write_want_read = false;
        return update_connection_events(fd, conn);
      } else if (status == darkpool::transport::TlsIoStatus::WantRead) {
        conn.tls_write_want_read = true;
        conn.tls_write_want_write = false;
        return update_connection_events(fd, conn);
      } else {
        return false;
      }
    }
    conn.tls_write_want_write = false;
    conn.tls_write_want_read = false;
    return update_connection_events(fd, conn);
  }

  while (!conn.output.pending().empty()) {
    const auto pending = conn.output.pending();
    const auto count = send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return true;
      }
      return false;
    }
    if (count == 0) {
      return false;
    }
    conn.output.consume(static_cast<std::size_t>(count));
  }
  return update_connection_events(fd, conn);
}

bool ServerRuntime::step(int timeout_ms) {
  if (!is_running_ || should_terminate_) {
    return false;
  }

  const int actual_timeout = (timeout_ms >= 0) ? timeout_ms : config_.epoll_timeout_ms;
  epoll_event events[16];
  const auto ready = epoll_wait(epoll_fd_, events, 16, actual_timeout);
  if (ready < 0) {
    if (errno == EINTR) {
      return true;
    }
    std::cerr << "epoll_wait: " << std::strerror(errno) << '\n';
    should_terminate_ = true;
    return false;
  }

  for (int i = 0; i < ready; ++i) {
    const auto fd = events[i].data.fd;
    if (signal_fd_ >= 0 && fd == signal_fd_) {
      signalfd_siginfo info{};
      const auto read_bytes = read(signal_fd_, &info, sizeof(info));
      (void)read_bytes;
      should_terminate_ = true;
      break;
    }
    if (tcp_listener_ >= 0 && fd == tcp_listener_) {
      accept_pending_clients(fd, false);
      continue;
    }
    if (tls_listener_ >= 0 && fd == tls_listener_) {
      accept_pending_clients(fd, true);
      continue;
    }
    if (unix_listener_ >= 0 && fd == unix_listener_) {
      accept_pending_clients(fd, false);
      continue;
    }

    const auto found = clients_.find(fd);
    if (found == clients_.end()) {
      continue;
    }
    const auto flags = events[i].events;
    if ((flags & EPOLLERR) != 0U) {
      drop_connection(fd);
      continue;
    }

    // Drive TLS handshake if still in progress
    if (found->second.transport == TransportType::Tls && !found->second.tls_handshake_done) {
      bool want_read = false;
      bool want_write = false;
      std::string err;
      const auto status = darkpool::transport::step_tls_handshake(
          found->second.ssl.get(), true, want_read, want_write, err);
      if (status == darkpool::transport::TlsHandshakeStatus::Established) {
        found->second.tls_handshake_done = true;
        found->second.tls_handshake_want_read = false;
        found->second.tls_handshake_want_write = false;
        (void)update_connection_events(fd, found->second);
      } else if (status == darkpool::transport::TlsHandshakeStatus::InProgress) {
        found->second.tls_handshake_want_read = want_read;
        found->second.tls_handshake_want_write = want_write;
        (void)update_connection_events(fd, found->second);
        continue;
      } else {
        std::cerr << "[server] connection " << fd << " closed: handshake failure\n";
        drop_connection(fd);
        continue;
      }
    }

    bool keep = true;
    const auto state = to_dispatch_state(found->second);
    const auto decisions = darkpool::transport::evaluate_reactor_dispatch(state, flags);

    // Cross-direction resume: pending TLS write was waiting for read readiness
    if (decisions.do_resume_tls_write) {
      keep = handle_client_write(fd, found->second);
    }

    // Normal read: only if not waiting for cross-direction readiness
    if (keep && decisions.do_normal_read) {
      keep = handle_client_read(fd, found->second);
    }

    // Cross-direction resume: pending TLS read was waiting for write readiness
    if (keep && decisions.do_resume_tls_read) {
      keep = handle_client_read(fd, found->second);
    }

    if (keep && decisions.do_mark_peer_closed) {
      found->second.peer_closed = true;
      keep = update_connection_events(fd, found->second);
    }

    // Normal write
    if (keep && decisions.do_normal_write &&
        !(found->second.transport == TransportType::Tls &&
          (found->second.tls_read_want_write ||
           found->second.tls_write_want_read ||
           found->second.tls_write_want_write))) {
      keep = handle_client_write(fd, found->second);
    }

    if (keep && found->second.peer_closed && found->second.output.pending().empty()) {
      keep = false;
    }
    if (!keep) {
      drop_connection(fd);
    }
  }

  // Handshake timeout enforcement
  const auto now = std::chrono::steady_clock::now();
  std::vector<int> expired;
  for (const auto& [client_fd, conn] : clients_) {
    const bool handshake_incomplete =
        (conn.transport == TransportType::Tls && !conn.tls_handshake_done) ||
        !conn.is_authenticated;
    if (handshake_incomplete && (now - conn.connected_at >= config_.handshake_timeout)) {
      expired.push_back(client_fd);
    }
  }
  for (const auto client_fd : expired) {
    drop_connection(client_fd);
  }

  if (tick_handler_) {
    tick_handler_();
  }

  return !should_terminate_;
}

int ServerRuntime::run() {
  while (step()) {
  }
  return should_terminate_ ? 0 : 1;
}

void ServerRuntime::stop() noexcept {
  should_terminate_ = true;
  is_running_ = false;

  for (auto& [client_fd, conn] : clients_) {
    if (conn.ssl != nullptr) {
      (void)SSL_shutdown(conn.ssl.get());
      conn.ssl.reset();
    }
    (void)::close(client_fd);
  }
  clients_.clear();

  if (signal_fd_ >= 0) {
    (void)::close(signal_fd_);
    signal_fd_ = -1;
  }
  if (epoll_fd_ >= 0) {
    (void)::close(epoll_fd_);
    epoll_fd_ = -1;
  }
  if (tcp_listener_ >= 0) {
    (void)::close(tcp_listener_);
    tcp_listener_ = -1;
  }
  if (tls_listener_ >= 0) {
    (void)::close(tls_listener_);
    tls_listener_ = -1;
  }
  if (unix_listener_ >= 0) {
    (void)::close(unix_listener_);
    unix_listener_ = -1;
  }
  server_ssl_ctx_.reset();
}

bool ServerRuntime::send_message(std::uint64_t session_id, const Frame& frame) {
  const auto found = clients_.find(static_cast<int>(session_id));
  if (found == clients_.end()) {
    return false;
  }
  return enqueue_frame(found->first, found->second, frame);
}

void ServerRuntime::send_multicast(std::span<const std::uint64_t> session_ids, const Frame& frame) {
  for (const auto sid : session_ids) {
    (void)send_message(sid, frame);
  }
}

void ServerRuntime::send_broadcast(const Frame& frame) {
  for (auto& [fd, conn] : clients_) {
    if (conn.is_authenticated) {
      (void)enqueue_frame(fd, conn, frame);
    }
  }
}

void ServerRuntime::close_session(std::uint64_t session_id) {
  drop_connection(static_cast<int>(session_id));
}

void ServerRuntime::set_authenticated(std::uint64_t session_id, bool auth) {
  const auto found = clients_.find(static_cast<int>(session_id));
  if (found != clients_.end()) {
    found->second.is_authenticated = auth;
  }
}

bool ServerRuntime::is_authenticated(std::uint64_t session_id) const {
  const auto found = clients_.find(static_cast<int>(session_id));
  if (found != clients_.end()) {
    return found->second.is_authenticated;
  }
  return false;
}

bool ServerRuntime::try_acquire_rate_limit(std::uint64_t session_id) {
  const auto found = clients_.find(static_cast<int>(session_id));
  if (found != clients_.end()) {
    return found->second.rate_limiter.try_acquire();
  }
  return false;
}

std::size_t ServerRuntime::connection_count() const noexcept {
  return clients_.size();
}

bool ServerRuntime::has_session(std::uint64_t session_id) const noexcept {
  return clients_.find(static_cast<int>(session_id)) != clients_.end();
}

}  // namespace darkpool::network

#endif  // defined(__linux__)
