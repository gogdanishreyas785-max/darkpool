#if defined(__linux__)

#include "darkpool/posix_shm_telemetry.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace darkpool::telemetry {

std::string default_shm_name(uid_t uid) {
  return "/darkpool_telemetry_" + std::to_string(uid);
}

std::string default_sem_name(uid_t uid) {
  return "/darkpool_telemetry_notify_" + std::to_string(uid);
}

bool valid_ipc_name(std::string_view name) noexcept {
  if (name.size() < 2U || name.size() > 255U) {
    return false;
  }
  if (name.front() != '/') {
    return false;
  }
  for (std::size_t i = 1U; i < name.size(); ++i) {
    const char c = name[i];
    if (c == '/') {
      return false;  // No internal slashes allowed in POSIX IPC names
    }
    const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!valid) {
      return false;
    }
  }
  return true;
}

namespace {

void make_abs_timespec(std::chrono::milliseconds timeout, struct timespec& ts) noexcept {
  ::clock_gettime(CLOCK_REALTIME, &ts);
  const auto sec = std::chrono::duration_cast<std::chrono::seconds>(timeout);
  const auto nsec = std::chrono::duration_cast<std::chrono::nanoseconds>(timeout - sec);
  ts.tv_sec += static_cast<time_t>(sec.count());
  ts.tv_nsec += static_cast<long>(nsec.count());
  if (ts.tv_nsec >= 1'000'000'000L) {
    ts.tv_sec += 1;
    ts.tv_nsec -= 1'000'000'000L;
  }
}

}  // namespace

TelemetryPublisher::TelemetryPublisher(std::string shm_name, std::string sem_name)
    : shm_name_(std::move(shm_name)), sem_name_(std::move(sem_name)) {}

TelemetryPublisher::~TelemetryPublisher() {
  stop();
}

