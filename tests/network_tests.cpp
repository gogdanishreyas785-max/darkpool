#include "darkpool/network.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace darkpool::network;

namespace {

int failures = 0;
void check(bool condition, const char* expression, int line) {
  if (!condition) { ++failures; std::cerr << "FAIL line " << line << ": " << expression << '\n'; }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

void test_frame_golden_and_round_trip() {
  const Frame frame(json{{"x", 1}});
  const auto bytes = frame.serialize();
  CHECK(bytes.size() >= 4U);
  CHECK(static_cast<unsigned char>(bytes[0]) == 0U);
  CHECK(static_cast<unsigned char>(bytes[1]) == 0U);
  CHECK(static_cast<unsigned char>(bytes[2]) == 0U);
  CHECK(static_cast<unsigned char>(bytes[3]) == 7U);
  std::size_t consumed = 0;
  const auto parsed = Frame::parse(bytes, consumed);
  CHECK(parsed.has_value() && parsed->payload() == frame.payload());
  CHECK(consumed == bytes.size());
}

void test_fragmented_and_coalesced_stream() {
  const auto first = Frame(json{{"n", 1}}).serialize();
  const auto second = Frame(json{{"n", 2}}).serialize();
  FrameDecoder decoder;
  std::vector<Frame> decoded;
  for (const char byte : first) {
    const std::array<char, 1> fragment{byte};
    auto items = decoder.feed(fragment);
    decoded.insert(decoded.end(), items.begin(), items.end());
  }
  CHECK(decoded.size() == 1U && decoded[0].payload()["n"] == 1);
  std::string coalesced = first + second;
  auto items = decoder.feed(std::span<const char>(coalesced.data(), coalesced.size()));
  CHECK(items.size() == 2U);
  CHECK(items[0].payload()["n"] == 1 && items[1].payload()["n"] == 2);
  CHECK(decoder.buffered_bytes() == 0U);
}

void test_persistent_frame_stream_queue() {
  const auto first = Frame(json{{"n", 1}}).serialize();
  const auto second = Frame(json{{"n", 2}}).serialize();
  FrameStream stream;
  Frame decoded(json{});

  // A fragmented frame is retained until its final byte arrives.
  for (std::size_t i = 0; i < first.size(); ++i) {
    stream.feed(std::span<const char>(first.data() + i, 1U));
    if (i + 1U < first.size()) CHECK(!stream.next(decoded));
  }
  CHECK(stream.next(decoded));
  CHECK(decoded.payload()["n"] == 1);
  CHECK(!stream.next(decoded));

  // Both frames are retained when one TCP read contains multiple frames.
  const std::string coalesced = first + second;
  stream.feed(std::span<const char>(coalesced.data(), coalesced.size()));
  CHECK(stream.pending_frames() == 2U);
  CHECK(stream.next(decoded) && decoded.payload()["n"] == 1);
  CHECK(stream.next(decoded) && decoded.payload()["n"] == 2);
  CHECK(!stream.next(decoded));
  CHECK(stream.pending_frames() == 0U);
}

void test_malformed_and_oversized_frames() {
  FrameDecoder malformed;
  const std::string bad_json{"\0\0\0\1{", 5U};
  bool malformed_rejected = false;
  try { (void)malformed.feed(std::span<const char>(bad_json.data(), bad_json.size())); }
  catch (const std::invalid_argument&) { malformed_rejected = true; }
  CHECK(malformed_rejected);

  FrameDecoder oversized;
  const std::array<char, 4> large{static_cast<char>(0x00), static_cast<char>(0x10), static_cast<char>(0x00), static_cast<char>(0x01)};
  bool oversized_rejected = false;
  try { (void)oversized.feed(large); }
  catch (const std::length_error&) { oversized_rejected = true; }
  CHECK(oversized_rejected);

  const std::array<char, 4> zero{0, 0, 0, 0};
  FrameDecoder empty;
  bool zero_rejected = false;
  try { (void)empty.feed(zero); }
  catch (const std::length_error&) { zero_rejected = true; }
  CHECK(zero_rejected);
}

void test_partial_output_and_queue_bound() {
  OutputQueue output;
  CHECK(output.append("abcdef"));
  CHECK(output.pending() == "abcdef");
  output.consume(2U);
  CHECK(output.pending() == "cdef");
  CHECK(output.append("gh"));
  CHECK(output.pending() == "cdefgh");
  output.consume(6U);
  CHECK(output.size() == 0U);
  bool past_end_rejected = false;
  try { output.consume(1U); } catch (const std::out_of_range&) { past_end_rejected = true; }
  CHECK(past_end_rejected);
  CHECK(output.append(std::string(kMaxQueuedBytes, 'x')));
  CHECK(!output.append("x"));
}

}  // namespace

int main() {
  test_frame_golden_and_round_trip();
  test_fragmented_and_coalesced_stream();
  test_persistent_frame_stream_queue();
  test_malformed_and_oversized_frames();
  test_partial_output_and_queue_bound();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "All network protocol tests passed.\n";
  return EXIT_SUCCESS;
}
