#include "darkpool/bounded_mpmc_queue.hpp"
#include "darkpool/event_notifier.hpp"
#include "darkpool/server_runtime.hpp"

#if defined(__linux__)

#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <fcntl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace std::chrono_literals;
using darkpool::ipc::BoundedMpmcQueue;
using darkpool::ipc::ConsumeStatus;
using darkpool::ipc::EventNotifier;
using darkpool::ipc::NotifyStatus;
using darkpool::network::ServerRuntime;
using darkpool::network::ServerRuntimeConfig;
using json = nlohmann::ordered_json;

constexpr std::size_t kQueueCapacity = 4096;
constexpr int kReactorStepTimeoutMs = 50;

// CLI limits
constexpr std::size_t kMaxItemsPerProducer = 500000;
constexpr std::size_t kMaxRepetitions = 20;
constexpr std::size_t kMaxTotalWorkload = 2000000;

// ============================================================================
// WorkItem Definition
// ============================================================================

struct WorkItem {
  std::uint32_t producer_id{0};
  std::uint32_t sequence{0};
  std::uint64_t item_id{0};
  std::chrono::steady_clock::time_point published_at{};
};

// ============================================================================
// PipeNotifier Baseline
// ============================================================================

class PipeNotifier {
 public:
  PipeNotifier() {
    int fds[2]{-1, -1};
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) == 0) {
      read_fd_ = fds[0];
      write_fd_ = fds[1];
    } else {
      creation_errno_ = errno;
    }
  }

  ~PipeNotifier() {
    close();
  }

  PipeNotifier(const PipeNotifier&) = delete;
  PipeNotifier& operator=(const PipeNotifier&) = delete;

  PipeNotifier(PipeNotifier&& other) noexcept
      : read_fd_(other.read_fd_), write_fd_(other.write_fd_), creation_errno_(other.creation_errno_) {
    other.read_fd_ = -1;
    other.write_fd_ = -1;
    other.creation_errno_ = 0;
  }

  PipeNotifier& operator=(PipeNotifier&& other) noexcept {
    if (this != &other) {
      close();
      read_fd_ = other.read_fd_;
      write_fd_ = other.write_fd_;
      creation_errno_ = other.creation_errno_;
      other.read_fd_ = -1;
      other.write_fd_ = -1;
      other.creation_errno_ = 0;
    }
    return *this;
  }

  [[nodiscard]] bool valid() const noexcept { return read_fd_ >= 0 && write_fd_ >= 0; }
  [[nodiscard]] int read_descriptor() const noexcept { return read_fd_; }
  [[nodiscard]] int write_descriptor() const noexcept { return write_fd_; }
  [[nodiscard]] int creation_errno() const noexcept { return creation_errno_; }

  void close() noexcept {
    if (read_fd_ >= 0) {
      ::close(read_fd_);
      read_fd_ = -1;
    }
    if (write_fd_ >= 0) {
      ::close(write_fd_);
      write_fd_ = -1;
    }
  }

  NotifyStatus notify(std::uint8_t token = 1, int* error_code = nullptr) noexcept {
    if (error_code) *error_code = 0;
    if (write_fd_ < 0) {
      if (error_code) *error_code = EBADF;
      return NotifyStatus::Error;
    }
    while (true) {
      const ssize_t rc = ::write(write_fd_, &token, 1);
      if (rc == 1) {
        return NotifyStatus::Success;
      }
      if (rc < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (error_code) *error_code = errno;
          return NotifyStatus::Saturated;
        }
        if (error_code) *error_code = errno;
        return NotifyStatus::Error;
      }
      if (error_code) *error_code = EIO;
      return NotifyStatus::Error;
    }
  }

  std::size_t drain_all(std::uint64_t* out_read_syscalls = nullptr,
                        int* error_code = nullptr,
                        bool* unexpected_eof = nullptr) noexcept {
    if (error_code) *error_code = 0;
    if (unexpected_eof) *unexpected_eof = false;
    if (read_fd_ < 0) return 0;
    std::size_t total_bytes = 0;
    char buf[4096];
    while (true) {
      if (out_read_syscalls) (*out_read_syscalls)++;
      const ssize_t rc = ::read(read_fd_, buf, sizeof(buf));
      if (rc > 0) {
        total_bytes += static_cast<std::size_t>(rc);
        continue;
      }
      if (rc == 0) {
        // Unexpected EOF while write descriptor should remain open
        if (unexpected_eof) *unexpected_eof = true;
        break;
      }
      if (rc < 0 && errno == EINTR) continue;
      if (rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
      if (error_code && rc < 0) *error_code = errno;
      break;
    }
    return total_bytes;
  }

 private:
  int read_fd_{-1};
  int write_fd_{-1};
  int creation_errno_{0};
};

// ============================================================================
// Metrics & Statistics Data Structures
// ============================================================================

struct LatencyStats {
  double p50_ns{0.0};
  double p95_ns{0.0};
  double p99_ns{0.0};
  double max_ns{0.0};
  double mean_ns{0.0};