bool TelemetryPublisher::start(std::string& error_message) {
  if (running_) {
    return true;
  }
  if (!valid_ipc_name(shm_name_)) {
    error_message = "invalid shared memory name: " + shm_name_;
    return false;
  }
  if (!valid_ipc_name(sem_name_)) {
    error_message = "invalid semaphore name: " + sem_name_;
    return false;
  }

  // Exclusively create the shared memory object with 0600 permissions
  shm_fd_ = ::shm_open(shm_name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (shm_fd_ < 0) {
    const int err = errno;
    if (err == EEXIST) {
      error_message = "shared memory object '" + shm_name_ +
                      "' already exists (active server running or stale resource; verify server is stopped and remove with 'rm /dev/shm" +
                      shm_name_ + "')";
    } else {
      error_message = "shm_open failed for '" + shm_name_ + "': " + std::strerror(err);
    }
    return false;
  }
  owns_shm_ = true;

  if (::ftruncate(shm_fd_, static_cast<off_t>(sizeof(TelemetrySharedMemory))) != 0) {
    const int err = errno;
    error_message = "ftruncate failed: " + std::string(std::strerror(err));
    stop();
    return false;
  }

  void* addr = ::mmap(nullptr, sizeof(TelemetrySharedMemory), PROT_READ | PROT_WRITE,
                      MAP_SHARED, shm_fd_, 0);
  if (addr == MAP_FAILED) {
    const int err = errno;
    error_message = "mmap failed: " + std::string(std::strerror(err));
    stop();
    return false;
  }
  shm_ = static_cast<TelemetrySharedMemory*>(addr);
  std::memset(shm_, 0, sizeof(TelemetrySharedMemory));

  shm_->magic = kTelemetryMagic;
  shm_->abi_version = kTelemetryAbiVersion;
  shm_->struct_size = static_cast<std::uint32_t>(sizeof(TelemetrySharedMemory));
  shm_->owner_uid = static_cast<std::uint32_t>(::geteuid());
  shm_->server_pid = static_cast<std::int64_t>(::getpid());
  shm_->state = static_cast<std::uint32_t>(ServerState::Uninitialized);

  // Initialize process-shared robust mutex
  pthread_mutexattr_t attr;
  int rc = ::pthread_mutexattr_init(&attr);
  if (rc != 0) {
    error_message = "pthread_mutexattr_init failed: " + std::string(std::strerror(rc));
    stop();
    return false;
  }
  rc = ::pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
  if (rc != 0) {
    error_message = "pthread_mutexattr_setpshared failed: " + std::string(std::strerror(rc));
    (void)::pthread_mutexattr_destroy(&attr);
    stop();
    return false;
  }
  rc = ::pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  if (rc != 0) {
    error_message = "pthread_mutexattr_setrobust failed: " + std::string(std::strerror(rc));
    (void)::pthread_mutexattr_destroy(&attr);
    stop();
    return false;
  }
  rc = ::pthread_mutex_init(&shm_->mutex, &attr);
  (void)::pthread_mutexattr_destroy(&attr);
  if (rc != 0) {
    error_message = "pthread_mutex_init failed: " + std::string(std::strerror(rc));
    stop();
    return false;
  }
  mutex_initialized_ = true;

  // Exclusively create named semaphore with initial value 0 and 0600 permissions
  sem_ = ::sem_open(sem_name_.c_str(), O_CREAT | O_EXCL, 0600, 0U);
  if (sem_ == SEM_FAILED) {
    const int err = errno;
    if (err == EEXIST) {
      error_message = "named semaphore '" + sem_name_ +
                      "' already exists (active server running or stale resource; verify server is stopped and remove with sem_unlink)";
    } else {
      error_message = "sem_open failed for '" + sem_name_ + "': " + std::strerror(err);
    }
    stop();
    return false;
  }
  owns_sem_ = true;

  shm_->state = static_cast<std::uint32_t>(ServerState::Running);
  running_ = true;
  return true;
}

void TelemetryPublisher::stop() noexcept {
  if (shm_ != nullptr && mutex_initialized_) {
    const int rc = ::pthread_mutex_trylock(&shm_->mutex);
    if (rc == 0 || rc == EOWNERDEAD) {
      if (rc == EOWNERDEAD) {
        (void)::pthread_mutex_consistent(&shm_->mutex);
      }
      ++sequence_;
      shm_->payload.sequence = sequence_;
      shm_->state = static_cast<std::uint32_t>(ServerState::Stopping);
      shm_->payload.server_state = static_cast<std::uint32_t>(ServerState::Stopping);
      (void)::pthread_mutex_unlock(&shm_->mutex);
      if (sem_ != SEM_FAILED) {
        int sval = 0;
        if (::sem_getvalue(sem_, &sval) == 0 && sval <= 0) {
          (void)::sem_post(sem_);
        }
      }
    }
    mutex_initialized_ = false;
  }

  if (shm_ != nullptr) {
    (void)::munmap(shm_, sizeof(TelemetrySharedMemory));
    shm_ = nullptr;
  }

  if (shm_fd_ >= 0) {
    (void)::close(shm_fd_);
    shm_fd_ = -1;
  }

  if (owns_shm_) {
    (void)::shm_unlink(shm_name_.c_str());
    owns_shm_ = false;
  }

  if (sem_ != SEM_FAILED) {
    (void)::sem_close(sem_);
    sem_ = SEM_FAILED;
  }

  if (owns_sem_) {
    (void)::sem_unlink(sem_name_.c_str());
    owns_sem_ = false;
  }

  running_ = false;
}

bool TelemetryPublisher::publish(const TelemetryInput& input, bool is_stopping) noexcept {
  if (!running_ || shm_ == nullptr || !mutex_initialized_) {
    return false;
  }

  // Server must never block waiting for reader: use trylock
  int rc = ::pthread_mutex_trylock(&shm_->mutex);
  if (rc == EBUSY) {
    return false;  // Reader is currently holding lock; skip nonblocking
  }
  if (rc == EOWNERDEAD) {
    (void)::pthread_mutex_consistent(&shm_->mutex);
  } else if (rc != 0) {
    return false;
  }

  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto now_ns = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());

  ++sequence_;
  const auto state_enum = is_stopping ? ServerState::Stopping : ServerState::Running;
  const auto state_val = static_cast<std::uint32_t>(state_enum);

  shm_->payload.sequence = sequence_;
  shm_->payload.timestamp_ns = now_ns;
  shm_->payload.server_pid = static_cast<std::int64_t>(::getpid());
  shm_->payload.server_state = state_val;
  shm_->payload.active_matches = input.active_matches;
  shm_->payload.connected_players = input.connected_players;
  shm_->payload.connected_moderators = input.connected_moderators;
  shm_->payload.match_capacity = input.match_capacity;
  shm_->payload.players_per_match_capacity = input.players_per_match_capacity;
  shm_->payload.player_connection_capacity = input.player_connection_capacity;
  shm_->state = state_val;

  (void)::pthread_mutex_unlock(&shm_->mutex);

  // Semaphore notification: wake-up hint with coalescing
  if (sem_ != SEM_FAILED) {
    int sval = 0;
    if (::sem_getvalue(sem_, &sval) == 0 && sval <= 0) {
      (void)::sem_post(sem_);
    }
  }

  return true;
}

TelemetryObserver::~TelemetryObserver() {
  close();
}

