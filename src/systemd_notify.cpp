#include "darkpool/systemd_notify.hpp"

#if defined(__linux__)

#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <string_view>
#include <unistd.h>

namespace darkpool::systemd {
namespace {

using Clock = Notifier::Clock;

bool make_address(const char* socket_name, sockaddr_un& address,
                  socklen_t& address_length) noexcept {
  if (socket_name == nullptr || socket_name[0] == '\0') {
    return false;
  }

  address = {};
  address.sun_family = AF_UNIX;
  const std::string_view name(socket_name);
  constexpr auto path_capacity = sizeof(address.sun_path);
  const auto path_offset = offsetof(sockaddr_un, sun_path);

  if (name.front() == '@') {
    const auto abstract_name = name.substr(1U);
    if (abstract_name.empty() || abstract_name.size() >= path_capacity) {
      return false;
    }
    std::memcpy(address.sun_path + 1, abstract_name.data(), abstract_name.size());
    address_length = static_cast<socklen_t>(path_offset + 1U + abstract_name.size());
    return true;
  }

  if (name.front() != '/' || name.size() >= path_capacity) {
    return false;
  }
  std::memcpy(address.sun_path, name.data(), name.size());
  address.sun_path[name.size()] = '\0';
  address_length = static_cast<socklen_t>(path_offset + name.size() + 1U);
  return true;
}

bool parse_unsigned(const char* text, std::uint64_t& value) noexcept {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  const auto* end = text + std::strlen(text);
  const auto result = std::from_chars(text, end, value, 10);
  return result.ec == std::errc{} && result.ptr == end;
}

bool parse_watchdog_half_interval(Clock::duration& half_interval) noexcept {
  const char* usec_text = std::getenv("WATCHDOG_USEC");
  std::uint64_t usec = 0U;
  if (!parse_unsigned(usec_text, usec) || usec == 0U) {
    return false;
  }

  const char* pid_text = std::getenv("WATCHDOG_PID");
  if (pid_text != nullptr) {
    std::uint64_t watchdog_pid = 0U;
    if (!parse_unsigned(pid_text, watchdog_pid) ||
        watchdog_pid != static_cast<std::uint64_t>(::getpid())) {
      return false;
    }
  }

  const long double ticks = static_cast<long double>(usec) *
      static_cast<long double>(Clock::period::den) /
      (1'000'000.0L * static_cast<long double>(Clock::period::num));
  const long double max_ticks =
      static_cast<long double>(std::numeric_limits<Clock::duration::rep>::max());
  if (!std::isfinite(ticks) || ticks <= 0.0L || ticks > max_ticks) {
    return false;
  }

  const auto interval_ticks = static_cast<Clock::duration::rep>(std::ceil(ticks));
  const Clock::duration interval(interval_ticks);
  half_interval = interval / 2;
  return half_interval > Clock::duration::zero();
}

}  // namespace

Notifier::Notifier() noexcept {
  const char* socket_name = std::getenv("NOTIFY_SOCKET");
  if (socket_name == nullptr || socket_name[0] == '\0') {
    return;
  }
  if (!make_address(socket_name, address_, address_length_)) {
    warn("invalid NOTIFY_SOCKET address");
    return;
  }

  socket_fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
  if (socket_fd_ < 0) {
    warn("could not open notification socket", errno);
    return;
  }

  Clock::duration half_interval{};
  const char* watchdog_usec = std::getenv("WATCHDOG_USEC");
  if (watchdog_usec != nullptr && watchdog_usec[0] != '\0') {
    if (parse_watchdog_half_interval(half_interval)) {
      watchdog_half_interval_ = half_interval;
    } else {
      const char* watchdog_pid = std::getenv("WATCHDOG_PID");
      if (watchdog_pid != nullptr) {
        std::uint64_t parsed_pid = 0U;
        if (!parse_unsigned(watchdog_pid, parsed_pid) ||
            parsed_pid != static_cast<std::uint64_t>(::getpid())) {
          warn("WATCHDOG_PID does not match this process; watchdog disabled");
        } else {
          warn("invalid WATCHDOG_USEC; watchdog disabled");
        }
      } else {
        warn("invalid WATCHDOG_USEC; watchdog disabled");
      }
    }
  }
}

Notifier::~Notifier() {
  if (socket_fd_ >= 0) {
    (void)::close(socket_fd_);
  }
}

void Notifier::ready(Clock::time_point now) noexcept {
  if (ready_called_) {
    return;
  }
  ready_called_ = true;
  if (socket_fd_ >= 0) {
    send("READY=1");
  }
  if (watchdog_enabled()) {
    next_watchdog_ = now + watchdog_half_interval_;
  }
}

void Notifier::check_watchdog(Clock::time_point now) noexcept {
  if (!ready_called_ || stopping_called_ || !watchdog_enabled() ||
      now < next_watchdog_) {
    return;
  }
  send("WATCHDOG=1");
  // Schedule from the current time. If a loop iteration was delayed, this
  // avoids sending a burst of catch-up notifications.
  next_watchdog_ = now + watchdog_half_interval_;
}

void Notifier::stopping() noexcept {
  if (stopping_called_) {
    return;
  }
  stopping_called_ = true;
  if (socket_fd_ >= 0) {
    send("STOPPING=1");
  }
}

int Notifier::max_poll_timeout_ms(int fallback_ms) const noexcept {
  if (!watchdog_enabled() || fallback_ms < 0) {
    return fallback_ms;
  }
  const auto half_interval_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(watchdog_half_interval_).count();
  if (half_interval_ms < static_cast<std::chrono::milliseconds::rep>(fallback_ms)) {
    return static_cast<int>(half_interval_ms);
  }
  return fallback_ms;
}

void Notifier::send(std::string_view message) noexcept {
  const auto sent = ::sendto(socket_fd_, message.data(), message.size(), MSG_DONTWAIT,
                             reinterpret_cast<const sockaddr*>(&address_),
                             address_length_);
  if (sent < 0) {
    warn("notification send failed", errno);
  } else if (static_cast<std::size_t>(sent) != message.size()) {
    warn("notification send was incomplete");
  }
}

void Notifier::warn(const char* reason, int error_number) noexcept {
  const auto now = Clock::now();
  constexpr auto throttle = std::chrono::seconds(30);
  if (has_warning_ && now - last_warning_ < throttle) {
    return;
  }
  has_warning_ = true;
  last_warning_ = now;
  std::cerr << "[systemd] " << reason;
  if (error_number != 0) {
    std::cerr << ": " << std::strerror(error_number);
  }
  std::cerr << '\n';
}

}  // namespace darkpool::systemd

#endif  // defined(__linux__)
