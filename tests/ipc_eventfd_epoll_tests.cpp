#include "darkpool/bounded_mpmc_queue.hpp"
#include "darkpool/event_notifier.hpp"
#include "darkpool/server_runtime.hpp"

#if defined(__linux__)

#include <sys/epoll.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using darkpool::ipc::BoundedMpmcQueue;
using darkpool::ipc::ConsumeStatus;
using darkpool::ipc::EventNotifier;
using darkpool::ipc::NotifyStatus;
using darkpool::network::ServerRuntime;
using darkpool::network::ServerRuntimeConfig;

inline std::atomic<int> g_failures{0};
inline std::mutex g_log_mutex;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_log_mutex);
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

// ============================================================================
// Test 1: Registration and Lifetime Checks
// ============================================================================

void test_registration_and_lifetime() {
  std::cerr << "[ipc_eventfd_epoll_tests] running test_registration_and_lifetime\n";

  EventNotifier notifier;
  CHECK(notifier.valid());
  const int notifier_fd = notifier.descriptor();
  CHECK(notifier_fd >= 0);

  ServerRuntimeConfig config{};
  config.enable_tcp = false;
  config.enable_tls = false;
  config.enable_unix = false;
  config.handle_signals = false;
  config.epoll_timeout_ms = 10;

  ServerRuntime runtime(config);

  // 1. Registration before ServerRuntime::start() must fail with documented error
  std::string err;
  bool registered = runtime.add_epoll_event_source(
      notifier_fd, [](std::uint32_t) {}, err);
  CHECK(!registered);
  CHECK(err == "epoll event source registration requires a started runtime");

  // 2. Start the runtime (without any TCP/TLS/UNIX listeners)
  std::string start_err;
  CHECK(runtime.start(start_err));
  CHECK(runtime.is_running());

  // 3. Registration after successful startup must succeed
  err.clear();
  std::atomic<bool> callback_invoked{false};
  registered = runtime.add_epoll_event_source(
      notifier_fd,
      [&](std::uint32_t events) {
        callback_invoked.store(true, std::memory_order_relaxed);
        CHECK((events & EPOLLIN) != 0U);
      },
      err);
  CHECK(registered);
  CHECK(err.empty());

  // 4. Registering the same descriptor twice must be rejected
  std::string dup_err;
  bool dup_registered = runtime.add_epoll_event_source(
      notifier_fd, [](std::uint32_t) {}, dup_err);
  CHECK(!dup_registered);
  CHECK(dup_err == "invalid or already registered epoll event source");

  // 5. Registering invalid descriptor or null handler must be rejected
  std::string inv_err;
  CHECK(!runtime.add_epoll_event_source(-1, [](std::uint32_t) {}, inv_err));
  std::string null_err;
  CHECK(!runtime.add_epoll_event_source(notifier_fd + 1, nullptr, null_err));

  // 6. Verify notification and callback invocation
  CHECK(notifier.notify(1) == NotifyStatus::Success);
  CHECK(runtime.step(50));
  CHECK(callback_invoked.load());

  // Consume to reset counter
  std::uint64_t val = 0;
  CHECK(notifier.consume(val) == ConsumeStatus::Success);
  CHECK(val == 1);

  // 7. Stop the runtime: must unregister external descriptor and not close it
  runtime.stop();
  CHECK(!runtime.is_running());

  // 8. EventNotifier descriptor must remain valid and operational after ServerRuntime::stop()
  CHECK(notifier.valid());
  CHECK(notifier.descriptor() == notifier_fd);

  int notify_err = 0;
  CHECK(notifier.notify(5, &notify_err) == NotifyStatus::Success);
  CHECK(notify_err == 0);

  int consume_err = 0;
  val = 0;
  CHECK(notifier.consume(val, &consume_err) == ConsumeStatus::Success);
  CHECK(val == 5);
  CHECK(consume_err == 0);

  // Close notifier safely
  notifier.close();
  CHECK(!notifier.valid());
}

// ============================================================================
// Test 2: Deterministic Two-Stage Wakeup and Coalescing Integration
// ============================================================================