  [[nodiscard]] json to_json() const {
    json j;
    j["p50_ns"] = p50_ns;
    j["p95_ns"] = p95_ns;
    j["p99_ns"] = p99_ns;
    j["max_ns"] = max_ns;
    j["mean_ns"] = mean_ns;
    return j;
  }
};

LatencyStats compute_latency_stats(std::vector<std::uint64_t>& latencies_ns) {
  LatencyStats stats{};
  if (latencies_ns.empty()) return stats;

  std::sort(latencies_ns.begin(), latencies_ns.end());
  const std::size_t n = latencies_ns.size();

  auto get_percentile = [&](double p) -> double {
    if (n == 1) return static_cast<double>(latencies_ns[0]);
    const double rank = p * static_cast<double>(n - 1);
    const auto lower = static_cast<std::size_t>(rank);
    const auto upper = std::min(lower + 1, n - 1);
    const double weight = rank - static_cast<double>(lower);
    return static_cast<double>(latencies_ns[lower]) * (1.0 - weight) +
           static_cast<double>(latencies_ns[upper]) * weight;
  };

  stats.p50_ns = get_percentile(0.50);
  stats.p95_ns = get_percentile(0.95);
  stats.p99_ns = get_percentile(0.99);
  stats.max_ns = static_cast<double>(latencies_ns.back());

  const double sum = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0);
  stats.mean_ns = sum / static_cast<double>(n);

  return stats;
}

struct SingleRunResult {
  std::size_t repetition_index{0};
  std::string notification_primitive;
  std::size_t producers{1};
  std::size_t items_per_producer{0};
  std::size_t total_target_items{0};
  std::size_t total_received_items{0};
  std::size_t queue_capacity{kQueueCapacity};
  double elapsed_ms{0.0};
  double throughput_items_per_sec{0.0};

  LatencyStats delivery_latency{};

  std::uint64_t callback_invocations{0};
  std::uint64_t callbacks_with_zero_work{0};
  std::uint64_t notifications_signaled{0};
  std::uint64_t notifications_consumed{0};
  double average_items_per_callback{0.0};
  double max_items_in_single_callback{0.0};
  double coalescing_ratio{0.0}; // notifications_signaled / callback_invocations (batching indicator)

  std::uint64_t push_retry_count{0};
  std::uint64_t notify_retry_count{0};

  bool correctness_passed{false};
  std::string correctness_error;

  [[nodiscard]] json to_json() const {
    json j;
    j["repetition_index"] = repetition_index;
    j["notification_primitive"] = notification_primitive;
    j["producers"] = producers;
    j["items_per_producer"] = items_per_producer;
    j["total_target_items"] = total_target_items;
    j["total_received_items"] = total_received_items;
    j["queue_capacity"] = queue_capacity;
    j["elapsed_ms"] = elapsed_ms;
    j["throughput_items_per_sec"] = throughput_items_per_sec;
    j["delivery_latency"] = delivery_latency.to_json();
    j["callback_invocations"] = callback_invocations;
    j["callbacks_with_zero_work"] = callbacks_with_zero_work;
    j["notifications_signaled"] = notifications_signaled;
    j["notifications_consumed"] = notifications_consumed;
    j["average_items_per_callback"] = average_items_per_callback;
    j["max_items_in_single_callback"] = max_items_in_single_callback;
    j["coalescing_ratio"] = coalescing_ratio;
    j["push_retry_count"] = push_retry_count;
    j["notify_retry_count"] = notify_retry_count;
    j["correctness_passed"] = correctness_passed;
    j["correctness_error"] = correctness_error;
    return j;
  }
};

struct ScenarioAggregateResult {
  std::string scenario_id; // e.g. "eventfd_1p", "pipe_1p", "eventfd_4p", "pipe_4p"
  std::string notification_primitive;
  std::size_t producers{1};
  std::size_t items_per_producer{0};
  std::size_t total_target_items{0};
  std::size_t repetitions{0};

  double median_throughput_items_per_sec{0.0};
  double median_p50_latency_ns{0.0};
  double median_p95_latency_ns{0.0};
  double median_p99_latency_ns{0.0};
  double median_max_latency_ns{0.0};
  double median_coalescing_ratio{0.0};
  double median_items_per_callback{0.0};

  std::vector<SingleRunResult> repetition_runs;
  bool all_repetitions_passed{false};

