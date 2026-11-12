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

  // Invalid role enum cast rejection
  CHECK(!ProcessSharedQueue::create("/dp_test_inv_role", 10, static_cast<QueueEndpointRole>(99), err).has_value());
  CHECK(err.find("Invalid QueueEndpointRole") != std::string::npos);
  CHECK(!ProcessSharedQueue::open_existing("/dp_test_inv_role", static_cast<QueueEndpointRole>(99), err).has_value());
  CHECK(err.find("Invalid QueueEndpointRole") != std::string::npos);

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
// Test 10: Deterministic Publication Protocol and Timeout Seam
// ============================================================================

void test_concurrent_create_and_open() {
  std::cerr << "[process_shared_queue_tests] running test_concurrent_create_and_open\n";
  const std::string shm_name = make_test_shm_name("concurrent_pub");
  ProcessSharedQueue::unlink(shm_name);

  // Synchronization pipes for deterministic staging:
  // p1: Creator tells child it has entered pre_init_sync_hook (memory mapped/sized, but mutex/cond/init unperformed)
  // p2: Child tells creator it verified incomplete object rejection, creator may now proceed
  int p1[2];
  int p2[2];
  CHECK(::pipe(p1) == 0);
  CHECK(::pipe(p2) == 0);

  const pid_t pid = ::fork();
  CHECK(pid >= 0);

  if (pid == 0) {
    ::close(p1[1]);
    ::close(p2[0]);

    // Wait until creator reaches the pre-init seam
    char token = 0;
    CHECK(::read(p1[0], &token, 1) == 1);
    ::close(p1[0]);

    // 1. Verify opener running while creator is paused does NOT accept incomplete object
    // (Testing with a very short timeout of 50ms so it fails fast and bounded)
    std::string child_err;
    auto incomplete_opt = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, child_err, 50ms);
    if (incomplete_opt.has_value()) {
      std::cerr << "Child consumer unexpectedly accepted uninitialized shared memory!\n";
      ::_exit(1);
    }
    if (child_err.find("not ready") == std::string::npos && child_err.find("magic mismatch") == std::string::npos) {
      std::cerr << "Child error did not indicate unready state: " << child_err << '\n';
      ::_exit(2);
    }

    // 2. Signal creator that inspection is complete and creator may proceed with initialization
    char ack = 'R';
    const ssize_t w_rc = ::write(p2[1], &ack, 1);
    (void)w_rc;
    ::close(p2[1]);

    // 3. Opener now waits for full publication
    auto cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, child_err, 5000ms);
    if (!cons.has_value()) {
      std::cerr << "Child consumer failed to open after publication: " << child_err << '\n';
      ::_exit(3);
    }

    std::string msg;
    QueueOpStatus st = cons->pop(msg, nullptr, 5000ms, &child_err);
    if (st != QueueOpStatus::Success || msg != "publication_handshake_ok") {
      std::cerr << "Child pop failed: status=" << static_cast<int>(st) << " err=" << child_err << '\n';
      ::_exit(4);
    }

    ::_exit(0);
  }

  // Parent (Creator)
  ::close(p1[0]);
  ::close(p2[1]);

  auto pre_init_seam = [&]() {
    // Notify child that memory is mapped/sized but uninitialized
    char t = 'S';
    const ssize_t w = ::write(p1[1], &t, 1);
    (void)w;
    ::close(p1[1]);

    // Wait for child to complete its verification of incomplete state
    char ack = 0;
    CHECK(::read(p2[0], &ack, 1) == 1);
    ::close(p2[0]);
  };

  std::string err;
  auto prod = ProcessSharedQueue::create(shm_name, 4, QueueEndpointRole::Producer, err, pre_init_seam);
  CHECK(prod.has_value());

  CHECK(prod->push("publication_handshake_ok", 5000ms, &err) == QueueOpStatus::Success);

  int status = 0;
  CHECK(wait_child_bounded(pid, status, 10s));
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);

  ProcessSharedQueue::unlink(shm_name);

  // 4. Test bounded failure when initialization never completes (creator crashes before publication)
  const std::string shm_unfin = make_test_shm_name("unfin_pub");
  ProcessSharedQueue::unlink(shm_unfin);

  int crash_pipe[2];
  CHECK(::pipe(crash_pipe) == 0);
  pid_t crash_pid = ::fork();
  CHECK(crash_pid >= 0);

  if (crash_pid == 0) {
    ::close(crash_pipe[0]);
    // Create shm file, ftruncate and mmap, but exit before publishing init_done or magic
    const int fd = ::shm_open(shm_unfin.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    const std::size_t sz = sizeof(QueueSharedHeader) + 4 * sizeof(QueueMessageSlot);
    const int tr = ::ftruncate(fd, static_cast<off_t>(sz));
    (void)tr;
    char c = 'C';
    const ssize_t w = ::write(crash_pipe[1], &c, 1);
    (void)w;
    ::close(crash_pipe[1]);
    ::close(fd);
    ::_exit(0); // Exited without publishing
  }

  ::close(crash_pipe[1]);
  char c_ack = 0;
  CHECK(::read(crash_pipe[0], &c_ack, 1) == 1);
  ::close(crash_pipe[0]);
  int crash_st = 0;
  CHECK(wait_child_bounded(crash_pid, crash_st));

  // Opener must fail safely within bounded timeout
  std::string timeout_err;
  const auto t0 = std::chrono::steady_clock::now();
  auto failed_open = ProcessSharedQueue::open_existing(shm_unfin, QueueEndpointRole::Consumer, timeout_err, 100ms);
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0);
  CHECK(!failed_open.has_value());
  CHECK(timeout_err.find("not ready") != std::string::npos || timeout_err.find("magic mismatch") != std::string::npos);
  CHECK(elapsed.count() >= 90);

  ProcessSharedQueue::unlink(shm_unfin);
}

