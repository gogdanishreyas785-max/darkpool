#if defined(__linux__)

#include "darkpool/server_worker_channel.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace darkpool::ipc {

ServerWorkerChannel::ServerWorkerChannel(ServerWorkerChannelConfig config,
                                         ProcessSharedQueue request_queue,
                                         ProcessSharedQueue response_queue,
                                         EventNotifier worker_notifier,
                                         EventNotifier server_notifier)
    : config_(std::move(config)),
      request_queue_(std::move(request_queue)),
      response_queue_(std::move(response_queue)),
      worker_notifier_(std::move(worker_notifier)),
      server_notifier_(std::move(server_notifier)) {}

ServerWorkerChannel::~ServerWorkerChannel() {
  close_channel();
}

ServerWorkerChannel::ServerWorkerChannel(ServerWorkerChannel&& other) noexcept
    : config_(std::move(other.config_)),
      request_queue_(std::move(other.request_queue_)),
      response_queue_(std::move(other.response_queue_)),
      worker_notifier_(std::move(other.worker_notifier_)),
      server_notifier_(std::move(other.server_notifier_)) {}

ServerWorkerChannel& ServerWorkerChannel::operator=(ServerWorkerChannel&& other) noexcept {
  if (this != &other) {
    close_channel();
    config_ = std::move(other.config_);
    request_queue_ = std::move(other.request_queue_);
    response_queue_ = std::move(other.response_queue_);
    worker_notifier_ = std::move(other.worker_notifier_);
    server_notifier_ = std::move(other.server_notifier_);
  }
  return *this;
}

std::optional<ServerWorkerChannel> ServerWorkerChannel::create(
    ServerWorkerChannelConfig config,
    std::string& error_msg) {
  // 1. Unlink any stale objects first
  ProcessSharedQueue::unlink(config.request_shm_name);
  ProcessSharedQueue::unlink(config.response_shm_name);

  // 2. Create request queue as Producer
  auto req_q = ProcessSharedQueue::create(
      config.request_shm_name, config.queue_capacity, QueueEndpointRole::Producer, error_msg);
  if (!req_q.has_value()) {
    return std::nullopt;
  }

  // 3. Create response queue as Consumer
  auto resp_q = ProcessSharedQueue::create(
      config.response_shm_name, config.queue_capacity, QueueEndpointRole::Consumer, error_msg);
  if (!resp_q.has_value()) {
    ProcessSharedQueue::unlink(config.request_shm_name);
    return std::nullopt;
  }

  // 4. Create EventNotifiers
  auto w_notif = EventNotifier::create(&error_msg);
  if (!w_notif.has_value()) {
    ProcessSharedQueue::unlink(config.request_shm_name);
    ProcessSharedQueue::unlink(config.response_shm_name);
    return std::nullopt;
  }

  auto s_notif = EventNotifier::create(&error_msg);
  if (!s_notif.has_value()) {
    ProcessSharedQueue::unlink(config.request_shm_name);
    ProcessSharedQueue::unlink(config.response_shm_name);
    return std::nullopt;
  }

  return ServerWorkerChannel(std::move(config),
                             std::move(*req_q),
                             std::move(*resp_q),
                             std::move(*w_notif),
                             std::move(*s_notif));
}

QueueOpStatus ServerWorkerChannel::send_request(const WorkerRequestMessage& request,
                                                std::chrono::milliseconds timeout,
                                                std::string* error_msg) {
  if (!request_queue_.has_value()) {
    if (error_msg) *error_msg = "Request queue is closed or not initialized";
    return QueueOpStatus::Closed;
  }

  const QueueOpStatus st = request_queue_->push(&request, sizeof(request), timeout, error_msg);
  if (st == QueueOpStatus::Success) {
    // Signal worker
    (void)worker_notifier_.notify(1);
  }
  return st;
}

std::size_t ServerWorkerChannel::drain_responses(const WorkerResponseHandler& handler) {
  if (!response_queue_.has_value()) {
    return 0;
  }

  std::size_t drained_count = 0;
  while (true) {
    WorkerResponseMessage resp{};
    std::size_t out_sz = 0;
    // Nonblocking check with 0ms timeout
    const QueueOpStatus st = response_queue_->pop(&resp, sizeof(resp), out_sz, nullptr,
                                                  std::chrono::milliseconds{0});
    if (st == QueueOpStatus::Success) {
      if (out_sz == sizeof(resp) && handler) {
        handler(resp);
      }
      ++drained_count;
    } else {
      break;
    }
  }
  return drained_count;
}

void ServerWorkerChannel::close_channel() noexcept {
  if (request_queue_.has_value()) {
    request_queue_->close();
    request_queue_.reset();
  }
  if (response_queue_.has_value()) {
    response_queue_.reset();
  }
  worker_notifier_.close();
  server_notifier_.close();

  if (!config_.request_shm_name.empty()) {
    ProcessSharedQueue::unlink(config_.request_shm_name);
  }
  if (!config_.response_shm_name.empty()) {
    ProcessSharedQueue::unlink(config_.response_shm_name);
  }
}

