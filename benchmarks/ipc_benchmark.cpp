#include "darkpool/bounded_mpmc_queue.hpp"
#include "darkpool/event_notifier.hpp"

#if defined(__linux__)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using namespace std::chrono_literals;
using darkpool::ipc::BoundedMpmcQueue;
using darkpool::ipc::ConsumeStatus;
using darkpool::ipc::EventNotifier;
using darkpool::ipc::NotifyStatus;
using json = nlohmann::ordered_json;

// ============================================================================
// Baseline Implementations
// ============================================================================

/**
 * @brief Bounded mutex-protected queue baseline.
 *
 * Implements nonblocking try_push / try_pop semantics using
 * std::mutex and a circular std::vector without heap allocations during operations.
 */
template <typename T>
class BoundedMutexQueue {
 public:
  explicit BoundedMutexQueue(std::size_t capacity)
      : capacity_(capacity), buffer_(capacity) {}

  ~BoundedMutexQueue() = default;

  BoundedMutexQueue(const BoundedMutexQueue&) = delete;
  BoundedMutexQueue& operator=(const BoundedMutexQueue&) = delete;

  bool try_push(const T& item) noexcept
    requires std::is_nothrow_copy_constructible_v<T> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ >= capacity_) {
      return false;
    }
    buffer_[tail_] = item;
    tail_ = (tail_ + 1) % capacity_;
    ++count_;
    return true;
  }

  bool try_push(T&& item) noexcept
    requires std::is_nothrow_move_constructible_v<T> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ >= capacity_) {
      return false;
    }
    buffer_[tail_] = std::move(item);
    tail_ = (tail_ + 1) % capacity_;
    ++count_;
    return true;
  }

  template <typename... Args>
    requires std::is_nothrow_constructible_v<T, Args...>
  bool try_emplace(Args&&... args) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ >= capacity_) {
      return false;
    }
    buffer_[tail_] = T(std::forward<Args>(args)...);
    tail_ = (tail_ + 1) % capacity_;
    ++count_;
    return true;
  }

  bool try_pop(T& item) noexcept
    requires std::is_nothrow_move_assignable_v<T> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) {
      return false;
    }
    item = std::move(buffer_[head_]);
    head_ = (head_ + 1) % capacity_;
    --count_;
    return true;
  }

  std::optional<T> try_pop() noexcept
    requires std::is_nothrow_move_constructible_v<T> {
    std::lock_guard<std::mutex> lock(mutex_);
    if (count_ == 0) {
      return std::nullopt;
    }
    std::optional<T> item(std::move(buffer_[head_]));
    head_ = (head_ + 1) % capacity_;
    --count_;
    return item;
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
  [[nodiscard]] bool empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ == 0;
  }
  [[nodiscard]] bool full() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_ >= capacity_;
  }

 private:
  const std::size_t capacity_;
  std::vector<T> buffer_;
  std::size_t head_{0};
  std::size_t tail_{0};
  std::size_t count_{0};
  mutable std::mutex mutex_;
};