  [[nodiscard]] json to_json() const {
    json j;
    j["scenario_id"] = scenario_id;
    j["notification_primitive"] = notification_primitive;
    j["producers"] = producers;
    j["items_per_producer"] = items_per_producer;
    j["total_target_items"] = total_target_items;
    j["repetitions"] = repetitions;
    j["median_throughput_items_per_sec"] = median_throughput_items_per_sec;
    j["median_p50_latency_ns"] = median_p50_latency_ns;
    j["median_p95_latency_ns"] = median_p95_latency_ns;
    j["median_p99_latency_ns"] = median_p99_latency_ns;
    j["median_max_latency_ns"] = median_max_latency_ns;
    j["median_coalescing_ratio"] = median_coalescing_ratio;
    j["median_items_per_callback"] = median_items_per_callback;
    j["all_repetitions_passed"] = all_repetitions_passed;

    json runs = json::array();
    for (const auto& r : repetition_runs) {
      runs.push_back(r.to_json());
    }
    j["repetition_runs"] = runs;
    return j;
  }
};

// ============================================================================
// Core Benchmark Execution Template
// ============================================================================

template <typename NotifierT>
SingleRunResult execute_scenario(std::size_t rep_index,
                                 const std::string& primitive_name,
                                 std::size_t producer_count,
                                 std::size_t items_per_producer) {
  SingleRunResult res{};
  res.repetition_index = rep_index;
  res.notification_primitive = primitive_name;
  res.producers = producer_count;
  res.items_per_producer = items_per_producer;
  res.total_target_items = producer_count * items_per_producer;
  res.queue_capacity = kQueueCapacity;

  // 1. Instantiate primitives
  NotifierT notifier;
  if (!notifier.valid()) {
    res.correctness_passed = false;
    res.correctness_error = "Notifier creation failed: errno=" + std::to_string(notifier.creation_errno());
    return res;
  }

  BoundedMpmcQueue<WorkItem, kQueueCapacity> queue;

  // 2. Configure and start ServerRuntime
  ServerRuntimeConfig config{};
  config.enable_tcp = false;
  config.enable_tls = false;
  config.enable_unix = false;
  config.handle_signals = false;
  config.epoll_timeout_ms = kReactorStepTimeoutMs;

  ServerRuntime runtime(config);
  std::string start_err;
  if (!runtime.start(start_err)) {
    res.correctness_passed = false;
    res.correctness_error = "ServerRuntime::start() failed: " + start_err;
    return res;
  }

  // 3. Shared synchronization state
  std::atomic<bool> abort_benchmark{false};
  std::string abort_reason;
  std::atomic<std::size_t> completed_producers{0};
  std::atomic<std::uint64_t> push_retries{0};
  std::atomic<std::uint64_t> notify_retries{0};
  std::atomic<std::uint64_t> total_signaled{0};

  std::uint64_t callback_invocations = 0;
  std::uint64_t callbacks_with_zero_work = 0;
  std::uint64_t notifications_consumed = 0;
  std::uint64_t max_items_in_callback = 0;

  std::vector<std::uint64_t> latencies_ns;
  latencies_ns.reserve(res.total_target_items);

  std::vector<WorkItem> received_items;
  received_items.reserve(res.total_target_items);

  // 4. Register epoll event source
  int register_fd = -1;
  if constexpr (std::is_same_v<NotifierT, EventNotifier>) {
    register_fd = notifier.descriptor();
  } else {
    register_fd = notifier.read_descriptor();
  }

  std::string reg_err;
  const bool reg_ok = runtime.add_epoll_event_source(
      register_fd,
      [&](std::uint32_t /*events*/) {
        callback_invocations++;

        // Consume notification primitive
        if constexpr (std::is_same_v<NotifierT, EventNotifier>) {
          std::uint64_t counter = 0;
          int c_err = 0;
          const ConsumeStatus st = notifier.consume(counter, &c_err);
          if (st == ConsumeStatus::Success) {
            notifications_consumed += counter;
          } else if (st == ConsumeStatus::Error) {
            abort_reason = "EventNotifier::consume error: errno=" + std::to_string(c_err);
            abort_benchmark.store(true, std::memory_order_relaxed);
          } else if (st == ConsumeStatus::Empty) {
            abort_reason = "EventNotifier::consume unexpected Empty on EPOLLIN callback";
            abort_benchmark.store(true, std::memory_order_relaxed);
          }
        } else {
          int d_err = 0;
          bool eof_seen = false;
          const std::size_t bytes = notifier.drain_all(nullptr, &d_err, &eof_seen);
          notifications_consumed += bytes;
          if (d_err != 0) {
            abort_reason = "PipeNotifier::drain_all read error: errno=" + std::to_string(d_err);
            abort_benchmark.store(true, std::memory_order_relaxed);
          } else if (eof_seen) {
            abort_reason = "PipeNotifier::drain_all unexpected EOF while write descriptor expected open";
            abort_benchmark.store(true, std::memory_order_relaxed);
          }
        }

        // Drain queue exclusively inside callback
        std::uint64_t items_in_this_cb = 0;
        WorkItem item{};
        while (queue.try_pop(item)) {
          const auto pop_time = std::chrono::steady_clock::now();
          const auto latency = std::chrono::duration_cast<std::chrono::nanoseconds>(
              pop_time - item.published_at).count();
          latencies_ns.push_back(static_cast<std::uint64_t>(std::max<std::int64_t>(0, latency)));
          received_items.push_back(item);
          items_in_this_cb++;
        }

        if (items_in_this_cb == 0) {
          callbacks_with_zero_work++;
        }

        if (items_in_this_cb > max_items_in_callback) {
          max_items_in_callback = items_in_this_cb;
        }
      },
      reg_err);

  if (!reg_ok) {
    runtime.stop();
    notifier.close();
    res.correctness_passed = false;
    res.correctness_error = "add_epoll_event_source failed: " + reg_err;
    return res;
  }

  // 5. Producer threads
  const auto benchmark_start = std::chrono::steady_clock::now();
  const auto benchmark_deadline = benchmark_start + 60s;

  std::vector<std::thread> producers;
  producers.reserve(producer_count);

  for (std::uint32_t p_id = 0; p_id < producer_count; ++p_id) {
    producers.emplace_back([&, p_id]() {
      for (std::uint32_t seq = 0; seq < items_per_producer; ++seq) {
        if (abort_benchmark.load(std::memory_order_relaxed)) return;

        WorkItem item{};
        item.producer_id = p_id;
        item.sequence = seq;
        item.item_id = (static_cast<std::uint64_t>(p_id) << 32) | seq;

        // Bounded try_push loop: update published_at immediately before each push attempt
        while (true) {
          item.published_at = std::chrono::steady_clock::now();
          if (queue.try_push(item)) {
            break;
          }
          push_retries.fetch_add(1, std::memory_order_relaxed);
          if (std::chrono::steady_clock::now() > benchmark_deadline ||
              abort_benchmark.load(std::memory_order_relaxed)) {
            abort_benchmark.store(true, std::memory_order_relaxed);
            return;
          }
          std::this_thread::yield();
        }

        // Bounded notify loop:
        while (true) {
          int n_err = 0;
          NotifyStatus st = NotifyStatus::Error;
          if constexpr (std::is_same_v<NotifierT, EventNotifier>) {
            st = notifier.notify(1, &n_err);
          } else {
            st = notifier.notify(1, &n_err);
          }

          if (st == NotifyStatus::Success) {
            total_signaled.fetch_add(1, std::memory_order_release);
            break;
          }
          if (st == NotifyStatus::Saturated) {
            notify_retries.fetch_add(1, std::memory_order_relaxed);
            if (std::chrono::steady_clock::now() > benchmark_deadline ||
                abort_benchmark.load(std::memory_order_relaxed)) {
              abort_benchmark.store(true, std::memory_order_relaxed);
              return;
            }
            std::this_thread::yield();
            continue;
          }

          // Real error
          abort_benchmark.store(true, std::memory_order_relaxed);
          return;
        }
      }
      completed_producers.fetch_add(1, std::memory_order_release);
    });
  }

  // 6. Reactor loop (consumer)
  // Drive runtime.step() until ALL three termination conditions are satisfied:
  // 1) completed_producers == producer_count
  // 2) received_items.size() == total_target_items
  // 3) notifications_consumed == total_signaled
  while (!abort_benchmark.load(std::memory_order_relaxed)) {
    const std::size_t prods_done = completed_producers.load(std::memory_order_acquire);
    const std::uint64_t signaled = total_signaled.load(std::memory_order_acquire);

    if (prods_done == producer_count &&
        received_items.size() == res.total_target_items &&
        notifications_consumed == signaled) {
      break;
    }

    if (std::chrono::steady_clock::now() > benchmark_deadline) {
      abort_reason = "Benchmark timed out waiting for producer completion and callback work/notification delivery";
      abort_benchmark.store(true, std::memory_order_relaxed);
      break;
    }

    const bool step_ok = runtime.step(kReactorStepTimeoutMs);
    if (!step_ok) {
      abort_reason = "ServerRuntime::step() returned false during reactor execution loop";
      abort_benchmark.store(true, std::memory_order_relaxed);
      break;
    }
  }

  // Join all producers safely before stopping runtime or destroying notifier
  for (auto& th : producers) {
    if (th.joinable()) th.join();
  }

  const auto benchmark_end = std::chrono::steady_clock::now();

  // 7. Stop runtime before closing notifier
  runtime.stop();
  notifier.close();

  // 8. Populate results
  res.elapsed_ms = std::chrono::duration<double, std::milli>(benchmark_end - benchmark_start).count();
  res.total_received_items = received_items.size();
  res.callback_invocations = callback_invocations;
  res.callbacks_with_zero_work = callbacks_with_zero_work;
  res.notifications_signaled = total_signaled.load(std::memory_order_relaxed);
  res.notifications_consumed = notifications_consumed;
  res.push_retry_count = push_retries.load(std::memory_order_relaxed);
  res.notify_retry_count = notify_retries.load(std::memory_order_relaxed);
  res.max_items_in_single_callback = static_cast<double>(max_items_in_callback);

  if (res.elapsed_ms > 0.0) {
    const double elapsed_sec = res.elapsed_ms / 1000.0;
    res.throughput_items_per_sec = static_cast<double>(res.total_received_items) / elapsed_sec;
  }
  if (callback_invocations > 0) {
    res.average_items_per_callback = static_cast<double>(res.total_received_items) / static_cast<double>(callback_invocations);
    res.coalescing_ratio = static_cast<double>(res.notifications_signaled) / static_cast<double>(callback_invocations);
  }

  res.delivery_latency = compute_latency_stats(latencies_ns);

  // 9. Validation
  if (abort_benchmark.load(std::memory_order_relaxed)) {
    res.correctness_passed = false;
    res.correctness_error = abort_reason.empty() ? "Benchmark aborted" : abort_reason;
    return res;
  }

  if (completed_producers.load(std::memory_order_relaxed) != producer_count) {
    res.correctness_passed = false;
    res.correctness_error = "Not all producers completed: expected " + std::to_string(producer_count) +
                            ", completed " + std::to_string(completed_producers.load(std::memory_order_relaxed));
    return res;
  }

  if (res.total_received_items != res.total_target_items) {
    res.correctness_passed = false;
    res.correctness_error = "Item count mismatch: expected " + std::to_string(res.total_target_items) +
                            ", got " + std::to_string(res.total_received_items);
    return res;
  }

  if (res.notifications_consumed != res.notifications_signaled) {
    res.correctness_passed = false;
    res.correctness_error = "Notification accounting mismatch: signaled=" +
                            std::to_string(res.notifications_signaled) +
                            " consumed=" + std::to_string(res.notifications_consumed);
    return res;
  }

  // Verify FIFO ordering and all identity fields
  if (producer_count == 1) {
    for (std::size_t i = 0; i < received_items.size(); ++i) {
      if (received_items[i].producer_id != 0) {
        res.correctness_passed = false;
        res.correctness_error = "Producer ID mismatch at index " + std::to_string(i) +
                                ": expected 0, got " + std::to_string(received_items[i].producer_id);
        return res;
      }
      if (received_items[i].sequence != static_cast<std::uint32_t>(i)) {
        res.correctness_passed = false;
        res.correctness_error = "Global FIFO violation at index " + std::to_string(i) +
                                ": expected seq=" + std::to_string(i) +
                                ", got seq=" + std::to_string(received_items[i].sequence);
        return res;
      }
      const std::uint64_t expected_item_id = static_cast<std::uint64_t>(i);
      if (received_items[i].item_id != expected_item_id) {
        res.correctness_passed = false;
        res.correctness_error = "Item ID mismatch at index " + std::to_string(i) +
                                ": expected " + std::to_string(expected_item_id) +
                                ", got " + std::to_string(received_items[i].item_id);
        return res;
      }
    }
  } else {
    std::vector<std::uint32_t> next_expected_seq(producer_count, 0);
    std::vector<std::size_t> per_producer_count(producer_count, 0);
    for (std::size_t i = 0; i < received_items.size(); ++i) {
      const auto p = received_items[i].producer_id;
      if (p >= producer_count) {
        res.correctness_passed = false;
        res.correctness_error = "Invalid producer ID " + std::to_string(p) + " at item " + std::to_string(i);
        return res;
      }
      if (received_items[i].sequence != next_expected_seq[p]) {
        res.correctness_passed = false;
        res.correctness_error = "Per-producer FIFO violation for producer " + std::to_string(p) +
                                " at item " + std::to_string(i) +
                                ": expected seq " + std::to_string(next_expected_seq[p]) +
                                ", got seq " + std::to_string(received_items[i].sequence);
        return res;
      }
      const std::uint64_t expected_item_id = (static_cast<std::uint64_t>(p) << 32) | next_expected_seq[p];
      if (received_items[i].item_id != expected_item_id) {
        res.correctness_passed = false;
        res.correctness_error = "Item ID mismatch for producer " + std::to_string(p) +
                                " at item " + std::to_string(i) +
                                ": expected " + std::to_string(expected_item_id) +
                                ", got " + std::to_string(received_items[i].item_id);
        return res;
      }
      next_expected_seq[p]++;
      per_producer_count[p]++;
    }

    for (std::size_t p = 0; p < producer_count; ++p) {
      if (per_producer_count[p] != items_per_producer) {
        res.correctness_passed = false;
        res.correctness_error = "Producer " + std::to_string(p) + " item count mismatch: expected " +
                                std::to_string(items_per_producer) + ", got " +
                                std::to_string(per_producer_count[p]);
        return res;
      }
    }
  }

  res.correctness_passed = true;
  return res;
}

