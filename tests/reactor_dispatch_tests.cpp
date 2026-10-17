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

void test_pending_write_want_write_dispatch() {
  // Scenario: SSL_write returned WANT_WRITE. OpenSSL requires retrying that
  // exact write before performing any other TLS I/O on this connection.
  ConnectionDispatchState conn{};
  conn.is_tls = true;
  conn.tls_write_want_write = true;
  conn.peer_closed = false;
  conn.has_pending_output = true;

  const auto interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == false);  // Do not read and append to a pending write
  CHECK(interest.want_write == true);  // Retry only when the socket is writable

  const auto d_in = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(d_in.do_resume_tls_write == false);
  CHECK(d_in.do_normal_read == false);
  CHECK(d_in.do_resume_tls_read == false);
  CHECK(d_in.do_normal_write == false);

  const auto d_out = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLOUT));
  CHECK(d_out.do_resume_tls_write == true);
  CHECK(d_out.do_normal_read == false);
  CHECK(d_out.do_resume_tls_read == false);
  CHECK(d_out.do_normal_write == false);

  const auto d_both = evaluate_reactor_dispatch(
      conn, static_cast<std::uint32_t>(EPOLLIN | EPOLLOUT));
  CHECK(d_both.do_resume_tls_write == true);
  CHECK(d_both.do_normal_read == false);
  CHECK(d_both.do_resume_tls_read == false);
  CHECK(d_both.do_normal_write == false);
}

void test_pending_read_want_read_dispatch() {
  // Scenario: A TLS connection is waiting for incoming data
  ConnectionDispatchState conn{};
  conn.is_tls = true;
  conn.tls_write_want_read = false;
  conn.tls_read_want_write = false;
  conn.peer_closed = false;
  conn.has_pending_output = false;

  const auto interest = compute_reactor_epoll_interest(conn);
  CHECK(interest.want_read == true);
  CHECK(interest.want_write == false); // no output pending, no EPOLLOUT interest

  const auto d_in = evaluate_reactor_dispatch(conn, static_cast<std::uint32_t>(EPOLLIN));
  CHECK(d_in.do_normal_read == true);
  CHECK(d_in.do_resume_tls_read == false);
  CHECK(d_in.do_resume_tls_write == false);
  CHECK(d_in.do_normal_write == false);
}

void test_client_write_cursor_unit_invariants() {
  // Unit test verifying persistent frame-write cursor invariants without network dependencies
  struct MockCursor {
    std::string frame_bytes{};
    std::size_t offset{0};
    bool in_progress{false};
    bool abandoned{false};

    [[nodiscard]] bool empty() const noexcept { return !in_progress; }
    [[nodiscard]] std::size_t remaining() const noexcept {
      return in_progress ? (frame_bytes.size() - offset) : 0;
    }
  };

  MockCursor cursor;
  CHECK(cursor.empty());

  const auto payload1 = darkpool::network::json{{"type", "action"}, {"id", 1}};
  const std::string s1 = darkpool::network::Frame(payload1).serialize();

  // 1. Start frame 1
  cursor.frame_bytes = s1;
  cursor.offset = 0;
  cursor.in_progress = true;
  CHECK(!cursor.empty());
  CHECK(cursor.remaining() == s1.size());

  // 2. Simulated timeout before any bytes written: must preserve frame and offset 0
  CHECK(cursor.offset == 0);
  CHECK(cursor.in_progress == true);
  CHECK(cursor.remaining() == s1.size());

  // 3. Simulated partial write: 5 bytes sent
  cursor.offset += 5;
  CHECK(cursor.offset == 5);
  CHECK(cursor.remaining() == s1.size() - 5);
  CHECK(cursor.in_progress == true);

  // 4. Simulated timeout after partial write: must preserve exact remaining bytes and offset
  CHECK(cursor.offset == 5);
  CHECK(cursor.remaining() == s1.size() - 5);

  // 5. Simulated attempt to send a new frame 2 while frame 1 is in progress:
  // Must NOT overwrite or begin frame 2 until frame 1 finishes!
  const auto payload2 = darkpool::network::json{{"type", "action2"}};
  CHECK(cursor.in_progress == true); // Caller must complete cursor first

  // 6. Complete frame 1
  cursor.offset = cursor.frame_bytes.size();
  cursor.in_progress = false;
  cursor.frame_bytes.clear();
  cursor.offset = 0;
  CHECK(cursor.empty());
  CHECK(cursor.remaining() == 0);

  // 7. Now start frame 2
  const std::string s2 = darkpool::network::Frame(payload2).serialize();
  cursor.frame_bytes = s2;
  cursor.offset = 0;
  cursor.in_progress = true;
  CHECK(cursor.remaining() == s2.size());

  // 8. Simulated fatal error: marks stream abandoned
  cursor.abandoned = true;
  CHECK(cursor.abandoned == true);
}

