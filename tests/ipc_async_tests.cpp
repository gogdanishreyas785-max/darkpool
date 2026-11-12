#include "darkpool/bounded_mpmc_queue.hpp"
#include "darkpool/event_notifier.hpp"

#if defined(__linux__)

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fcntl.h>
#include <iostream>
#include <numeric>
#include <poll.h>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace {

using namespace std::chrono_literals;
using darkpool::ipc::BoundedMpmcQueue;
using darkpool::ipc::ConsumeStatus;
using darkpool::ipc::EventNotifier;
using darkpool::ipc::NotifyStatus;

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

// ============================================================================
// EventNotifier Tests
// ============================================================================

void test_event_notifier_initial_state_and_nonblocking() {
  std::cerr << "[ipc_async_tests] running test_event_notifier_initial_state_and_nonblocking\n";
  EventNotifier notifier;
  CHECK(notifier.valid());
  CHECK(notifier.descriptor() >= 0);

  // Initial read should return Empty (EAGAIN/EWOULDBLOCK)
  std::uint64_t val = 999;
  int err = 0;
  CHECK(notifier.consume(val, &err) == ConsumeStatus::Empty);
  CHECK(val == 0);
  CHECK(err == EAGAIN || err == EWOULDBLOCK);
  CHECK(!notifier.try_consume().has_value());

  // Invalid increments
  CHECK(notifier.notify(0) == NotifyStatus::InvalidIncrement);
  CHECK(notifier.notify(0xffffffffffffffffULL) == NotifyStatus::InvalidIncrement);
  CHECK(notifier.notify(0xfffffffffffffffeULL) == NotifyStatus::InvalidIncrement);
}

void test_event_notifier_notify_and_consume() {
  std::cerr << "[ipc_async_tests] running test_event_notifier_notify_and_consume\n";
  EventNotifier notifier;
  CHECK(notifier.valid());

  // Notify with 1
  CHECK(notifier.notify(1) == NotifyStatus::Success);

  // Consume should succeed with 1
  std::uint64_t val = 0;
  int err = 0;
  CHECK(notifier.consume(val, &err) == ConsumeStatus::Success);
  CHECK(val == 1);
  CHECK(err == 0);

  // Subsequent consume is empty
  CHECK(notifier.consume(val, &err) == ConsumeStatus::Empty);
  CHECK(!notifier.try_consume().has_value());
}

void test_event_notifier_coalescing() {
  std::cerr << "[ipc_async_tests] running test_event_notifier_coalescing\n";
  EventNotifier notifier;
  CHECK(notifier.valid());

  // Multiple notifications before consumption
  CHECK(notifier.notify(5) == NotifyStatus::Success);
  CHECK(notifier.notify(10) == NotifyStatus::Success);
  CHECK(notifier.notify(3) == NotifyStatus::Success);

  // Single consume should retrieve the sum: 5 + 10 + 3 = 18
  auto consumed = notifier.try_consume();
  CHECK(consumed.has_value());
  CHECK(consumed.value() == 18);

  // Now counter is zero
  CHECK(!notifier.try_consume().has_value());
}

void test_event_notifier_concurrent_producers() {
  std::cerr << "[ipc_async_tests] running test_event_notifier_concurrent_producers\n";
  EventNotifier notifier;
  CHECK(notifier.valid());

  constexpr int kNumThreads = 4;
  constexpr std::uint64_t kIncrementsPerThread = 2500;
  std::vector<std::thread> threads;
  threads.reserve(kNumThreads);

  for (int t = 0; t < kNumThreads; ++t) {
    threads.emplace_back([&notifier]() {
      for (std::uint64_t i = 0; i < kIncrementsPerThread; ++i) {
        CHECK(notifier.notify(1) == NotifyStatus::Success);
      }
    });
  }

  for (auto& th : threads) {
    th.join();
  }

  // Sum should equal kNumThreads * kIncrementsPerThread
  auto total = notifier.try_consume();
  CHECK(total.has_value());
  CHECK(total.value() == kNumThreads * kIncrementsPerThread);
  CHECK(!notifier.try_consume().has_value());
}

