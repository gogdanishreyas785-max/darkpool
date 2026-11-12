#if defined(__linux__)

#include "darkpool/process_shared_queue.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <limits>
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

// Checked arithmetic for shared memory size
bool compute_total_mapped_size(std::size_t capacity, std::size_t& out_size) noexcept {
  if (capacity == 0 || capacity > 1'000'000) {
    return false;
  }
  constexpr std::size_t kSlotSize = sizeof(QueueMessageSlot);
  constexpr std::size_t kHdrSize = sizeof(QueueSharedHeader);

  if (capacity > (std::numeric_limits<std::size_t>::max() - kHdrSize) / kSlotSize) {
    return false;
  }
  out_size = kHdrSize + capacity * kSlotSize;
  return true;
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
                                       std::size_t capacity,
                                       QueueEndpointRole role,
                                       bool is_creator)
    : shm_name_(std::move(shm_name)),
      fd_(fd),
      total_mapped_size_(total_mapped_size),
      mapped_base_(mapped_base),
      capacity_(capacity),
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
      capacity_(other.capacity_),
      role_(other.role_),
      is_creator_(other.is_creator_) {
  other.fd_ = -1;
  other.total_mapped_size_ = 0;
  other.mapped_base_ = nullptr;
  other.header_ = nullptr;
  other.slots_ = nullptr;
  other.capacity_ = 0;
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
    capacity_ = other.capacity_;
    role_ = other.role_;
    is_creator_ = other.is_creator_;

    other.fd_ = -1;
    other.total_mapped_size_ = 0;
    other.mapped_base_ = nullptr;
    other.header_ = nullptr;
    other.slots_ = nullptr;
    other.capacity_ = 0;
    other.is_creator_ = false;
  }
  return *this;
}

