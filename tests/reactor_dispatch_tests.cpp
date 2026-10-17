#include "darkpool/reactor_dispatch.hpp"
#include "darkpool/network.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>
#include <sys/epoll.h>

namespace {

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

using darkpool::transport::ConnectionDispatchState;
using darkpool::transport::evaluate_reactor_dispatch;
using darkpool::transport::compute_reactor_epoll_interest;

void test_pending_write_want_read_dispatch() {
  // Scenario: A TLS connection is trying to write data (e.g. handshake or frame),
  // but OpenSSL returned SSL_ERROR_WANT_READ (e.g. during TLS 1.3 KeyUpdate or rekey).
  ConnectionDispatchState conn{};
  conn.is_tls = true;
  conn.tls_write_want_read = true;
  conn.tls_read_want_write = false;
  conn.peer_closed = false;
  conn.has_pending_output = true;

  // 1. Verify epoll interest computation:
  // Must request read readiness (EPOLLIN) to satisfy the pending write.
  // CRITICAL: Must NOT request write readiness (EPOLLOUT), otherwise in level-triggered epoll
  // the socket will continuously fire EPOLLOUT, spinning at 100% CPU without making progress.
  const auto interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == true);
  CHECK(interest.want_write == false);

  // 2. Event iteration with EPOLLIN:
  // Must retry the pending write operation and SUPPRESS normal reading.
  const auto d_in = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(d_in.do_resume_tls_write == true);
  CHECK(d_in.do_normal_read == false);
  CHECK(d_in.do_resume_tls_read == false);
  CHECK(d_in.do_normal_write == false);

  // 3. Spurious EPOLLOUT event (if stale or simultaneous):
  // Must NOT trigger normal write or resume read.
  const auto d_out = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLOUT));
  CHECK(d_out.do_resume_tls_write == false);
  CHECK(d_out.do_normal_read == false);
  CHECK(d_out.do_resume_tls_read == false);
  CHECK(d_out.do_normal_write == false);

  // 4. Simultaneous EPOLLIN | EPOLLOUT:
  // Must only resume write; normal read and normal write remain suppressed.
  const auto d_both = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT));
  CHECK(d_both.do_resume_tls_write == true);
  CHECK(d_both.do_normal_read == false);
  CHECK(d_both.do_resume_tls_read == false);
  CHECK(d_both.do_normal_write == false);
}

void test_pending_read_want_write_dispatch() {
  // Scenario: A TLS connection is trying to read data,
  // but OpenSSL returned SSL_ERROR_WANT_WRITE.
  ConnectionDispatchState conn{};
  conn.is_tls = true;
  conn.tls_write_want_read = false;
  conn.tls_read_want_write = true;
  conn.peer_closed = false;
  conn.has_pending_output = false;

  // 1. Verify epoll interest computation:
  // Must request write readiness (EPOLLOUT) to satisfy the pending read.
  // Must NOT request read readiness (EPOLLIN).
  const auto interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == false);
  CHECK(interest.want_write == true);

  // 2. Event iteration with EPOLLOUT:
  // Must retry the pending read operation and SUPPRESS normal writing.
  const auto d_out = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLOUT));
  CHECK(d_out.do_resume_tls_read == true);
  CHECK(d_out.do_normal_read == false);
  CHECK(d_out.do_resume_tls_write == false);
  CHECK(d_out.do_normal_write == false);

  // 3. Spurious EPOLLIN event:
  // Must NOT trigger normal read or resume write.
  const auto d_in = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(d_in.do_resume_tls_read == false);
  CHECK(d_in.do_normal_read == false);
  CHECK(d_in.do_resume_tls_write == false);
  CHECK(d_in.do_normal_write == false);

  // 4. Simultaneous EPOLLIN | EPOLLOUT:
  // Must only resume read; normal read and normal write remain suppressed.
  const auto d_both = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT));
  CHECK(d_both.do_resume_tls_read == true);
  CHECK(d_both.do_normal_read == false);
  CHECK(d_both.do_resume_tls_write == false);
  CHECK(d_both.do_normal_write == false);
}