/**
 * @brief Nonblocking pipe baseline for asynchronous notifications.
 *
 * Uses pipe2(fds, O_NONBLOCK | O_CLOEXEC) to measure nonblocking byte stream
 * signaling against Linux eventfd counter-based notifications.
 */
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

  ConsumeStatus consume(std::uint8_t& token, int* error_code = nullptr) noexcept {
    token = 0;
    if (error_code) *error_code = 0;
    if (read_fd_ < 0) {
      if (error_code) *error_code = EBADF;
      return ConsumeStatus::Error;
    }
    while (true) {
      const ssize_t rc = ::read(read_fd_, &token, 1);
      if (rc == 1) {
        return ConsumeStatus::Success;
      }
      if (rc < 0) {
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          if (error_code) *error_code = errno;
          return ConsumeStatus::Empty;
        }
        if (error_code) *error_code = errno;
        return ConsumeStatus::Error;
      }
      if (error_code) *error_code = EIO;
      return ConsumeStatus::Error;
    }
  }

  std::size_t drain(std::uint64_t* out_read_syscalls = nullptr, int* error_code = nullptr) noexcept {
    if (error_code) *error_code = 0;
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
// Benchmark Result Structures
// ============================================================================

struct BenchmarkResult {
  std::string benchmark;
  std::string implementation;
  std::size_t producers{1};
  std::size_t consumers{1};
  std::size_t capacity{0};
  std::size_t target_items{0};
  std::size_t transferred_items{0};
  std::uint64_t attempted_operations{0};
  std::uint64_t successful_operations{0};
  std::uint64_t push_failures{0};
  std::uint64_t pop_failures{0};
  std::optional<std::uint64_t> write_syscalls;
  std::optional<std::uint64_t> read_syscalls;
  double elapsed_ms{0.0};
  double transferred_items_per_sec{0.0};
  double total_ops_per_sec{0.0};
  std::optional<double> avg_roundtrip_latency_ns;
  std::optional<double> estimated_half_rtt_latency_ns;
  bool correctness_passed{false};
  std::string correctness_error;

  [[nodiscard]] json to_json() const {
    json j;
    j["benchmark"] = benchmark;
    j["implementation"] = implementation;
    j["producers"] = producers;
    j["consumers"] = consumers;
    if (capacity > 0) {
      j["capacity"] = capacity;
    } else {
      j["capacity"] = nullptr;
    }
    j["target_items"] = target_items;
    j["transferred_items"] = transferred_items;
    j["attempted_operations"] = attempted_operations;
    j["successful_operations"] = successful_operations;
    j["push_failures"] = push_failures;
    j["pop_failures"] = pop_failures;
    if (write_syscalls.has_value()) {
      j["write_syscalls"] = *write_syscalls;
    } else {
      j["write_syscalls"] = nullptr;
    }
    if (read_syscalls.has_value()) {
      j["read_syscalls"] = *read_syscalls;
    } else {
      j["read_syscalls"] = nullptr;
    }
    j["elapsed_ms"] = elapsed_ms;
    j["transferred_items_per_sec"] = transferred_items_per_sec;
    j["total_ops_per_sec"] = total_ops_per_sec;
    if (avg_roundtrip_latency_ns.has_value()) {
      j["avg_roundtrip_latency_ns"] = *avg_roundtrip_latency_ns;
    } else {
      j["avg_roundtrip_latency_ns"] = nullptr;
    }
    if (estimated_half_rtt_latency_ns.has_value()) {
      j["estimated_half_rtt_latency_ns"] = *estimated_half_rtt_latency_ns;
    } else {
      j["estimated_half_rtt_latency_ns"] = nullptr;
    }
    j["correctness_passed"] = correctness_passed;
    j["correctness_error"] = correctness_error;
    return j;
  }
};

// ============================================================================
// Queue Benchmarks
// ============================================================================

template <typename QueueType>
BenchmarkResult run_queue_spsc(const std::string& impl_name, std::size_t capacity, std::size_t items) {
  BenchmarkResult res{};
  res.benchmark = "queue_spsc";
  res.implementation = impl_name;
  res.producers = 1;
  res.consumers = 1;
  res.capacity = capacity;
  res.target_items = items;

  QueueType queue(capacity);

  std::atomic<bool> producer_done{false};
  std::atomic<bool> abort_test{false};
  std::atomic<std::uint64_t> push_failures{0};
  std::atomic<std::uint64_t> pop_failures{0};
  std::vector<std::uint64_t> consumed_items;
  consumed_items.reserve(items);

  const auto start_time = std::chrono::steady_clock::now();

  std::thread consumer([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (consumed_items.size() < items && !abort_test.load(std::memory_order_relaxed)) {
      std::uint64_t val = 0;
      if (queue.try_pop(val)) {
        consumed_items.push_back(val);
      } else {
        pop_failures.fetch_add(1, std::memory_order_relaxed);
        if (producer_done.load(std::memory_order_acquire) && queue.empty()) {
          while (queue.try_pop(val)) {
            consumed_items.push_back(val);
          }
          if (consumed_items.size() >= items) break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
          abort_test.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      }
    }
  });

  std::thread producer([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    for (std::uint64_t i = 0; i < items; ++i) {
      if (abort_test.load(std::memory_order_relaxed)) break;
      while (!queue.try_push(i)) {
        push_failures.fetch_add(1, std::memory_order_relaxed);
        if (std::chrono::steady_clock::now() > deadline) {
          abort_test.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = consumed_items.size();
  res.push_failures = push_failures.load();
  res.pop_failures = pop_failures.load();
  res.successful_operations = res.transferred_items * 2; // 1 push + 1 pop per item
  res.attempted_operations = res.successful_operations + res.push_failures + res.pop_failures;

  if (res.elapsed_ms > 0.0) {
    const double elapsed_sec = res.elapsed_ms / 1000.0;
    res.transferred_items_per_sec = static_cast<double>(res.transferred_items) / elapsed_sec;
    res.total_ops_per_sec = static_cast<double>(res.attempted_operations) / elapsed_sec;
  }

  // Verification: exact count, sequential ordering
  if (abort_test.load()) {
    res.correctness_passed = false;
    res.correctness_error = "Benchmark timed out before completing";
  } else if (consumed_items.size() != items) {
    res.correctness_passed = false;
    res.correctness_error = "Item count mismatch: expected " + std::to_string(items) +
                            ", got " + std::to_string(consumed_items.size());
  } else {
    bool ordered = true;
    for (std::uint64_t i = 0; i < items; ++i) {
      if (consumed_items[i] != i) {
        ordered = false;
        res.correctness_error = "FIFO ordering violated at index " + std::to_string(i) +
                                ": expected " + std::to_string(i) + ", got " + std::to_string(consumed_items[i]);
        break;
      }
    }
    res.correctness_passed = ordered;
  }

  return res;
}

template <typename QueueType>
BenchmarkResult run_queue_mpmc(const std::string& impl_name, std::size_t capacity,
                               std::size_t num_producers, std::size_t num_consumers,
                               std::size_t items_per_producer) {
  const std::size_t total_items = num_producers * items_per_producer;

  BenchmarkResult res{};
  res.benchmark = "queue_mpmc";
  res.implementation = impl_name;
  res.producers = num_producers;
  res.consumers = num_consumers;
  res.capacity = capacity;
  res.target_items = total_items;

  QueueType queue(capacity);

  std::atomic<bool> producers_done{false};
  std::atomic<bool> abort_test{false};
  std::atomic<std::uint64_t> push_failures{0};
  std::atomic<std::uint64_t> pop_failures{0};
  std::atomic<std::size_t> total_consumed{0};

  std::vector<std::vector<std::uint64_t>> consumer_results(num_consumers);
  for (auto& vec : consumer_results) {
    vec.reserve(total_items / num_consumers + 1024);
  }

  const auto start_time = std::chrono::steady_clock::now();

  std::vector<std::thread> consumers;
  consumers.reserve(num_consumers);
  for (std::size_t c = 0; c < num_consumers; ++c) {
    consumers.emplace_back([&, c]() {
      auto& my_vec = consumer_results[c];
      const auto deadline = std::chrono::steady_clock::now() + 45s;
      while (total_consumed.load(std::memory_order_relaxed) < total_items && !abort_test.load(std::memory_order_relaxed)) {
        std::uint64_t val = 0;
        if (queue.try_pop(val)) {
          my_vec.push_back(val);
          total_consumed.fetch_add(1, std::memory_order_relaxed);
        } else {
          pop_failures.fetch_add(1, std::memory_order_relaxed);
          if (producers_done.load(std::memory_order_acquire) && queue.empty()) {
            while (queue.try_pop(val)) {
              my_vec.push_back(val);
              total_consumed.fetch_add(1, std::memory_order_relaxed);
            }
            if (total_consumed.load(std::memory_order_relaxed) >= total_items) break;
          }
          if (std::chrono::steady_clock::now() > deadline) {
            abort_test.store(true, std::memory_order_relaxed);
            break;
          }
          std::this_thread::yield();
        }
      }
    });
  }

  std::vector<std::thread> producers;
  producers.reserve(num_producers);
  for (std::size_t p = 0; p < num_producers; ++p) {
    producers.emplace_back([&, p]() {
      const auto deadline = std::chrono::steady_clock::now() + 45s;
      for (std::uint64_t i = 0; i < items_per_producer; ++i) {
        if (abort_test.load(std::memory_order_relaxed)) break;
        const std::uint64_t item = (static_cast<std::uint64_t>(p) << 32) | (i & 0xFFFFFFFFULL);
        while (!queue.try_push(item)) {
          push_failures.fetch_add(1, std::memory_order_relaxed);
          if (std::chrono::steady_clock::now() > deadline) {
            abort_test.store(true, std::memory_order_relaxed);
            break;
          }
          std::this_thread::yield();
        }
      }
    });
  }

  for (auto& th : producers) th.join();
  producers_done.store(true, std::memory_order_release);
  for (auto& th : consumers) th.join();
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = total_consumed.load();
  res.push_failures = push_failures.load();
  res.pop_failures = pop_failures.load();
  res.successful_operations = res.transferred_items * 2;
  res.attempted_operations = res.successful_operations + res.push_failures + res.pop_failures;

  if (res.elapsed_ms > 0.0) {
    const double elapsed_sec = res.elapsed_ms / 1000.0;
    res.transferred_items_per_sec = static_cast<double>(res.transferred_items) / elapsed_sec;
    res.total_ops_per_sec = static_cast<double>(res.attempted_operations) / elapsed_sec;
  }

  // Verification: every producer ID and sequence consumed exactly once
  if (abort_test.load()) {
    res.correctness_passed = false;
    res.correctness_error = "MPMC benchmark timed out";
  } else if (res.transferred_items != total_items) {
    res.correctness_passed = false;
    res.correctness_error = "Item count mismatch: expected " + std::to_string(total_items) +
                            ", got " + std::to_string(res.transferred_items);
  } else {
    std::vector<std::vector<bool>> seen(num_producers, std::vector<bool>(items_per_producer, false));
    bool valid = true;
    for (const auto& vec : consumer_results) {
      for (std::uint64_t item : vec) {
        const auto p = item >> 32;
        const auto seq = item & 0xFFFFFFFFULL;
        if (p >= num_producers || seq >= items_per_producer) {
          valid = false;
          res.correctness_error = "Invalid item received: producer=" + std::to_string(p) +
                                  " seq=" + std::to_string(seq);
          break;
        }
        if (seen[p][seq]) {
          valid = false;
          res.correctness_error = "Duplicate item received: producer=" + std::to_string(p) +
                                  " seq=" + std::to_string(seq);
          break;
        }
        seen[p][seq] = true;
      }
      if (!valid) break;
    }
    if (valid) {
      for (std::size_t p = 0; p < num_producers; ++p) {
        for (std::size_t i = 0; i < items_per_producer; ++i) {
          if (!seen[p][i]) {
            valid = false;
            res.correctness_error = "Missing item from producer " + std::to_string(p) +
                                    " seq " + std::to_string(i);
            break;
          }
        }
        if (!valid) break;
      }
    }
    res.correctness_passed = valid;
  }

  return res;
}

// ============================================================================
// Notification Benchmarks
// ============================================================================

BenchmarkResult run_notify_spsc_eventfd(std::size_t num_notifications) {
  BenchmarkResult res{};
  res.benchmark = "notify_spsc";
  res.implementation = "EventNotifier (eventfd)";
  res.producers = 1;
  res.consumers = 1;
  res.target_items = num_notifications;

  EventNotifier notifier;
  if (!notifier.valid()) {
    res.correctness_passed = false;
    res.correctness_error = "Failed to create EventNotifier: errno=" + std::to_string(notifier.creation_errno());
    return res;
  }

  std::atomic<bool> producer_done{false};
  std::atomic<bool> abort_test{false};
  std::atomic<bool> operation_error{false};
  std::string error_desc;
  std::mutex error_mutex;

  std::atomic<std::uint64_t> total_consumed_count{0};
  std::atomic<std::uint64_t> read_syscalls{0};
  std::atomic<std::uint64_t> write_syscalls{0};
  std::atomic<std::uint64_t> write_retries{0};
  std::atomic<std::uint64_t> empty_consumes{0};

  auto report_error = [&](const std::string& err) {
    std::lock_guard<std::mutex> lk(error_mutex);
    if (!operation_error.load()) {
      error_desc = err;
      operation_error.store(true, std::memory_order_relaxed);
      abort_test.store(true, std::memory_order_relaxed);
    }
  };

  const auto start_time = std::chrono::steady_clock::now();

  std::thread consumer([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (total_consumed_count.load(std::memory_order_relaxed) < num_notifications &&
           !abort_test.load(std::memory_order_relaxed)) {
      std::uint64_t val = 0;
      int err = 0;
      read_syscalls.fetch_add(1, std::memory_order_relaxed);
      const auto status = notifier.consume(val, &err);
      if (status == ConsumeStatus::Success) {
        if (val == 0) {
          report_error("Unexpected zero counter received on Success consume");
          return;
        }
        total_consumed_count.fetch_add(val, std::memory_order_relaxed);
      } else if (status == ConsumeStatus::Empty) {
        empty_consumes.fetch_add(1, std::memory_order_relaxed);
        if (producer_done.load(std::memory_order_acquire)) {
          read_syscalls.fetch_add(1, std::memory_order_relaxed);
          if (notifier.consume(val, &err) == ConsumeStatus::Success) {
            total_consumed_count.fetch_add(val, std::memory_order_relaxed);
          }
          if (total_consumed_count.load(std::memory_order_relaxed) >= num_notifications) break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
          abort_test.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      } else {
        report_error("EventNotifier consume failed with errno=" + std::to_string(err));
        return;
      }
    }
  });

  std::thread producer([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    for (std::size_t i = 0; i < num_notifications; ++i) {
      if (abort_test.load(std::memory_order_relaxed)) break;
      while (true) {
        write_syscalls.fetch_add(1, std::memory_order_relaxed);
        int err = 0;
        const auto st = notifier.notify(1, &err);
        if (st == NotifyStatus::Success) {
          break;
        }
        if (st == NotifyStatus::Saturated) {
          write_retries.fetch_add(1, std::memory_order_relaxed);
          if (std::chrono::steady_clock::now() > deadline) {
            abort_test.store(true, std::memory_order_relaxed);
            break;
          }
          std::this_thread::yield();
          continue;
        }
        report_error("EventNotifier notify failed with errno=" + std::to_string(err));
        return;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = total_consumed_count.load();
  res.write_syscalls = write_syscalls.load();
  res.read_syscalls = read_syscalls.load();
  res.push_failures = write_retries.load();
  res.pop_failures = empty_consumes.load();
  res.successful_operations = num_notifications;
  res.attempted_operations = *res.write_syscalls + *res.read_syscalls;

  if (res.elapsed_ms > 0.0) {
    const double elapsed_sec = res.elapsed_ms / 1000.0;
    res.transferred_items_per_sec = static_cast<double>(res.transferred_items) / elapsed_sec;
    res.total_ops_per_sec = static_cast<double>(res.attempted_operations) / elapsed_sec;
  }

  if (operation_error.load()) {
    res.correctness_passed = false;
    res.correctness_error = error_desc;
  } else if (abort_test.load()) {
    res.correctness_passed = false;
    res.correctness_error = "Notification benchmark timed out";
  } else if (res.transferred_items != num_notifications) {
    res.correctness_passed = false;
    res.correctness_error = "Counter sum mismatch: expected " + std::to_string(num_notifications) +
                            ", got " + std::to_string(res.transferred_items);
  } else {
    res.correctness_passed = true;
  }

  return res;
}

BenchmarkResult run_notify_spsc_pipe(std::size_t num_notifications) {
  BenchmarkResult res{};
  res.benchmark = "notify_spsc";
  res.implementation = "PipeNotifier (nonblocking pipe)";
  res.producers = 1;
  res.consumers = 1;
  res.target_items = num_notifications;

  PipeNotifier notifier;
  if (!notifier.valid()) {
    res.correctness_passed = false;
    res.correctness_error = "Failed to create PipeNotifier: errno=" + std::to_string(notifier.creation_errno());
    return res;
  }

  std::atomic<bool> producer_done{false};
  std::atomic<bool> abort_test{false};
  std::atomic<bool> operation_error{false};
  std::string error_desc;
  std::mutex error_mutex;

  std::atomic<std::uint64_t> total_consumed_count{0};
  std::atomic<std::uint64_t> write_syscalls{0};
  std::atomic<std::uint64_t> read_syscalls{0};
  std::atomic<std::uint64_t> write_retries{0};
  std::atomic<std::uint64_t> empty_consumes{0};

  auto report_error = [&](const std::string& err) {
    std::lock_guard<std::mutex> lk(error_mutex);
    if (!operation_error.load()) {
      error_desc = err;
      operation_error.store(true, std::memory_order_relaxed);
      abort_test.store(true, std::memory_order_relaxed);
    }
  };

  const auto start_time = std::chrono::steady_clock::now();

  std::thread consumer([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (total_consumed_count.load(std::memory_order_relaxed) < num_notifications &&
           !abort_test.load(std::memory_order_relaxed)) {
      std::uint8_t token = 0;
      int err = 0;
      read_syscalls.fetch_add(1, std::memory_order_relaxed);
      const auto status = notifier.consume(token, &err);
      if (status == ConsumeStatus::Success) {
        total_consumed_count.fetch_add(1, std::memory_order_relaxed);
      } else if (status == ConsumeStatus::Empty) {
        empty_consumes.fetch_add(1, std::memory_order_relaxed);
        if (producer_done.load(std::memory_order_acquire)) {
          read_syscalls.fetch_add(1, std::memory_order_relaxed);
          if (notifier.consume(token, &err) == ConsumeStatus::Success) {
            total_consumed_count.fetch_add(1, std::memory_order_relaxed);
          }
          if (total_consumed_count.load(std::memory_order_relaxed) >= num_notifications) break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
          abort_test.store(true, std::memory_order_relaxed);
          break;
        }
        std::this_thread::yield();
      } else {
        report_error("PipeNotifier consume failed with errno=" + std::to_string(err));
        return;
      }
    }
  });

  std::thread producer([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    for (std::size_t i = 0; i < num_notifications; ++i) {
      if (abort_test.load(std::memory_order_relaxed)) break;
      while (true) {
        write_syscalls.fetch_add(1, std::memory_order_relaxed);
        int err = 0;
        const auto st = notifier.notify(1, &err);
        if (st == NotifyStatus::Success) {
          break;
        }
        if (st == NotifyStatus::Saturated) {
          write_retries.fetch_add(1, std::memory_order_relaxed);
          if (std::chrono::steady_clock::now() > deadline) {
            abort_test.store(true, std::memory_order_relaxed);
            break;
          }
          std::this_thread::yield();
          continue;
        }
        report_error("PipeNotifier notify failed with errno=" + std::to_string(err));
        return;
      }
    }
    producer_done.store(true, std::memory_order_release);
  });

  producer.join();
  consumer.join();
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = total_consumed_count.load();
  res.write_syscalls = write_syscalls.load();
  res.read_syscalls = read_syscalls.load();
  res.push_failures = write_retries.load();
  res.pop_failures = empty_consumes.load();
  res.successful_operations = num_notifications;
  res.attempted_operations = *res.write_syscalls + *res.read_syscalls;

  if (res.elapsed_ms > 0.0) {
    const double elapsed_sec = res.elapsed_ms / 1000.0;
    res.transferred_items_per_sec = static_cast<double>(res.transferred_items) / elapsed_sec;
    res.total_ops_per_sec = static_cast<double>(res.attempted_operations) / elapsed_sec;
  }

  if (operation_error.load()) {
    res.correctness_passed = false;
    res.correctness_error = error_desc;
  } else if (abort_test.load()) {
    res.correctness_passed = false;
    res.correctness_error = "Pipe notification benchmark timed out";
  } else if (res.transferred_items != num_notifications) {
    res.correctness_passed = false;
    res.correctness_error = "Byte count mismatch: expected " + std::to_string(num_notifications) +
                            ", got " + std::to_string(res.transferred_items);
  } else {
    res.correctness_passed = true;
  }

  return res;
}

BenchmarkResult run_notify_batched_eventfd(std::size_t batches, std::size_t batch_size) {
  const std::size_t total_items = batches * batch_size;
  BenchmarkResult res{};
  res.benchmark = "notify_batched";
  res.implementation = "EventNotifier (coalesced counter)";
  res.producers = 1;
  res.consumers = 1;
  res.target_items = total_items;

  EventNotifier notifier;
  if (!notifier.valid()) {
    res.correctness_passed = false;
    res.correctness_error = "Failed to create EventNotifier: errno=" + std::to_string(notifier.creation_errno());
    return res;
  }

  std::uint64_t total_consumed = 0;
  std::uint64_t writes = 0;
  std::uint64_t reads = 0;

  const auto start_time = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < batches; ++b) {
    for (std::size_t k = 0; k < batch_size; ++k) {
      ++writes;
      int err = 0;
      if (notifier.notify(1, &err) != NotifyStatus::Success) {
        res.correctness_passed = false;
        res.correctness_error = "EventNotifier notify failed with errno=" + std::to_string(err);
        return res;
      }
    }
    // Single read consumes the accumulated counter from the entire batch
    ++reads;
    std::uint64_t val = 0;
    int err = 0;
    const auto st = notifier.consume(val, &err);
    if (st == ConsumeStatus::Success) {
      total_consumed += val;
    } else {
      res.correctness_passed = false;
      res.correctness_error = "EventNotifier consume failed with status=" +
                              std::to_string(static_cast<int>(st)) + " errno=" + std::to_string(err);
      return res;
    }
  }
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = total_consumed;
  res.write_syscalls = writes;
  res.read_syscalls = reads;
  res.successful_operations = writes + reads;
  res.attempted_operations = writes + reads;

  if (res.elapsed_ms > 0.0) {
    const double elapsed_sec = res.elapsed_ms / 1000.0;
    res.transferred_items_per_sec = static_cast<double>(res.transferred_items) / elapsed_sec;
    res.total_ops_per_sec = static_cast<double>(res.attempted_operations) / elapsed_sec;
  }
  res.correctness_passed = (total_consumed == total_items);
  if (!res.correctness_passed) {
    res.correctness_error = "Batched counter mismatch: expected " + std::to_string(total_items) +
                            ", got " + std::to_string(total_consumed);
  }
  return res;
}

BenchmarkResult run_notify_batched_pipe(std::size_t batches, std::size_t batch_size) {
  const std::size_t total_items = batches * batch_size;
  BenchmarkResult res{};
  res.benchmark = "notify_batched";
  res.implementation = "PipeNotifier (batched stream drain)";
  res.producers = 1;
  res.consumers = 1;
  res.target_items = total_items;

  PipeNotifier notifier;
  if (!notifier.valid()) {
    res.correctness_passed = false;
    res.correctness_error = "Failed to create PipeNotifier: errno=" + std::to_string(notifier.creation_errno());
    return res;
  }

  std::uint64_t total_consumed = 0;
  std::uint64_t writes = 0;
  std::uint64_t reads = 0;

  const auto start_time = std::chrono::steady_clock::now();
  for (std::size_t b = 0; b < batches; ++b) {
    for (std::size_t k = 0; k < batch_size; ++k) {
      ++writes;
      int err = 0;
      if (notifier.notify(1, &err) != NotifyStatus::Success) {
        res.correctness_passed = false;
        res.correctness_error = "PipeNotifier notify failed with errno=" + std::to_string(err);
        return res;
      }
    }
    // Drains the buffered bytes in batch, accurately counting read syscalls
    int err = 0;
    const std::size_t drained = notifier.drain(&reads, &err);
    if (err != 0) {
      res.correctness_passed = false;
      res.correctness_error = "PipeNotifier drain failed with errno=" + std::to_string(err);
      return res;
    }
    total_consumed += drained;
  }
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = total_consumed;
  res.write_syscalls = writes;
  res.read_syscalls = reads;
  res.successful_operations = writes + (reads > 0 ? reads - batches : 0); // read calls returning data
  res.attempted_operations = writes + reads;

  if (res.elapsed_ms > 0.0) {
    const double elapsed_sec = res.elapsed_ms / 1000.0;
    res.transferred_items_per_sec = static_cast<double>(res.transferred_items) / elapsed_sec;
    res.total_ops_per_sec = static_cast<double>(res.attempted_operations) / elapsed_sec;
  }
  res.correctness_passed = (total_consumed == total_items);
  if (!res.correctness_passed) {
    res.correctness_error = "Batched pipe byte mismatch: expected " + std::to_string(total_items) +
                            ", got " + std::to_string(total_consumed);
  }
  return res;
}

BenchmarkResult run_notify_latency_eventfd(std::size_t roundtrips) {
  BenchmarkResult res{};
  res.benchmark = "notify_latency";
  res.implementation = "EventNotifier (eventfd ping-pong)";
  res.producers = 1;
  res.consumers = 1;
  res.target_items = roundtrips;

  EventNotifier n1;
  EventNotifier n2;
  if (!n1.valid() || !n2.valid()) {
    res.correctness_passed = false;
    res.correctness_error = "Failed to initialize EventNotifiers";
    return res;
  }

  std::atomic<bool> abort_test{false};
  std::atomic<bool> operation_error{false};
  std::string error_desc;
  std::mutex error_mutex;

  std::atomic<std::uint64_t> write_syscalls{0};
  std::atomic<std::uint64_t> read_syscalls{0};

  auto report_error = [&](const std::string& err) {
    std::lock_guard<std::mutex> lk(error_mutex);
    if (!operation_error.load()) {
      error_desc = err;
      operation_error.store(true, std::memory_order_relaxed);
      abort_test.store(true, std::memory_order_relaxed);
    }
  };

  const auto start_time = std::chrono::steady_clock::now();

  std::thread worker([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    pollfd pfd{.fd = n1.descriptor(), .events = POLLIN, .revents = 0};
    for (std::size_t i = 0; i < roundtrips; ++i) {
      if (abort_test.load(std::memory_order_relaxed)) break;
      // Wait for signal from main thread
      while (true) {
        pfd.revents = 0;
        int p_rc = ::poll(&pfd, 1, 100);
        if (p_rc > 0 && (pfd.revents & POLLIN) != 0) {
          std::uint64_t val = 0;
          int err = 0;
          read_syscalls.fetch_add(1, std::memory_order_relaxed);
          if (n1.consume(val, &err) != ConsumeStatus::Success) {
            report_error("Worker n1 consume failed with errno=" + std::to_string(err));
            return;
          }
          break;
        }
        if (p_rc < 0 && errno == EINTR) continue;
        if (std::chrono::steady_clock::now() > deadline) {
          abort_test.store(true, std::memory_order_relaxed);
          return;
        }
      }
      // Signal main thread back
      write_syscalls.fetch_add(1, std::memory_order_relaxed);
      int err = 0;
      if (n2.notify(1, &err) != NotifyStatus::Success) {
        report_error("Worker n2 notify failed with errno=" + std::to_string(err));
        return;
      }
    }
  });

  pollfd pfd{.fd = n2.descriptor(), .events = POLLIN, .revents = 0};
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  std::size_t completed = 0;

  for (std::size_t i = 0; i < roundtrips; ++i) {
    if (abort_test.load(std::memory_order_relaxed)) break;
    // Signal worker
    write_syscalls.fetch_add(1, std::memory_order_relaxed);
    int err = 0;
    if (n1.notify(1, &err) != NotifyStatus::Success) {
      report_error("Main thread n1 notify failed with errno=" + std::to_string(err));
      break;
    }
    // Wait for worker reply
    while (true) {
      pfd.revents = 0;
      int p_rc = ::poll(&pfd, 1, 100);
      if (p_rc > 0 && (pfd.revents & POLLIN) != 0) {
        std::uint64_t val = 0;
        int c_err = 0;
        read_syscalls.fetch_add(1, std::memory_order_relaxed);
        if (n2.consume(val, &c_err) != ConsumeStatus::Success) {
          report_error("Main thread n2 consume failed with errno=" + std::to_string(c_err));
          break;
        }
        ++completed;
        break;
      }
      if (p_rc < 0 && errno == EINTR) continue;
      if (std::chrono::steady_clock::now() > deadline) {
        abort_test.store(true, std::memory_order_relaxed);
        break;
      }
    }
  }

  worker.join();
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = completed;
  res.write_syscalls = write_syscalls.load();
  res.read_syscalls = read_syscalls.load();
  res.successful_operations = completed * 2; // full roundtrips
  res.attempted_operations = *res.write_syscalls + *res.read_syscalls;

  if (completed > 0) {
    const double total_ns = std::chrono::duration<double, std::nano>(end_time - start_time).count();
    const double avg_rtt = total_ns / static_cast<double>(completed);
    res.avg_roundtrip_latency_ns = avg_rtt;
    res.estimated_half_rtt_latency_ns = avg_rtt / 2.0;
    res.transferred_items_per_sec = (static_cast<double>(completed) / (res.elapsed_ms / 1000.0));
    res.total_ops_per_sec = (static_cast<double>(res.attempted_operations) / (res.elapsed_ms / 1000.0));
  }

  if (operation_error.load()) {
    res.correctness_passed = false;
    res.correctness_error = error_desc;
  } else if (abort_test.load()) {
    res.correctness_passed = false;
    res.correctness_error = "Ping-pong latency test timed out";
  } else if (completed != roundtrips) {
    res.correctness_passed = false;
    res.correctness_error = "Ping-pong roundtrip count mismatch: expected " + std::to_string(roundtrips) +
                            ", got " + std::to_string(completed);
  } else {
    res.correctness_passed = true;
  }
  return res;
}

BenchmarkResult run_notify_latency_pipe(std::size_t roundtrips) {
  BenchmarkResult res{};
  res.benchmark = "notify_latency";
  res.implementation = "PipeNotifier (pipe ping-pong)";
  res.producers = 1;
  res.consumers = 1;
  res.target_items = roundtrips;

  PipeNotifier p1;
  PipeNotifier p2;
  if (!p1.valid() || !p2.valid()) {
    res.correctness_passed = false;
    res.correctness_error = "Failed to initialize PipeNotifiers";
    return res;
  }

  std::atomic<bool> abort_test{false};
  std::atomic<bool> operation_error{false};
  std::string error_desc;
  std::mutex error_mutex;

  std::atomic<std::uint64_t> write_syscalls{0};
  std::atomic<std::uint64_t> read_syscalls{0};

  auto report_error = [&](const std::string& err) {
    std::lock_guard<std::mutex> lk(error_mutex);
    if (!operation_error.load()) {
      error_desc = err;
      operation_error.store(true, std::memory_order_relaxed);
      abort_test.store(true, std::memory_order_relaxed);
    }
  };

  const auto start_time = std::chrono::steady_clock::now();

  std::thread worker([&]() {
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    pollfd pfd{.fd = p1.read_descriptor(), .events = POLLIN, .revents = 0};
    for (std::size_t i = 0; i < roundtrips; ++i) {
      if (abort_test.load(std::memory_order_relaxed)) break;
      while (true) {
        pfd.revents = 0;
        int p_rc = ::poll(&pfd, 1, 100);
        if (p_rc > 0 && (pfd.revents & POLLIN) != 0) {
          std::uint8_t tok = 0;
          int err = 0;
          read_syscalls.fetch_add(1, std::memory_order_relaxed);
          if (p1.consume(tok, &err) != ConsumeStatus::Success) {
            report_error("Worker p1 consume failed with errno=" + std::to_string(err));
            return;
          }
          break;
        }
        if (p_rc < 0 && errno == EINTR) continue;
        if (std::chrono::steady_clock::now() > deadline) {
          abort_test.store(true, std::memory_order_relaxed);
          return;
        }
      }
      write_syscalls.fetch_add(1, std::memory_order_relaxed);
      int err = 0;
      if (p2.notify(1, &err) != NotifyStatus::Success) {
        report_error("Worker p2 notify failed with errno=" + std::to_string(err));
        return;
      }
    }
  });

  pollfd pfd{.fd = p2.read_descriptor(), .events = POLLIN, .revents = 0};
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  std::size_t completed = 0;

  for (std::size_t i = 0; i < roundtrips; ++i) {
    if (abort_test.load(std::memory_order_relaxed)) break;
    write_syscalls.fetch_add(1, std::memory_order_relaxed);
    int err = 0;
    if (p1.notify(1, &err) != NotifyStatus::Success) {
      report_error("Main thread p1 notify failed with errno=" + std::to_string(err));
      break;
    }
    while (true) {
      pfd.revents = 0;
      int p_rc = ::poll(&pfd, 1, 100);
      if (p_rc > 0 && (pfd.revents & POLLIN) != 0) {
        std::uint8_t tok = 0;
        int c_err = 0;
        read_syscalls.fetch_add(1, std::memory_order_relaxed);
        if (p2.consume(tok, &c_err) != ConsumeStatus::Success) {
          report_error("Main thread p2 consume failed with errno=" + std::to_string(c_err));
          break;
        }
        ++completed;
        break;
      }
      if (p_rc < 0 && errno == EINTR) continue;
      if (std::chrono::steady_clock::now() > deadline) {
        abort_test.store(true, std::memory_order_relaxed);
        break;
      }
    }
  }

  worker.join();
  const auto end_time = std::chrono::steady_clock::now();

  res.elapsed_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();
  res.transferred_items = completed;
  res.write_syscalls = write_syscalls.load();
  res.read_syscalls = read_syscalls.load();
  res.successful_operations = completed * 2;
  res.attempted_operations = *res.write_syscalls + *res.read_syscalls;

  if (completed > 0) {
    const double total_ns = std::chrono::duration<double, std::nano>(end_time - start_time).count();
    const double avg_rtt = total_ns / static_cast<double>(completed);
    res.avg_roundtrip_latency_ns = avg_rtt;
    res.estimated_half_rtt_latency_ns = avg_rtt / 2.0;
    res.transferred_items_per_sec = (static_cast<double>(completed) / (res.elapsed_ms / 1000.0));
    res.total_ops_per_sec = (static_cast<double>(res.attempted_operations) / (res.elapsed_ms / 1000.0));
  }

  if (operation_error.load()) {
    res.correctness_passed = false;
    res.correctness_error = error_desc;
  } else if (abort_test.load()) {
    res.correctness_passed = false;
    res.correctness_error = "Pipe latency test timed out";
  } else if (completed != roundtrips) {
    res.correctness_passed = false;
    res.correctness_error = "Pipe roundtrip count mismatch: expected " + std::to_string(roundtrips) +
                            ", got " + std::to_string(completed);
  } else {
    res.correctness_passed = true;
  }
  return res;
}

// ============================================================================
// Environment Metadata Helper
// ============================================================================

json gather_environment_metadata() {
  json meta;

  // Timestamp
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

void print_human_readable(const std::vector<BenchmarkResult>& results) {
  std::cout << "\n"
            << std::left << std::setw(16) << "Benchmark"
            << std::setw(34) << "Implementation"
            << std::setw(8) << "P/C"
            << std::right << std::setw(10) << "Items"
            << std::setw(12) << "Elapsed(ms)"
            << std::setw(16) << "Throughput(it/s)"
            << std::setw(14) << "AvgRTT(ns)"
            << std::setw(14) << "HalfRTT(ns)"
            << std::setw(10) << "PushFail"
            << std::setw(10) << "PopFail"
            << "  Result\n";

  std::cout << std::string(148, '-') << "\n";

  for (const auto& r : results) {
    std::string pc_str = std::to_string(r.producers) + "/" + std::to_string(r.consumers);
    std::cout << std::left << std::setw(16) << r.benchmark
              << std::setw(34) << r.implementation
              << std::setw(8) << pc_str
              << std::right << std::setw(10) << r.transferred_items
              << std::fixed << std::setprecision(2) << std::setw(12) << r.elapsed_ms
              << std::fixed << std::setprecision(1) << std::setw(16) << r.transferred_items_per_sec;

    if (r.avg_roundtrip_latency_ns.has_value()) {
      std::cout << std::fixed << std::setprecision(1) << std::setw(14) << *r.avg_roundtrip_latency_ns;
    } else {
      std::cout << std::setw(14) << "-";
    }

    if (r.estimated_half_rtt_latency_ns.has_value()) {
      std::cout << std::fixed << std::setprecision(1) << std::setw(14) << *r.estimated_half_rtt_latency_ns;
    } else {
      std::cout << std::setw(14) << "-";
    }

    std::cout << std::setw(10) << r.push_failures
              << std::setw(10) << r.pop_failures
              << "  " << (r.correctness_passed ? "PASS" : ("FAIL: " + r.correctness_error))
              << "\n";
  }
  std::cout << std::string(148, '-') << "\n\n";
}

void print_help(const char* prog_name) {
  std::cout << "Usage: " << prog_name << " [options]\n\n"
            << "Options:\n"
            << "  -b, --benchmark=<name>  Benchmark family to run: all (default), queue, notify,\n"
            << "                          queue_spsc, queue_mpmc, notify_spsc, notify_batched, notify_latency\n"
            << "  -n, --items=<count>     Base item/notification count (default: 500000 for queue, 250000 for notify)\n"
            << "  -c, --capacity=<size>   Queue capacity (power of 2, default: 1024)\n"
            << "  --json                  Output JSON results to stdout\n"
            << "  --json-out=<path>       Write JSON results to specified file\n"
            << "  -h, --help              Show this help message\n";
}

// Helper to safely parse positive unsigned 64-bit integer
bool parse_positive_uint64(std::string_view str, std::uint64_t& out_val) {
  if (str.empty()) return false;
  // Disallow leading +/- signs or non-digits
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

}  // namespace

int main(int argc, char* argv[]) {
  std::string benchmark_filter = "all";
  std::uint64_t base_items = 500000;
  std::uint64_t queue_capacity = 1024;
  bool json_stdout = false;
  std::string json_out_path;

  // Parse command-line options
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
    } else if (arg.starts_with("--benchmark=")) {
      benchmark_filter = std::string(arg.substr(12));
    } else if (arg == "-b" && i + 1 < argc) {
      benchmark_filter = argv[++i];
    } else if (arg.starts_with("--items=")) {
      if (!parse_positive_uint64(arg.substr(8), base_items)) {
        std::cerr << "Error: Invalid or non-positive value for --items: " << arg.substr(8) << "\n";
        return 1;
      }
    } else if (arg == "-n" && i + 1 < argc) {
      if (!parse_positive_uint64(argv[++i], base_items)) {
        std::cerr << "Error: Invalid or non-positive value for -n: " << argv[i] << "\n";
        return 1;
      }
    } else if (arg.starts_with("--capacity=")) {
      if (!parse_positive_uint64(arg.substr(11), queue_capacity)) {
        std::cerr << "Error: Invalid or non-positive value for --capacity: " << arg.substr(11) << "\n";
        return 1;
      }
    } else if (arg == "-c" && i + 1 < argc) {
      if (!parse_positive_uint64(argv[++i], queue_capacity)) {
        std::cerr << "Error: Invalid or non-positive value for -c: " << argv[i] << "\n";
        return 1;
      }
    } else {
      std::cerr << "Unknown or invalid argument: " << arg << "\n";
      print_help(argv[0]);
      return 1;
    }
  }

  // Validate benchmark name
  const std::vector<std::string> valid_benchmarks = {
      "all", "queue", "notify", "queue_spsc", "queue_mpmc",
      "notify_spsc", "notify_batched", "notify_latency"
  };
  if (std::find(valid_benchmarks.begin(), valid_benchmarks.end(), benchmark_filter) == valid_benchmarks.end()) {
    std::cerr << "Error: Unknown benchmark name: '" << benchmark_filter << "'\n";
    print_help(argv[0]);
    return 1;
  }

  // Validate queue capacity (power of two >= 2 and <= 2^30)
  if (queue_capacity < 2 || queue_capacity > (1ULL << 30) || (queue_capacity & (queue_capacity - 1)) != 0) {
    std::cerr << "Error: Queue capacity must be a power of two between 2 and 1073741824, got " << queue_capacity << "\n";
    return 1;
  }

  // Validate items bound (prevent overflow in calculations)
  if (base_items > 100000000ULL) {
    std::cerr << "Error: Item count exceeds maximum allowed limit (100,000,000), got " << base_items << "\n";
    return 1;
  }

  std::vector<BenchmarkResult> results;
  bool all_pass = true;

  auto should_run = [&](std::string_view name) {
    if (benchmark_filter == "all") return true;
    if (benchmark_filter == "queue" && name.starts_with("queue_")) return true;
    if (benchmark_filter == "notify" && name.starts_with("notify_")) return true;
    return benchmark_filter == name;
  };

  std::cerr << "=== Starting DARKPOOL IPC-6 Benchmark Suite ===\n";
  std::cerr << "Base items: " << base_items << ", Queue capacity: " << queue_capacity << "\n";

  // 1. Queue SPSC: BoundedMpmcQueue vs BoundedMutexQueue
  if (should_run("queue_spsc")) {
    std::cerr << "[run] queue_spsc (BoundedMpmcQueue)...\n";
    auto r1 = run_queue_spsc<BoundedMpmcQueue<std::uint64_t>>("BoundedMpmcQueue", static_cast<std::size_t>(queue_capacity), static_cast<std::size_t>(base_items));
    if (!r1.correctness_passed) all_pass = false;
    results.push_back(r1);

    std::cerr << "[run] queue_spsc (BoundedMutexQueue)...\n";
    auto r2 = run_queue_spsc<BoundedMutexQueue<std::uint64_t>>("BoundedMutexQueue", static_cast<std::size_t>(queue_capacity), static_cast<std::size_t>(base_items));
    if (!r2.correctness_passed) all_pass = false;
    results.push_back(r2);
  }

  // 2. Queue MPMC (4P / 4C): BoundedMpmcQueue vs BoundedMutexQueue
  if (should_run("queue_mpmc")) {
    constexpr std::size_t kProducers = 4;
    constexpr std::size_t kConsumers = 4;
    const std::size_t items_per_prod = std::max<std::size_t>(1000, static_cast<std::size_t>(base_items / kProducers));

    // Ensure items_per_prod fits into lower 32-bits without truncation
    if (items_per_prod > 0xFFFFFFFFULL) {
      std::cerr << "Error: items_per_producer exceeds 32-bit integer limit: " << items_per_prod << "\n";
      return 1;
    }

    std::cerr << "[run] queue_mpmc (BoundedMpmcQueue 4P/4C)...\n";
    auto r1 = run_queue_mpmc<BoundedMpmcQueue<std::uint64_t>>("BoundedMpmcQueue", static_cast<std::size_t>(queue_capacity),
                                                              kProducers, kConsumers, items_per_prod);
    if (!r1.correctness_passed) all_pass = false;
    results.push_back(r1);

    std::cerr << "[run] queue_mpmc (BoundedMutexQueue 4P/4C)...\n";
    auto r2 = run_queue_mpmc<BoundedMutexQueue<std::uint64_t>>("BoundedMutexQueue", static_cast<std::size_t>(queue_capacity),
                                                              kProducers, kConsumers, items_per_prod);
    if (!r2.correctness_passed) all_pass = false;
    results.push_back(r2);
  }

  // 3. Notification SPSC Throughput: EventNotifier vs PipeNotifier
  if (should_run("notify_spsc")) {
    const std::size_t notify_count = std::max<std::size_t>(1000, static_cast<std::size_t>(base_items / 2));

    std::cerr << "[run] notify_spsc (EventNotifier)...\n";
    auto r1 = run_notify_spsc_eventfd(notify_count);
    if (!r1.correctness_passed) all_pass = false;
    results.push_back(r1);

    std::cerr << "[run] notify_spsc (PipeNotifier)...\n";
    auto r2 = run_notify_spsc_pipe(notify_count);
    if (!r2.correctness_passed) all_pass = false;
    results.push_back(r2);
  }

  // 4. Notification Batched / Coalesced Throughput
  if (should_run("notify_batched")) {
    constexpr std::size_t kBatchSize = 64;
    const std::size_t batches = std::max<std::size_t>(100, static_cast<std::size_t>(base_items / (kBatchSize * 4)));

    // Check multiplication overflow
    if (batches > std::numeric_limits<std::size_t>::max() / kBatchSize) {
      std::cerr << "Error: batches * batch_size causes arithmetic overflow\n";
      return 1;
    }

    std::cerr << "[run] notify_batched (EventNotifier coalesced)...\n";
    auto r1 = run_notify_batched_eventfd(batches, kBatchSize);
    if (!r1.correctness_passed) all_pass = false;
    results.push_back(r1);

    std::cerr << "[run] notify_batched (PipeNotifier batched drain)...\n";
    auto r2 = run_notify_batched_pipe(batches, kBatchSize);
    if (!r2.correctness_passed) all_pass = false;
    results.push_back(r2);
  }

  // 5. Notification Ping-Pong Round-Trip Latency
  if (should_run("notify_latency")) {
    const std::size_t latency_roundtrips = std::min<std::size_t>(50000, std::max<std::size_t>(1000, static_cast<std::size_t>(base_items / 20)));

    std::cerr << "[run] notify_latency (EventNotifier ping-pong)...\n";
    auto r1 = run_notify_latency_eventfd(latency_roundtrips);
    if (!r1.correctness_passed) all_pass = false;
    results.push_back(r1);

    std::cerr << "[run] notify_latency (PipeNotifier ping-pong)...\n";
    auto r2 = run_notify_latency_pipe(latency_roundtrips);
    if (!r2.correctness_passed) all_pass = false;
    results.push_back(r2);
  }

  // Human-readable summary
  if (!json_stdout) {
    print_human_readable(results);
  }

  // Machine-readable JSON output
  json root;
  root["benchmark_suite"] = "darkpool_ipc_benchmark";
  root["metadata"] = gather_environment_metadata();
  root["results"] = json::array();
  for (const auto& r : results) {
    root["results"].push_back(r.to_json());
  }

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
  std::cerr << "darkpool_ipc_benchmark requires Linux\n";
  return 1;
}

#endif