// ============================================================================
// Test 11: Slot Integrity and Sequence Verification in pop()
// ============================================================================

void test_slot_integrity_rejection() {
  std::cerr << "[process_shared_queue_tests] running test_slot_integrity_rejection\n";
  const std::string shm_name = make_test_shm_name("slot_integrity");
  ProcessSharedQueue::unlink(shm_name);

  std::string err;
  auto prod = ProcessSharedQueue::create(shm_name, 4, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());
  auto cons = ProcessSharedQueue::open_existing(shm_name, QueueEndpointRole::Consumer, err);
  CHECK(cons.has_value());

  CHECK(prod->push("msg1") == QueueOpStatus::Success);

  // Directly tamper with slot sequence in shared memory
  const int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
  CHECK(fd >= 0);
  std::size_t total_size = sizeof(QueueSharedHeader) + 4 * sizeof(QueueMessageSlot);
  auto* base = reinterpret_cast<std::uint8_t*>(
      ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
  CHECK(base != MAP_FAILED);

  auto* slots = reinterpret_cast<QueueMessageSlot*>(base + sizeof(QueueSharedHeader));

  // Case A: Corrupt sequence
  slots[0].sequence = 999; // Expected 0

  std::string pop_msg;
  std::string pop_err;
  QueueOpStatus st = cons->pop(pop_msg, nullptr, 100ms, &pop_err);
  CHECK(st == QueueOpStatus::Faulted);
  CHECK(pop_err.find("Slot corruption") != std::string::npos);
  CHECK(cons->is_faulted());
  CHECK(prod->is_faulted());

  ::munmap(base, total_size);
  ::close(fd);
  ProcessSharedQueue::unlink(shm_name);

  // Case B: Corrupt slot.size = 0
  const std::string shm_name2 = make_test_shm_name("slot_integrity_sz");
  ProcessSharedQueue::unlink(shm_name2);
  prod = ProcessSharedQueue::create(shm_name2, 4, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());
  cons = ProcessSharedQueue::open_existing(shm_name2, QueueEndpointRole::Consumer, err);
  CHECK(cons.has_value());

  CHECK(prod->push("msg2") == QueueOpStatus::Success);

  const int fd2 = ::shm_open(shm_name2.c_str(), O_RDWR, 0);
  CHECK(fd2 >= 0);
  base = reinterpret_cast<std::uint8_t*>(
      ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd2, 0));
  CHECK(base != MAP_FAILED);
  slots = reinterpret_cast<QueueMessageSlot*>(base + sizeof(QueueSharedHeader));
  slots[0].size = 0; // Invalid slot size

  pop_err.clear();
  st = cons->pop(pop_msg, nullptr, 100ms, &pop_err);
  CHECK(st == QueueOpStatus::Faulted);
  CHECK(pop_err.find("Slot corruption") != std::string::npos);
  CHECK(cons->is_faulted());

  ::munmap(base, total_size);
  ::close(fd2);
  ProcessSharedQueue::unlink(shm_name2);

  // Case C: Corrupt slot.size > kMaxMessageDataSize (e.g. kMaxMessageDataSize + 1)
  const std::string shm_name3 = make_test_shm_name("slot_integrity_oversz");
  ProcessSharedQueue::unlink(shm_name3);
  prod = ProcessSharedQueue::create(shm_name3, 4, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());
  cons = ProcessSharedQueue::open_existing(shm_name3, QueueEndpointRole::Consumer, err);
  CHECK(cons.has_value());

  CHECK(prod->push("msg3") == QueueOpStatus::Success);

  const int fd3 = ::shm_open(shm_name3.c_str(), O_RDWR, 0);
  CHECK(fd3 >= 0);
  base = reinterpret_cast<std::uint8_t*>(
      ::mmap(nullptr, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd3, 0));
  CHECK(base != MAP_FAILED);
  slots = reinterpret_cast<QueueMessageSlot*>(base + sizeof(QueueSharedHeader));
  slots[0].size = static_cast<std::uint32_t>(darkpool::ipc::kMaxMessageDataSize + 1); // Oversized slot size

  pop_err.clear();
  char oversized_buf[darkpool::ipc::kMaxMessageDataSize * 2];
  std::size_t out_sz = 0;
  st = cons->pop(oversized_buf, sizeof(oversized_buf), out_sz, nullptr, 100ms, &pop_err);
  CHECK(st == QueueOpStatus::Faulted);
  CHECK(pop_err.find("Slot corruption") != std::string::npos);
  CHECK(out_sz == 0);
  CHECK(cons->is_faulted());

  ::munmap(base, total_size);
  ::close(fd3);
  ProcessSharedQueue::unlink(shm_name3);
}

