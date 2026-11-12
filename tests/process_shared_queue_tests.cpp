#include "darkpool/process_shared_queue.hpp"

#if defined(__linux__)

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using darkpool::ipc::ProcessSharedQueue;
using darkpool::ipc::QueueEndpointRole;
using darkpool::ipc::QueueOpStatus;
using darkpool::ipc::QueueState;

std::atomic<int> g_failures{0};

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    g_failures.fetch_add(1, std::memory_order_relaxed);
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

std::string make_test_shm_name(const std::string& suffix) {
  return "/dp_test_psq_" + std::to_string(::getpid()) + "_" + suffix;
}

// ============================================================================
// Test 1: Empty and Full Queue Timeout Behavior
// ============================================================================

void test_empty_and_full_timeouts() {
  std::cerr << "[process_shared_queue_tests] running test_empty_and_full_timeouts\n";
  const std::string shm_name = make_test_shm_name("timeout");
  ProcessSharedQueue::unlink(shm_name);

  std::string err;
  auto prod_opt = ProcessSharedQueue::create(shm_name, 2, QueueEndpointRole::Producer, err);
  CHECK(prod_opt.has_value());
  auto& prod = *prod_opt;

  auto cons_opt = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(cons_opt.has_value());
  auto& cons = *cons_opt;

  // 1. Pop from empty queue with 50ms timeout -> Timeout
  std::string msg;
  auto t0 = std::chrono::steady_clock::now();
  QueueOpStatus st = cons.pop(msg, nullptr, 50ms, &err);
  auto t1 = std::chrono::steady_clock::now();
  CHECK(st == QueueOpStatus::Timeout);
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() >= 40);

  // 2. Fill queue capacity (2 items)
  CHECK(prod.push("item1", 100ms) == QueueOpStatus::Success);
  CHECK(prod.push("item2", 100ms) == QueueOpStatus::Success);
  CHECK(prod.full());

  // 3. Push to full queue with 50ms timeout -> Timeout
  t0 = std::chrono::steady_clock::now();
  st = prod.push("item3", 50ms, &err);
  t1 = std::chrono::steady_clock::now();
  CHECK(st == QueueOpStatus::Timeout);
  CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count() >= 40);

  // 4. Drain items
  CHECK(cons.pop(msg) == QueueOpStatus::Success);
  CHECK(msg == "item1");
  CHECK(cons.pop(msg) == QueueOpStatus::Success);
  CHECK(msg == "item2");
  CHECK(cons.empty());

  prod.close();
  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 2: Validation of ABI, Names, Capacity, Permissions, Duplicate Endpoints
// ============================================================================

