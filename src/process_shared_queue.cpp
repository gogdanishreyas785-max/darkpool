#if defined(__linux__)

#include "darkpool/process_shared_queue.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace darkpool::ipc {

namespace {

void make_monotonic_abs_timespec(std::chrono::milliseconds timeout, struct timespec& ts) noexcept {
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
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

bool is_valid_queue_shm_name(std::string_view name) noexcept {
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

ProcessSharedQueue::ProcessSharedQueue(std::string shm_name,
                                       int fd,
                                       std::size_t total_mapped_size,
                                       void* mapped_base,
                                       QueueEndpointRole role,
                                       bool is_creator)
    : shm_name_(std::move(shm_name)),
      fd_(fd),
      total_mapped_size_(total_mapped_size),
      mapped_base_(mapped_base),
      role_(role),
      is_creator_(is_creator) {
  if (mapped_base_) {
    header_ = reinterpret_cast<QueueSharedHeader*>(mapped_base_);
    slots_ = reinterpret_cast<QueueMessageSlot*>(
        reinterpret_cast<std::uint8_t*>(mapped_base_) + sizeof(QueueSharedHeader));
  }
}

ProcessSharedQueue::~ProcessSharedQueue() {
  cleanup();
}

ProcessSharedQueue::ProcessSharedQueue(ProcessSharedQueue&& other) noexcept
    : shm_name_(std::move(other.shm_name_)),
      fd_(other.fd_),
      total_mapped_size_(other.total_mapped_size_),
      mapped_base_(other.mapped_base_),
      header_(other.header_),
      slots_(other.slots_),
      role_(other.role_),
      is_creator_(other.is_creator_) {
  other.fd_ = -1;
  other.total_mapped_size_ = 0;
  other.mapped_base_ = nullptr;
  other.header_ = nullptr;
  other.slots_ = nullptr;
  other.is_creator_ = false;
}

ProcessSharedQueue& ProcessSharedQueue::operator=(ProcessSharedQueue&& other) noexcept {
  if (this != &other) {
    cleanup();
    shm_name_ = std::move(other.shm_name_);
    fd_ = other.fd_;
    total_mapped_size_ = other.total_mapped_size_;
    mapped_base_ = other.mapped_base_;
    header_ = other.header_;
    slots_ = other.slots_;
    role_ = other.role_;
    is_creator_ = other.is_creator_;

    other.fd_ = -1;
    other.total_mapped_size_ = 0;
    other.mapped_base_ = nullptr;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.is_creator_ = false;
  }
  return *this;
}

void ProcessSharedQueue::cleanup() noexcept {
  if (header_) {
    // Unbind PID if matching
    const pid_t my_pid = ::getpid();
    int lock_rc = acquire_mutex(nullptr);
    if (lock_rc == 0 || lock_rc == EOWNERDEAD) {
      if (role_ == QueueEndpointRole::Producer && header_->producer_pid == my_pid) {
        header_->producer_pid = 0;
      } else if (role_ == QueueEndpointRole::Consumer && header_->consumer_pid == my_pid) {
        header_->consumer_pid = 0;
      }
      release_mutex();
    }
  }

  if (mapped_base_) {
    ::munmap(mapped_base_, total_mapped_size_);
    mapped_base_ = nullptr;
    header_ = nullptr;
    slots_ = nullptr;
    total_mapped_size_ = 0;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

bool ProcessSharedQueue::unlink(std::string_view shm_name) noexcept {
  std::string name(shm_name);
  return ::shm_unlink(name.c_str()) == 0;
}

std::optional<ProcessSharedQueue> ProcessSharedQueue::create(
    std::string_view shm_name,
    std::size_t capacity,
    QueueEndpointRole role,
    std::string& error_msg) {
  if (!is_valid_queue_shm_name(shm_name)) {
    error_msg = "Invalid shared memory name: " + std::string(shm_name);
    return std::nullopt;
  }
  if (capacity == 0 || capacity > 1'000'000) {
    error_msg = "Invalid capacity: " + std::to_string(capacity) + " (must be between 1 and 1000000)";
    return std::nullopt;
  }

  const std::size_t total_size = sizeof(QueueSharedHeader) + capacity * sizeof(QueueMessageSlot);
  std::string name(shm_name);

  // Exclusive creation
  const int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    const int err = errno;
    error_msg = "shm_open O_CREAT|O_EXCL failed: " + std::string(std::strerror(err));
    return std::nullopt;
  }

  if (::ftruncate(fd, static_cast<off_t>(total_size)) != 0) {
    const int err = errno;
    error_msg = "ftruncate failed: " + std::string(std::strerror(err));
    ::close(fd);
    ::shm_unlink(name.c_str());
    return std::nullopt;
  }

  void* addr = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    const int err = errno;
    error_msg = "mmap failed: " + std::string(std::strerror(err));
    ::close(fd);
    ::shm_unlink(name.c_str());
    return std::nullopt;
  }

  std::memset(addr, 0, total_size);
  auto* hdr = reinterpret_cast<QueueSharedHeader*>(addr);

  // 1. Initialize process-shared robust mutex
  pthread_mutexattr_t mattr;
  int rc = ::pthread_mutexattr_init(&mattr);
  if (rc != 0) {
    error_msg = "pthread_mutexattr_init failed: " + std::string(std::strerror(rc));
    ::munmap(addr, total_size);
    ::close(fd);
    ::shm_unlink(name.c_str());
    return std::nullopt;
  }
  ::pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
  ::pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
  rc = ::pthread_mutex_init(&hdr->mutex, &mattr);
  ::pthread_mutexattr_destroy(&mattr);
  if (rc != 0) {
    error_msg = "pthread_mutex_init failed: " + std::string(std::strerror(rc));
    ::munmap(addr, total_size);
    ::close(fd);
    ::shm_unlink(name.c_str());
    return std::nullopt;
  }

  // 2. Initialize condition variables with CLOCK_MONOTONIC
  pthread_condattr_t cattr;
  rc = ::pthread_condattr_init(&cattr);
  if (rc != 0) {
    error_msg = "pthread_condattr_init failed: " + std::string(std::strerror(rc));
    ::munmap(addr, total_size);
    ::close(fd);
    ::shm_unlink(name.c_str());
    return std::nullopt;
  }
  ::pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
  ::pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);

  rc = ::pthread_cond_init(&hdr->not_empty_cond, &cattr);
  if (rc == 0) {
    rc = ::pthread_cond_init(&hdr->not_full_cond, &cattr);
  }
  ::pthread_condattr_destroy(&cattr);
  if (rc != 0) {
    error_msg = "pthread_cond_init failed: " + std::string(std::strerror(rc));
    ::munmap(addr, total_size);
    ::close(fd);
    ::shm_unlink(name.c_str());
    return std::nullopt;
  }

  // 3. Populate header
  hdr->magic = kQueueMagic;
  hdr->abi_version = kQueueAbiVersion;
  hdr->struct_size = static_cast<std::uint32_t>(sizeof(QueueSharedHeader));
  hdr->owner_uid = static_cast<std::uint32_t>(::geteuid());
  hdr->creator_pid = static_cast<std::int64_t>(::getpid());
  hdr->capacity = static_cast<std::uint32_t>(capacity);
  hdr->state = static_cast<std::uint32_t>(QueueState::Active);

  if (role == QueueEndpointRole::Producer) {
    hdr->producer_pid = hdr->creator_pid;
    hdr->consumer_pid = 0;
  } else {
    hdr->producer_pid = 0;
    hdr->consumer_pid = hdr->creator_pid;
  }

  return ProcessSharedQueue(std::move(name), fd, total_size, addr, role, true);
}

std::optional<ProcessSharedQueue> ProcessSharedQueue::open_existing(
    std::string_view shm_name,
    QueueEndpointRole role,
    std::string& error_msg) {
  if (!is_valid_queue_shm_name(shm_name)) {
    error_msg = "Invalid shared memory name: " + std::string(shm_name);
    return std::nullopt;
  }

  std::string name(shm_name);
  const int fd = ::shm_open(name.c_str(), O_RDWR, 0);
  if (fd < 0) {
    const int err = errno;
    error_msg = "shm_open O_RDWR failed: " + std::string(std::strerror(err));
    return std::nullopt;
  }

  struct stat sb {};
  if (::fstat(fd, &sb) != 0) {
    const int err = errno;
    error_msg = "fstat failed: " + std::string(std::strerror(err));
    ::close(fd);
    return std::nullopt;
  }

  if (static_cast<std::size_t>(sb.st_size) < sizeof(QueueSharedHeader)) {
    error_msg = "Shared memory size too small for QueueSharedHeader: " + std::to_string(sb.st_size);
    ::close(fd);
    return std::nullopt;
  }

  const std::size_t total_size = static_cast<std::size_t>(sb.st_size);
  void* addr = ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    const int err = errno;
    error_msg = "mmap failed: " + std::string(std::strerror(err));
    ::close(fd);
    return std::nullopt;
  }

  auto* hdr = reinterpret_cast<QueueSharedHeader*>(addr);

  // Validate ABI and metadata
  if (hdr->magic != kQueueMagic) {
    error_msg = "Shared memory magic mismatch: expected 0x" +
                std::to_string(kQueueMagic) + ", got 0x" + std::to_string(hdr->magic);
    ::munmap(addr, total_size);
    ::close(fd);
    return std::nullopt;
  }

  if (hdr->abi_version != kQueueAbiVersion) {
    error_msg = "Shared memory ABI version mismatch: expected " +
                std::to_string(kQueueAbiVersion) + ", got " + std::to_string(hdr->abi_version);
    ::munmap(addr, total_size);
    ::close(fd);
    return std::nullopt;
  }

  if (hdr->struct_size != sizeof(QueueSharedHeader)) {
    error_msg = "QueueSharedHeader struct size mismatch: expected " +
                std::to_string(sizeof(QueueSharedHeader)) + ", got " + std::to_string(hdr->struct_size);
    ::munmap(addr, total_size);
    ::close(fd);
    return std::nullopt;
  }

  const std::size_t expected_size = sizeof(QueueSharedHeader) + hdr->capacity * sizeof(QueueMessageSlot);
  if (total_size != expected_size) {
    error_msg = "Shared memory size mismatch: expected " + std::to_string(expected_size) +
                ", got " + std::to_string(total_size);
    ::munmap(addr, total_size);
    ::close(fd);
    return std::nullopt;
  }

  ProcessSharedQueue queue(std::move(name), fd, total_size, addr, role, false);

  // Bind role PID atomically
  const pid_t my_pid = ::getpid();
  const int lock_rc = queue.acquire_mutex(&error_msg);
  if (lock_rc != 0 && lock_rc != EOWNERDEAD) {
    return std::nullopt;
  }

  if (hdr->state == static_cast<std::uint32_t>(QueueState::Faulted)) {
    queue.release_mutex();
    error_msg = "Queue is in Faulted state";
    return std::nullopt;
  }

  if (role == QueueEndpointRole::Producer) {
    if (hdr->producer_pid != 0 && hdr->producer_pid != my_pid) {
      // Check if existing producer PID is still alive
      if (::kill(static_cast<pid_t>(hdr->producer_pid), 0) == 0 || errno == EPERM) {
        queue.release_mutex();
        error_msg = "Producer endpoint already bound to active PID " + std::to_string(hdr->producer_pid);
        return std::nullopt;
      }
    }
    hdr->producer_pid = my_pid;
  } else {
    if (hdr->consumer_pid != 0 && hdr->consumer_pid != my_pid) {
      if (::kill(static_cast<pid_t>(hdr->consumer_pid), 0) == 0 || errno == EPERM) {
        queue.release_mutex();
        error_msg = "Consumer endpoint already bound to active PID " + std::to_string(hdr->consumer_pid);
        return std::nullopt;
      }
    }
    hdr->consumer_pid = my_pid;
  }

  queue.release_mutex();
  return queue;
}

int ProcessSharedQueue::acquire_mutex(std::string* error_msg) noexcept {
  if (!header_) {
    if (error_msg) *error_msg = "Queue unmapped or invalid";
    return EINVAL;
  }

  int rc = ::pthread_mutex_lock(&header_->mutex);
  if (rc == 0) {
    return 0;
  }

  if (rc == EOWNERDEAD) {
    // Robust mutex owner-death detected!
    // Fail-stop policy: mark state as Faulted, make consistent, wake blocked threads.
    header_->state = static_cast<std::uint32_t>(QueueState::Faulted);
    ::pthread_cond_broadcast(&header_->not_empty_cond);
    ::pthread_cond_broadcast(&header_->not_full_cond);

    const int cons_rc = ::pthread_mutex_consistent(&header_->mutex);
    if (cons_rc != 0) {
      if (error_msg) *error_msg = "pthread_mutex_consistent failed: " + std::string(std::strerror(cons_rc));
      return ENOTRECOVERABLE;
    }
    if (error_msg) *error_msg = "Mutex owner died; queue marked permanently Faulted (EOWNERDEAD)";
    return EOWNERDEAD;
  }

  if (rc == ENOTRECOVERABLE) {
    if (error_msg) *error_msg = "Mutex is ENOTRECOVERABLE";
    return ENOTRECOVERABLE;
  }

  if (error_msg) *error_msg = "pthread_mutex_lock failed: " + std::string(std::strerror(rc));
  return rc;
}

void ProcessSharedQueue::release_mutex() noexcept {
  if (header_) {
    ::pthread_mutex_unlock(&header_->mutex);
  }
}

QueueOpStatus ProcessSharedQueue::push(
    const void* data,
    std::size_t size,
    std::chrono::milliseconds timeout,
    std::string* error_msg) {
  if (role_ != QueueEndpointRole::Producer) {
    if (error_msg) *error_msg = "Endpoint role is not Producer";
    return QueueOpStatus::Invalid;
  }
  if (!data || size == 0 || size > kMaxMessageDataSize) {
    if (error_msg) {
      *error_msg = "Invalid payload size: " + std::to_string(size) +
                   " (must be 1.." + std::to_string(kMaxMessageDataSize) + ")";
    }
    return QueueOpStatus::Invalid;
  }

  const int lock_rc = acquire_mutex(error_msg);
  if (lock_rc == EOWNERDEAD || lock_rc == ENOTRECOVERABLE) {
    release_mutex();
    return QueueOpStatus::Faulted;
  }
  if (lock_rc != 0) {
    return QueueOpStatus::Invalid;
  }

  struct timespec deadline {};
  make_monotonic_abs_timespec(timeout, deadline);

  while (true) {
    if (header_->state == static_cast<std::uint32_t>(QueueState::Faulted)) {
      release_mutex();
      if (error_msg) *error_msg = "Queue is in Faulted state";
      return QueueOpStatus::Faulted;
    }
    if (header_->state == static_cast<std::uint32_t>(QueueState::Closed)) {
      release_mutex();
      if (error_msg) *error_msg = "Queue is Closed";
      return QueueOpStatus::Closed;
    }

    if (header_->count < header_->capacity) {
      // Slot available
      const std::size_t slot_idx = header_->tail % header_->capacity;
      auto& slot = slots_[slot_idx];

      slot.sequence = header_->total_pushed;
      slot.size = static_cast<std::uint32_t>(size);
      slot.reserved = 0;
      std::memcpy(slot.data, data, size);

      header_->tail++;
      header_->count++;
      header_->total_pushed++;

      // Signal waiting consumer
      ::pthread_cond_signal(&header_->not_empty_cond);
      release_mutex();
      return QueueOpStatus::Success;
    }

    // Queue is full: wait on not_full_cond
    const int wait_rc = ::pthread_cond_timedwait(&header_->not_full_cond, &header_->mutex, &deadline);
    if (wait_rc == ETIMEDOUT) {
      release_mutex();
      if (error_msg) *error_msg = "Timed out waiting for free slot";
      return QueueOpStatus::Timeout;
    }
    if (wait_rc == EOWNERDEAD) {
      header_->state = static_cast<std::uint32_t>(QueueState::Faulted);
      ::pthread_cond_broadcast(&header_->not_empty_cond);
      ::pthread_cond_broadcast(&header_->not_full_cond);
      (void)::pthread_mutex_consistent(&header_->mutex);
      release_mutex();
      if (error_msg) *error_msg = "Owner died during condition wait";
      return QueueOpStatus::Faulted;
    }
    if (wait_rc != 0) {
      release_mutex();
      if (error_msg) *error_msg = "pthread_cond_timedwait error: " + std::string(std::strerror(wait_rc));
      return QueueOpStatus::Invalid;
    }
  }
}

QueueOpStatus ProcessSharedQueue::pop(
    void* buffer,
    std::size_t buffer_capacity,
    std::size_t& out_size,
    std::uint64_t* out_sequence,
    std::chrono::milliseconds timeout,
    std::string* error_msg) {
  out_size = 0;
  if (role_ != QueueEndpointRole::Consumer) {
    if (error_msg) *error_msg = "Endpoint role is not Consumer";
    return QueueOpStatus::Invalid;
  }
  if (!buffer || buffer_capacity == 0) {
    if (error_msg) *error_msg = "Invalid destination buffer";
    return QueueOpStatus::Invalid;
  }

  const int lock_rc = acquire_mutex(error_msg);
  if (lock_rc == EOWNERDEAD || lock_rc == ENOTRECOVERABLE) {
    release_mutex();
    return QueueOpStatus::Faulted;
  }
  if (lock_rc != 0) {
    return QueueOpStatus::Invalid;
  }

  struct timespec deadline {};
  make_monotonic_abs_timespec(timeout, deadline);

  while (true) {
    if (header_->state == static_cast<std::uint32_t>(QueueState::Faulted)) {
      release_mutex();
      if (error_msg) *error_msg = "Queue is in Faulted state";
      return QueueOpStatus::Faulted;
    }

    if (header_->count > 0) {
      // Message available
      const std::size_t slot_idx = header_->head % header_->capacity;
      const auto& slot = slots_[slot_idx];

      if (buffer_capacity < slot.size) {
        release_mutex();
        if (error_msg) {
          *error_msg = "Buffer capacity (" + std::to_string(buffer_capacity) +
                       ") smaller than message size (" + std::to_string(slot.size) + ")";
        }
        return QueueOpStatus::Invalid;
      }

      std::memcpy(buffer, slot.data, slot.size);
      out_size = slot.size;
      if (out_sequence) {
        *out_sequence = slot.sequence;
      }

      header_->head++;
      header_->count--;
      header_->total_popped++;

      // Signal waiting producer
      ::pthread_cond_signal(&header_->not_full_cond);
      release_mutex();
      return QueueOpStatus::Success;
    }

    // Queue empty: check if Closed
    if (header_->state == static_cast<std::uint32_t>(QueueState::Closed)) {
      release_mutex();
      if (error_msg) *error_msg = "Queue is Closed and empty";
      return QueueOpStatus::Closed;
    }

    // Queue is empty: wait on not_empty_cond
    const int wait_rc = ::pthread_cond_timedwait(&header_->not_empty_cond, &header_->mutex, &deadline);
    if (wait_rc == ETIMEDOUT) {
      release_mutex();
      if (error_msg) *error_msg = "Timed out waiting for message";
      return QueueOpStatus::Timeout;
    }
    if (wait_rc == EOWNERDEAD) {
      header_->state = static_cast<std::uint32_t>(QueueState::Faulted);
      ::pthread_cond_broadcast(&header_->not_empty_cond);
      ::pthread_cond_broadcast(&header_->not_full_cond);
      (void)::pthread_mutex_consistent(&header_->mutex);
      release_mutex();
      if (error_msg) *error_msg = "Owner died during condition wait";
      return QueueOpStatus::Faulted;
    }
    if (wait_rc != 0) {
      release_mutex();
      if (error_msg) *error_msg = "pthread_cond_timedwait error: " + std::string(std::strerror(wait_rc));
      return QueueOpStatus::Invalid;
    }
  }
}

QueueOpStatus ProcessSharedQueue::pop(
    std::string& out_message,
    std::uint64_t* out_sequence,
    std::chrono::milliseconds timeout,
    std::string* error_msg) {
  char buf[kMaxMessageDataSize];
  std::size_t size = 0;
  const QueueOpStatus st = pop(buf, sizeof(buf), size, out_sequence, timeout, error_msg);
  if (st == QueueOpStatus::Success) {
    out_message.assign(buf, size);
  } else {
    out_message.clear();
  }
  return st;
}

bool ProcessSharedQueue::close(std::string* error_msg) {
  const int lock_rc = acquire_mutex(error_msg);
  if (lock_rc == EOWNERDEAD || lock_rc == ENOTRECOVERABLE) {
    release_mutex();
    return false;
  }
  if (lock_rc != 0) {
    return false;
  }

  if (header_->state == static_cast<std::uint32_t>(QueueState::Active)) {
    header_->state = static_cast<std::uint32_t>(QueueState::Closed);
    // Wake any producers waiting for space and consumers waiting for items
    ::pthread_cond_broadcast(&header_->not_empty_cond);
    ::pthread_cond_broadcast(&header_->not_full_cond);
  }

  release_mutex();
  return true;
}

std::size_t ProcessSharedQueue::capacity() const noexcept {
  return header_ ? header_->capacity : 0;
}

std::size_t ProcessSharedQueue::size() const noexcept {
  return header_ ? header_->count : 0;
}

bool ProcessSharedQueue::empty() const noexcept {
  return size() == 0;
}

bool ProcessSharedQueue::full() const noexcept {
  return size() >= capacity();
}

QueueState ProcessSharedQueue::state() const noexcept {
  if (!header_) return QueueState::Uninitialized;
  return static_cast<QueueState>(header_->state);
}

}  // namespace darkpool::ipc

#endif