// ============================================================================
// Environment Metadata
// ============================================================================

json gather_environment_metadata() {
  json meta;

  const auto now = std::chrono::system_clock::now();
  const auto in_time_t = std::chrono::system_clock::to_time_t(now);
  std::stringstream ss;
  ss << std::put_time(std::gmtime(&in_time_t), "%Y-%m-%dT%H:%M:%SZ");
  meta["timestamp_utc"] = ss.str();

#ifndef DARKPOOL_SOURCE_COMMIT
#define DARKPOOL_SOURCE_COMMIT "unknown"
#endif
  meta["git_commit"] = DARKPOOL_SOURCE_COMMIT;

#if defined(__clang__)
  meta["compiler"] = "Clang " __clang_version__;
#elif defined(__GNUC__)
  meta["compiler"] = "GCC " __VERSION__;
#else
  meta["compiler"] = "unknown";
#endif

#if defined(NDEBUG)
  meta["build_type"] = "Release";
#else
  meta["build_type"] = "Debug";
#endif

  meta["hardware_concurrency"] = std::thread::hardware_concurrency();

  struct utsname u {};
  if (::uname(&u) == 0) {
    meta["os_sysname"] = u.sysname;
    meta["os_release"] = u.release;
    meta["os_version"] = u.version;
    meta["os_machine"] = u.machine;
  } else {
    meta["os_sysname"] = "unknown";
    meta["os_release"] = "unknown";
    meta["os_version"] = "unknown";
    meta["os_machine"] = "unknown";
  }

  return meta;
}

