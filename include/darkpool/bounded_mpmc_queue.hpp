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
 * Progress Guarantees:
 * - Nonblocking: Operations do not acquire OS mutexes, sleep, or make kernel syscalls.
 * - Thread arbitration for slot tickets is lock-free (via compare_exchange_weak on positions).
 * - Not strictly wait-free or lock-free end-to-end: If a thread claims a slot and is
 *   suspended before publishing its sequence update, other operations on that specific slot
 *   cannot complete until the thread is resumed.
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
  static_assert(std::is_nothrow_destructible_v<T>, "T must be nothrow destructible");
  static_assert(std::is_move_constructible_v<T> || std::is_copy_constructible_v<T>,
                "T must be move constructible or copy constructible");
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

  ~BoundedMpmcQueue() {
    clear();
  }

  // Non-copyable, non-movable concurrent primitive
  BoundedMpmcQueue(const BoundedMpmcQueue&) = delete;
  BoundedMpmcQueue& operator=(const BoundedMpmcQueue&) = delete;
  BoundedMpmcQueue(BoundedMpmcQueue&&) = delete;
  BoundedMpmcQueue& operator=(BoundedMpmcQueue&&) = delete;

  template <typename... Args>
  bool try_emplace(Args&&... args) {
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

  bool try_push(const T& item) {
    return try_emplace(item);
  }

  bool try_push(T&& item) {
    return try_emplace(std::move(item));
  }

  bool try_pop(T& item) {
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

  std::optional<T> try_pop() {
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

 private:
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