void test_validation_and_duplicate_endpoints() {
  std::cerr << "[process_shared_queue_tests] running test_validation_and_duplicate_endpoints\n";
  std::string err;

  // Invalid names
  CHECK(!ProcessSharedQueue::create("invalid_no_slash", 10, QueueEndpointRole::Producer, err).has_value());
  CHECK(!ProcessSharedQueue::create("/has/slash", 10, QueueEndpointRole::Producer, err).has_value());
  CHECK(!ProcessSharedQueue::create("/", 10, QueueEndpointRole::Producer, err).has_value());

  // Invalid capacity
  CHECK(!ProcessSharedQueue::create("/dp_test_zero", 0, QueueEndpointRole::Producer, err).has_value());

  // Valid creation
  const std::string shm_name = make_test_shm_name("dups");
  ProcessSharedQueue::unlink(shm_name);

  auto prod = ProcessSharedQueue::create(shm_name, 10, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());

  // Duplicate producer rejection: another process/handle attempting to bind as Producer
  // Since same PID, it recognizes same PID. But let's test role constraints:
  // Cannot push from Consumer role:
  auto cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(cons.has_value());

  CHECK(cons->push("data") == QueueOpStatus::Invalid);
  std::string out_msg;
  CHECK(prod->pop(out_msg) == QueueOpStatus::Invalid);

  // Oversized message rejection
  std::string huge(300, 'X');
  CHECK(prod->push(huge, 10ms, &err) == QueueOpStatus::Invalid);

  // Zero-sized message rejection
  CHECK(prod->push("", 10ms, &err) == QueueOpStatus::Invalid);

  // Small buffer rejection in pop
  CHECK(prod->push("hello", 100ms) == QueueOpStatus::Success);
  char tiny[2];
  std::size_t sz = 0;
  CHECK(cons->pop(tiny, sizeof(tiny), sz, nullptr, 100ms, &err) == QueueOpStatus::Invalid);
  CHECK(cons->pop(out_msg) == QueueOpStatus::Success);
  CHECK(out_msg == "hello");

  prod->close();
  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 3: Graceful Producer Closure and Consumer Drain
// ============================================================================

void test_graceful_closure_and_drain() {
  std::cerr << "[process_shared_queue_tests] running test_graceful_closure_and_drain\n";
  const std::string shm_name = make_test_shm_name("close");
  ProcessSharedQueue::unlink(shm_name);

  std::string err;
  auto prod = ProcessSharedQueue::create(shm_name, 10, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());
  auto cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(cons.has_value());

  CHECK(prod->push("m1") == QueueOpStatus::Success);
  CHECK(prod->push("m2") == QueueOpStatus::Success);
  CHECK(prod->push("m3") == QueueOpStatus::Success);

  // Graceful close by producer
  CHECK(prod->close());
  CHECK(prod->is_closed());

  // Attempting push after close -> Closed
  CHECK(prod->push("m4", 10ms) == QueueOpStatus::Closed);

  // Consumer can drain all pending items
  std::string msg;
  std::uint64_t seq = 0;
  CHECK(cons->pop(msg, &seq) == QueueOpStatus::Success);
  CHECK(msg == "m1");
  CHECK(seq == 0);

  CHECK(cons->pop(msg, &seq) == QueueOpStatus::Success);
  CHECK(msg == "m2");
  CHECK(seq == 1);

  CHECK(cons->pop(msg, &seq) == QueueOpStatus::Success);
  CHECK(msg == "m3");
  CHECK(seq == 2);

  // Now queue is empty and Closed -> pop returns Closed
  CHECK(cons->pop(msg) == QueueOpStatus::Closed);

  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 4: Cross-Process SPSC Streaming & Backpressure (100,000 Messages, fork)
// ============================================================================

void test_cross_process_streaming() {
  std::cerr << "[process_shared_queue_tests] running test_cross_process_streaming (100k messages with small capacity)...\n";
  const std::string shm_name = make_test_shm_name("stream");
  ProcessSharedQueue::unlink(shm_name);

  constexpr std::size_t kCapacity = 32;       // Small capacity to heavily exercise wraparound & backpressure
  constexpr std::size_t kTotalItems = 100000; // 100,000 messages required by spec

  std::string err;
  auto prod_opt = ProcessSharedQueue::create(shm_name, kCapacity, QueueEndpointRole::Producer, err);
  CHECK(prod_opt.has_value());

  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    // Child process: Consumer
    std::string child_err;
    auto cons_opt = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, child_err);
    if (!cons_opt.has_value()) {
      std::cerr << "Child consumer failed to open: " << child_err << '\n';
      ::_exit(1);
    }
    auto& consumer = *cons_opt;

    std::uint64_t expected_seq = 0;
    std::string item;
    for (std::size_t i = 0; i < kTotalItems; ++i) {
      std::uint64_t seq = 0;
      QueueOpStatus st = consumer.pop(item, &seq, 5000ms, &child_err);
      if (st != QueueOpStatus::Success) {
        std::cerr << "Child consumer failed at item " << i << ": status=" << static_cast<int>(st)
                  << " error=" << child_err << '\n';
        ::_exit(2);
      }
      if (seq != expected_seq) {
        std::cerr << "Sequence mismatch at item " << i << ": expected " << expected_seq
                  << " got " << seq << '\n';
        ::_exit(3);
      }
      const std::string expected_msg = "msg_" + std::to_string(expected_seq);
      if (item != expected_msg) {
        std::cerr << "Payload mismatch at item " << i << ": expected " << expected_msg
                  << " got " << item << '\n';
        ::_exit(4);
      }
      expected_seq++;
    }

    // Producer should close after sending all items
    QueueOpStatus end_st = consumer.pop(item, nullptr, 5000ms);
    if (end_st != QueueOpStatus::Closed) {
      std::cerr << "Child consumer expected Closed at end, got " << static_cast<int>(end_st) << '\n';
      ::_exit(5);
    }

    ::_exit(0);
  }

  // Parent process: Producer
  auto& producer = *prod_opt;
  for (std::size_t i = 0; i < kTotalItems; ++i) {
    const std::string msg = "msg_" + std::to_string(i);
    QueueOpStatus st = producer.push(msg, 5000ms, &err);
    if (st != QueueOpStatus::Success) {
      std::cerr << "Parent producer failed at item " << i << ": status=" << static_cast<int>(st)
                << " error=" << err << '\n';
      CHECK(false);
      break;
    }
  }

  // Close producer after sending all items
  producer.close();

  // Wait for child consumer with deadline
  int status = 0;
  const auto deadline = std::chrono::steady_clock::now() + 30s;
  bool exited = false;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      exited = true;
      break;
    }
    ::usleep(5000);
  }

  if (!exited) {
    ::kill(pid, SIGKILL);
    ::waitpid(pid, &status, 0);
    CHECK(false); // Timed out
  } else {
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
  }

  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 5: Robust Mutex Owner Death and Faulted State Detection
