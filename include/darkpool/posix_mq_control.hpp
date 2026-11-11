#pragma once

#if defined(__linux__)

#include "darkpool/network.hpp"

#include <chrono>
#include <csignal>
#include <deque>
#include <functional>
#include <map>
#include <mqueue.h>
#include <optional>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace darkpool::control {

inline constexpr long kRequestQueueDepth = 10;
inline constexpr long kReplyQueueDepth = 1;
inline constexpr long kMaxMessageBytes = 1024;
inline constexpr long kMaxMessagesPerDispatch = kRequestQueueDepth;
inline constexpr std::size_t kRequestCacheCapacity = 256;

struct QueueLimits {
  long max_messages{kRequestQueueDepth};
  long message_size{kMaxMessageBytes};
};

class MessageQueue {
 public:
  MessageQueue() noexcept = default;
  ~MessageQueue();

  MessageQueue(const MessageQueue&) = delete;
  MessageQueue& operator=(const MessageQueue&) = delete;
  MessageQueue(MessageQueue&& other) noexcept;
  MessageQueue& operator=(MessageQueue&& other) noexcept;

  [[nodiscard]] static std::optional<MessageQueue> create_exclusive(
      std::string name, int access_flags, QueueLimits limits,
      std::string& error_message, mode_t permissions = 0600,
      const volatile sig_atomic_t* interrupted = nullptr,
      std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::time_point::max());
  [[nodiscard]] static std::optional<MessageQueue> open_existing(
      std::string name, int access_flags, std::string& error_message,
      const volatile sig_atomic_t* interrupted = nullptr,
      std::chrono::steady_clock::time_point deadline =
          std::chrono::steady_clock::time_point::max());

  [[nodiscard]] bool send(std::string_view message, unsigned priority,
                          int& error_number,
                          const volatile sig_atomic_t* interrupted = nullptr,
                          std::chrono::steady_clock::time_point deadline =
                              std::chrono::steady_clock::time_point::max()) const noexcept;
  [[nodiscard]] std::optional<std::string> receive(unsigned& priority,
                                                   int& error_number,
                                                   const volatile sig_atomic_t* interrupted = nullptr,
                                                   std::chrono::steady_clock::time_point deadline =
                                                       std::chrono::steady_clock::time_point::max()) const;
  [[nodiscard]] bool has_permissions(uid_t owner, mode_t permissions) const noexcept;
  [[nodiscard]] bool get_attributes(mq_attr& attributes,
                                    int& error_number,
                                    const volatile sig_atomic_t* interrupted = nullptr,
                                    std::chrono::steady_clock::time_point deadline =
                                        std::chrono::steady_clock::time_point::max()) const noexcept;
  [[nodiscard]] int descriptor() const noexcept;
  [[nodiscard]] mqd_t native_handle() const noexcept { return descriptor_; }
  [[nodiscard]] const std::string& name() const noexcept { return name_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ != static_cast<mqd_t>(-1); }

  void reset() noexcept;

  [[nodiscard]] static bool valid_name(std::string_view name) noexcept;

 private:
  MessageQueue(mqd_t descriptor, std::string name, bool owns_name) noexcept;
  [[nodiscard]] bool set_nonblocking(
      std::string& error_message, const volatile sig_atomic_t* interrupted,
      std::chrono::steady_clock::time_point deadline) noexcept;

  mqd_t descriptor_{static_cast<mqd_t>(-1)};
  std::string name_;
  bool owns_name_{false};
};

struct StatusSnapshot {
  std::size_t active_matches{};
  std::size_t connected_players{};
  std::size_t match_capacity{4};
  std::size_t players_per_match_capacity{64};
  std::size_t player_connection_capacity{256};
};

class ControlServer {
 public:
  using StatusProvider = std::function<StatusSnapshot()>;

  explicit ControlServer(std::string queue_name = default_queue_name());
  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  [[nodiscard]] bool start(std::string& error_message);
  [[nodiscard]] const std::string& queue_name() const noexcept { return queue_name_; }
  [[nodiscard]] int descriptor() const noexcept { return queue_.descriptor(); }
  bool process_readable(const StatusProvider& status_provider) noexcept;

  [[nodiscard]] static std::string default_queue_name();
  [[nodiscard]] static bool valid_server_queue_name(std::string_view name);
  [[nodiscard]] static bool valid_request_id(std::string_view request_id) noexcept;
  [[nodiscard]] static bool valid_reply_queue_name(std::string_view name);

 private:
  struct CachedResponse {
    std::string reply_queue;
    std::string operation;
    network::json response;
  };

  [[nodiscard]] bool process_message(std::string_view message,
                                     const StatusProvider& status_provider);
  [[nodiscard]] bool send_response(MessageQueue& reply_queue,
                                   const network::json& response) noexcept;
  [[nodiscard]] static network::json error_response(
      std::string_view request_id, std::string_view operation,
      std::string_view error_code);

  std::string queue_name_;
  MessageQueue queue_;
  std::map<std::string, CachedResponse> response_cache_;
  std::deque<std::string> response_order_;
};

struct ClientResponse {
  network::json message;
  std::string request_id;
};

[[nodiscard]] std::optional<ClientResponse> request(
    std::string_view server_queue_name, std::string_view operation,
    std::chrono::milliseconds timeout, std::string& error_message,
    const volatile sig_atomic_t* interrupted = nullptr);

}  // namespace darkpool::control

namespace darkpool::control::testing {

using MqGetattrHook = int (*)(mqd_t, struct mq_attr*);
using MqSetattrHook = int (*)(mqd_t, const struct mq_attr*, struct mq_attr*);
using MqSendHook = int (*)(mqd_t, const char*, std::size_t, unsigned);
using MqReceiveHook = ssize_t (*)(mqd_t, char*, std::size_t, unsigned*);
using MqRetryWaitHook = int (*)(std::chrono::nanoseconds, std::chrono::nanoseconds*);
using MqRetryClockHook = std::chrono::steady_clock::time_point (*)();

void set_mq_syscall_hooks(MqGetattrHook getattr, MqSetattrHook setattr,
                          MqSendHook send, MqReceiveHook receive,
                          MqRetryWaitHook retry_wait = nullptr,
                          MqRetryClockHook retry_clock = nullptr) noexcept;
void reset_mq_syscall_hooks() noexcept;

}  // namespace darkpool::control::testing

#endif  // defined(__linux__)
