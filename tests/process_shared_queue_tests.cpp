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
using darkpool::ipc::QueueSharedHeader;
using darkpool::ipc::QueueMessageSlot;
using darkpool::ipc::kQueueMagic;
using darkpool::ipc::kQueueAbiVersion;

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

// Helper to wait for child with monotonic deadline
bool wait_child_bounded(pid_t pid, int& status, std::chrono::milliseconds timeout = 5000ms) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      return true;
    }
    if (r < 0) {
      return false;
    }
    ::usleep(5000);
  }
  // Timed out: kill and reap
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return false;
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
// Test 2: Validation of ABI, Names, Capacity, Permissions, Roles
// ============================================================================

void test_validation_and_permissions() {
  std::cerr << "[process_shared_queue_tests] running test_validation_and_permissions\n";
  std::string err;

  // Invalid names
  CHECK(!ProcessSharedQueue::create("invalid_no_slash", 10, QueueEndpointRole::Producer, err).has_value());
  CHECK(!ProcessSharedQueue::create("/has/slash", 10, QueueEndpointRole::Producer, err).has_value());
  CHECK(!ProcessSharedQueue::create("/", 10, QueueEndpointRole::Producer, err).has_value());

  // Invalid capacity
  CHECK(!ProcessSharedQueue::create("/dp_test_zero", 0, QueueEndpointRole::Producer, err).has_value());
  CHECK(!ProcessSharedQueue::create("/dp_test_too_large", 1000001, QueueEndpointRole::Producer, err).has_value());

  const std::string shm_name = make_test_shm_name("perm_val");
  ProcessSharedQueue::unlink(shm_name);

  auto prod = ProcessSharedQueue::create(shm_name, 10, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());

  // Producer-only closure enforcement
  auto cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(cons.has_value());
  CHECK(!cons->close(&err));
  CHECK(err.find("Producer") != std::string::npos);

  // Role constraints
  CHECK(cons->push("data") == QueueOpStatus::Invalid);
  std::string out_msg;
  CHECK(prod->pop(out_msg) == QueueOpStatus::Invalid);

  // Oversized message
  std::string huge(300, 'X');
  CHECK(prod->push(huge, 10ms, &err) == QueueOpStatus::Invalid);

  // Zero-sized message
  CHECK(prod->push("", 10ms, &err) == QueueOpStatus::Invalid);

  prod->close();
  cons.reset();
  prod.reset();
  ProcessSharedQueue::unlink(shm_name);

  // Test insecure permissions rejection: create file with 0666 permissions
  const std::string shm_perm_name = make_test_shm_name("insecure_perm");
  ProcessSharedQueue::unlink(shm_perm_name);
  const int fd = ::shm_open(shm_perm_name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0666);
  CHECK(fd >= 0);
  ::fchmod(fd, 0666); // explicitly ensure group/other permissions
  const std::size_t sz = sizeof(QueueSharedHeader) + 10 * sizeof(QueueMessageSlot);
  const int tr_rc = ::ftruncate(fd, static_cast<off_t>(sz));
  CHECK(tr_rc == 0);
  ::close(fd);

  auto insecure_open = ProcessSharedQueue::open_existing(shm_perm_name, QueueEndpointRole::Consumer, err);
  CHECK(!insecure_open.has_value());
  CHECK(err.find("insecure") != std::string::npos || err.find("group/other") != std::string::npos);
  ProcessSharedQueue::unlink(shm_perm_name);
}

// ============================================================================
// Test 3: Metadata Validation (Magic, ABI, struct_size, UID, state, capacity)
// ============================================================================