void ProcessSharedQueue::cleanup() noexcept {
  if (header_) {
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
    std::string& error_msg,
    std::function<void()> pre_init_sync_hook) {
  if (role != QueueEndpointRole::Producer && role != QueueEndpointRole::Consumer) {
    error_msg = "Invalid QueueEndpointRole";
    return std::nullopt;
  }

  if (!is_valid_queue_shm_name(shm_name)) {
    error_msg = "Invalid shared memory name: " + std::string(shm_name);
    return std::nullopt;
  }

  std::size_t total_size = 0;
  if (!compute_total_mapped_size(capacity, total_size)) {
    error_msg = "Invalid capacity: " + std::to_string(capacity) + " (must be between 1 and 1000000)";
    return std::nullopt;
  }

  std::string name(shm_name);

  // Exclusive creation with 0600 permissions
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

  // If a pre-init synchronization hook was provided (for testing publication sequencing),
  // invoke it now after memory has been mapped and zeroed but BEFORE any pthread objects
  // or readiness indicators are initialized.
  if (pre_init_sync_hook) {
    pre_init_sync_hook();
  }

  // Helper lambda for partial initialization abort cleanup
  auto abort_creation = [&](const std::string& msg, bool destroy_mutex, bool destroy_not_empty) {
    error_msg = msg;
    if (destroy_not_empty) {
      ::pthread_cond_destroy(&hdr->not_empty_cond);
    }
    if (destroy_mutex) {
      ::pthread_mutex_destroy(&hdr->mutex);
    }
    ::munmap(addr, total_size);
    ::close(fd);
    ::shm_unlink(name.c_str());
    return std::nullopt;
  };

  // 1. Initialize process-shared robust mutex
  pthread_mutexattr_t mattr;
  int rc = ::pthread_mutexattr_init(&mattr);
  if (rc != 0) {
    return abort_creation("pthread_mutexattr_init failed: " + std::string(std::strerror(rc)), false, false);
  }
  rc = ::pthread_mutexattr_setpshared(&mattr, PTHREAD_PROCESS_SHARED);
  if (rc != 0) {
    ::pthread_mutexattr_destroy(&mattr);
    return abort_creation("pthread_mutexattr_setpshared failed: " + std::string(std::strerror(rc)), false, false);
  }
  rc = ::pthread_mutexattr_setrobust(&mattr, PTHREAD_MUTEX_ROBUST);
  if (rc != 0) {
    ::pthread_mutexattr_destroy(&mattr);
    return abort_creation("pthread_mutexattr_setrobust failed: " + std::string(std::strerror(rc)), false, false);
  }
  rc = ::pthread_mutex_init(&hdr->mutex, &mattr);
  ::pthread_mutexattr_destroy(&mattr);
  if (rc != 0) {
    return abort_creation("pthread_mutex_init failed: " + std::string(std::strerror(rc)), false, false);
  }

  // 2. Initialize condition variables with CLOCK_MONOTONIC
  pthread_condattr_t cattr;
  rc = ::pthread_condattr_init(&cattr);
  if (rc != 0) {
    return abort_creation("pthread_condattr_init failed: " + std::string(std::strerror(rc)), true, false);
  }
  rc = ::pthread_condattr_setpshared(&cattr, PTHREAD_PROCESS_SHARED);
  if (rc != 0) {
    ::pthread_condattr_destroy(&cattr);
    return abort_creation("pthread_condattr_setpshared failed: " + std::string(std::strerror(rc)), true, false);
  }
  rc = ::pthread_condattr_setclock(&cattr, CLOCK_MONOTONIC);
  if (rc != 0) {
    ::pthread_condattr_destroy(&cattr);
    return abort_creation("pthread_condattr_setclock failed: " + std::string(std::strerror(rc)), true, false);
  }

  rc = ::pthread_cond_init(&hdr->not_empty_cond, &cattr);
  if (rc != 0) {
    ::pthread_condattr_destroy(&cattr);
    return abort_creation("pthread_cond_init(not_empty_cond) failed: " + std::string(std::strerror(rc)), true, false);
  }

  rc = ::pthread_cond_init(&hdr->not_full_cond, &cattr);
  ::pthread_condattr_destroy(&cattr);
  if (rc != 0) {
    return abort_creation("pthread_cond_init(not_full_cond) failed: " + std::string(std::strerror(rc)), true, true);
  }

  // 3. Populate header metadata and publication flags
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

  // Set magic before publishing readiness flag
  hdr->magic = kQueueMagic;

  // Publish readiness LAST using explicit atomic store with release semantics.
  // Guarantees all prior writes (pthread_mutex_init, pthread_cond_init, metadata, magic)
  // are visible to any peer before init_done is observed as 1.
  __atomic_store_n(&hdr->init_done, 1U, __ATOMIC_RELEASE);

  return ProcessSharedQueue(std::move(name), fd, total_size, addr, capacity, role, true);
}

std::optional<ProcessSharedQueue> ProcessSharedQueue::open_existing(
    std::string_view shm_name,
    QueueEndpointRole role,
    std::string& error_msg,
    std::chrono::milliseconds wait_timeout) {
  if (role != QueueEndpointRole::Producer && role != QueueEndpointRole::Consumer) {
    error_msg = "Invalid QueueEndpointRole";
    return std::nullopt;
  }

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

  // 1. Validate file owner UID against effective UID
  const uid_t expected_uid = ::geteuid();
  if (sb.st_uid != expected_uid) {
    error_msg = "Shared memory owner UID mismatch: expected " + std::to_string(expected_uid) +
                ", got " + std::to_string(sb.st_uid);
    ::close(fd);
    return std::nullopt;
  }

  // 2. Validate file permissions: must not grant group or other permissions (0600 boundary)
  if ((sb.st_mode & 0077) != 0) {
    error_msg = "Shared memory permissions insecure: permissions contain group/other access (mode: 0" +
                std::to_string(sb.st_mode & 0777) + ")";
    ::close(fd);
    return std::nullopt;
  }

  // 3. Minimum size check before mapping header
  if (static_cast<std::size_t>(sb.st_size) < sizeof(QueueSharedHeader)) {
    error_msg = "Shared memory size too small for QueueSharedHeader: " + std::to_string(sb.st_size);
    ::close(fd);
    return std::nullopt;
  }

  // Map header first to validate capacity and metadata without arbitrarily large initial mapping
  void* hdr_map = ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (hdr_map == MAP_FAILED) {
    const int err = errno;
    error_msg = "mmap header failed: " + std::string(std::strerror(err));
    ::close(fd);
    return std::nullopt;
  }

  auto* hdr = reinterpret_cast<QueueSharedHeader*>(hdr_map);

  // 4. Bounded wait for publication readiness using explicit atomic load with acquire semantics.
  // Ensures open_existing() never inspects partially initialized metadata or locks uninitialized pthread objects.
  // If the creator crashes before publication, open_existing() times out cleanly and safely unmaps
  // without touching uninitialized synchronization objects.
  const int max_attempts = std::max(1, static_cast<int>(wait_timeout.count()));
  bool is_ready = false;
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const std::uint32_t ready_flag = __atomic_load_n(&hdr->init_done, __ATOMIC_ACQUIRE);
    if (ready_flag == 1U) {
      if (hdr->magic == kQueueMagic) {
        is_ready = true;
      }
      break;
    }
    struct timespec req { 0, 1'000'000L };  // 1ms
    ::nanosleep(&req, nullptr);
  }

  if (!is_ready) {
    const std::uint32_t observed_ready = __atomic_load_n(&hdr->init_done, __ATOMIC_ACQUIRE);
    error_msg = "Shared memory queue not ready or magic mismatch: magic=0x" +
                std::to_string(hdr->magic) + ", init_done=" + std::to_string(observed_ready);
    ::munmap(hdr_map, sizeof(QueueSharedHeader));
    ::close(fd);
    return std::nullopt;
  }

  // Validate ABI version, struct size, owner UID
  if (hdr->abi_version != kQueueAbiVersion) {
    error_msg = "Shared memory ABI version mismatch: expected " +
                std::to_string(kQueueAbiVersion) + ", got " + std::to_string(hdr->abi_version);
    ::munmap(hdr_map, sizeof(QueueSharedHeader));
    ::close(fd);
    return std::nullopt;
  }

  if (hdr->struct_size != sizeof(QueueSharedHeader)) {
    error_msg = "QueueSharedHeader struct size mismatch: expected " +
                std::to_string(sizeof(QueueSharedHeader)) + ", got " + std::to_string(hdr->struct_size);
    ::munmap(hdr_map, sizeof(QueueSharedHeader));
    ::close(fd);
    return std::nullopt;
  }

  if (hdr->owner_uid != static_cast<std::uint32_t>(expected_uid)) {
    error_msg = "Header owner UID mismatch: expected " + std::to_string(expected_uid) +
                ", got " + std::to_string(hdr->owner_uid);
    ::munmap(hdr_map, sizeof(QueueSharedHeader));
    ::close(fd);
    return std::nullopt;
  }

  // 5. Validate declared capacity and compute expected size with checked arithmetic
  std::size_t expected_total_size = 0;
  if (!compute_total_mapped_size(hdr->capacity, expected_total_size)) {
    error_msg = "Header declared capacity invalid or out of range: " + std::to_string(hdr->capacity);
    ::munmap(hdr_map, sizeof(QueueSharedHeader));
    ::close(fd);
    return std::nullopt;
  }

  // Re-check actual file size against expected_total_size
  if (::fstat(fd, &sb) != 0 || static_cast<std::size_t>(sb.st_size) != expected_total_size) {
    error_msg = "Shared memory size mismatch: expected " + std::to_string(expected_total_size) +
                ", got " + std::to_string(sb.st_size);
    ::munmap(hdr_map, sizeof(QueueSharedHeader));
    ::close(fd);
    return std::nullopt;
  }

  const std::size_t capacity = hdr->capacity;

  // Unmap preliminary header mapping and map the exact full region
  ::munmap(hdr_map, sizeof(QueueSharedHeader));

  void* addr = ::mmap(nullptr, expected_total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (addr == MAP_FAILED) {
    const int err = errno;
    error_msg = "mmap full region failed: " + std::string(std::strerror(err));
    ::close(fd);
    return std::nullopt;
  }

  ProcessSharedQueue queue(std::move(name), fd, expected_total_size, addr, capacity, role, false);

  // 6. Acquire robust lock before inspecting mutable lifecycle state or binding role
  const pid_t my_pid = ::getpid();
  const int lock_rc = queue.acquire_mutex(&error_msg);
  if (lock_rc == EOWNERDEAD) {
    // acquire_mutex handled fail-stop transition and left lock held
    queue.release_mutex();
    error_msg = "Queue encountered owner death during open and transitioned to Faulted";
    return std::nullopt;
  }
  if (lock_rc != 0) {
    // ENOTRECOVERABLE or lock failure (mutex NOT held)
    error_msg = "Failed to acquire queue mutex during open: " + error_msg;
    return std::nullopt;
  }

  // 7. Validate mutable lifecycle state under lock
  if (queue.header_->state == static_cast<std::uint32_t>(QueueState::Faulted)) {
    queue.release_mutex();
    error_msg = "Queue is in Faulted state";
    return std::nullopt;
  }
  if (queue.header_->state != static_cast<std::uint32_t>(QueueState::Active) &&
      queue.header_->state != static_cast<std::uint32_t>(QueueState::Closed)) {
    queue.release_mutex();
    error_msg = "Queue is in non-connectable state: " + std::to_string(queue.header_->state);
    return std::nullopt;
  }

  if (role == QueueEndpointRole::Producer) {
    if (queue.header_->producer_pid != 0 && queue.header_->producer_pid != my_pid) {
      if (::kill(static_cast<pid_t>(queue.header_->producer_pid), 0) == 0 || errno == EPERM) {
        queue.release_mutex();
        error_msg = "Producer endpoint already bound to active PID " + std::to_string(queue.header_->producer_pid);
        return std::nullopt;
      }
    }
    queue.header_->producer_pid = my_pid;
  } else {
    if (queue.header_->consumer_pid != 0 && queue.header_->consumer_pid != my_pid) {
      if (::kill(static_cast<pid_t>(queue.header_->consumer_pid), 0) == 0 || errno == EPERM) {
        queue.release_mutex();
        error_msg = "Consumer endpoint already bound to active PID " + std::to_string(queue.header_->consumer_pid);
        return std::nullopt;
      }
    }
    queue.header_->consumer_pid = my_pid;
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
    // Mutex ownership IS acquired!
    // Fail-stop policy: transition state to Faulted, wake all waiting threads, make mutex consistent.
    header_->state = static_cast<std::uint32_t>(QueueState::Faulted);
    ::pthread_cond_broadcast(&header_->not_empty_cond);
    ::pthread_cond_broadcast(&header_->not_full_cond);

    const int cons_rc = ::pthread_mutex_consistent(&header_->mutex);
    if (cons_rc != 0) {
      // pthread_mutex_consistent failed; unlock since we own it, and return ENOTRECOVERABLE
      ::pthread_mutex_unlock(&header_->mutex);
      if (error_msg) *error_msg = "pthread_mutex_consistent failed: " + std::string(std::strerror(cons_rc));
      return ENOTRECOVERABLE;
    }
    if (error_msg) *error_msg = "Mutex owner died; queue marked permanently Faulted (EOWNERDEAD)";
    return EOWNERDEAD; // Mutex IS held!
  }

  if (rc == ENOTRECOVERABLE) {
    // Mutex was NOT acquired!
    if (error_msg) *error_msg = "Mutex is in ENOTRECOVERABLE state";
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
  if (lock_rc == EOWNERDEAD) {
    // Lock held, state marked Faulted by acquire_mutex
    release_mutex();
    return QueueOpStatus::Faulted;
  }
  if (lock_rc == ENOTRECOVERABLE) {
    // Lock not held
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
      // Mutex ownership re-acquired upon return from condition wait!
      header_->state = static_cast<std::uint32_t>(QueueState::Faulted);
      ::pthread_cond_broadcast(&header_->not_empty_cond);
      ::pthread_cond_broadcast(&header_->not_full_cond);
      const int cons_rc = ::pthread_mutex_consistent(&header_->mutex);
      release_mutex();
      if (cons_rc != 0) {
        if (error_msg) *error_msg = "Owner died during wait; pthread_mutex_consistent failed: " + std::string(std::strerror(cons_rc));
      } else {
        if (error_msg) *error_msg = "Owner died during condition wait; queue marked Faulted";
      }
      return QueueOpStatus::Faulted;
    }
    if (wait_rc == ENOTRECOVERABLE) {
      // Mutex is not held
      if (error_msg) *error_msg = "Mutex became ENOTRECOVERABLE during condition wait";
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
  if (lock_rc == EOWNERDEAD) {
    release_mutex();
    return QueueOpStatus::Faulted;
  }
  if (lock_rc == ENOTRECOVERABLE) {
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

      // Validate slot integrity before copying
      if (slot.size == 0 || slot.size > kMaxMessageDataSize || slot.sequence != header_->total_popped) {
        header_->state = static_cast<std::uint32_t>(QueueState::Faulted);
        ::pthread_cond_broadcast(&header_->not_empty_cond);
        ::pthread_cond_broadcast(&header_->not_full_cond);
        release_mutex();
        if (error_msg) {
          *error_msg = "Slot corruption detected: slot.size=" + std::to_string(slot.size) +
                       ", slot.sequence=" + std::to_string(slot.sequence) +
                       ", expected_sequence=" + std::to_string(header_->total_popped);
        }
        return QueueOpStatus::Faulted;
      }

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
      const int cons_rc = ::pthread_mutex_consistent(&header_->mutex);
      release_mutex();
      if (cons_rc != 0) {
        if (error_msg) *error_msg = "Owner died during wait; pthread_mutex_consistent failed: " + std::string(std::strerror(cons_rc));
      } else {
        if (error_msg) *error_msg = "Owner died during condition wait; queue marked Faulted";
      }
      return QueueOpStatus::Faulted;
    }
    if (wait_rc == ENOTRECOVERABLE) {
      if (error_msg) *error_msg = "Mutex became ENOTRECOVERABLE during condition wait";
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
  if (role_ != QueueEndpointRole::Producer) {
    if (error_msg) *error_msg = "Only the Producer endpoint may close the queue";
    return false;
  }

  const int lock_rc = acquire_mutex(error_msg);
  if (lock_rc == EOWNERDEAD) {
    release_mutex();
    return false;
  }
  if (lock_rc == ENOTRECOVERABLE || lock_rc != 0) {
    return false;
  }

  if (header_->state == static_cast<std::uint32_t>(QueueState::Active)) {
    header_->state = static_cast<std::uint32_t>(QueueState::Closed);
    ::pthread_cond_broadcast(&header_->not_empty_cond);
    ::pthread_cond_broadcast(&header_->not_full_cond);
  }

  release_mutex();
  return true;
}

QueueSnapshot ProcessSharedQueue::snapshot() const noexcept {
  QueueSnapshot snap{};
  snap.capacity = capacity_;

  if (!header_) {
    return snap;
  }

  auto* mutable_this = const_cast<ProcessSharedQueue*>(this);
  const int lock_rc = mutable_this->acquire_mutex(nullptr);
  if (lock_rc == 0 || lock_rc == EOWNERDEAD) {
    snap.size = header_->count;
    snap.empty = (header_->count == 0);
    snap.full = (header_->count >= header_->capacity);
    snap.state = static_cast<QueueState>(header_->state);
    snap.is_faulted = (snap.state == QueueState::Faulted);
    snap.is_closed = (snap.state == QueueState::Closed);
    mutable_this->release_mutex();
  } else {
    // ENOTRECOVERABLE or lock failure -> report Faulted
    snap.state = QueueState::Faulted;
    snap.is_faulted = true;
  }
  return snap;
}

std::size_t ProcessSharedQueue::size() const noexcept {
  return snapshot().size;
}

bool ProcessSharedQueue::empty() const noexcept {
  return snapshot().empty;
}

bool ProcessSharedQueue::full() const noexcept {
  return snapshot().full;
}

QueueState ProcessSharedQueue::state() const noexcept {
  return snapshot().state;
}

}  // namespace darkpool::ipc

#endif