TelemetryObserver::TelemetryObserver(TelemetryObserver&& other) noexcept
    : shm_name_(std::move(other.shm_name_)),
      sem_name_(std::move(other.sem_name_)),
      shm_fd_(other.shm_fd_),
      shm_(other.shm_),
      sem_(other.sem_) {
  other.shm_fd_ = -1;
  other.shm_ = nullptr;
  other.sem_ = SEM_FAILED;
}

TelemetryObserver& TelemetryObserver::operator=(TelemetryObserver&& other) noexcept {
  if (this != &other) {
    close();
    shm_name_ = std::move(other.shm_name_);
    sem_name_ = std::move(other.sem_name_);
    shm_fd_ = other.shm_fd_;
    shm_ = other.shm_;
    sem_ = other.sem_;
    other.shm_fd_ = -1;
    other.shm_ = nullptr;
    other.sem_ = SEM_FAILED;
  }
  return *this;
}

void TelemetryObserver::close() noexcept {
  if (shm_ != nullptr) {
    (void)::munmap(shm_, sizeof(TelemetrySharedMemory));
    shm_ = nullptr;
  }
  if (shm_fd_ >= 0) {
    (void)::close(shm_fd_);
    shm_fd_ = -1;
  }
  if (sem_ != SEM_FAILED) {
    (void)::sem_close(sem_);
    sem_ = SEM_FAILED;
  }
  shm_name_.clear();
  sem_name_.clear();
}

bool TelemetryObserver::open(std::string_view shm_name, std::string_view sem_name,
                            std::string& error_message) {
  close();

  if (!valid_ipc_name(shm_name)) {
    error_message = "invalid shared memory name: " + std::string(shm_name);
    return false;
  }
  if (!valid_ipc_name(sem_name)) {
    error_message = "invalid semaphore name: " + std::string(sem_name);
    return false;
  }

  // Open existing shared memory with O_RDWR (required for robust mutex operations)
  shm_fd_ = ::shm_open(shm_name.data(), O_RDWR, 0);
  if (shm_fd_ < 0) {
    const int err = errno;
    if (err == ENOENT) {
      error_message = "shared memory object '" + std::string(shm_name) +
                      "' does not exist (is darkpool_server running?)";
    } else if (err == EACCES) {
      error_message = "permission denied accessing shared memory object '" +
                      std::string(shm_name) +
                      "' (observer must run as same user as server)";
    } else {
      error_message = "shm_open failed for '" + std::string(shm_name) + "': " +
                      std::strerror(err);
    }
    return false;
  }

  struct stat st{};
  if (::fstat(shm_fd_, &st) != 0) {
    const int err = errno;
    error_message = "fstat on shared memory failed: " + std::string(std::strerror(err));
    close();
    return false;
  }

  if (st.st_uid != ::geteuid()) {
    error_message = "ownership mismatch: shared memory is owned by UID " +
                    std::to_string(st.st_uid) + ", but observer is UID " +
                    std::to_string(::geteuid());
    close();
    return false;
  }

  if (static_cast<std::size_t>(st.st_size) < sizeof(TelemetrySharedMemory)) {
    error_message = "invalid shared memory size: " + std::to_string(st.st_size) +
                    " bytes (expected at least " +
                    std::to_string(sizeof(TelemetrySharedMemory)) + " bytes)";
    close();
    return false;
  }

  void* addr = ::mmap(nullptr, sizeof(TelemetrySharedMemory), PROT_READ | PROT_WRITE,
                      MAP_SHARED, shm_fd_, 0);
  if (addr == MAP_FAILED) {
    const int err = errno;
    error_message = "mmap on shared memory failed: " + std::string(std::strerror(err));
    close();
    return false;
  }
  shm_ = static_cast<TelemetrySharedMemory*>(addr);

  // Validate shared memory header
  if (shm_->magic != kTelemetryMagic) {
    std::ostringstream ss;
    ss << "invalid telemetry header magic: 0x" << std::hex << shm_->magic
       << " (expected 0x" << kTelemetryMagic << ")";
    error_message = ss.str();
    close();
    return false;
  }

  if (shm_->abi_version != kTelemetryAbiVersion) {
    error_message = "unsupported telemetry ABI version: " +
                    std::to_string(shm_->abi_version) + " (expected " +
                    std::to_string(kTelemetryAbiVersion) + ")";
    close();
    return false;
  }

  if (shm_->struct_size != sizeof(TelemetrySharedMemory)) {
    error_message = "telemetry structure size mismatch: " +
                    std::to_string(shm_->struct_size) + " (expected " +
                    std::to_string(sizeof(TelemetrySharedMemory)) + ")";
    close();
    return false;
  }

  if (shm_->owner_uid != static_cast<std::uint32_t>(::geteuid())) {
    error_message = "telemetry owner UID mismatch: " +
                    std::to_string(shm_->owner_uid) + " (expected " +
                    std::to_string(::geteuid()) + ")";
    close();
    return false;
  }

  // Open existing named semaphore (does NOT create)
  sem_ = ::sem_open(sem_name.data(), 0);
  if (sem_ == SEM_FAILED) {
    const int err = errno;
    if (err == ENOENT) {
      error_message = "named semaphore '" + std::string(sem_name) +
                      "' does not exist (is darkpool_server running?)";
    } else {
      error_message = "sem_open failed for '" + std::string(sem_name) + "': " +
                      std::strerror(err);
    }
    close();
    return false;
  }

  shm_name_ = std::string(shm_name);
  sem_name_ = std::string(sem_name);
  return true;
}