bool ServerWorkerChannel::is_faulted() const noexcept {
  return (request_queue_.has_value() && request_queue_->is_faulted()) ||
         (response_queue_.has_value() && response_queue_->is_faulted());
}

// -----------------------------------------------------------------------------
// WorkerClientChannel implementation
// -----------------------------------------------------------------------------

WorkerClientChannel::WorkerClientChannel(ProcessSharedQueue req_q,
                                         ProcessSharedQueue resp_q,
                                         EventNotifier worker_notifier,
                                         EventNotifier server_notifier)
    : request_queue_(std::move(req_q)),
      response_queue_(std::move(resp_q)),
      worker_notifier_(std::move(worker_notifier)),
      server_notifier_(std::move(server_notifier)) {}

WorkerClientChannel::~WorkerClientChannel() {
  close();
}

WorkerClientChannel::WorkerClientChannel(WorkerClientChannel&& other) noexcept
    : request_queue_(std::move(other.request_queue_)),
      response_queue_(std::move(other.response_queue_)),
      worker_notifier_(std::move(other.worker_notifier_)),
      server_notifier_(std::move(other.server_notifier_)) {}

WorkerClientChannel& WorkerClientChannel::operator=(WorkerClientChannel&& other) noexcept {
  if (this != &other) {
    close();
    request_queue_ = std::move(other.request_queue_);
    response_queue_ = std::move(other.response_queue_);
    worker_notifier_ = std::move(other.worker_notifier_);
    server_notifier_ = std::move(other.server_notifier_);
  }
  return *this;
}

std::optional<WorkerClientChannel> WorkerClientChannel::open(
    std::string_view req_shm,
    std::string_view resp_shm,
    int worker_eventfd,
    int server_eventfd,
    std::string& error_msg,
    std::chrono::milliseconds wait_timeout) {
  // Worker opens request queue as Consumer
  auto req_q = ProcessSharedQueue::open_existing(req_shm, QueueEndpointRole::Consumer, error_msg, wait_timeout);
  if (!req_q.has_value()) {
    return std::nullopt;
  }

  // Worker opens response queue as Producer
  auto resp_q = ProcessSharedQueue::open_existing(resp_shm, QueueEndpointRole::Producer, error_msg, wait_timeout);
  if (!resp_q.has_value()) {
    return std::nullopt;
  }

  // Wrap passed eventfd descriptors. Note: duplicating or using directly.
  int dup_w_fd = ::fcntl(worker_eventfd, F_DUPFD_CLOEXEC, 0);
  if (dup_w_fd < 0) {
    error_msg = "fcntl F_DUPFD_CLOEXEC failed on worker_eventfd";
    return std::nullopt;
  }
  int dup_s_fd = ::fcntl(server_eventfd, F_DUPFD_CLOEXEC, 0);
  if (dup_s_fd < 0) {
    ::close(dup_w_fd);
    error_msg = "fcntl F_DUPFD_CLOEXEC failed on server_eventfd";
    return std::nullopt;
  }

  // Wrap in EventNotifiers (using constructor taking existing fd)
  // EventNotifier::EventNotifier(int fd) is private; let's check or make a static helper/friend
  // Wait, let's check EventNotifier constructors in event_notifier.hpp!
  return WorkerClientChannel(std::move(*req_q),
                             std::move(*resp_q),
                             EventNotifier::from_fd(dup_w_fd),
                             EventNotifier::from_fd(dup_s_fd));
}

QueueOpStatus WorkerClientChannel::pop_request(WorkerRequestMessage& req,
                                               std::chrono::milliseconds timeout,
                                               std::string* error_msg) {
  if (!request_queue_.has_value()) {
    if (error_msg) *error_msg = "Request queue is not open";
    return QueueOpStatus::Closed;
  }

  std::size_t out_sz = 0;
  const QueueOpStatus st = request_queue_->pop(&req, sizeof(req), out_sz, nullptr, timeout, error_msg);
  if (st == QueueOpStatus::Success && out_sz != sizeof(req)) {
    if (error_msg) *error_msg = "Received malformed request message size";
    return QueueOpStatus::Faulted;
  }
  return st;
}

QueueOpStatus WorkerClientChannel::send_response(const WorkerResponseMessage& resp,
                                                 std::chrono::milliseconds timeout,
                                                 std::string* error_msg) {
  if (!response_queue_.has_value()) {
    if (error_msg) *error_msg = "Response queue is not open";
    return QueueOpStatus::Closed;
  }

  const QueueOpStatus st = response_queue_->push(&resp, sizeof(resp), timeout, error_msg);
  if (st == QueueOpStatus::Success) {
    (void)server_notifier_.notify(1);
  }
  return st;
}

void WorkerClientChannel::close() noexcept {
  if (response_queue_.has_value()) {
    response_queue_->close();
    response_queue_.reset();
  }
  if (request_queue_.has_value()) {
    request_queue_.reset();
  }
  worker_notifier_.close();
  server_notifier_.close();
}

bool WorkerClientChannel::is_faulted() const noexcept {
  return (request_queue_.has_value() && request_queue_->is_faulted()) ||
         (response_queue_.has_value() && response_queue_->is_faulted());
}

}  // namespace darkpool::ipc

#endif  // defined(__linux__)