// ============================================================================
// Test 12: Deterministic ENOTRECOVERABLE Handling & Condition-Wait Owner Death
// ============================================================================

void test_deterministic_enotrecoverable_and_condition_wait_death() {
  std::cerr << "[process_shared_queue_tests] running test_deterministic_enotrecoverable_and_condition_wait_death\n";

  // --------------------------------------------------------------------------
  // Part 1: Condition-wait owner death
  // A participant blocks inside a real queue condition wait (pop() on empty queue).
  // A narrowly scoped test seam triggers right before pthread_cond_timedwait() while
  // the waiter still owns the mutex, signaling the test controller.
  // The killer process attempts to acquire the robust mutex; its successful acquisition
  // PROVES the waiter entered the condition wait and released the mutex.
  // The killer then dies with SIGKILL.
  // The waiting participant wakes with EOWNERDEAD, transitions the queue to Faulted,
  // makes the mutex consistent, broadcasts, and returns QueueOpStatus::Faulted.
  // --------------------------------------------------------------------------
  const std::string shm_cond_death = make_test_shm_name("cond_owner_death");
  ProcessSharedQueue::unlink(shm_cond_death);

  std::string err;
  auto prod = ProcessSharedQueue::create(shm_cond_death, 4, QueueEndpointRole::Producer, err);
  CHECK(prod.has_value());

  int sync_pipe_waiter[2];
  CHECK(::pipe(sync_pipe_waiter) == 0);

  // Child 1: opens consumer and enters pop(..., timeout=5000ms).
  // Uses pre_cond_wait_hook to notify parent immediately before pthread_cond_timedwait
  pid_t pid_waiter = ::fork();
  CHECK(pid_waiter >= 0);
  if (pid_waiter == 0) {
    ::close(sync_pipe_waiter[0]);
    std::string c_err;
    auto cons = ProcessSharedQueue::open_existing(shm_cond_death, QueueEndpointRole::Consumer, c_err);
    if (!cons.has_value()) {
      std::cerr << "Child waiter failed to open: " << c_err << '\n';
      ::_exit(1);
    }

    auto cond_wait_seam = [&]() {
      // Waiter still owns the mutex here; immediately after this return it calls pthread_cond_timedwait
      char ready = 'W';
      const ssize_t w = ::write(sync_pipe_waiter[1], &ready, 1);
      (void)w;
      ::close(sync_pipe_waiter[1]);
    };

    // Blocks in pthread_cond_timedwait(&header_->not_empty_cond, &header_->mutex, ...)
    std::string msg;
    QueueOpStatus st = cons->pop(msg, nullptr, 5000ms, &c_err, cond_wait_seam);
    if (st != QueueOpStatus::Faulted) {
      std::cerr << "Child waiter expected Faulted from owner death, got " << static_cast<int>(st)
                << " err=" << c_err << '\n';
      ::_exit(2);
    }
    if (!cons->is_faulted()) {
      std::cerr << "Child waiter is_faulted() returned false\n";
      ::_exit(3);
    }
    ::_exit(0);
  }

  ::close(sync_pipe_waiter[1]);
  char w_ack = 0;
  // Wait until waiter has entered pop() and is at the condition wait seam
  CHECK(::read(sync_pipe_waiter[0], &w_ack, 1) == 1);
  ::close(sync_pipe_waiter[0]);

  // Child 2 (Killer): acquires robust mutex and signals successful lock acquisition BEFORE dying.
  // Successful acquisition guarantees the waiter has released the mutex into pthread_cond_timedwait!
  int sync_pipe_killer[2];
  CHECK(::pipe(sync_pipe_killer) == 0);
  pid_t pid_killer = ::fork();
  CHECK(pid_killer >= 0);
  if (pid_killer == 0) {
    ::close(sync_pipe_killer[0]);
    const int fd = ::shm_open(shm_cond_death.c_str(), O_RDWR, 0);
    auto* hdr = reinterpret_cast<QueueSharedHeader*>(
        ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));

    // Acquires mutex (blocking until waiter releases it inside pthread_cond_timedwait)
    int l_rc = ::pthread_mutex_lock(&hdr->mutex);
    if (l_rc != 0) {
      std::cerr << "Killer failed to acquire mutex: " << l_rc << '\n';
      ::_exit(10);
    }

    // Signal controller that mutex has been acquired
    char k = 'K';
    const ssize_t w = ::write(sync_pipe_killer[1], &k, 1);
    (void)w;
    ::close(sync_pipe_killer[1]);

    // Die while holding robust mutex!
    ::raise(SIGKILL);
    ::_exit(42);
  }

  ::close(sync_pipe_killer[1]);
  char k_ack = 0;
  CHECK(::read(sync_pipe_killer[0], &k_ack, 1) == 1);
  ::close(sync_pipe_killer[0]);

  int st_killer = 0;
  CHECK(wait_child_bounded(pid_killer, st_killer));
  CHECK(WIFSIGNALED(st_killer) && WTERMSIG(st_killer) == SIGKILL);

  // The condition waiter in child 1 must be awakened by EOWNERDEAD, recover consistency, and exit 0.
  int st_waiter = 0;
  CHECK(wait_child_bounded(pid_waiter, st_waiter, 10s));
  CHECK(WIFEXITED(st_waiter) && WEXITSTATUS(st_waiter) == 0);

  // Producer handle in parent must also observe Faulted
  CHECK(prod->is_faulted());
  CHECK(prod->state() == QueueState::Faulted);

  ProcessSharedQueue::unlink(shm_cond_death);

  // --------------------------------------------------------------------------
  // Part 2: Complete ENOTRECOVERABLE coverage
  // Open both producer and consumer handles BEFORE deliberately making the mutex ENOTRECOVERABLE.
  // Verify producer push(), consumer pop(), producer close(), snapshot(), and new open_existing()
  // all fail safely without attempting to unlock an unowned mutex.
  // --------------------------------------------------------------------------
  const std::string shm_enot = make_test_shm_name("enotrec_test");
  ProcessSharedQueue::unlink(shm_enot);

  auto prod_holder = ProcessSharedQueue::create(shm_enot, 4, QueueEndpointRole::Producer, err);
  CHECK(prod_holder.has_value());

  auto cons_holder = ProcessSharedQueue::open_existing(shm_enot, QueueEndpointRole::Consumer, err);
  CHECK(cons_holder.has_value());

  // Step A: Child locks mutex and dies with SIGKILL
  int sync_pipe1[2];
  CHECK(::pipe(sync_pipe1) == 0);
  pid_t p1 = ::fork();
  CHECK(p1 >= 0);
  if (p1 == 0) {
    ::close(sync_pipe1[0]);
    const int fd = ::shm_open(shm_enot.c_str(), O_RDWR, 0);
    auto* hdr = reinterpret_cast<QueueSharedHeader*>(
        ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    ::pthread_mutex_lock(&hdr->mutex);

    char b = '1';
    const ssize_t w = ::write(sync_pipe1[1], &b, 1);
    (void)w;
    ::close(sync_pipe1[1]);
    ::raise(SIGKILL);
    ::_exit(1);
  }

  ::close(sync_pipe1[1]);
  char ack = 0;
  CHECK(::read(sync_pipe1[0], &ack, 1) == 1);
  ::close(sync_pipe1[0]);
  int st1 = 0;
  CHECK(wait_child_bounded(p1, st1));

  // Step B: Child locks mutex (gets EOWNERDEAD), DOES NOT call pthread_mutex_consistent(), and unlocks!
  int sync_pipe2[2];
  CHECK(::pipe(sync_pipe2) == 0);
  pid_t p2 = ::fork();
  CHECK(p2 >= 0);
  if (p2 == 0) {
    ::close(sync_pipe2[0]);
    const int fd = ::shm_open(shm_enot.c_str(), O_RDWR, 0);
    auto* hdr = reinterpret_cast<QueueSharedHeader*>(
        ::mmap(nullptr, sizeof(QueueSharedHeader), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));

    int l_rc = ::pthread_mutex_lock(&hdr->mutex);
    if (l_rc != EOWNERDEAD) {
      std::cerr << "Child 2 expected EOWNERDEAD, got " << l_rc << '\n';
      ::_exit(2);
    }
    // Deliberately do NOT call pthread_mutex_consistent()!
    // Unlocking an inconsistent robust mutex puts it permanently in ENOTRECOVERABLE state.
    ::pthread_mutex_unlock(&hdr->mutex);

    char b = '2';
    const ssize_t w = ::write(sync_pipe2[1], &b, 1);
    (void)w;
    ::close(sync_pipe2[1]);
    ::_exit(0);
  }

  ::close(sync_pipe2[1]);
  CHECK(::read(sync_pipe2[0], &ack, 1) == 1);
  ::close(sync_pipe2[0]);
  int st2 = 0;
  CHECK(wait_child_bounded(p2, st2));
  CHECK(WIFEXITED(st2) && WEXITSTATUS(st2) == 0);

  // Now, the mutex in shm_enot is ENOTRECOVERABLE!
  // 1. Verify already-open producer push() returns Faulted safely
  std::string op_err;
  QueueOpStatus push_st = prod_holder->push("should_fail", 100ms, &op_err);
  CHECK(push_st == QueueOpStatus::Faulted);
  CHECK(op_err.find("ENOTRECOVERABLE") != std::string::npos);

  // 2. Verify already-open consumer pop() returns Faulted safely
  std::string pop_msg;
  std::string pop_op_err;
  QueueOpStatus pop_st = cons_holder->pop(pop_msg, nullptr, 100ms, &pop_op_err);
  CHECK(pop_st == QueueOpStatus::Faulted);
  CHECK(pop_op_err.find("ENOTRECOVERABLE") != std::string::npos);

  // 3. Verify already-open producer close() fails safely
  std::string close_err;
  CHECK(!prod_holder->close(&close_err));

  // 4. Verify snapshot() reports Faulted safely
  auto snap_prod = prod_holder->snapshot();
  CHECK(snap_prod.is_faulted);
  CHECK(snap_prod.state == QueueState::Faulted);

  auto snap_cons = cons_holder->snapshot();
  CHECK(snap_cons.is_faulted);
  CHECK(snap_cons.state == QueueState::Faulted);

  // 5. Verify new open_existing() fails safely
  std::string open_err;
  auto cons_fail = ProcessSharedQueue::open_existing(shm_enot, QueueEndpointRole::Consumer, open_err);
  CHECK(!cons_fail.has_value());
  CHECK(open_err.find("ENOTRECOVERABLE") != std::string::npos || open_err.find("Failed to acquire") != std::string::npos);

  ProcessSharedQueue::unlink(shm_enot);
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
  test_concurrent_create_and_open();
  test_slot_integrity_rejection();
  test_deterministic_enotrecoverable_and_condition_wait_death();

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