void test_eventfd_epoll_queue_integration() {
  std::cerr << "[ipc_eventfd_epoll_tests] running test_eventfd_epoll_queue_integration\n";

  constexpr std::size_t kItemCount = 512;
  constexpr std::size_t kQueueCapacity = 1024; // Power-of-two compile-time capacity >= 512

  // 1. Instantiate primitives with compile-time queue capacity
  EventNotifier notifier;
  CHECK(notifier.valid());

  BoundedMpmcQueue<std::uint64_t, kQueueCapacity> queue;

  ServerRuntimeConfig config{};
  config.enable_tcp = false;
  config.enable_tls = false;
  config.enable_unix = false;
  config.handle_signals = false;
  config.epoll_timeout_ms = 200;

  ServerRuntime runtime(config);
  std::string start_err;
  CHECK(runtime.start(start_err));
  CHECK(runtime.is_running());

  // 2. Deterministic coordination primitives with cooperative cancellation
  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  bool stage1_ack = false;
  bool stage2_burst_done = false;
  bool abort_requested = false;
  bool producer_failed = false;
  std::string producer_error_msg;

  std::vector<std::uint64_t> consumed_items;
  consumed_items.reserve(kItemCount);

  std::vector<std::uint64_t> callback_counter_values;
  std::size_t callback_executions = 0;
  std::size_t spurious_empty_consumes = 0;
  std::size_t unexpected_errors = 0;

  // 3. Register callback on EventNotifier descriptor
  std::string reg_err;
  const bool reg_ok = runtime.add_epoll_event_source(
      notifier.descriptor(),
      [&](std::uint32_t events) {
        ++callback_executions;
        CHECK((events & EPOLLIN) != 0U);

        // a. Consume accumulated counter
        std::uint64_t counter_val = 0;
        int c_err = 0;
        const auto c_status = notifier.consume(counter_val, &c_err);
        if (c_status == ConsumeStatus::Success) {
          callback_counter_values.push_back(counter_val);
        } else if (c_status == ConsumeStatus::Empty) {
          ++spurious_empty_consumes;
        } else {
          ++unexpected_errors;
          std::cerr << "Unexpected consume error in callback: " << c_err << '\n';
        }

        // b. Drain the MPMC queue (authoritative source of work)
        while (true) {
          std::uint64_t item = 0;
          if (queue.try_pop(item)) {
            consumed_items.push_back(item);
          } else {
            break;
          }
        }

        // c. If stage 1 just processed item 0, acknowledge to producer
        if (callback_executions == 1) {
          std::lock_guard<std::mutex> lock(sync_mutex);
          stage1_ack = true;
          sync_cv.notify_all();
        }
      },
      reg_err);
  CHECK(reg_ok);
  CHECK(reg_err.empty());

  // 4. Producer thread executing controlled two-stage sequence with cooperative cancellation
  std::thread producer([&]() {
    // Helper to record failure without self-deadlock
    auto record_failure = [&](std::unique_lock<std::mutex>& lock, const std::string& msg) {
      producer_failed = true;
      producer_error_msg = msg;
      sync_cv.notify_all();
      lock.unlock();
    };

    // Initial cancellation check
    {
      std::unique_lock<std::mutex> lock(sync_mutex);
      if (abort_requested) return;
    }

    // Stage 1: Brief initial yield so reactor enters bounded step(), then enqueue item 0
    std::this_thread::sleep_for(20ms);

    {
      std::unique_lock<std::mutex> lock(sync_mutex);
      if (abort_requested) return;

      if (!queue.try_push(0)) {
        record_failure(lock, "Queue push failed for item 0");
        return;
      }
      int err = 0;
      if (notifier.notify(1, &err) != NotifyStatus::Success) {
        record_failure(lock, "EventNotifier notify failed for item 0: errno=" + std::to_string(err));
        return;
      }

      // Wait for reactor to acknowledge processing item 0 via stage1_ack
      const auto stage1_deadline = std::chrono::steady_clock::now() + 5s;
      const bool ack_received = sync_cv.wait_until(
          lock, stage1_deadline, [&] { return stage1_ack || abort_requested || producer_failed; });
      if (!ack_received) {
        record_failure(lock, "Timed out waiting for reactor stage 1 acknowledgement");
        return;
      }
      if (abort_requested || producer_failed) return;
    }

    // Stage 2: Push remaining items 1..511 in a continuous burst while reactor is waiting
    for (std::uint64_t i = 1; i < kItemCount; ++i) {
      {
        std::unique_lock<std::mutex> lock(sync_mutex);
        if (abort_requested) return;
      }

      if (!queue.try_push(i)) {
        std::unique_lock<std::mutex> lock(sync_mutex);
        record_failure(lock, "Queue push failed during burst at item " + std::to_string(i));
        return;
      }
      int n_err = 0;
      if (notifier.notify(1, &n_err) != NotifyStatus::Success) {
        std::unique_lock<std::mutex> lock(sync_mutex);
        record_failure(lock, "EventNotifier notify failed during burst at item " + std::to_string(n_err));
        return;
      }
    }

    // Signal burst completion to reactor
    {
      std::lock_guard<std::mutex> lock(sync_mutex);
      if (!abort_requested) {
        stage2_burst_done = true;
      }
      sync_cv.notify_all();
    }
  });

  // 5. Reactor Thread (Main thread) executing controlled two-stage steps with safe failure handling
  auto cancel_producer_and_join = [&]() {
    {
      std::lock_guard<std::mutex> lock(sync_mutex);
      abort_requested = true;
      sync_cv.notify_all();
    }
    if (producer.joinable()) {
      producer.join();
    }
  };

  // Step A: Drive reactor for stage 1 (wake on item 0)
  bool stage1_ok = runtime.step(500);
  if (!stage1_ok || callback_executions != 1 || consumed_items.empty() || consumed_items[0] != 0) {
    CHECK(false);
    std::cerr << "Stage 1 reactor step failed: step=" << stage1_ok
              << " callbacks=" << callback_executions
              << " consumed=" << consumed_items.size() << '\n';
    cancel_producer_and_join();
    runtime.stop();
    return;
  }

  // Wait for producer to complete Stage 2 burst before reactor calls step() again
  bool burst_ok = false;
  {
    std::unique_lock<std::mutex> lock(sync_mutex);
    const auto burst_deadline = std::chrono::steady_clock::now() + 5s;
    burst_ok = sync_cv.wait_until(
        lock, burst_deadline, [&] { return stage2_burst_done || producer_failed; });
    if (!burst_ok || producer_failed) {
      CHECK(false);
      std::cerr << "Stage 2 producer burst failed or timed out: burst_ok=" << burst_ok
                << " producer_failed=" << producer_failed
                << " error=" << producer_error_msg << '\n';
    }
  }

  if (!burst_ok || producer_failed) {
    cancel_producer_and_join();
    runtime.stop();
    return;
  }

  // Step B: Drive reactor for stage 2 (wake on coalesced burst of 511 items)
  bool stage2_ok = runtime.step(500);
  CHECK(stage2_ok);
  CHECK(callback_executions == 2);

  // Cleanly join producer thread
  if (producer.joinable()) {
    producer.join();
  }

  // 6. Validate deterministic two-stage execution results
  CHECK(consumed_items.size() == kItemCount);
  CHECK(callback_executions == 2);
  CHECK(callback_counter_values.size() == 2);
  if (callback_counter_values.size() == 2) {
    CHECK(callback_counter_values[0] == 1);
    CHECK(callback_counter_values[1] == 511);
  }

  std::uint64_t total_counter_sum = 0;
  for (std::uint64_t val : callback_counter_values) {
    total_counter_sum += val;
  }
  CHECK(total_counter_sum == kItemCount);
  CHECK(spurious_empty_consumes == 0);
  CHECK(unexpected_errors == 0);

  // Verify exact-once FIFO ordering
  bool ordered = true;
  for (std::size_t i = 0; i < consumed_items.size(); ++i) {
    if (consumed_items[i] != i) {
      ordered = false;
      CHECK(false);
      std::cerr << "Ordering mismatch at index " << i << ": expected " << i
                << ", got " << consumed_items[i] << '\n';
      break;
    }
  }
  CHECK(ordered);

  std::cerr << "  Work items delivered: " << consumed_items.size() << '\n';
  std::cerr << "  Callback executions: " << callback_executions << '\n';
  if (callback_counter_values.size() == 2) {
    std::cerr << "  Callback 1 counter: " << callback_counter_values[0] << '\n';
    std::cerr << "  Callback 2 counter: " << callback_counter_values[1] << '\n';
  }

  // 7. Nonblocking step when queue and eventfd are empty: must not invoke callback
  const std::size_t callbacks_before = callback_executions;
  CHECK(runtime.step(0)); // Nonblocking step
  CHECK(callback_executions == callbacks_before);

  runtime.stop();
  CHECK(!runtime.is_running());
}