void test_corrupted_metadata_rejection() {
  std::cerr << "[process_shared_queue_tests] running test_corrupted_metadata_rejection\n";

  auto test_corrupted_header = [](auto mutate_fn, const std::string& expected_err_sub) {
    const std::string shm_name = make_test_shm_name("corrupt");
    ProcessSharedQueue::unlink(shm_name);
    std::string err;
    auto q = ProcessSharedQueue::create(shm_name, 4, QueueEndpointRole::Producer, err);
    CHECK(q.has_value());
    q.reset(); // close handle

    const int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
    CHECK(fd >= 0);
    auto* hdr = reinterpret_cast<QueueSharedHeader*>(
        ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    CHECK(hdr != MAP_FAILED);

    mutate_fn(hdr);

    ::munmap(hdr, sizeof(QueueSharedHeader));
    ::close(fd);

    std::string open_err;
    auto res = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, open_err);
    CHECK(!res.has_value());
    CHECK(open_err.find(expected_err_sub) != std::string::npos);

    ProcessSharedQueue::unlink(shm_name);
  };

  test_corrupted_header([](QueueSharedHeader* h) { h->magic = 0x12345678; }, "magic mismatch");
  test_corrupted_header([](QueueSharedHeader* h) { h->abi_version = 999; }, "ABI version mismatch");
  test_corrupted_header([](QueueSharedHeader* h) { h->struct_size = 42; }, "struct size mismatch");
  test_corrupted_header([](QueueSharedHeader* h) { h->owner_uid = ::geteuid() + 1; }, "owner UID mismatch");
  test_corrupted_header([](QueueSharedHeader* h) { h->capacity = 0; }, "capacity invalid");
  test_corrupted_header([](QueueSharedHeader* h) { h->state = 99; }, "non-connectable state");
}

// ============================================================================
// Test 4: Duplicate Endpoint Binding Rejection from Separate Processes
// ============================================================================

void test_duplicate_endpoint_from_separate_process() {
  std::cerr << "[process_shared_queue_tests] running test_duplicate_endpoint_from_separate_process\n";
  const std::string shm_name = make_test_shm_name("dup_proc");
  ProcessSharedQueue::unlink(shm_name);

  std::string err;
  auto prod = ProcessSharedQueue::create(shm_name, 10, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());
  auto cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(cons.has_value());

  int sync_pipe[2];
  CHECK(::pipe(sync_pipe) == 0);

  // Fork a child process attempting duplicate Producer binding
  const pid_t child_pid = ::fork();
  CHECK(child_pid >= 0);

  if (child_pid == 0) {
    ::close(sync_pipe[0]);
    std::string child_err;
    // Attempt duplicate Producer
    auto dup_prod = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Producer, child_err);
    int code = 0;
    if (!dup_prod.has_value() && child_err.find("already bound") != std::string::npos) {
      code |= 1; // correctly rejected
    }

    // Attempt duplicate Consumer
    auto dup_cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, child_err);
    if (!dup_cons.has_value() && child_err.find("already bound") != std::string::npos) {
      code |= 2; // correctly rejected
    }

    const ssize_t w = ::write(sync_pipe[1], &code, sizeof(code));
    (void)w;
    ::close(sync_pipe[1]);
    ::_exit(code == 3 ? 0 : 1);
  }

  ::close(sync_pipe[1]);
  int child_code = 0;
  const ssize_t r = ::read(sync_pipe[0], &child_code, sizeof(child_code));
  CHECK(r == sizeof(child_code));
  CHECK(child_code == 3);
  ::close(sync_pipe[0]);

  int status = 0;
  CHECK(wait_child_bounded(child_pid, status));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  prod->close();
  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 5: Graceful Producer Closure and Consumer Drain
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
// Test 6: Cross-Process SPSC Streaming & Backpressure (100,000 Messages, fork)
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

  int status = 0;
  CHECK(wait_child_bounded(pid, status, 30s));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 7: Deterministic EOWNERDEAD on New Participant Open
// ============================================================================

