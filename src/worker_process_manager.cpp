#if defined(__linux__)

#include "darkpool/worker_process_manager.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace darkpool::ipc {

WorkerProcessManager::WorkerProcessManager(WorkerProcessConfig config,
                                           pid_t pid,
                                           ServerWorkerChannel channel)
    : config_(std::move(config)),
      worker_pid_(pid),
      channel_(std::move(channel)) {}

WorkerProcessManager::~WorkerProcessManager() {
  shutdown(std::chrono::milliseconds{1000});
}

WorkerProcessManager::WorkerProcessManager(WorkerProcessManager&& other) noexcept
    : config_(std::move(other.config_)),
      worker_pid_(other.worker_pid_),
      channel_(std::move(other.channel_)) {
  other.worker_pid_ = -1;
}

WorkerProcessManager& WorkerProcessManager::operator=(WorkerProcessManager&& other) noexcept {
  if (this != &other) {
    shutdown(std::chrono::milliseconds{1000});
    config_ = std::move(other.config_);
    worker_pid_ = other.worker_pid_;
    channel_ = std::move(other.channel_);
    other.worker_pid_ = -1;
  }
  return *this;
}

std::optional<WorkerProcessManager> WorkerProcessManager::launch(
    WorkerProcessConfig config,
    std::string& error_msg) {
  // 1. Create ServerWorkerChannel
  ServerWorkerChannelConfig chan_cfg{
      .request_shm_name = config.request_shm_name,
      .response_shm_name = config.response_shm_name,
      .queue_capacity = config.queue_capacity
  };

  auto channel = ServerWorkerChannel::create(std::move(chan_cfg), error_msg);
  if (!channel.has_value()) {
    return std::nullopt;
  }

  const int w_efd = channel->worker_notify_fd();
  const int s_efd = channel->server_notify_fd();

  // Pipe for child exec status reporting
  int exec_pipe[2] = {-1, -1};
  if (::pipe2(exec_pipe, O_CLOEXEC) != 0) {
    error_msg = "pipe2 failed: " + std::string(std::strerror(errno));
    return std::nullopt;
  }

  // 2. Fork worker process
  const pid_t pid = ::fork();
  if (pid < 0) {
    error_msg = "fork failed: " + std::string(std::strerror(errno));
    ::close(exec_pipe[0]);
    ::close(exec_pipe[1]);
    return std::nullopt;
  }

  if (pid == 0) {
    // Child process
    ::close(exec_pipe[0]);

    // Clear O_CLOEXEC on eventfd descriptors so they survive execv
    int flags = ::fcntl(w_efd, F_GETFD);
    if (flags >= 0) ::fcntl(w_efd, F_SETFD, flags & ~FD_CLOEXEC);
    flags = ::fcntl(s_efd, F_GETFD);
    if (flags >= 0) ::fcntl(s_efd, F_SETFD, flags & ~FD_CLOEXEC);

    const std::string w_efd_str = std::to_string(w_efd);
    const std::string s_efd_str = std::to_string(s_efd);

    ::execl(config.worker_binary_path.c_str(),
            config.worker_binary_path.c_str(),
            "--request-shm", config.request_shm_name.c_str(),
            "--response-shm", config.response_shm_name.c_str(),
            "--worker-eventfd", w_efd_str.c_str(),
            "--server-eventfd", s_efd_str.c_str(),
            static_cast<char*>(nullptr));

    // If exec failed:
    const int err = errno;
    ssize_t written = ::write(exec_pipe[1], &err, sizeof(err));
    (void)written;
    ::close(exec_pipe[1]);
    ::_exit(127);
  }

  // Parent process
  ::close(exec_pipe[1]);

  // Check if exec failed
  int child_errno = 0;
  const ssize_t r = ::read(exec_pipe[0], &child_errno, sizeof(child_errno));
  ::close(exec_pipe[0]);

  if (r > 0 && child_errno != 0) {
    error_msg = "Child exec failed: " + std::string(std::strerror(child_errno));
    int status = 0;
    ::waitpid(pid, &status, 0);
    return std::nullopt;
  }

  WorkerProcessManager mgr(std::move(config), pid, std::move(*channel));

  // 3. Genuine readiness handshake: wait boundedly for Ready message from worker
  bool ready_received = false;
  const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{3000};
  while (std::chrono::steady_clock::now() < ready_deadline) {
    if (!mgr.is_alive()) {
      error_msg = "Worker process exited before completing readiness handshake";
      mgr.shutdown(std::chrono::milliseconds{500});
      return std::nullopt;
    }

    std::size_t drained = mgr.drain_responses([&](const WorkerResponseMessage& resp) {
      if (resp.opcode == static_cast<std::int32_t>(WorkerOpcode::Ready)) {
        ready_received = true;
      }
    });

    if (ready_received) {
      break;
    }

    if (drained == 0) {
      struct timespec ts{0, 10'000'000L}; // 10ms
      ::nanosleep(&ts, nullptr);
    }
  }

  if (!ready_received) {
    error_msg = "Worker readiness handshake timed out";
    mgr.shutdown(std::chrono::milliseconds{500});
    return std::nullopt;
  }

  return mgr;
}