std::optional<TelemetrySnapshot> TelemetryObserver::read_snapshot(
    std::chrono::milliseconds timeout, std::string& error_message) const {
  if (shm_ == nullptr) {
    error_message = "telemetry observer is not open";
    return std::nullopt;
  }

  struct timespec ts{};
  make_abs_timespec(timeout, ts);

  int rc = 0;
  while ((rc = ::pthread_mutex_timedlock(&shm_->mutex, &ts)) != 0) {
    if (rc == EINTR) {
      continue;
    }
    break;
  }

  if (rc == EOWNERDEAD) {
    (void)::pthread_mutex_consistent(&shm_->mutex);
  } else if (rc == ETIMEDOUT) {
    error_message = "timed out waiting for telemetry mutex lock";
    return std::nullopt;
  } else if (rc == EINVAL) {
    // If the server tore down the mutex during stopping, return stopping snapshot if marked
    if (shm_->payload.server_state == static_cast<std::uint32_t>(ServerState::Stopping)) {
      TelemetrySnapshot snapshot{
          .sequence = shm_->payload.sequence,
          .timestamp_ns = shm_->payload.timestamp_ns,
          .server_pid = static_cast<pid_t>(shm_->payload.server_pid),
          .server_state = ServerState::Stopping,
          .active_matches = shm_->payload.active_matches,
          .connected_players = shm_->payload.connected_players,
          .connected_moderators = shm_->payload.connected_moderators,
          .match_capacity = shm_->payload.match_capacity,
          .players_per_match_capacity = shm_->payload.players_per_match_capacity,
          .player_connection_capacity = shm_->payload.player_connection_capacity,
      };
      return snapshot;
    }
    error_message = "pthread_mutex_timedlock failed: " + std::string(std::strerror(rc));
    return std::nullopt;
  } else if (rc != 0) {
    error_message = "pthread_mutex_timedlock failed: " + std::string(std::strerror(rc));
    return std::nullopt;
  }

  if (shm_->state == static_cast<std::uint32_t>(ServerState::Uninitialized)) {
    (void)::pthread_mutex_unlock(&shm_->mutex);
    error_message = "telemetry snapshot is in uninitialized state";
    return std::nullopt;
  }

  TelemetrySnapshot snapshot{
      .sequence = shm_->payload.sequence,
      .timestamp_ns = shm_->payload.timestamp_ns,
      .server_pid = static_cast<pid_t>(shm_->payload.server_pid),
      .server_state = server_state_from_uint32(shm_->payload.server_state),
      .active_matches = shm_->payload.active_matches,
      .connected_players = shm_->payload.connected_players,
      .connected_moderators = shm_->payload.connected_moderators,
      .match_capacity = shm_->payload.match_capacity,
      .players_per_match_capacity = shm_->payload.players_per_match_capacity,
      .player_connection_capacity = shm_->payload.player_connection_capacity,
  };

  (void)::pthread_mutex_unlock(&shm_->mutex);
  return snapshot;
}

bool TelemetryObserver::wait_for_notification(
    std::chrono::milliseconds timeout,
    const volatile sig_atomic_t* interrupted) const noexcept {
  if (sem_ == SEM_FAILED) {
    return false;
  }

  struct timespec ts{};
  make_abs_timespec(timeout, ts);

  while (true) {
    if (interrupted != nullptr && *interrupted != 0) {
      return false;
    }
    const int rc = ::sem_timedwait(sem_, &ts);
    if (rc == 0) {
      // Drain any additional pending posts to coalesce wake-ups
      while (::sem_trywait(sem_) == 0) {}
      return true;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == ETIMEDOUT) {
      return false;
    }
    return false;
  }
}

}  // namespace darkpool::telemetry

#endif  // defined(__linux__)