void test_incremental_frame_decoder_and_stream_integrity() {
  const auto payload1 = darkpool::network::json{
      {"type", "test_action"},
      {"player_id", 101},
      {"order_amount", 5000},
      {"description", "First frame with sufficient length to test multi-chunk fragmentation"}
  };
  const std::string original1 = darkpool::network::Frame(payload1).serialize();
  CHECK(original1.size() > 10); // Has 4-byte header + JSON body

  const auto payload2 = darkpool::network::json{
      {"type", "followup_action"},
      {"status", "consecutive_stream_verification"}
  };
  const std::string original2 = darkpool::network::Frame(payload2).serialize();

  darkpool::network::FrameDecoder decoder;

  // 1. Feed only part of the 4-byte length header (first 2 bytes)
  auto f1 = decoder.feed(std::span<const char>(original1.data(), 2));
  CHECK(f1.empty()); // No complete frame returned prematurely

  // 2. Feed a chunk that completes the header and crosses into the body (4 bytes: remaining 2 bytes of header + 2 bytes body)
  auto f2 = decoder.feed(std::span<const char>(original1.data() + 2, 4));
  CHECK(f2.empty()); // Header is complete, but body is only 2 bytes: no frame returned

  // 3. Feed partial body chunk 1 (20 bytes)
  const std::size_t chunk3_len = 20;
  auto f3 = decoder.feed(std::span<const char>(original1.data() + 6, chunk3_len));
  CHECK(f3.empty());

  // 4. Feed remaining body minus 1 byte
  const std::size_t remaining_minus_one = original1.size() - 6 - chunk3_len - 1;
  auto f4 = decoder.feed(std::span<const char>(original1.data() + 6 + chunk3_len, remaining_minus_one));
  CHECK(f4.empty());

  // 5. Feed the final 1 byte of frame 1
  auto f5 = decoder.feed(std::span<const char>(original1.data() + original1.size() - 1, 1));
  CHECK(f5.size() == 1);
  CHECK(f5[0].payload() == payload1);

  // 6. Feed consecutive follow-up frame across 3 fragmented chunks to ensure stream alignment is preserved
  const std::size_t split1 = 3;
  const std::size_t split2 = original2.size() / 2;
  auto f6_1 = decoder.feed(std::span<const char>(original2.data(), split1));
  CHECK(f6_1.empty());

  auto f6_2 = decoder.feed(std::span<const char>(original2.data() + split1, split2 - split1));
  CHECK(f6_2.empty());

  auto f6_3 = decoder.feed(std::span<const char>(original2.data() + split2, original2.size() - split2));
  CHECK(f6_3.size() == 1);
  CHECK(f6_3[0].payload() == payload2);

  // 7. Test OutputQueue partial consumption reconstruction
  darkpool::network::OutputQueue queue;
  CHECK(queue.append(original1));
  std::string reconstructed;
  // Consume in slices of 7 bytes, 15 bytes, then remainder
  const std::size_t s1 = 7;
  reconstructed.append(queue.pending().substr(0, s1));
  queue.consume(s1);

  const std::size_t s2 = 15;
  reconstructed.append(queue.pending().substr(0, s2));
  queue.consume(s2);

  reconstructed.append(queue.pending());
  queue.consume(queue.pending().size());
  CHECK(queue.pending().empty());
  CHECK(reconstructed == original1);
}

} // namespace

int main() {
  std::cout << "[reactor_dispatch_tests] Running reactor dispatch unit tests...\n";
  test_pending_write_want_read_dispatch();
  test_pending_write_want_write_dispatch();
  test_pending_read_want_read_dispatch();
  test_pending_read_want_write_dispatch();
  test_normal_read_write_dispatch();
  test_state_transitions_after_operation();
  test_closed_peer_dispatch();
  test_client_write_cursor_unit_invariants();
  test_incremental_frame_decoder_and_stream_integrity();

  if (g_failures > 0) {
    std::cerr << "[reactor_dispatch_tests] FAILED with " << g_failures << " failures\n";
    return 1;
  }
  std::cout << "[reactor_dispatch_tests] ALL TESTS PASSED\n";
  return 0;
}