bool WorkerProcessManager::is_alive() noexcept {
  if (worker_pid_ <= 0) return false;
  int status = 0;
  const pid_t rc = ::waitpid(worker_pid_, &status, WNOHANG);
  if (rc == 0) {
    return true;  // Still running
  }
  if (rc == worker_pid_) {
    // Process has exited and was reaped by waitpid!
    // Never leave a stale PID.
    worker_pid_ = -1;
    return false;
  }
  if (rc < 0 && errno == ECHILD) {
    // Already reaped or not a child
    worker_pid_ = -1;
    return false;
  }
  return false;
}

bool WorkerProcessManager::try_reap(int* exit_code) {
  if (worker_pid_ <= 0) return false;
  int status = 0;
  const pid_t rc = ::waitpid(worker_pid_, &status, WNOHANG);
  if (rc == worker_pid_) {
    if (exit_code) {
      if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        *exit_code = 128 + WTERMSIG(status);
      } else {
        *exit_code = -1;
      }
    }
    worker_pid_ = -1;
    return true;
  }
  return false;
}

bool WorkerProcessManager::shutdown(std::chrono::milliseconds timeout) {
  if (worker_pid_ <= 0) {
    return true;
  }

  // 1. Try sending graceful Shutdown opcode if channel is operational
  if (channel_ && !channel_->is_faulted()) {
    WorkerRequestMessage shutdown_req{};
    shutdown_req.opcode = static_cast<std::int32_t>(WorkerOpcode::Shutdown);
    channel_->send_request(shutdown_req, std::chrono::milliseconds{50});
  }

  // 2. Bounded poll for process exit
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (try_reap()) {
      if (channel_) channel_->close_channel();
      return true;
    }
    struct timespec req{0, 10'000'000L};  // 10ms
    ::nanosleep(&req, nullptr);
  }

  // 3. Stubborn process: send SIGTERM
  ::kill(worker_pid_, SIGTERM);
  const auto kill_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{500};
  while (std::chrono::steady_clock::now() < kill_deadline) {
    if (try_reap()) {
      if (channel_) channel_->close_channel();
      return true;
    }
    struct timespec req{0, 10'000'000L};  // 10ms
    ::nanosleep(&req, nullptr);
  }

  // 4. Force kill with SIGKILL if still alive
  ::kill(worker_pid_, SIGKILL);
  int status = 0;
  ::waitpid(worker_pid_, &status, 0);
  worker_pid_ = -1;

  if (channel_) {
    channel_->close_channel();
  }
  return true;
}

}  // namespace darkpool::ipc

#endif  // defined(__linux__)

