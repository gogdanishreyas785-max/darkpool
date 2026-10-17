#pragma once

#if defined(__linux__)

#include <cstdint>
#include <sys/epoll.h>

namespace darkpool::transport {

struct ConnectionDispatchState {
  bool is_tls{false};
  bool tls_write_want_read{false};
  bool tls_write_want_write{false};
  bool tls_read_want_write{false};
  bool peer_closed{false};
  bool has_pending_output{false};
};

struct DispatchDecisions {
  bool do_resume_tls_write{false};
  bool do_normal_read{false};
  bool do_resume_tls_read{false};
  bool do_mark_peer_closed{false};
  bool do_normal_write{false};
};

[[nodiscard]] constexpr DispatchDecisions evaluate_reactor_dispatch(
    const ConnectionDispatchState& conn,
    std::uint32_t epoll_flags) noexcept {
  constexpr std::uint32_t read_or_hangup =
      static_cast<std::uint32_t>(EPOLLIN) |
      static_cast<std::uint32_t>(EPOLLRDHUP) |
      static_cast<std::uint32_t>(EPOLLHUP);

  const bool initial_write_want_read = conn.is_tls && conn.tls_write_want_read;
  const bool initial_write_want_write = conn.is_tls && conn.tls_write_want_write;
  const bool initial_read_want_write = conn.is_tls && conn.tls_read_want_write;

  DispatchDecisions d{};

  // Resume the exact pending SSL_write operation only on the readiness it requested.
  if ((initial_write_want_read && (epoll_flags & read_or_hangup) != 0U) ||
      (initial_write_want_write && (epoll_flags & EPOLLOUT) != 0U)) {
    d.do_resume_tls_write = true;
  }

  // Do not start any other TLS I/O while an SSL_write retry is outstanding.
  // Likewise, a pending SSL_read that returned WANT_WRITE must be resumed first.
  if (!initial_write_want_read && !initial_write_want_write &&
      !initial_read_want_write && (epoll_flags & read_or_hangup) != 0U) {
    d.do_normal_read = true;
  }

  // Cross-direction resume: pending read was waiting for write readiness
  if (initial_read_want_write && (epoll_flags & EPOLLOUT) != 0U) {
    d.do_resume_tls_read = true;
  }

  if ((epoll_flags & (EPOLLRDHUP | EPOLLHUP)) != 0U && !conn.peer_closed) {
    d.do_mark_peer_closed = true;
  }

  // Normal write: only if not waiting for cross-direction readiness
  if (!initial_read_want_write && !initial_write_want_read &&
      !initial_write_want_write &&
      ((epoll_flags & EPOLLOUT) != 0U || conn.peer_closed) &&
      conn.has_pending_output) {
    d.do_normal_write = true;
  }

  return d;
}

struct ReactorEpollInterest {
  bool want_read{false};
  bool want_write{false};
};

[[nodiscard]] constexpr ReactorEpollInterest compute_reactor_epoll_interest(
    const ConnectionDispatchState& conn) noexcept {
  // A suspended OpenSSL operation has exclusive ownership of the SSL object
  // until that exact operation is retried. Register only the readiness it needs;
  // doing other TLS I/O here can violate OpenSSL's retry contract.
  if (conn.is_tls && conn.tls_write_want_read) {
    return ReactorEpollInterest{true, false};
  }
  if (conn.is_tls && (conn.tls_write_want_write || conn.tls_read_want_write)) {
    return ReactorEpollInterest{false, true};
  }

  return ReactorEpollInterest{!conn.peer_closed, conn.has_pending_output};
}

}  // namespace darkpool::transport

#endif  // defined(__linux__)