// ============================================================================
// Test 3: Failure & Cancellation Path Verification
// ============================================================================

void test_producer_cancellation_and_timeout_path() {
  std::cerr << "[ipc_eventfd_epoll_tests] running test_producer_cancellation_and_timeout_path\n";

  constexpr std::size_t kQueueCapacity = 16;
  EventNotifier notifier;
  CHECK(notifier.valid());
  BoundedMpmcQueue<std::uint64_t, kQueueCapacity> queue;

  std::mutex sync_mutex;
  std::condition_variable sync_cv;
  bool stage1_ack = false;
  bool abort_requested = false;
  bool producer_failed = false;
  std::string producer_error_msg;

  // Launch a producer with a tight stage 1 acknowledgement deadline (100ms)
  // to deliberately trigger the timeout failure path and verify no self-deadlock occurs
  std::thread producer([&]() {
    auto record_failure = [&](std::unique_lock<std::mutex>& lock, const std::string& msg) {
      producer_failed = true;
      producer_error_msg = msg;
      sync_cv.notify_all();
      lock.unlock();
    };

    std::unique_lock<std::mutex> lock(sync_mutex);
    if (!queue.try_push(999)) {
      record_failure(lock, "Queue push failed");
      return;
    }
    if (notifier.notify(1) != NotifyStatus::Success) {
      record_failure(lock, "Notify failed");
      return;
    }

    // Deliberately wait with a short 100ms deadline where reactor does NOT acknowledge
    const auto timeout_deadline = std::chrono::steady_clock::now() + 100ms;
    const bool ack = sync_cv.wait_until(
        lock, timeout_deadline, [&] { return stage1_ack || abort_requested || producer_failed; });
    if (!ack) {
      // Must unlock safely and record failure without deadlocking
      record_failure(lock, "Intentional timeout waiting for stage 1 acknowledgement");
      return;
    }
  });

  // Wait for producer to time out and report failure
  {
    std::unique_lock<std::mutex> lock(sync_mutex);
    const auto wait_deadline = std::chrono::steady_clock::now() + 2s;
    const bool done = sync_cv.wait_until(
        lock, wait_deadline, [&] { return producer_failed || abort_requested; });
    CHECK(done);
    CHECK(producer_failed);
    CHECK(producer_error_msg == "Intentional timeout waiting for stage 1 acknowledgement");
  }

  // Cleanly join producer
  producer.join();

  // Test cooperative cancellation before producer starts
  {
    std::unique_lock<std::mutex> lock(sync_mutex);
    abort_requested = true;
    producer_failed = false;
    producer_error_msg.clear();
  }

  std::thread cancelled_producer([&]() {
    std::unique_lock<std::mutex> lock(sync_mutex);
    if (abort_requested) return;
    (void)queue.try_push(123);
  });

  cancelled_producer.join();

  // Drain the single unacknowledged item from queue and notifier to clean up
  std::uint64_t dummy = 0;
  CHECK(queue.try_pop(dummy));
  CHECK(dummy == 999);
  CHECK(notifier.consume(dummy) == ConsumeStatus::Success);
  CHECK(dummy == 1);
}

} // namespace

int main() {
  test_registration_and_lifetime();
  test_eventfd_epoll_queue_integration();
  test_producer_cancellation_and_timeout_path();

  if (g_failures.load() == 0) {
    std::cout << "All IPC eventfd-epoll integration tests PASSED\n";
    return 0;
  }
  std::cerr << g_failures.load() << " test check(s) FAILED\n";
  return 1;
}

#else

int main() {
  std::cerr << "darkpool_ipc_epoll_tests requires Linux\n";
  return 0;
}

#endif