void test_deterministic_owner_death_on_open() {
  std::cerr << "[process_shared_queue_tests] running test_deterministic_owner_death_on_open\n";
  const std::string shm_name = make_test_shm_name("eownerdead_open");
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
    auto* hdr = reinterpret_cast<QueueSharedHeader*>(
        ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    ::pthread_mutex_lock(&hdr->mutex);

    char b = 'K';
    const ssize_t w_rc = ::write(sync_pipe[1], &b, 1);
    (void)w_rc;
    ::close(sync_pipe[1]);

    // Die while holding robust mutex!
    ::raise(SIGKILL);
    ::_exit(42);
  }

  ::close(sync_pipe[1]);
  char ack = 0;
  CHECK(::read(sync_pipe[0], &ack, 1) == 1);
  ::close(sync_pipe[0]);

  int status = 0;
  CHECK(wait_child_bounded(pid, status));
  CHECK(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGKILL);

  // Parent opens existing queue as Consumer
  std::string err;
  auto consumer_opt = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(!consumer_opt.has_value());
  CHECK(err.find("Faulted") != std::string::npos || err.find("owner death") != std::string::npos);

  // Directly inspect header to verify state is Faulted
  const int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
  CHECK(fd >= 0);
  auto* hdr = reinterpret_cast<QueueSharedHeader*>(
      ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  CHECK(hdr != MAP_FAILED);
  CHECK(hdr->state == static_cast<std::uint32_t>(QueueState::Faulted));
  ::munmap(hdr, sizeof(QueueSharedHeader));
  ::close(fd);

  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 8: Deterministic EOWNERDEAD on Already-Open Participant
// ============================================================================

void test_owner_death_on_already_open_participant() {
  std::cerr << "[process_shared_queue_tests] running test_owner_death_on_already_open_participant\n";
  const std::string shm_name = make_test_shm_name("eownerdead_active");
  ProcessSharedQueue::unlink(shm_name);

  std::string err;
  auto prod = ProcessSharedQueue::create(shm_name, 4, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());

  int sync_pipe[2];
  CHECK(::pipe(sync_pipe) == 0);

  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    ::close(sync_pipe[0]);
    std::string child_err;
    auto cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, child_err);
    if (!cons.has_value()) {
      ::_exit(1);
    }

    // Access raw header to acquire robust mutex and crash
    const int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
    auto* hdr = reinterpret_cast<QueueSharedHeader*>(
        ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    ::pthread_mutex_lock(&hdr->mutex);

    char b = 'R';
    const ssize_t w_rc = ::write(sync_pipe[1], &b, 1);
    (void)w_rc;
    ::close(sync_pipe[1]);

    ::raise(SIGKILL);
    ::_exit(10);
  }

  ::close(sync_pipe[1]);
  char ack = 0;
  CHECK(::read(sync_pipe[0], &ack, 1) == 1);
  ::close(sync_pipe[0]);

  int status = 0;
  CHECK(wait_child_bounded(pid, status));
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);

  // The producer was already open! A subsequent push must detect EOWNERDEAD and return Faulted
  std::string push_err;
  QueueOpStatus st = prod->push("test_payload", 100ms, &push_err);
  CHECK(st == QueueOpStatus::Faulted);
  CHECK(prod->is_faulted());
  CHECK(prod->state() == QueueState::Faulted);

  // Subsequent operations also return Faulted
  st = prod->push("another", 100ms);
  CHECK(st == QueueOpStatus::Faulted);

  ProcessSharedQueue::unlink(shm_name);
}

// ============================================================================
// Test 9: Synchronized Public Accessors
// ============================================================================

void test_synchronized_accessors() {
  std::cerr << "[process_shared_queue_tests] running test_synchronized_accessors\n";
  const std::string shm_name = make_test_shm_name("sync_access");
  ProcessSharedQueue::unlink(shm_name);

  std::string err;
  auto prod = ProcessSharedQueue::create(shm_name, 5, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());

  CHECK(prod->capacity() == 5);
  CHECK(prod->size() == 0);
  CHECK(prod->empty());
  CHECK(!prod->full());
  CHECK(prod->state() == QueueState::Active);

  CHECK(prod->push("a") == QueueOpStatus::Success);
  CHECK(prod->size() == 1);
  CHECK(!prod->empty());
  CHECK(!prod->full());

  auto snap = prod->snapshot();
  CHECK(snap.size == 1);
  CHECK(snap.capacity == 5);
  CHECK(!snap.empty);
  CHECK(!snap.full);
  CHECK(snap.state == QueueState::Active);

  prod->close();
  CHECK(prod->is_closed());
  CHECK(prod->state() == QueueState::Closed);

  ProcessSharedQueue::unlink(shm_name);
}

} // namespace

int main() {
  test_empty_and_full_timeouts();
  test_validation_and_permissions();
  test_corrupted_metadata_rejection();
  test_duplicate_endpoint_from_separate_process();
  test_graceful_closure_and_drain();
  test_cross_process_streaming();
  test_deterministic_owner_death_on_open();
  test_owner_death_on_already_open_participant();
  test_synchronized_accessors();

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