void test_normal_read_write_dispatch() {
  // Normal TLS connection: idle with no pending output
  ConnectionDispatchState conn{};
  conn.is_tls = true;
  conn.tls_write_want_read = false;
  conn.tls_read_want_write = false;
  conn.peer_closed = false;
  conn.has_pending_output = false;

  auto interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == true);
  CHECK(interest.want_write == false);

  auto d = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(d.do_normal_read == true);
  CHECK(d.do_normal_write == false);
  CHECK(d.do_resume_tls_write == false);
  CHECK(d.do_resume_tls_read == false);

  // Normal TLS connection with pending output
  conn.has_pending_output = true;
  interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == true);
  CHECK(interest.want_write == true);

  d = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLOUT));
  CHECK(d.do_normal_read == false);
  CHECK(d.do_normal_write == true);

  d = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT));
  CHECK(d.do_normal_read == true);
  CHECK(d.do_normal_write == true);
}

void test_state_transitions_after_operation() {
  // Simulation of complete cross-direction lifecycle
  ConnectionDispatchState conn{};
  conn.is_tls = true;
  conn.has_pending_output = true;

  // 1. Initial write encountered WANT_READ
  conn.tls_write_want_read = true;
  auto interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == true);
  CHECK(interest.want_write == false); // no busy-spin on EPOLLOUT

  // 2. EPOLLIN arrives -> dispatch resumes write
  auto d = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(d.do_resume_tls_write == true);
  CHECK(d.do_normal_read == false);

  // 3. Resumed write completed successfully: cleared tls_write_want_read and drained output
  conn.tls_write_want_read = false;
  conn.has_pending_output = false;
  interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == true);
  CHECK(interest.want_write == false);

  // 4. Now ordinary reading is restored
  d = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(d.do_normal_read == true);
  CHECK(d.do_resume_tls_write == false);
}

void test_closed_peer_dispatch() {
  // Scenario: Peer closes connection
  ConnectionDispatchState conn{};
  conn.is_tls = true;
  conn.peer_closed = false;
  conn.has_pending_output = true;

  // Event with EPOLLRDHUP
  const auto d = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLRDHUP | EPOLLIN));
  CHECK(d.do_mark_peer_closed == true);

  // Once marked peer_closed:
  conn.peer_closed = true;

  // Interest computation: read interest suppressed
  auto interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == false);
  CHECK(interest.want_write == true); // still writes remaining output

  // When output drained:
  conn.has_pending_output = false;
  interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == false);
  CHECK(interest.want_write == false);
}

void test_frame_integrity_across_retry() {
  // Verify that frame serialization and mock retry logic preserves exact bytes
  const auto payload = darkpool::network::json{{"type", "test_frame"}, {"sequence", 42}};
  const std::string original = darkpool::network::Frame(payload).serialize();

  darkpool::network::OutputQueue queue;
  CHECK(queue.append(original));
  CHECK(queue.pending().size() == original.size());

  // Simulate partial write of first 6 bytes (e.g. 4-byte header + 2 bytes body)
  const std::size_t partial = 6;
  queue.consume(partial);
  CHECK(queue.pending().size() == original.size() - partial);
  CHECK(queue.pending() == original.substr(partial));

  // Resume remaining bytes
  queue.consume(queue.pending().size());
  CHECK(queue.pending().empty());

  // Verify reassembly via FrameDecoder
  darkpool::network::FrameDecoder decoder;
  auto frames = decoder.feed(std::span<const char>(original.data(), original.size()));
  CHECK(frames.size() == 1);
  CHECK(frames[0].payload() == payload);
}

} // namespace

int main() {
  std::cout << "[reactor_dispatch_tests] Running reactor dispatch unit tests...\n";
  test_pending_write_want_read_dispatch();
  test_pending_read_want_write_dispatch();
  test_normal_read_write_dispatch();
  test_state_transitions_after_operation();
  test_closed_peer_dispatch();
  test_frame_integrity_across_retry();

  if (g_failures > 0) {
    std::cerr << "[reactor_dispatch_tests] FAILED with " << g_failures << " failures\n";
    return 1;
  }
  std::cout << "[reactor_dispatch_tests] ALL TESTS PASSED\n";
  return 0;
}
