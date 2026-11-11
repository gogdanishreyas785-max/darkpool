#include "darkpool/posix_mq_control.hpp"

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cctype>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <poll.h>
#include <span>
#include <cstdint>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace darkpool::control {
namespace {

using Json = network::json;
using Clock = std::chrono::steady_clock;

constexpr mode_t kQueuePermissions = 0600;
constexpr std::size_t kMaximumQueueName = 255;

struct MqSyscallHooks {
  testing::MqGetattrHook getattr{nullptr};
  testing::MqSetattrHook setattr{nullptr};
  testing::MqSendHook send{nullptr};
  testing::MqReceiveHook receive{nullptr};
};

MqSyscallHooks mq_hooks;

int mq_getattr_call(mqd_t descriptor, mq_attr* attributes) {
  return mq_hooks.getattr != nullptr ? mq_hooks.getattr(descriptor, attributes)
                                     : ::mq_getattr(descriptor, attributes);
}

int mq_setattr_call(mqd_t descriptor, const mq_attr* desired, mq_attr* old_attributes) {
  return mq_hooks.setattr != nullptr ? mq_hooks.setattr(descriptor, desired, old_attributes)
                                     : ::mq_setattr(descriptor, desired, old_attributes);
}

int mq_send_call(mqd_t descriptor, const char* message, std::size_t size, unsigned priority) {
  return mq_hooks.send != nullptr ? mq_hooks.send(descriptor, message, size, priority)
                                  : ::mq_send(descriptor, message, size, priority);
}

ssize_t mq_receive_call(mqd_t descriptor, char* buffer, std::size_t size,
                        unsigned* priority) {
  return mq_hooks.receive != nullptr ? mq_hooks.receive(descriptor, buffer, size, priority)
                                     : ::mq_receive(descriptor, buffer, size, priority);
}

bool retry_eintr(const volatile sig_atomic_t* interrupted, Clock::time_point deadline,
                int& error_number) {
  if (interrupted != nullptr && *interrupted != 0) {
    error_number = EINTR;
    return false;
  }
  if (deadline != Clock::time_point::max() && Clock::now() >= deadline) {
    error_number = ETIMEDOUT;
    return false;
  }
  if (deadline != Clock::time_point::max() && Clock::now() >= deadline) {
    error_number = ETIMEDOUT;
    return false;
  }
  return true;
}

bool wait_after_eintr(int descriptor, short events, const volatile sig_atomic_t* interrupted,
                      Clock::time_point deadline, int& error_number) {
  if (deadline == Clock::time_point::max()) {
    error_number = EINTR;
    return false;
  }
  if (!retry_eintr(interrupted, deadline, error_number)) return false;
  const auto remaining = deadline == Clock::time_point::max()
      ? std::chrono::milliseconds(1)
      : std::chrono::ceil<std::chrono::milliseconds>(deadline - Clock::now());
  if (remaining <= std::chrono::milliseconds::zero()) {
    error_number = ETIMEDOUT;
    return false;
  }
  pollfd state{.fd = descriptor, .events = events, .revents = 0};
  const auto timeout = std::min(remaining, std::chrono::milliseconds(1));
  const int result = ::poll(&state, 1U, static_cast<int>(timeout.count()));
  if (result < 0 && errno != EINTR) {
    error_number = errno;
    return false;
  }
  return retry_eintr(interrupted, deadline, error_number);
}

std::string error_text(std::string_view operation, int error_number) {
  return std::string(operation) + ": " + std::strerror(error_number);
}

bool safe_queue_access_flags(int flags) noexcept {
  const int access = flags & O_ACCMODE;
  return access == O_RDONLY || access == O_WRONLY || access == O_RDWR;
}

bool random_bytes(std::span<unsigned char> bytes) noexcept {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<std::string> create_request_id() {
  std::array<unsigned char, 16> bytes{};
  if (!random_bytes(bytes)) return std::nullopt;
  constexpr char hex[] = "0123456789abcdef";
  std::string id;
  id.resize(bytes.size() * 2U);
  for (std::size_t i = 0U; i < bytes.size(); ++i) {
    id[i * 2U] = hex[(bytes[i] >> 4U) & 0x0fU];
    id[i * 2U + 1U] = hex[bytes[i] & 0x0fU];
  }
  return id;
}

std::optional<int> wait_for_queue(int descriptor, short events,
                                  std::chrono::steady_clock::time_point deadline,
                                  const volatile sig_atomic_t* interrupted,
                                  std::string& error_message) {
  while (true) {
    if (interrupted != nullptr && *interrupted != 0) {
      error_message = "interrupted";
      return std::nullopt;
    }
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      error_message = "control request timed out";
      return std::nullopt;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    const auto remaining_count = remaining.count();
    const int timeout_ms = static_cast<int>(std::min<std::chrono::milliseconds::rep>(
        remaining_count, static_cast<std::chrono::milliseconds::rep>(INT_MAX)));
    pollfd descriptor_state{.fd = descriptor, .events = events, .revents = 0};
    const int ready = ::poll(&descriptor_state, 1U, timeout_ms);
    if (ready > 0) {
      if ((descriptor_state.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
        error_message = "control queue became unavailable";
        return std::nullopt;
      }
      if ((descriptor_state.revents & events) != 0) return 0;
      continue;
    }
    if (ready == 0) {
      error_message = "control request timed out";
      return std::nullopt;
    }
    if (errno == EINTR) continue;
    error_message = error_text("poll", errno);
    return std::nullopt;
  }
}

bool valid_message_queue_attributes(const MessageQueue& queue, QueueLimits expected,
                                    std::string& error_message,
                                    const volatile sig_atomic_t* interrupted = nullptr,
                                    Clock::time_point deadline = Clock::time_point::max()) noexcept {
  mq_attr attributes{};
  int error_number = 0;
  if (!queue.get_attributes(attributes, error_number, interrupted, deadline)) {
    error_message = error_text("mq_getattr", error_number);
    return false;
  }
  if ((attributes.mq_flags & O_NONBLOCK) == 0 ||
      attributes.mq_maxmsg != expected.max_messages ||
      attributes.mq_msgsize != expected.message_size) {
    error_message = "control queue attributes do not match the protocol limits";
    return false;
  }
  return true;
}

bool valid_json_keys(const Json& request) {
  static constexpr std::array<std::string_view, 4> allowed{
      "version", "request_id", "operation", "reply_to"};
  if (request.size() != allowed.size()) return false;
  return std::all_of(allowed.begin(), allowed.end(), [&](std::string_view key) {
    return request.contains(std::string(key));
  });
}

}  // namespace

MessageQueue::MessageQueue(mqd_t descriptor, std::string name, bool owns_name) noexcept
    : descriptor_(descriptor), name_(std::move(name)), owns_name_(owns_name) {}

MessageQueue::~MessageQueue() { reset(); }

MessageQueue::MessageQueue(MessageQueue&& other) noexcept
    : descriptor_(other.descriptor_), name_(std::move(other.name_)),
      owns_name_(other.owns_name_) {
  other.descriptor_ = static_cast<mqd_t>(-1);
  other.owns_name_ = false;
}

MessageQueue& MessageQueue::operator=(MessageQueue&& other) noexcept {
  if (this != &other) {
    reset();
    descriptor_ = other.descriptor_;
    name_ = std::move(other.name_);
    owns_name_ = other.owns_name_;
    other.descriptor_ = static_cast<mqd_t>(-1);
    other.owns_name_ = false;
  }
  return *this;
}

std::optional<MessageQueue> MessageQueue::create_exclusive(
    std::string name, int access_flags, QueueLimits limits,
    std::string& error_message, mode_t permissions,
    const volatile sig_atomic_t* interrupted, Clock::time_point deadline) {
  if (!valid_name(name) || !safe_queue_access_flags(access_flags)) {
    error_message = "invalid POSIX message queue name or access mode";
    return std::nullopt;
  }
  if (limits.max_messages < 1 || limits.max_messages > kRequestQueueDepth ||
      limits.message_size < 1 || limits.message_size > kMaxMessageBytes ||
      permissions != kQueuePermissions) {
    error_message = "POSIX message queue limits or permissions are outside policy";
    return std::nullopt;
  }

  mq_attr requested{};
  requested.mq_flags = O_NONBLOCK;
  requested.mq_maxmsg = limits.max_messages;
  requested.mq_msgsize = limits.message_size;
  const int flags = access_flags | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC;
  const mqd_t descriptor = ::mq_open(name.c_str(), flags, permissions, &requested);
  if (descriptor == static_cast<mqd_t>(-1)) {
    error_message = error_text("mq_open create", errno);
    return std::nullopt;
  }

  MessageQueue queue(descriptor, std::move(name), true);
  if (!queue.set_nonblocking(error_message, interrupted, deadline)) return std::nullopt;
  if (::fchmod(queue.descriptor(), permissions) != 0) {
    error_message = error_text("fchmod message queue", errno);
    return std::nullopt;
  }
  if (!queue.has_permissions(::geteuid(), permissions)) {
    error_message = "created message queue has unexpected owner or permissions";
    return std::nullopt;
  }
  if (!valid_message_queue_attributes(queue, limits, error_message, interrupted, deadline)) {
    return std::nullopt;
  }
  return std::optional<MessageQueue>(std::move(queue));
}

std::optional<MessageQueue> MessageQueue::open_existing(
    std::string name, int access_flags, std::string& error_message,
    const volatile sig_atomic_t* interrupted, Clock::time_point deadline) {
  if (!valid_name(name) || !safe_queue_access_flags(access_flags) ||
      (access_flags & O_CREAT) != 0) {
    error_message = "invalid POSIX message queue name or access mode";
    return std::nullopt;
  }
  const int flags = access_flags | O_NONBLOCK | O_CLOEXEC;
  const mqd_t descriptor = ::mq_open(name.c_str(), flags);
  if (descriptor == static_cast<mqd_t>(-1)) {
    error_message = error_text("mq_open", errno);
    return std::nullopt;
  }
  MessageQueue queue(descriptor, std::move(name), false);
  if (!queue.set_nonblocking(error_message, interrupted, deadline)) return std::nullopt;
  return queue;
}

bool MessageQueue::send(std::string_view message, unsigned priority,
                        int& error_number,
                        const volatile sig_atomic_t* interrupted,
                        Clock::time_point deadline) const noexcept {
  if (!valid()) {
    error_number = EBADF;
    return false;
  }
  if (interrupted != nullptr && *interrupted != 0) {
    error_number = EINTR;
    return false;
  }
  mq_attr attributes{};
  while (mq_getattr_call(descriptor_, &attributes) != 0) {
    if (errno == EINTR && wait_after_eintr(descriptor_, POLLIN, interrupted, deadline,
                                           error_number)) continue;
    if (errno != EINTR) error_number = errno;
    return false;
  }
  if (message.size() > static_cast<std::size_t>(attributes.mq_msgsize)) {
    error_number = EMSGSIZE;
    return false;
  }
  while (mq_send_call(descriptor_, message.data(), message.size(), priority) != 0) {
    if (errno == EINTR && wait_after_eintr(descriptor_, POLLOUT, interrupted, deadline,
                                           error_number)) continue;
    if (errno != EINTR) error_number = errno;
    return false;
  }
  error_number = 0;
  return true;
}

std::optional<std::string> MessageQueue::receive(unsigned& priority,
                                                int& error_number,
                                                const volatile sig_atomic_t* interrupted,
                                                Clock::time_point deadline) const {
  if (!valid()) {
    error_number = EBADF;
    return std::nullopt;
  }
  if (interrupted != nullptr && *interrupted != 0) {
    error_number = EINTR;
    return std::nullopt;
  }
  if (deadline != Clock::time_point::max() && Clock::now() >= deadline) {
    error_number = ETIMEDOUT;
    return std::nullopt;
  }
  mq_attr attributes{};
  while (mq_getattr_call(descriptor_, &attributes) != 0) {
    if (errno == EINTR && wait_after_eintr(descriptor_, POLLIN, interrupted, deadline,
                                           error_number)) continue;
    if (errno != EINTR) error_number = errno;
    return std::nullopt;
  }
  if (attributes.mq_msgsize < 1 || attributes.mq_msgsize > kMaxMessageBytes) {
    error_number = EMSGSIZE;
    return std::nullopt;
  }
  std::vector<char> buffer(static_cast<std::size_t>(attributes.mq_msgsize));
  unsigned received_priority = 0U;
  ssize_t count = -1;
  do {
    count = mq_receive_call(descriptor_, buffer.data(), buffer.size(), &received_priority);
    if (count < 0 && errno == EINTR &&
        wait_after_eintr(descriptor_, POLLIN, interrupted, deadline, error_number)) {
      continue;
    }
    break;
  } while (true);
  if (count < 0) {
    if (errno != EINTR) error_number = errno;
    return std::nullopt;
  }
  priority = received_priority;
  error_number = 0;
  return std::string(buffer.data(), static_cast<std::size_t>(count));
}

bool MessageQueue::has_permissions(uid_t owner, mode_t permissions) const noexcept {
  if (!valid()) return false;
  struct stat status {};
  if (::fstat(descriptor_, &status) != 0) return false;
  return status.st_uid == owner && (status.st_mode & 0777) == permissions;
}

bool MessageQueue::get_attributes(mq_attr& attributes,
                                  int& error_number,
                                  const volatile sig_atomic_t* interrupted,
                                  Clock::time_point deadline) const noexcept {
  if (!valid()) {
    error_number = EBADF;
    return false;
  }
  while (mq_getattr_call(descriptor_, &attributes) != 0) {
    if (errno == EINTR && wait_after_eintr(descriptor_, POLLIN, interrupted, deadline,
                                           error_number)) continue;
    if (errno != EINTR) error_number = errno;
    return false;
  }
  error_number = 0;
  return true;
}

int MessageQueue::descriptor() const noexcept {
  return valid() ? static_cast<int>(descriptor_) : -1;
}

void MessageQueue::reset() noexcept {
  if (!valid()) return;
  if (owns_name_) (void)::mq_unlink(name_.c_str());
  (void)::mq_close(descriptor_);
  descriptor_ = static_cast<mqd_t>(-1);
  owns_name_ = false;
  name_.clear();
}

bool MessageQueue::valid_name(std::string_view name) noexcept {
  if (name.size() < 2U || name.size() > kMaximumQueueName || name.front() != '/') return false;
  return std::all_of(name.begin() + 1, name.end(), [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-' || value == '.';
  });
}

bool MessageQueue::set_nonblocking(std::string& error_message,
                                   const volatile sig_atomic_t* interrupted,
                                   Clock::time_point deadline) noexcept {
  mq_attr current{};
  int result = mq_getattr_call(descriptor_, &current);
  while (result != 0 && errno == EINTR) {
    int error_number = EINTR;
    if (!wait_after_eintr(descriptor_, POLLIN, interrupted, deadline, error_number)) {
      error_message = error_text("mq_getattr", error_number);
      return false;
    }
    result = mq_getattr_call(descriptor_, &current);
  }
  if (result != 0) {
    error_message = error_text("mq_getattr", errno);
    return false;
  }
  mq_attr desired{};
  desired.mq_flags = current.mq_flags | O_NONBLOCK;
  result = mq_setattr_call(descriptor_, &desired, nullptr);
  while (result != 0 && errno == EINTR) {
    int error_number = EINTR;
    if (!wait_after_eintr(descriptor_, POLLOUT, interrupted, deadline, error_number)) {
      error_message = error_text("mq_setattr", error_number);
      return false;
    }
    result = mq_setattr_call(descriptor_, &desired, nullptr);
  }
  if (result != 0) {
    error_message = error_text("mq_setattr", errno);
    return false;
  }
  result = mq_getattr_call(descriptor_, &current);
  while (result != 0 && errno == EINTR) {
    int error_number = EINTR;
    if (!wait_after_eintr(descriptor_, POLLIN, interrupted, deadline, error_number)) {
      error_message = error_text("mq_getattr", error_number);
      return false;
    }
    result = mq_getattr_call(descriptor_, &current);
  }
  if (result != 0 ||
      (current.mq_flags & O_NONBLOCK) == 0) {
    error_message = "message queue did not enter nonblocking mode";
    return false;
  }
  return true;
}

std::string ControlServer::default_queue_name() {
  return "/darkpool_ctl_" + std::to_string(::geteuid());
}

bool ControlServer::valid_server_queue_name(std::string_view name) {
  const auto base = default_queue_name();
  if (name == base) return true;
  if (name.size() <= base.size() + 1U || name.size() > kMaximumQueueName ||
      name.substr(0U, base.size()) != base || name[base.size()] != '_') {
    return false;
  }
  return std::all_of(name.begin() + static_cast<std::ptrdiff_t>(base.size() + 1U),
                     name.end(), [](char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') ||
           (value >= '0' && value <= '9') || value == '_' || value == '-';
  });
}

bool ControlServer::valid_request_id(std::string_view request_id) noexcept {
  return request_id.size() == 32U && std::all_of(request_id.begin(), request_id.end(), [](char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
  });
}

bool ControlServer::valid_reply_queue_name(std::string_view name) {
  const auto prefix = "/darkpool_ctl_reply_" + std::to_string(::geteuid()) + "_";
  if (name.size() <= prefix.size() + 1U + 32U || name.size() > kMaximumQueueName ||
      name.substr(0U, prefix.size()) != prefix) {
    return false;
  }
  const auto rest = name.substr(prefix.size());
  const auto separator = rest.find('_');
  if (separator == std::string_view::npos || separator == 0U ||
      rest.size() - separator - 1U != 32U) {
    return false;
  }
  std::uint64_t pid = 0U;
  const auto* pid_begin = rest.data();
  const auto* pid_end = rest.data() + separator;
  const auto parsed = std::from_chars(pid_begin, pid_end, pid, 10);
  return parsed.ec == std::errc{} && parsed.ptr == pid_end && pid > 0U &&
      pid <= static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max()) &&
      std::all_of(rest.begin() + static_cast<std::ptrdiff_t>(separator + 1U), rest.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
      });
}

