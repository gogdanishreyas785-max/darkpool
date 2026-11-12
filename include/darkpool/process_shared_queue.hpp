#pragma once

#if defined(__linux__)

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <pthread.h>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <type_traits>

namespace darkpool::ipc {

inline constexpr std::uint32_t kQueueMagic = 0x44505155U;  // 'DPQU' (DarkPool QUeue)
inline constexpr std::uint32_t kQueueAbiVersion = 1U;
inline constexpr std::size_t kMaxMessageDataSize = 256U;

enum class QueueState : std::uint32_t {
  Uninitialized = 0U,
  Active = 1U,
  Closed = 2U,
  Faulted = 3U
};

enum class QueueOpStatus {
  Success,
  Timeout,
  Closed,
  Faulted,
  Invalid
};

enum class QueueEndpointRole {
  Producer,
  Consumer
};

#pragma pack(push, 8)
struct QueueMessageSlot {
  std::uint64_t sequence;
  std::uint32_t size;
  std::uint32_t reserved;
  std::uint8_t data[kMaxMessageDataSize];
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<QueueMessageSlot>, "QueueMessageSlot must be standard layout");

#pragma pack(push, 8)
struct QueueSharedHeader {
  std::uint32_t magic;
  std::uint32_t abi_version;
  std::uint32_t struct_size;
  std::uint32_t owner_uid;
  std::int64_t creator_pid;
  std::int64_t producer_pid;
  std::int64_t consumer_pid;

  std::uint32_t capacity;
  std::uint32_t state;
  std::uint32_t init_done;  // 1 when fully initialized (mutex, cond, fields ready)
  std::uint32_t reserved;

  std::uint64_t head;       // Next dequeue index
  std::uint64_t tail;       // Next enqueue index
  std::uint64_t count;      // Current number of items in ring
  std::uint64_t total_pushed;
  std::uint64_t total_popped;

  pthread_mutex_t mutex;
  pthread_cond_t not_empty_cond;
  pthread_cond_t not_full_cond;
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<QueueSharedHeader>, "QueueSharedHeader must be standard layout");

struct QueueSnapshot {
  std::size_t capacity{0};
  std::size_t size{0};
  bool empty{true};
  bool full{false};
  QueueState state{QueueState::Uninitialized};
  bool is_faulted{false};
  bool is_closed{false};
};

bool is_valid_queue_shm_name(std::string_view name) noexcept;

class ProcessSharedQueue {
 public:
  static constexpr std::chrono::milliseconds kDefaultTimeout{5000};

  ~ProcessSharedQueue();

  ProcessSharedQueue(const ProcessSharedQueue&) = delete;
  ProcessSharedQueue& operator=(const ProcessSharedQueue&) = delete;
  ProcessSharedQueue(ProcessSharedQueue&& other) noexcept;
  ProcessSharedQueue& operator=(ProcessSharedQueue&& other) noexcept;

  static std::optional<ProcessSharedQueue> create(
      std::string_view shm_name,
      std::size_t capacity,
      QueueEndpointRole role,
      std::string& error_msg);

  static std::optional<ProcessSharedQueue> open_existing(
      std::string_view shm_name,
      QueueEndpointRole role,
      std::string& error_msg);

  QueueOpStatus push(
      const void* data,
      std::size_t size,
      std::chrono::milliseconds timeout = kDefaultTimeout,
      std::string* error_msg = nullptr);

  QueueOpStatus push(
      std::string_view message,
      std::chrono::milliseconds timeout = kDefaultTimeout,
      std::string* error_msg = nullptr) {
    return push(message.data(), message.size(), timeout, error_msg);
  }

  QueueOpStatus pop(
      void* buffer,
      std::size_t buffer_capacity,
      std::size_t& out_size,
      std::uint64_t* out_sequence = nullptr,
      std::chrono::milliseconds timeout = kDefaultTimeout,
      std::string* error_msg = nullptr);

  QueueOpStatus pop(
      std::string& out_message,
      std::uint64_t* out_sequence = nullptr,
      std::chrono::milliseconds timeout = kDefaultTimeout,
      std::string* error_msg = nullptr);

  // Producer-owned graceful closure: consumer close attempts return false with error
  bool close(std::string* error_msg = nullptr);

  [[nodiscard]] bool valid() const noexcept { return header_ != nullptr; }
  [[nodiscard]] std::string_view name() const noexcept { return shm_name_; }
  [[nodiscard]] QueueEndpointRole role() const noexcept { return role_; }

  // Immutable metadata
  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  // Synchronized accessors: acquires shared mutex to safely snapshot mutable state
  [[nodiscard]] std::size_t size() const noexcept;
  [[nodiscard]] bool empty() const noexcept;
  [[nodiscard]] bool full() const noexcept;
  [[nodiscard]] QueueState state() const noexcept;
  [[nodiscard]] bool is_faulted() const noexcept { return state() == QueueState::Faulted; }
  [[nodiscard]] bool is_closed() const noexcept { return state() == QueueState::Closed; }

  // Atomic multi-attribute snapshot
  [[nodiscard]] QueueSnapshot snapshot() const noexcept;

  // Manual unlink helper
  static bool unlink(std::string_view shm_name) noexcept;

 private:
  explicit ProcessSharedQueue(std::string shm_name,
                              int fd,
                              std::size_t total_mapped_size,
                              void* mapped_base,
                              std::size_t capacity,
                              QueueEndpointRole role,
                              bool is_creator);

  void cleanup() noexcept;

  // Robust mutex lock helper:
  // Returns:
  //   0: Lock acquired normally.
  //   EOWNERDEAD: Previous owner died while holding mutex. Mutex IS OWNED by caller;
  //               State marked Faulted, condition variables broadcast,
  //               and pthread_mutex_consistent() succeeded. Caller MUST unlock.
  //   ENOTRECOVERABLE: Mutex cannot be acquired / recovery failed. Caller does NOT own mutex.
  //   other errno: Failure, mutex not owned.
  int acquire_mutex(std::string* error_msg = nullptr) noexcept;
  void release_mutex() noexcept;

  std::string shm_name_;
  int fd_{-1};
  std::size_t total_mapped_size_{0};
  void* mapped_base_{nullptr};
  QueueSharedHeader* header_{nullptr};
  QueueMessageSlot* slots_{nullptr};
  std::size_t capacity_{0};
  QueueEndpointRole role_{QueueEndpointRole::Producer};
  bool is_creator_{false};
};

}  // namespace darkpool::ipc

#endif
