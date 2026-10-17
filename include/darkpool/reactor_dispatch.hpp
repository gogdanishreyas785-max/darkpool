#pragma once

#if defined(__linux__)

#include <cstdint>
#include <sys/epoll.h>

namespace darkpool::transport {

struct ConnectionDispatchState {
  bool is_tls{false};
  bool tls_write_want_read{false};
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
  const bool initial_read_want_write = conn.is_tls && conn.tls_read_want_write;

  DispatchDecisions d{};

  // Cross-direction resume: pending write was waiting for read readiness
  if (initial_write_want_read && (epoll_flags & read_or_hangup) != 0U) {
    d.do_resume_tls_write = true;
  }

  // Normal read: only if not waiting for cross-direction readiness
  if (!initial_write_want_read && !initial_read_want_write && (epoll_flags & read_or_hangup) != 0U) {
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
  bool reading = false;
  bool writing = false;

  // Read readiness requirements
  if (!conn.peer_closed) {
    if (conn.is_tls && conn.tls_read_want_write) {
      writing = true; // SSL_read needs write readiness to resume
    } else {
      reading = true; // normal read or tls_read_want_read
    }
  }

  // Write readiness requirements
  if (conn.has_pending_output) {
    if (conn.is_tls && conn.tls_write_want_read) {
      reading = true; // SSL_write needs read readiness to resume; DO NOT set writing (prevents spin)
    } else {
      writing = true; // normal write or tls_write_want_write
    }
  }

  return ReactorEpollInterest{reading, writing};
}

}  // namespace darkpool::transport

#endif  // defined(__linux__)
