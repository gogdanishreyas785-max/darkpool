#pragma once

#if defined(__linux__)

#include "darkpool/server_worker_channel.hpp"
#include "darkpool/worker_protocol.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <sys/types.h>

namespace darkpool::ipc {

struct WorkerProcessConfig {
  std::string worker_binary_path;
  std::string request_shm_name;
  std::string response_shm_name;
  std::size_t queue_capacity{kWorkerQueueCapacity};
};

class WorkerProcessManager {
 public:
  ~WorkerProcessManager();

  WorkerProcessManager(const WorkerProcessManager&) = delete;
  WorkerProcessManager& operator=(const WorkerProcessManager&) = delete;
  WorkerProcessManager(WorkerProcessManager&&) noexcept;
  WorkerProcessManager& operator=(WorkerProcessManager&&) noexcept;

  static std::optional<WorkerProcessManager> launch(
      WorkerProcessConfig config,
      std::string& error_msg);

  [[nodiscard]] pid_t worker_pid() const noexcept { return worker_pid_; }
  [[nodiscard]] bool is_alive() noexcept;

  // File descriptor for ServerRuntime epoll loop registration (server receives notifications on this fd)
  [[nodiscard]] int server_notify_fd() const noexcept {
    return channel_ ? channel_->server_notify_fd() : -1;
  }

  [[nodiscard]] const WorkerProcessConfig& config() const noexcept { return config_; }

  // Submit request to worker
  QueueOpStatus send_request(const WorkerRequestMessage& request,
                             std::chrono::milliseconds timeout = std::chrono::milliseconds{100},
                             std::string* error_msg = nullptr) {
    if (!channel_) return QueueOpStatus::Closed;
    return channel_->send_request(request, timeout, error_msg);
  }

  // Drain responses
  std::size_t drain_responses(const WorkerResponseHandler& handler) {
    if (!channel_) return 0;
    return channel_->drain_responses(handler);
  }

  // Consume notification counter in epoll callback
  std::optional<std::uint64_t> consume_server_notification() noexcept {
    if (!channel_) return std::nullopt;
    return channel_->consume_server_notification();
  }

  // Gracefully shutdown worker: send Shutdown opcode, wait boundedly, kill if stubborn, reap PID
  bool shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds{2000});

  // Reaps zombie child process nonblocking
  bool try_reap(int* exit_code = nullptr);

  // Check if channel is faulted
  [[nodiscard]] bool is_faulted() const noexcept {
    return channel_ ? channel_->is_faulted() : true;
  }

 private:
  WorkerProcessManager(WorkerProcessConfig config,
                       pid_t pid,
                       ServerWorkerChannel channel);

  WorkerProcessConfig config_;
  pid_t worker_pid_{-1};
  std::optional<ServerWorkerChannel> channel_;
};

}  // namespace darkpool::ipc

#endif  // defined(__linux__)