// ============================================================================
// CLI & Table Formatting
// ============================================================================

void print_help(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " [options]\n\n"
            << "Options:\n"
            << "  --items-per-producer=<N>  Items generated per producer thread (1.." << kMaxItemsPerProducer << ", default: 100000)\n"
            << "  --repetitions=<N>         Number of repetitions per scenario (1.." << kMaxRepetitions << ", default: 3)\n"
            << "  --json                    Output JSON results to stdout\n"
            << "  --json-out=<path>         Write JSON results to specified file\n"
            << "  -h, --help                Show this help message\n\n"
            << "Constraints:\n"
            << "  Maximum total workload per scenario (producers * items_per_producer) is " << kMaxTotalWorkload << " items.\n";
}

bool parse_positive_uint64(std::string_view str, std::uint64_t& out_val) {
  if (str.empty()) return false;
  for (char ch : str) {
    if (ch < '0' || ch > '9') return false;
  }
  try {
    std::size_t idx = 0;
    std::string s(str);
    unsigned long long v = std::stoull(s, &idx);
    if (idx != s.size()) return false;
    if (v == 0) return false;
    out_val = static_cast<std::uint64_t>(v);
    return true;
  } catch (...) {
    return false;
  }
}

void print_human_readable_summary(const std::vector<ScenarioAggregateResult>& aggregates) {
  std::cout << "\n"
            << std::string(140, '=') << "\n"
            << " DARKPOOL IPC-8: End-to-End epoll Reactor Work-Delivery Benchmark Summary (Median of Repetitions)\n"
            << std::string(140, '=') << "\n"
            << std::left << std::setw(20) << "Scenario"
            << std::setw(16) << "Primitive"
            << std::setw(8) << "P/C"
            << std::right << std::setw(12) << "Items"
            << std::setw(16) << "Throughput(it/s)"
            << std::setw(12) << "p50(ns)"
            << std::setw(12) << "p95(ns)"
            << std::setw(12) << "p99(ns)"
            << std::setw(12) << "max(ns)"
            << std::setw(10) << "Coalesce"
            << "  Status\n"
            << std::string(140, '-') << "\n";

  for (const auto& agg : aggregates) {
    std::string pc_str = std::to_string(agg.producers) + "/1";
    std::cout << std::left << std::setw(20) << agg.scenario_id
              << std::setw(16) << agg.notification_primitive
              << std::setw(8) << pc_str
              << std::right << std::setw(12) << agg.total_target_items
              << std::fixed << std::setprecision(1) << std::setw(16) << agg.median_throughput_items_per_sec
              << std::fixed << std::setprecision(1) << std::setw(12) << agg.median_p50_latency_ns
              << std::fixed << std::setprecision(1) << std::setw(12) << agg.median_p95_latency_ns
              << std::fixed << std::setprecision(1) << std::setw(12) << agg.median_p99_latency_ns
              << std::fixed << std::setprecision(1) << std::setw(12) << agg.median_max_latency_ns
              << std::fixed << std::setprecision(1) << std::setw(10) << agg.median_coalescing_ratio
              << "  " << (agg.all_repetitions_passed ? "PASS" : "FAIL")
              << "\n";
  }
  std::cout << std::string(140, '=') << "\n\n";
}

} // namespace