void test_event_notifier_poll_readiness() {
  std::cerr << "[ipc_async_tests] running test_event_notifier_poll_readiness\n";
  EventNotifier notifier;
  CHECK(notifier.valid());

  pollfd pfd{.fd = notifier.descriptor(), .events = POLLIN, .revents = 0};

  // Initially empty -> poll should time out with 0 events
  int poll_rc = ::poll(&pfd, 1, 10);
  CHECK(poll_rc == 0);
  CHECK((pfd.revents & POLLIN) == 0);

  // Notify
  CHECK(notifier.notify(1) == NotifyStatus::Success);

  // Poll should immediately report POLLIN
  poll_rc = ::poll(&pfd, 1, 500);
  CHECK(poll_rc == 1);
  CHECK((pfd.revents & POLLIN) != 0);

  // Consume counter
  auto val = notifier.try_consume();
  CHECK(val.has_value() && *val == 1);

  // Poll again -> should be not readable
  pfd.revents = 0;
  poll_rc = ::poll(&pfd, 1, 10);
  CHECK(poll_rc == 0);
  CHECK((pfd.revents & POLLIN) == 0);
}

void test_event_notifier_move_and_cleanup() {
  std::cerr << "[ipc_async_tests] running test_event_notifier_move_and_cleanup\n";
  int original_fd = -1;
  {
    EventNotifier n1;
    CHECK(n1.valid());
    original_fd = n1.descriptor();
    CHECK(::fcntl(original_fd, F_GETFD) != -1);

    // Move construction
    EventNotifier n2(std::move(n1));
    CHECK(!n1.valid());
    CHECK(n1.descriptor() == -1);
    CHECK(n2.valid());
    CHECK(n2.descriptor() == original_fd);

    // Move assignment
    EventNotifier n3;
    int n3_fd = n3.descriptor();
    CHECK(::fcntl(n3_fd, F_GETFD) != -1);

    n3 = std::move(n2);
    CHECK(!n2.valid());
    CHECK(n2.descriptor() == -1);
    CHECK(n3.valid());
    CHECK(n3.descriptor() == original_fd);
    // n3's old fd should have been closed
    CHECK(::fcntl(n3_fd, F_GETFD) == -1 && errno == EBADF);

    // Explicit close on n3
    n3.close();
    CHECK(!n3.valid());
    CHECK(n3.descriptor() == -1);
    CHECK(::fcntl(original_fd, F_GETFD) == -1 && errno == EBADF);
  }
}

// ============================================================================
// Bounded MPMC Queue Tests
// ============================================================================

void test_queue_empty_and_full() {
  std::cerr << "[ipc_async_tests] running test_queue_empty_and_full\n";
  BoundedMpmcQueue<int> queue(4);
  CHECK(queue.capacity() == 4);
  CHECK(queue.empty());
  CHECK(!queue.full());
  CHECK(queue.size() == 0);

  int val = 0;
  CHECK(!queue.try_pop(val));
  CHECK(!queue.try_pop().has_value());

  // Fill the queue
  CHECK(queue.try_push(10));
  CHECK(queue.try_push(20));
  CHECK(queue.try_push(30));
  CHECK(queue.try_push(40));

  CHECK(queue.full());
  CHECK(!queue.empty());
  CHECK(queue.size() == 4);

  // Try push on full queue
  CHECK(!queue.try_push(50));
  CHECK(!queue.try_emplace(60));

  // Pop elements in order
  CHECK(queue.try_pop(val) && val == 10);
  CHECK(queue.try_pop(val) && val == 20);
  CHECK(queue.try_pop(val) && val == 30);
  CHECK(queue.try_pop(val) && val == 40);

  // Queue is now empty
  CHECK(queue.empty());
  CHECK(!queue.full());
  CHECK(queue.size() == 0);
  CHECK(!queue.try_pop(val));
}

void test_queue_single_producer_consumer_fifo() {
  std::cerr << "[ipc_async_tests] running test_queue_single_producer_consumer_fifo\n";
  BoundedMpmcQueue<int, 128> queue;  // Compile-time static capacity
  CHECK(queue.capacity() == 128);

  constexpr int kCount = 100;
  for (int i = 0; i < kCount; ++i) {
    CHECK(queue.try_push(i));
  }

  CHECK(queue.size() == kCount);

  for (int i = 0; i < kCount; ++i) {
    auto popped = queue.try_pop();
    CHECK(popped.has_value());
    CHECK(*popped == i);
  }

  CHECK(queue.empty());
}