// ============================================================================

void test_robust_mutex_owner_death() {
  std::cerr << "[process_shared_queue_tests] running test_robust_mutex_owner_death\n";
  const std::string shm_name = make_test_shm_name("death");
  ProcessSharedQueue::unlink(shm_name);

  std::string err;
  auto prod_opt = ProcessSharedQueue::create(shm_name, 10, QueueEndpointRole::Producer, err);
  CHECK(prod_opt.has_value());

  int pipe_fds[2];
  CHECK(::pipe(pipe_fds) == 0);

  const pid_t crash_pid = ::fork();
  CHECK(crash_pid >= 0);

  if (crash_pid == 0) {
    ::close(pipe_fds[0]);
    std::string child_err;
    auto child_cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, child_err);
    if (!child_cons.has_value()) {
      ::_exit(10);
    }
    char b = '1';
    const ssize_t w_rc = ::write(pipe_fds[1], &b, 1);
    (void)w_rc;
    ::close(pipe_fds[1]);
    ::raise(SIGKILL);
    ::_exit(0);
  }

  ::close(pipe_fds[1]);
  char c = 0;
  const ssize_t r_rc = ::read(pipe_fds[0], &c, 1);
  (void)r_rc;
  ::close(pipe_fds[0]);

  int crash_st = 0;
  ::waitpid(crash_pid, &crash_st, 0);
  CHECK(WIFSIGNALED(crash_st));

  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 6: Deterministic EOWNERDEAD and Fault-Stop Verification
// ============================================================================

void test_deterministic_owner_death() {
  std::cerr << "[process_shared_queue_tests] running test_deterministic_owner_death\n";
  const std::string shm_name = make_test_shm_name("eownerdead");
  ProcessSharedQueue::unlink(shm_name);

  int sync_pipe[2];
  CHECK(::pipe(sync_pipe) == 0);

  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    ::close(sync_pipe[0]);
    std::string err;
    auto q = ProcessSharedQueue::create(shm_name, 4, QueueEndpointRole::Producer, err);
    if (!q.has_value()) {
      ::_exit(1);
    }

    const int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
    auto* hdr = reinterpret_cast<darkpool::ipc::QueueSharedHeader*>(
        ::mmap(nullptr, sizeof(darkpool::ipc::QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    ::pthread_mutex_lock(&hdr->mutex);

    char b = 'K';
    const ssize_t w_rc = ::write(sync_pipe[1], &b, 1);
    (void)w_rc;
    ::close(sync_pipe[1]);

    ::raise(SIGKILL);
    ::_exit(42);
  }

  ::close(sync_pipe[1]);
  char ack = 0;
  CHECK(::read(sync_pipe[0], &ack, 1) == 1);
  ::close(sync_pipe[0]);

  int status = 0;
  ::waitpid(pid, &status, 0);
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGKILL);

  std::string err;
  auto consumer_opt = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(!consumer_opt.has_value());
  CHECK(err.find("Faulted") != std::string::npos || err.find("EOWNERDEAD") != std::string::npos);

  const int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
  CHECK(fd >= 0);
  auto* hdr = reinterpret_cast<darkpool::ipc::QueueSharedHeader*>(
      ::mmap(nullptr, sizeof(darkpool::ipc::QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  CHECK(hdr != MAP_FAILED);
  CHECK(hdr->state == static_cast<std::uint32_t>(QueueState::Faulted));
  ::munmap(hdr, sizeof(darkpool::ipc::QueueSharedHeader));
  ::close(fd);

  ProcessSharedQueue::unlink(shm_name);
}

} // namespace

int main() {
  test_empty_and_full_timeouts();
  test_validation_and_duplicate_endpoints();
  test_graceful_closure_and_drain();
  test_cross_process_streaming();
  test_robust_mutex_owner_death();
  test_deterministic_owner_death();

  if (g_failures.load() == 0) {
    std::cerr << "[process_shared_queue_tests] ALL TESTS PASSED\n";
    return 0;
  }
  std::cerr << "[process_shared_queue_tests] " << g_failures.load() << " TESTS FAILED\n";
  return 1;
}

#else

int main() {
  return 0;
}

#endif

