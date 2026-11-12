#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace darkpool::ipc {

/**
 * @brief Bounded Multi-Producer Multi-Consumer (MPMC) Queue.
 *
 * Implements Dmitry Vyukov's bounded MPMC queue algorithm using per-slot atomic
 * sequence counters with acquire/release memory ordering.
 *
 * Progress and Availability Semantics:
 * - Nonblocking: Operations do not acquire OS mutexes, sleep, or make kernel syscalls.
 * - Ticket arbitration on enqueue_pos_ and dequeue_pos_ is lock-free (via compare_exchange_weak).
 * - Not strictly lock-free or wait-free end-to-end: If a producer thread claims an earlier slot
 *   and is suspended before publishing its sequence update, later producers may still complete
 *   their enqueue operations in subsequent slots. A consumer inspecting the queue will receive
 *   `false` from `try_pop()` because the FIFO head slot is not yet published.
 * - Therefore, `try_pop() == false` does NOT necessarily mean that no producer has completed
 *   an enqueue anywhere in the queue. A stalled producer temporarily prevents progress at the FIFO head.
 * - Observation metrics `size()`, `empty()`, and `full()` are approximate snapshots during
 *   concurrent operations and must NOT be relied upon as synchronization or correctness conditions.
 * - Destruction requires all concurrent queue operations to have stopped (quiescence precondition).
 *   `clear()` is private and executed by the destructor during quiescence.
 *
 * Exception Safety Constraints:
 * - `try_emplace` requires `std::is_nothrow_constructible_v<T, Args...>`.
 * - `try_push(const T&)` requires `std::is_nothrow_copy_constructible_v<T>`.
 * - `try_push(T&&)` requires `std::is_nothrow_move_constructible_v<T>`.
 * - `try_pop(T&)` requires `std::is_nothrow_move_assignable_v<T>`.
 * - `try_pop()` requires `std::is_nothrow_move_constructible_v<T>`.
 * - `T` must have a non-throwing destructor (`std::is_nothrow_destructible_v<T>`).
 * - No potentially throwing user operation can occur after a ticket is claimed and before
 *   the slot's sequence state is published.
 *
 * Invariants:
 * - Capacity must be a power of two >= 2.
 * - Each slot starts with sequence = slot_index.
 * - A producer at generation pos claims slot (pos & mask) if sequence == pos, writes the item,
 *   and sets sequence = pos + 1 (release).
 * - A consumer at generation pos claims slot (pos & mask) if sequence == pos + 1, moves the item,
 *   destroys it, and sets sequence = pos + capacity (release).
 */
template <typename T, std::size_t StaticCapacity = 0>
class BoundedMpmcQueue {
  static_assert(std::is_destructible_v<T>, "T must be destructible");
  static_assert(std::is_nothrow_destructible_v<T>, "T must have a nothrow destructor");
  static_assert(StaticCapacity == 0 ||
                    (StaticCapacity >= 2 && (StaticCapacity & (StaticCapacity - 1)) == 0),
                "StaticCapacity must be a power of two and >= 2");

 public:
  using value_type = T;

  // Compile-time capacity constructor
  BoundedMpmcQueue() requires(StaticCapacity > 0)
      : BoundedMpmcQueue(StaticCapacity) {}

  // Construction-time capacity constructor
  explicit BoundedMpmcQueue(std::size_t capacity)
      : capacity_(capacity), buffer_mask_(capacity - 1) {
    if (capacity < 2 || (capacity & (capacity - 1)) != 0) {
      throw std::invalid_argument("Capacity must be a power of two and >= 2");
    }
    buffer_ = std::make_unique<Slot[]>(capacity_);
    for (std::size_t i = 0; i < capacity_; ++i) {
      buffer_[i].sequence.store(i, std::memory_order_relaxed);
    }
  }

  // Precondition: All concurrent queue operations must have stopped (quiescence).
  ~BoundedMpmcQueue() {
    clear();
  }

