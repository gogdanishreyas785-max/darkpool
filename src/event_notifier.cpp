#include "darkpool/event_notifier.hpp"

#if defined(__linux__)

#include <cerrno>
#include <cstring>
#include <sys/eventfd.h>
#include <unistd.h>

namespace darkpool::ipc {

EventNotifier::EventNotifier() {
  fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (fd_ < 0) {
    creation_errno_ = errno;
  }
}

EventNotifier::EventNotifier(int fd) noexcept : fd_(fd) {}

EventNotifier::~EventNotifier() {
  close();
}

EventNotifier::EventNotifier(EventNotifier&& other) noexcept
    : fd_(other.fd_), creation_errno_(other.creation_errno_) {
  other.fd_ = -1;
  other.creation_errno_ = 0;
}

EventNotifier& EventNotifier::operator=(EventNotifier&& other) noexcept {
  if (this != &other) {
    close();
    fd_ = other.fd_;
    creation_errno_ = other.creation_errno_;
    other.fd_ = -1;
    other.creation_errno_ = 0;
  }
  return *this;
}

void EventNotifier::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

NotifyStatus EventNotifier::notify(std::uint64_t increment, int* error_code) noexcept {
  if (error_code) *error_code = 0;
  if (fd_ < 0) {
    if (error_code) *error_code = EBADF;
    return NotifyStatus::Error;
  }
  // Linux eventfd maximum value is 0xFFFFFFFFFFFFFFFEULL. Writing 0 returns EINVAL.
  if (increment == 0 || increment >= 0xfffffffffffffffeULL) {
    if (error_code) *error_code = EINVAL;
    return NotifyStatus::InvalidIncrement;
  }

  while (true) {
    const ssize_t rc = ::write(fd_, &increment, sizeof(increment));
    if (rc == static_cast<ssize_t>(sizeof(increment))) {
      return NotifyStatus::Success;
    }
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (error_code) *error_code = errno;
        return NotifyStatus::Saturated;
      }
      if (error_code) *error_code = errno;
      return NotifyStatus::Error;
    }
    if (error_code) *error_code = EIO;
    return NotifyStatus::Error;
  }
}

ConsumeStatus EventNotifier::consume(std::uint64_t& value, int* error_code) noexcept {
  value = 0;
  if (error_code) *error_code = 0;
  if (fd_ < 0) {
    if (error_code) *error_code = EBADF;
    return ConsumeStatus::Error;
  }

  while (true) {
    std::uint64_t val = 0;
    const ssize_t rc = ::read(fd_, &val, sizeof(val));
    if (rc == static_cast<ssize_t>(sizeof(val))) {
      value = val;
      return ConsumeStatus::Success;
    }
    if (rc < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        if (error_code) *error_code = errno;
        return ConsumeStatus::Empty;
      }
      if (error_code) *error_code = errno;
      return ConsumeStatus::Error;
    }
    if (error_code) *error_code = EIO;
    return ConsumeStatus::Error;
  }
}

std::optional<std::uint64_t> EventNotifier::try_consume() noexcept {
  std::uint64_t val = 0;
  if (consume(val) == ConsumeStatus::Success) {
    return val;
  }
  return std::nullopt;
}

std::optional<EventNotifier> EventNotifier::create(std::string* error_msg) {
  EventNotifier notifier;
  if (!notifier.valid()) {
    if (error_msg) {
      *error_msg = std::strerror(notifier.creation_errno());
    }
    return std::nullopt;
  }
  return notifier;
}

}  // namespace darkpool::ipc

#endif  // defined(__linux__)