ControlServer::ControlServer(std::string queue_name) : queue_name_(std::move(queue_name)) {}

bool ControlServer::start(std::string& error_message) {
  if (queue_.valid()) return true;
  if (!valid_server_queue_name(queue_name_)) {
    error_message = "invalid control queue name; use the per-user /darkpool_ctl_<uid> namespace";
    return false;
  }
  auto created = MessageQueue::create_exclusive(
      queue_name_, O_RDONLY, QueueLimits{kRequestQueueDepth, kMaxMessageBytes}, error_message);
  if (!created) {
    error_message = "could not acquire POSIX MQ control queue " + queue_name_ + ": " + error_message;
    return false;
  }
  queue_ = std::move(*created);
  return true;
}

void ControlServer::process_readable(const StatusProvider& status_provider) noexcept {
  if (!queue_.valid()) return;
  try {
    for (long processed = 0; processed < kMaxMessagesPerDispatch; ++processed) {
      unsigned priority = 0U;
      int error_number = 0;
      const auto message = queue_.receive(priority, error_number);
      if (!message) {
        if (error_number == EAGAIN || error_number == EWOULDBLOCK) return;
        if (error_number == EINTR) continue;
        return;
      }
      if (priority != 0U) continue;
      process_message(*message, status_provider);
    }
  } catch (...) {
    // Malformed or resource-exhausting requests must not escape the epoll loop.
  }
}