int main(int argc, char* argv[]) {
  std::uint64_t items_per_producer_arg = 100000;
  std::uint64_t repetitions_arg = 3;
  bool json_stdout = false;
  std::string json_out_path;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help(argv[0]);
      return 0;
    } else if (arg == "--json") {
      json_stdout = true;
    } else if (arg.starts_with("--json-out=")) {
      json_out_path = std::string(arg.substr(11));
      if (json_out_path.empty()) {
        std::cerr << "Error: --json-out path cannot be empty\n";
        return 1;
      }
    } else if (arg.starts_with("--items-per-producer=")) {
      if (!parse_positive_uint64(arg.substr(21), items_per_producer_arg)) {
        std::cerr << "Error: Invalid value for --items-per-producer: " << arg.substr(21) << "\n";
        return 1;
      }
    } else if (arg.starts_with("--repetitions=")) {
      if (!parse_positive_uint64(arg.substr(14), repetitions_arg)) {
        std::cerr << "Error: Invalid value for --repetitions: " << arg.substr(14) << "\n";
        return 1;
      }
    } else {
      std::cerr << "Error: Unknown argument: " << arg << "\n";
      print_help(argv[0]);
      return 1;
    }
  }

  // Validate bounds
  if (items_per_producer_arg > kMaxItemsPerProducer) {
    std::cerr << "Error: --items-per-producer (" << items_per_producer_arg
              << ") exceeds maximum limit (" << kMaxItemsPerProducer << ")\n";
    return 1;
  }

  if (repetitions_arg > kMaxRepetitions) {
    std::cerr << "Error: --repetitions (" << repetitions_arg
              << ") exceeds maximum limit (" << kMaxRepetitions << ")\n";
    return 1;
  }

  // Check 4-producer total workload against maximum total workload limit
  constexpr std::size_t kMaxProducers = 4;
  if (items_per_producer_arg > kMaxTotalWorkload / kMaxProducers) {
    std::cerr << "Error: Total workload for 4-producer scenario ("
              << (kMaxProducers * items_per_producer_arg)
              << ") exceeds maximum limit (" << kMaxTotalWorkload << ")\n";
    return 1;
  }

  const std::size_t items_per_producer = static_cast<std::size_t>(items_per_producer_arg);
  const std::size_t repetitions = static_cast<std::size_t>(repetitions_arg);

  std::cerr << "[darkpool_ipc_reactor_benchmark] Configuration:\n"
            << "  Items per producer: " << items_per_producer << "\n"
            << "  Repetitions:        " << repetitions << "\n"
            << "  Queue capacity:     " << kQueueCapacity << " (BoundedMpmcQueue compile-time capacity)\n"
            << "  Reactor timeout:    " << kReactorStepTimeoutMs << " ms\n\n";

  // Repetition storage for the 4 scenarios:
  // 0: EventNotifier 1P/1C
  // 1: PipeNotifier  1P/1C
  // 2: EventNotifier 4P/1C
  // 3: PipeNotifier  4P/1C
  std::vector<std::vector<SingleRunResult>> scenario_runs(4);

  bool all_pass = true;

  for (std::size_t rep = 0; rep < repetitions; ++rep) {
    std::cerr << "=== Repetition " << (rep + 1) << " / " << repetitions << " ===\n";

    // Alternate order across repetitions to mitigate warmup bias:
    // Even rep: EventNotifier first, then PipeNotifier
    // Odd rep:  PipeNotifier first, then EventNotifier
    const bool eventfd_first = (rep % 2 == 0);

    // --- 1-Producer Scenarios ---
    if (eventfd_first) {
      std::cerr << "  [Run] 1 Producer -> EventNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r1 = execute_scenario<EventNotifier>(rep, "EventNotifier", 1, items_per_producer);
      if (!r1.correctness_passed) {
        std::cerr << "    FAIL: " << r1.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[0].push_back(r1);

      std::cerr << "  [Run] 1 Producer -> PipeNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r2 = execute_scenario<PipeNotifier>(rep, "PipeNotifier", 1, items_per_producer);
      if (!r2.correctness_passed) {
        std::cerr << "    FAIL: " << r2.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[1].push_back(r2);
    } else {
      std::cerr << "  [Run] 1 Producer -> PipeNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r2 = execute_scenario<PipeNotifier>(rep, "PipeNotifier", 1, items_per_producer);
      if (!r2.correctness_passed) {
        std::cerr << "    FAIL: " << r2.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[1].push_back(r2);

      std::cerr << "  [Run] 1 Producer -> EventNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r1 = execute_scenario<EventNotifier>(rep, "EventNotifier", 1, items_per_producer);
      if (!r1.correctness_passed) {
        std::cerr << "    FAIL: " << r1.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[0].push_back(r1);
    }

    // --- 4-Producer Scenarios ---
    if (eventfd_first) {
      std::cerr << "  [Run] 4 Producers -> EventNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r3 = execute_scenario<EventNotifier>(rep, "EventNotifier", 4, items_per_producer);
      if (!r3.correctness_passed) {
        std::cerr << "    FAIL: " << r3.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[2].push_back(r3);

      std::cerr << "  [Run] 4 Producers -> PipeNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r4 = execute_scenario<PipeNotifier>(rep, "PipeNotifier", 4, items_per_producer);
      if (!r4.correctness_passed) {
        std::cerr << "    FAIL: " << r4.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[3].push_back(r4);
    } else {
      std::cerr << "  [Run] 4 Producers -> PipeNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r4 = execute_scenario<PipeNotifier>(rep, "PipeNotifier", 4, items_per_producer);
      if (!r4.correctness_passed) {
        std::cerr << "    FAIL: " << r4.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[3].push_back(r4);

      std::cerr << "  [Run] 4 Producers -> EventNotifier -> ServerRuntime (rep " << rep + 1 << ")...\n";
      auto r3 = execute_scenario<EventNotifier>(rep, "EventNotifier", 4, items_per_producer);
      if (!r3.correctness_passed) {
        std::cerr << "    FAIL: " << r3.correctness_error << "\n";
        all_pass = false;
      }
      scenario_runs[2].push_back(r3);
    }
  }

  // Calculate Median Summaries for each scenario
  std::vector<ScenarioAggregateResult> aggregates;
  aggregates.reserve(4);

  const std::vector<std::pair<std::string, std::size_t>> scenario_meta = {
      {"EventNotifier", 1},
      {"PipeNotifier", 1},
      {"EventNotifier", 4},
      {"PipeNotifier", 4}
  };

  const std::vector<std::string> scenario_ids = {
      "eventfd_1p", "pipe_1p", "eventfd_4p", "pipe_4p"
  };

  for (std::size_t s = 0; s < 4; ++s) {
    ScenarioAggregateResult agg{};
    agg.scenario_id = scenario_ids[s];
    agg.notification_primitive = scenario_meta[s].first;
    agg.producers = scenario_meta[s].second;
    agg.items_per_producer = items_per_producer;
    agg.total_target_items = agg.producers * items_per_producer;
    agg.repetitions = repetitions;
    agg.repetition_runs = scenario_runs[s];

    bool agg_passed = true;
    std::vector<double> throughputs;
    std::vector<double> p50s, p95s, p99s, maxs;
    std::vector<double> coalescings;
    std::vector<double> items_per_cbs;

    for (const auto& run : agg.repetition_runs) {
      if (!run.correctness_passed) agg_passed = false;
      throughputs.push_back(run.throughput_items_per_sec);
      p50s.push_back(run.delivery_latency.p50_ns);
      p95s.push_back(run.delivery_latency.p95_ns);
      p99s.push_back(run.delivery_latency.p99_ns);
      maxs.push_back(run.delivery_latency.max_ns);
      coalescings.push_back(run.coalescing_ratio);
      items_per_cbs.push_back(run.average_items_per_callback);
    }
    agg.all_repetitions_passed = agg_passed;

    auto median_of = [](std::vector<double>& v) -> double {
      if (v.empty()) return 0.0;
      std::sort(v.begin(), v.end());
      const std::size_t mid = v.size() / 2;
      if (v.size() % 2 == 1) return v[mid];
      return (v[mid - 1] + v[mid]) / 2.0;
    };

    agg.median_throughput_items_per_sec = median_of(throughputs);
    agg.median_p50_latency_ns = median_of(p50s);
    agg.median_p95_latency_ns = median_of(p95s);
    agg.median_p99_latency_ns = median_of(p99s);
    agg.median_max_latency_ns = median_of(maxs);
    agg.median_coalescing_ratio = median_of(coalescings);
    agg.median_items_per_callback = median_of(items_per_cbs);

    aggregates.push_back(agg);
  }

  // Human-readable summary table
  if (!json_stdout) {
    print_human_readable_summary(aggregates);
  }

  // Machine-readable JSON
  json root;
  root["benchmark_suite"] = "darkpool_ipc_reactor_benchmark";
  root["metadata"] = gather_environment_metadata();
  root["configuration"] = {
      {"items_per_producer", items_per_producer},
      {"repetitions", repetitions},
      {"queue_capacity", kQueueCapacity},
      {"reactor_step_timeout_ms", kReactorStepTimeoutMs},
      {"latency_measurement_boundary", "published_at (immediately preceding successful queue.try_push()) to callback queue.try_pop()"},
      {"max_items_per_producer_limit", kMaxItemsPerProducer},
      {"max_repetitions_limit", kMaxRepetitions},
      {"max_total_workload_limit", kMaxTotalWorkload}
  };

  json agg_array = json::array();
  for (const auto& a : aggregates) {
    agg_array.push_back(a.to_json());
  }
  root["scenarios"] = agg_array;

  if (json_stdout) {
    std::cout << root.dump(2) << "\n";
  }

  if (!json_out_path.empty()) {
    std::ofstream ofs(json_out_path);
    if (ofs) {
      ofs << root.dump(2) << "\n";
      std::cerr << "Wrote JSON results to " << json_out_path << "\n";
    } else {
      std::cerr << "Error: Could not open output JSON file: " << json_out_path << "\n";
      all_pass = false;
    }
  }

  return all_pass ? 0 : 2;
}

#else

int main() {
  std::cerr << "darkpool_ipc_reactor_benchmark requires Linux\n";
  return 1;
}

#endif
