#pragma once

#if defined(__linux__)

#include <chrono>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>

namespace darkpool::systemd {

class Notifier {
 public:
  using Clock = std::chrono::steady_clock;

  Notifier() noexcept;
  ~Notifier();

  Notifier(const Notifier&) = delete;
  Notifier& operator=(const Notifier&) = delete;

  void ready(Clock::time_point now = Clock::now()) noexcept;
  void check_watchdog(Clock::time_point now = Clock::now()) noexcept;
  void stopping() noexcept;
  [[nodiscard]] int max_poll_timeout_ms(int fallback_ms) const noexcept;

  [[nodiscard]] bool watchdog_enabled() const noexcept {
    return socket_fd_ >= 0 && watchdog_half_interval_ > Clock::duration::zero();
  }

 private:
  void send(std::string_view message) noexcept;
  void warn(const char* reason, int error_number = 0) noexcept;

  int socket_fd_{-1};
  sockaddr_un address_{};
  socklen_t address_length_{0};
  Clock::duration watchdog_half_interval_{};
  Clock::time_point next_watchdog_{};
  Clock::time_point last_warning_{};
  bool has_warning_{false};
  bool ready_called_{false};
  bool stopping_called_{false};
};

}  // namespace darkpool::systemd

#endif  // defined(__linux__)