  // Non-copyable, non-movable concurrent primitive
  BoundedMpmcQueue(const BoundedMpmcQueue&) = delete;
  BoundedMpmcQueue& operator=(const BoundedMpmcQueue&) = delete;
  BoundedMpmcQueue(BoundedMpmcQueue&&) = delete;
  BoundedMpmcQueue& operator=(BoundedMpmcQueue&&) = delete;

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  bool try_emplace(Args&&... args) noexcept {
    std::size_t pos = enqueue_pos_.load(std::memory_order_relaxed);
    while (true) {
      auto& slot = buffer_[pos & buffer_mask_];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq - pos);
      if (diff == 0) {
        if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          ::new (static_cast<void*>(slot.storage)) T(std::forward<Args>(args)...);
          slot.sequence.store(pos + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        // Queue is full for this position
        return false;
      } else {
        pos = enqueue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  bool try_push(const T& item) noexcept
    requires std::is_nothrow_copy_constructible_v<T> {
    return try_emplace(item);
  }

  bool try_push(T&& item) noexcept
    requires std::is_nothrow_move_constructible_v<T> {
    return try_emplace(std::move(item));
  }

  bool try_pop(T& item) noexcept
    requires std::is_nothrow_move_assignable_v<T> {
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    while (true) {
      auto& slot = buffer_[pos & buffer_mask_];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq - (pos + 1));
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          auto* ptr = reinterpret_cast<T*>(slot.storage);
          item = std::move(*ptr);
          ptr->~T();
          slot.sequence.store(pos + buffer_mask_ + 1, std::memory_order_release);
          return true;
        }
      } else if (diff < 0) {
        // Queue is empty for this position
        return false;
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  std::optional<T> try_pop() noexcept
    requires std::is_nothrow_move_constructible_v<T> {
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    while (true) {
      auto& slot = buffer_[pos & buffer_mask_];
      const std::size_t seq = slot.sequence.load(std::memory_order_acquire);
      const auto diff = static_cast<std::ptrdiff_t>(seq - (pos + 1));
      if (diff == 0) {
        if (dequeue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
          auto* ptr = reinterpret_cast<T*>(slot.storage);
          std::optional<T> result(std::move(*ptr));
          ptr->~T();
          slot.sequence.store(pos + buffer_mask_ + 1, std::memory_order_release);
          return result;
        }
      } else if (diff < 0) {
        return std::nullopt;
      } else {
        pos = dequeue_pos_.load(std::memory_order_relaxed);
      }
    }
  }

  [[nodiscard]] std::size_t capacity() const noexcept {
    return capacity_;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
    std::size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
    if (enq >= deq) {
      const std::size_t s = enq - deq;
      return s > capacity_ ? capacity_ : s;
    }
    return 0;
  }

  [[nodiscard]] bool empty() const noexcept {
    std::size_t deq = dequeue_pos_.load(std::memory_order_relaxed);
    std::size_t enq = enqueue_pos_.load(std::memory_order_relaxed);
    return enq <= deq;
  }

  [[nodiscard]] bool full() const noexcept {
    return size() >= capacity_;
  }

 private:
  // Clears and destructs all remaining queued items.
  // Precondition: All concurrent queue operations must have stopped (quiescence).
  void clear() noexcept {
    std::size_t pos = dequeue_pos_.load(std::memory_order_relaxed);
    while (true) {
      auto& slot = buffer_[pos & buffer_mask_];
      const std::size_t seq = slot.sequence.load(std::memory_order_relaxed);
      const auto diff = static_cast<std::ptrdiff_t>(seq - (pos + 1));
      if (diff == 0) {
        auto* ptr = reinterpret_cast<T*>(slot.storage);
        ptr->~T();
        slot.sequence.store(pos + buffer_mask_ + 1, std::memory_order_relaxed);
        ++pos;
      } else {
        break;
      }
    }
    dequeue_pos_.store(pos, std::memory_order_relaxed);
  }

  struct Slot {
    std::atomic<std::size_t> sequence{0};
    alignas(alignof(T)) std::byte storage[sizeof(T)];
  };

  const std::size_t capacity_;
  const std::size_t buffer_mask_;
  std::unique_ptr<Slot[]> buffer_;

  alignas(64) std::atomic<std::size_t> enqueue_pos_{0};
  alignas(64) std::atomic<std::size_t> dequeue_pos_{0};
};

}  // namespace darkpool::ipc