void ControlServer::process_message(std::string_view message,
                                    const StatusProvider& status_provider) {
  Json request_json = Json::parse(message.begin(), message.end(), nullptr, false);
  if (request_json.is_discarded() || !request_json.is_object()) return;
  const auto request_id_it = request_json.find("request_id");
  const auto reply_to_it = request_json.find("reply_to");
  if (request_id_it == request_json.end() || !request_id_it->is_string() ||
      reply_to_it == request_json.end() || !reply_to_it->is_string()) {
    return;
  }
  const auto request_id = request_id_it->get<std::string>();
  const auto reply_to = reply_to_it->get<std::string>();
  if (!valid_request_id(request_id) || !valid_reply_queue_name(reply_to)) return;

  const auto operation_it = request_json.find("operation");
  const std::string operation = operation_it != request_json.end() && operation_it->is_string()
      ? operation_it->get<std::string>() : std::string{};

  std::string open_error;
  auto reply = MessageQueue::open_existing(reply_to, O_WRONLY, open_error);
  if (!reply || !reply->has_permissions(::geteuid(), kQueuePermissions)) return;
  std::string attributes_error;
  if (!valid_message_queue_attributes(*reply, QueueLimits{kReplyQueueDepth, kMaxMessageBytes}, attributes_error)) return;

  auto respond_error = [&](std::string_view code) {
    send_response(*reply, error_response(request_id, operation, code));
  };

  const auto version_it = request_json.find("version");
  if (version_it == request_json.end() || !version_it->is_number_integer()) {
    respond_error("invalid_request");
    return;
  }
  const bool supported_version = version_it->is_number_unsigned()
      ? version_it->get<std::uint64_t>() == 1U
      : version_it->get<std::int64_t>() == 1;
  if (!supported_version) {
    respond_error("unsupported_version");
    return;
  }
  if (!valid_json_keys(request_json) || operation.empty()) {
    respond_error("invalid_request");
    return;
  }

  const auto cached = response_cache_.find(request_id);
  if (cached != response_cache_.end()) {
    if (cached->second.reply_queue == reply_to && cached->second.operation == operation) {
      send_response(*reply, cached->second.response);
    } else {
      respond_error("request_id_conflict");
    }
    return;
  }

  Json response;
  if (operation == "ping") {
    response = Json{{"version", 1}, {"request_id", request_id}, {"operation", operation},
                    {"ok", true}, {"result", {{"server_available", true},
                                                {"control_channel", "ready"}}}};
  } else if (operation == "status") {
    StatusSnapshot status{};
    try {
      status = status_provider();
    } catch (...) {
      response = error_response(request_id, operation, "status_unavailable");
    }
    if (response.is_null()) {
      response = Json{{"version", 1}, {"request_id", request_id}, {"operation", operation},
                      {"ok", true}, {"result", {{"server_available", true},
                                                  {"active_matches", status.active_matches},
                                                  {"connected_players", status.connected_players},
                                                  {"match_capacity", status.match_capacity},
                                                  {"players_per_match_capacity", status.players_per_match_capacity},
                                                  {"player_connection_capacity", status.player_connection_capacity},
                                                  {"control_channel", "ready"},
                                                  {"transport", "posix_mq_epoll"}}}};
    }
  } else {
    response = error_response(request_id, operation, "unsupported_operation");
  }

  response_cache_.insert_or_assign(request_id, CachedResponse{reply_to, operation, response});
  response_order_.push_back(request_id);
  while (response_order_.size() > kRequestCacheCapacity) {
    response_cache_.erase(response_order_.front());
    response_order_.pop_front();
  }
  send_response(*reply, response);
}

