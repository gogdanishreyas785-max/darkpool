#pragma once

#if defined(__linux__)

#include <cstdint>
#include <optional>
#include <string>

namespace darkpool::ipc {

enum class NotifyStatus {
  Success,
  InvalidIncrement,
  Saturated,
  Error
};

enum class ConsumeStatus {
  Success,
  Empty,
  Error
};

/**
 * @brief Move-only RAII wrapper around Linux eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC).
 *
 * Normal counter/coalescing semantics (NOT EFD_SEMAPHORE).
 * Writes increment the 64-bit kernel counter.
 * Reads consume the accumulated counter and reset it to zero.
 */
class EventNotifier {
 public:
  EventNotifier();
  ~EventNotifier();

  // Move-only RAII semantics
  EventNotifier(const EventNotifier&) = delete;
  EventNotifier& operator=(const EventNotifier&) = delete;
  EventNotifier(EventNotifier&& other) noexcept;
  EventNotifier& operator=(EventNotifier&& other) noexcept;

  [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
  [[nodiscard]] int descriptor() const noexcept { return fd_; }
  [[nodiscard]] int creation_errno() const noexcept { return creation_errno_; }

  void close() noexcept;

  // Increment the eventfd counter.
  // Returns:
  // - Success: counter incremented (coalesced with any existing counter value)
  // - InvalidIncrement: increment is 0 or UINT64_MAX (0xFFFFFFFFFFFFFFFFULL)
  // - Saturated: counter would exceed 0xFFFFFFFFFFFFFFFEULL (EAGAIN under EFD_NONBLOCK)
  // - Error: syscall failure (e.g. invalid descriptor)
  [[nodiscard]] NotifyStatus notify(std::uint64_t increment = 1, int* error_code = nullptr) noexcept;

  // Consume the accumulated counter value and reset it to zero.
  // Returns:
  // - Success: counter read into value and reset to 0
  // - Empty: counter was 0 (EAGAIN under EFD_NONBLOCK)
  // - Error: syscall failure
  [[nodiscard]] ConsumeStatus consume(std::uint64_t& value, int* error_code = nullptr) noexcept;

  // Convenient nonblocking consume helper returning std::optional.
  // Note: Returns std::nullopt both when the counter is empty and if an unexpected read error occurs.
  // Callers requiring explicit error distinction must use consume().
  [[nodiscard]] std::optional<std::uint64_t> try_consume() noexcept;

  static std::optional<EventNotifier> create(std::string* error_msg = nullptr);
  static EventNotifier from_fd(int fd) noexcept { return EventNotifier(fd); }

 private:
  explicit EventNotifier(int fd) noexcept;

  int fd_{-1};
  int creation_errno_{0};
};

}  // namespace darkpool::ipc

#endif  // defined(__linux__)