void test_queue_repeated_wraparound() {
  std::cerr << "[ipc_async_tests] running test_queue_repeated_wraparound\n";
  // Small capacity of 4 to force rapid and repeated wraparounds
  BoundedMpmcQueue<int> queue(4);

  constexpr int kIterations = 10000;
  for (int i = 0; i < kIterations; ++i) {
    CHECK(queue.try_push(i));
    auto item = queue.try_pop();
    CHECK(item.has_value() && *item == i);
  }

  CHECK(queue.empty());
}

void test_queue_mpmc_concurrent() {
  std::cerr << "[ipc_async_tests] running test_queue_mpmc_concurrent\n";
  constexpr std::size_t kCapacity = 64;
  BoundedMpmcQueue<std::uint64_t> queue(kCapacity);

  constexpr int kProducers = 4;
  constexpr int kConsumers = 4;
  constexpr std::uint64_t kItemsPerProducer = 10000;
  constexpr std::uint64_t kTotalItems = kProducers * kItemsPerProducer;

  std::atomic<bool> producers_done{false};
  std::atomic<std::uint64_t> total_consumed{0};

  // Start consumers
  std::vector<std::vector<std::uint64_t>> consumer_results(kConsumers);
  std::vector<std::thread> consumers;
  consumers.reserve(kConsumers);

  for (int c = 0; c < kConsumers; ++c) {
    consumers.emplace_back([&queue, &producers_done, &total_consumed, &consumer_results, c]() {
      auto& my_results = consumer_results[static_cast<std::size_t>(c)];
      while (true) {
        std::uint64_t item = 0;
        if (queue.try_pop(item)) {
          my_results.push_back(item);
          total_consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          if (producers_done.load(std::memory_order_acquire) && queue.empty()) {
            // Check once more to prevent race between empty and last item pushed
            if (!queue.try_pop(item)) {
              break;
            }
            my_results.push_back(item);
            total_consumed.fetch_add(1, std::memory_order_relaxed);
          }
          std::this_thread::yield();
        }
      }
    });
  }

  // Start producers
  std::vector<std::thread> producers;
  producers.reserve(kProducers);

  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue, p]() {
      for (std::uint64_t i = 0; i < kItemsPerProducer; ++i) {
        const std::uint64_t item = (static_cast<std::uint64_t>(p) << 32) | i;
        while (!queue.try_push(item)) {
          std::this_thread::yield();
        }
      }
    });
  }

  // Join producers
  for (auto& th : producers) {
    th.join();
  }
  producers_done.store(true, std::memory_order_release);

  // Join consumers
  for (auto& th : consumers) {
    th.join();
  }

  CHECK(total_consumed.load() == kTotalItems);

  // Verify all items are unique and cover every item from every producer
  std::unordered_set<std::uint64_t> seen;
  seen.reserve(kTotalItems);

  for (const auto& res : consumer_results) {
    for (std::uint64_t item : res) {
      auto [it, inserted] = seen.insert(item);
      CHECK(inserted);  // No duplicates!
    }
  }

  CHECK(seen.size() == kTotalItems);
}

// Track object lifetimes to test queue cleanup upon destruction
struct LifetimeTracker {
  static inline std::atomic<int> constructions{0};
  static inline std::atomic<int> destructions{0};

  int id{0};

  static void reset() noexcept {
    constructions.store(0);
    destructions.store(0);
  }

  explicit LifetimeTracker(int v = 0) noexcept : id(v) {
    constructions.fetch_add(1, std::memory_order_relaxed);
  }

  LifetimeTracker(const LifetimeTracker& other) noexcept : id(other.id) {
    constructions.fetch_add(1, std::memory_order_relaxed);
  }

  LifetimeTracker(LifetimeTracker&& other) noexcept : id(other.id) {
    constructions.fetch_add(1, std::memory_order_relaxed);
  }

  LifetimeTracker& operator=(const LifetimeTracker& other) noexcept {
    id = other.id;
    return *this;
  }

  LifetimeTracker& operator=(LifetimeTracker&& other) noexcept {
    id = other.id;
    return *this;
  }

  ~LifetimeTracker() noexcept {
    destructions.fetch_add(1, std::memory_order_relaxed);
  }
};

void test_queue_destruction_with_queued_items() {
  std::cerr << "[ipc_async_tests] running test_queue_destruction_with_queued_items\n";
  LifetimeTracker::reset();

  {
    BoundedMpmcQueue<LifetimeTracker> queue(8);
    CHECK(queue.try_emplace(1));
    CHECK(queue.try_emplace(2));
    CHECK(queue.try_emplace(3));
    CHECK(queue.size() == 3);

    CHECK(LifetimeTracker::constructions.load() == 3);
    CHECK(LifetimeTracker::destructions.load() == 0);

    // Queue goes out of scope here with 3 items queued
  }

  // All 3 items should have been properly destructed by ~BoundedMpmcQueue
  CHECK(LifetimeTracker::constructions.load() == 3);
  CHECK(LifetimeTracker::destructions.load() == 3);
}

