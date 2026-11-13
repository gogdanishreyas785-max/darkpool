#pragma once

#if defined(__linux__)

#include "darkpool/event_notifier.hpp"
#include "darkpool/process_shared_queue.hpp"
#include "darkpool/worker_protocol.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace darkpool::ipc {

using WorkerResponseHandler = std::function<void(const WorkerResponseMessage&)>;

struct ServerWorkerChannelConfig {
  std::string request_shm_name;
  std::string response_shm_name;
  std::size_t queue_capacity{kWorkerQueueCapacity};
};

class ServerWorkerChannel {
 public:
  ~ServerWorkerChannel();

  ServerWorkerChannel(const ServerWorkerChannel&) = delete;
  ServerWorkerChannel& operator=(const ServerWorkerChannel&) = delete;
  ServerWorkerChannel(ServerWorkerChannel&&) noexcept;
  ServerWorkerChannel& operator=(ServerWorkerChannel&&) noexcept;

  // Server-side initialization: creates request queue (Producer) and response queue (Consumer)
  // and initializes eventfd notification primitives.
  static std::optional<ServerWorkerChannel> create(
      ServerWorkerChannelConfig config,
      std::string& error_msg);

  // Cross-process eventfd descriptors for worker communication
  [[nodiscard]] int worker_notify_fd() const noexcept { return worker_notifier_.descriptor(); }
  [[nodiscard]] int server_notify_fd() const noexcept { return server_notifier_.descriptor(); }

  // Submit request from server to worker
  QueueOpStatus send_request(const WorkerRequestMessage& request,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{100},
                             std::string* error_msg = nullptr);

  // Consume eventfd notification on server side
  std::optional<std::uint64_t> consume_server_notification() noexcept {
    return server_notifier_.try_consume();
  }

  // Drain responses from response queue
  std::size_t drain_responses(const WorkerResponseHandler& handler);

  // Graceful shutdown signaling
  void close_channel() noexcept;

  [[nodiscard]] bool is_faulted() const noexcept;
  [[nodiscard]] const std::string& request_shm_name() const noexcept { return config_.request_shm_name; }
  [[nodiscard]] const std::string& response_shm_name() const noexcept { return config_.response_shm_name; }

 private:
  ServerWorkerChannel(ServerWorkerChannelConfig config,
                      ProcessSharedQueue request_queue,
                      ProcessSharedQueue response_queue,
                      EventNotifier worker_notifier,
                      EventNotifier server_notifier);

  ServerWorkerChannelConfig config_;
  std::optional<ProcessSharedQueue> request_queue_;
  std::optional<ProcessSharedQueue> response_queue_;
  EventNotifier worker_notifier_;
  EventNotifier server_notifier_;
};

// Worker-side connection endpoint
class WorkerClientChannel {
 public:
  ~WorkerClientChannel();

  WorkerClientChannel(const WorkerClientChannel&) = delete;
  WorkerClientChannel& operator=(const WorkerClientChannel&) = delete;
  WorkerClientChannel(WorkerClientChannel&&) noexcept;
  WorkerClientChannel& operator=(WorkerClientChannel&&) noexcept;

  static std::optional<WorkerClientChannel> open(
      std::string_view req_shm,
      std::string_view resp_shm,
      int worker_eventfd,
      int server_eventfd,
      std::string& error_msg,
      std::chrono::milliseconds wait_timeout = std::chrono::milliseconds{5000});

  [[nodiscard]] int worker_eventfd() const noexcept { return worker_notifier_.descriptor(); }

  std::optional<std::uint64_t> consume_worker_notification() noexcept {
    return worker_notifier_.try_consume();
  }

  QueueOpStatus pop_request(WorkerRequestMessage& req,
                            std::chrono::milliseconds timeout = std::chrono::milliseconds{5000},
                            std::string* error_msg = nullptr);

  QueueOpStatus send_response(const WorkerResponseMessage& resp,
                              std::chrono::milliseconds timeout = std::chrono::milliseconds{1000},
                              std::string* error_msg = nullptr);

  void close() noexcept;
  [[nodiscard]] bool is_faulted() const noexcept;

 private:
  WorkerClientChannel(ProcessSharedQueue req_q,
                      ProcessSharedQueue resp_q,
                      EventNotifier worker_notifier,
                      EventNotifier server_notifier);

  std::optional<ProcessSharedQueue> request_queue_;
  std::optional<ProcessSharedQueue> response_queue_;
  EventNotifier worker_notifier_;
  EventNotifier server_notifier_;
};

}  // namespace darkpool::ipc

#endif  // defined(__linux__)

