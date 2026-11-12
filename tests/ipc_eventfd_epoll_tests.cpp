#include "darkpool/bounded_mpmc_queue.hpp"
#include "darkpool/event_notifier.hpp"
#include "darkpool/server_runtime.hpp"

#if defined(__linux__)

#include <atomic>
#include <chrono>
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
// Test 2: End-to-End Producer -> Queue -> EventNotifier -> epoll -> Reactor
// ============================================================================

void test_eventfd_epoll_queue_integration() {
  std::cerr << "[ipc_eventfd_epoll_tests] running test_eventfd_epoll_queue_integration\n";

  constexpr std::size_t kItemCount = 512;
  constexpr std::size_t kQueueCapacity = 1024; // Capacity >= workload ensures no queue overflow

  // 1. Instantiate primitives
  EventNotifier notifier;
  CHECK(notifier.valid());

  BoundedMpmcQueue<std::uint64_t> queue(kQueueCapacity);

  ServerRuntimeConfig config{};
  config.enable_tcp = false;
  config.enable_tls = false;
  config.enable_unix = false;
  config.handle_signals = false;
  config.epoll_timeout_ms = 50;

  ServerRuntime runtime(config);
  std::string start_err;
  CHECK(runtime.start(start_err));
  CHECK(runtime.is_running());

  // 2. Tracking state
  std::atomic<bool> producer_done{false};
  std::atomic<bool> producer_error{false};
  std::string producer_error_msg;

  std::vector<std::uint64_t> consumed_items;
  consumed_items.reserve(kItemCount);

  std::uint64_t total_notifications_accumulated = 0;
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
          total_notifications_accumulated += counter_val;
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
      },
      reg_err);
  CHECK(reg_ok);
  CHECK(reg_err.empty());

  // 4. Launch producer thread
  std::thread producer([&]() {
    for (std::uint64_t i = 0; i < kItemCount; ++i) {
      // Small intermittent sleep every 32 items to test both immediate wakeups
      // and accumulated/coalesced burst notifications while reactor steps
      if (i % 32 == 0) {
        std::this_thread::sleep_for(1ms);
      }

      // Step 1: Push item to queue
      if (!queue.try_push(i)) {
        producer_error.store(true, std::memory_order_relaxed);
        producer_error_msg = "Queue push failed at item " + std::to_string(i);
        break;
      }

      // Step 2: Notify eventfd
      int n_err = 0;
      const auto n_status = notifier.notify(1, &n_err);
      if (n_status != NotifyStatus::Success) {
        producer_error.store(true, std::memory_order_relaxed);
        producer_error_msg = "EventNotifier notify failed with error: " + std::to_string(n_err);
        break;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  // 5. Main test thread acts as reactor driving ServerRuntime::step() with monotonic deadline
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (consumed_items.size() < kItemCount) {
    if (std::chrono::steady_clock::now() > deadline) {
      CHECK(false);
      std::cerr << "Test timed out waiting for work items to be processed! Consumed: "
                << consumed_items.size() << " / " << kItemCount << '\n';
      break;
    }
    // Bounded step wait (up to 100ms)
    CHECK(runtime.step(100));
  }

  producer.join();

  // Verify producer completed without error
  CHECK(!producer_error.load());
  if (producer_error.load()) {
    std::cerr << "Producer error: " << producer_error_msg << '\n';
  }

  // Drain any remaining items in queue just in case
  while (true) {
    std::uint64_t item = 0;
    if (queue.try_pop(item)) {
      consumed_items.push_back(item);
    } else {
      break;
    }
  }

  // 6. Assert all 512 items consumed exactly once in order
  CHECK(consumed_items.size() == kItemCount);
  std::vector<bool> seen(kItemCount, false);
  bool all_unique_and_ordered = true;
  for (std::size_t i = 0; i < consumed_items.size(); ++i) {
    const auto val = consumed_items[i];
    if (val >= kItemCount) {
      all_unique_and_ordered = false;
      CHECK(false);
      break;
    }
    if (seen[val]) {
      all_unique_and_ordered = false;
      CHECK(false);
      break;
    }
    seen[val] = true;
    if (val != i) {
      all_unique_and_ordered = false;
    }
  }
  CHECK(all_unique_and_ordered);

  // 7. Assert notification accounting & coalescing evidence
  // Every single one of the 512 notify(1) calls must be reflected in the counter sum
  CHECK(total_notifications_accumulated == kItemCount);
  CHECK(spurious_empty_consumes == 0);
  CHECK(unexpected_errors == 0);

  // Because the producer sleeps every 32 items and pushes in bursts,
  // multiple notifications coalesce into single eventfd counter increments.
  // Therefore, callback_executions must be less than kItemCount.
  std::cerr << "  Work items: " << kItemCount << '\n';
  std::cerr << "  Total counter notifications accumulated: " << total_notifications_accumulated << '\n';
  std::cerr << "  Total epoll callback executions: " << callback_executions << '\n';
  CHECK(callback_executions > 0);
  CHECK(callback_executions < kItemCount);

  // 8. Nonblocking step when queue and eventfd are empty: must not invoke callback or spin
  const std::size_t callbacks_before = callback_executions;
  CHECK(runtime.step(0)); // 0ms timeout nonblocking check
  CHECK(callback_executions == callbacks_before);

  runtime.stop();
  CHECK(!runtime.is_running());
}

} // namespace

int main() {
  test_registration_and_lifetime();
  test_eventfd_epoll_queue_integration();

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