// ============================================================================
// Combined Queue + EventNotifier Test
// ============================================================================

void test_queue_with_event_notifier() {
  std::cerr << "[ipc_async_tests] running test_queue_with_event_notifier\n";
  BoundedMpmcQueue<std::uint64_t> queue(32);
  EventNotifier notifier;
  CHECK(notifier.valid());

  constexpr int kProducers = 3;
  constexpr std::uint64_t kItemsPerProducer = 5000;
  constexpr std::uint64_t kTotalItems = kProducers * kItemsPerProducer;

  std::atomic<bool> producers_done{false};
  std::vector<std::uint64_t> consumed_items;
  consumed_items.reserve(kTotalItems);

  // Single consumer thread using poll on EventNotifier descriptor to drain queue
  std::thread consumer([&queue, &notifier, &producers_done, &consumed_items]() {
    pollfd pfd{.fd = notifier.descriptor(), .events = POLLIN, .revents = 0};

    while (consumed_items.size() < kTotalItems) {
      // Bounded poll wait (100ms)
      pfd.revents = 0;
      int p_rc = ::poll(&pfd, 1, 100);
      if (p_rc > 0 && (pfd.revents & POLLIN) != 0) {
        // Clear the notification counter
        (void)notifier.try_consume();
      }

      // CRITICAL: The queue contents are the authoritative source of truth,
      // NOT the notification count! Drain all available elements from the queue.
      std::uint64_t val = 0;
      while (queue.try_pop(val)) {
        consumed_items.push_back(val);
      }

      if (producers_done.load(std::memory_order_acquire) && queue.empty()) {
        // Final drain check
        while (queue.try_pop(val)) {
          consumed_items.push_back(val);
        }
        if (consumed_items.size() >= kTotalItems) {
          break;
        }
      }
    }
  });

  // Multiple producer threads
  std::vector<std::thread> producers;
  producers.reserve(kProducers);

  for (int p = 0; p < kProducers; ++p) {
    producers.emplace_back([&queue, &notifier, p]() {
      for (std::uint64_t i = 0; i < kItemsPerProducer; ++i) {
        const std::uint64_t item = (static_cast<std::uint64_t>(p) << 32) | i;
        while (!queue.try_push(item)) {
          std::this_thread::yield();
        }
        // Signal readiness
        CHECK(notifier.notify(1) == NotifyStatus::Success);
      }
    });
  }

  // Join producers
  for (auto& th : producers) {
    th.join();
  }
  producers_done.store(true, std::memory_order_release);

  // Wake up consumer in case it is blocked in poll
  (void)notifier.notify(1);

  // Join consumer
  consumer.join();

  CHECK(consumed_items.size() == kTotalItems);

  // Verify all elements present and unique
  std::unordered_set<std::uint64_t> seen;
  seen.reserve(kTotalItems);
  for (std::uint64_t item : consumed_items) {
    auto [it, inserted] = seen.insert(item);
    CHECK(inserted);
  }
  CHECK(seen.size() == kTotalItems);
}

}  // namespace

int main() {
  std::cerr << "=== darkpool_ipc_async_tests starting ===\n";

  // EventNotifier tests
  test_event_notifier_initial_state_and_nonblocking();
  test_event_notifier_notify_and_consume();
  test_event_notifier_coalescing();
  test_event_notifier_concurrent_producers();
  test_event_notifier_poll_readiness();
  test_event_notifier_move_and_cleanup();

  // BoundedMpmcQueue tests
  test_queue_empty_and_full();
  test_queue_single_producer_consumer_fifo();
  test_queue_repeated_wraparound();
  test_queue_mpmc_concurrent();
  test_queue_destruction_with_queued_items();

  // Combined test
  test_queue_with_event_notifier();

  if (g_failures == 0) {
    std::cerr << "=== all darkpool_ipc_async_tests PASSED ===\n";
    return 0;
  }
  std::cerr << "=== darkpool_ipc_async_tests FAILED: " << g_failures << " failures ===\n";
  return 1;
}

#else

int main() {
  return 0;
}

#endif