void ControlServer::send_response(MessageQueue& reply_queue,
                                  const Json& response) noexcept {
  try {
    const auto encoded = response.dump();
    if (encoded.size() > static_cast<std::size_t>(kMaxMessageBytes)) return;
    int error_number = 0;
    (void)reply_queue.send(encoded, 0U, error_number);
  } catch (...) {
    // A reply queue may be gone/full; control-plane failures must not escape the event loop.
  }
}

Json ControlServer::error_response(std::string_view request_id,
                                   std::string_view operation,
                                   std::string_view error_code) {
  return Json{{"version", 1}, {"request_id", request_id}, {"operation", operation},
              {"ok", false}, {"error", error_code}};
}

std::optional<ClientResponse> request(
    std::string_view server_queue_name, std::string_view operation,
    std::chrono::milliseconds timeout, std::string& error_message,
    const volatile sig_atomic_t* interrupted) {
  if (!ControlServer::valid_server_queue_name(server_queue_name) ||
      (operation != "ping" && operation != "status") || timeout <= std::chrono::milliseconds::zero() ||
      timeout > std::chrono::seconds(60)) {
    error_message = "invalid control request parameters";
    return std::nullopt;
  }
  const auto request_id = create_request_id();
  if (!request_id) {
    error_message = "could not generate a request identifier";
    return std::nullopt;
  }

  const auto reply_name = "/darkpool_ctl_reply_" + std::to_string(::geteuid()) + "_" +
      std::to_string(::getpid()) + "_" + *request_id;
  const auto deadline = Clock::now() + timeout;
  if (interrupted != nullptr && *interrupted != 0) {
    error_message = "interrupted";
    return std::nullopt;
  }
  auto reply = MessageQueue::create_exclusive(
      reply_name, O_RDONLY, QueueLimits{kReplyQueueDepth, kMaxMessageBytes}, error_message,
      0600, interrupted, deadline);
  if (!reply) return std::nullopt;

  auto server = MessageQueue::open_existing(std::string(server_queue_name), O_WRONLY,
                                            error_message, interrupted, deadline);
  if (!server) return std::nullopt;
  if (!server->has_permissions(::geteuid(), kQueuePermissions)) {
    error_message = "server queue owner or permissions do not match the local-user policy";
    return std::nullopt;
  }
  if (!valid_message_queue_attributes(*server,
                                      QueueLimits{kRequestQueueDepth, kMaxMessageBytes},
                                      error_message, interrupted, deadline)) {
    return std::nullopt;
  }

  const Json request_message{{"version", 1}, {"request_id", *request_id},
                             {"operation", operation}, {"reply_to", reply_name}};
  const auto encoded = request_message.dump();
  while (true) {
    int error_number = 0;
    if (server->send(encoded, 0U, error_number, interrupted, deadline)) break;
    if (error_number == EINTR && interrupted != nullptr && *interrupted != 0) {
      error_message = "interrupted";
      return std::nullopt;
    }
    if (error_number != EAGAIN && error_number != EWOULDBLOCK) {
      error_message = error_text("mq_send", error_number);
      return std::nullopt;
    }
    if (!wait_for_queue(server->descriptor(), POLLOUT, deadline, interrupted, error_message)) {
      return std::nullopt;
    }
  }

  if (!wait_for_queue(reply->descriptor(), POLLIN, deadline, interrupted, error_message)) {
    return std::nullopt;
  }
  unsigned priority = 0U;
  int error_number = 0;
  std::optional<std::string> reply_message;
  while (true) {
    reply_message = reply->receive(priority, error_number, interrupted, deadline);
    if (reply_message) break;
    if (error_number == EINTR && interrupted != nullptr && *interrupted != 0) {
      error_message = "interrupted";
      return std::nullopt;
    }
    if (error_number != EAGAIN && error_number != EWOULDBLOCK && error_number != EINTR) {
      error_message = error_text("mq_receive reply", error_number);
      return std::nullopt;
    }
    if (!wait_for_queue(reply->descriptor(), POLLIN, deadline, interrupted, error_message)) {
      return std::nullopt;
    }
  }
  Json parsed = Json::parse(reply_message->begin(), reply_message->end(), nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object() || parsed.size() != 5U ||
      !parsed.contains("version") || !parsed["version"].is_number_integer() ||
      parsed["version"].get<std::int64_t>() != 1 ||
      !parsed.contains("request_id") || !parsed["request_id"].is_string() ||
      parsed["request_id"].get<std::string>() != *request_id ||
      !parsed.contains("operation") || !parsed["operation"].is_string() ||
      parsed["operation"].get<std::string>() != operation ||
      !parsed.contains("ok") || !parsed["ok"].is_boolean() ||
      (parsed["ok"].get<bool>() && (!parsed.contains("result") || parsed.contains("error"))) ||
      (!parsed["ok"].get<bool>() && (!parsed.contains("error") || !parsed["error"].is_string() || parsed.contains("result")))) {
    error_message = "malformed or mismatched control response";
    return std::nullopt;
  }
  return ClientResponse{std::move(parsed), *request_id};
}

namespace testing {

void set_mq_syscall_hooks(MqGetattrHook getattr, MqSetattrHook setattr,
                          MqSendHook send, MqReceiveHook receive) noexcept {
  mq_hooks = MqSyscallHooks{getattr, setattr, send, receive};
}

void reset_mq_syscall_hooks() noexcept {
  mq_hooks = MqSyscallHooks{};
}

}  // namespace testing

}  // namespace darkpool::control

#endif  // defined(__linux__)
