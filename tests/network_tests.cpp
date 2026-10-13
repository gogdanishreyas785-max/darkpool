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

void test_payload_boundary_limits() {
  // Determine JSON formatting overhead for {"k":""}
  const auto overhead = json{{"k", ""}}.dump().size();
  const std::string pad_below(kMaxFramePayloadSize - 1U - overhead, 'x');
  const json payload_below{{"k", pad_below}};
  CHECK(payload_below.dump().size() == kMaxFramePayloadSize - 1U);  // 4,095 bytes

  const std::string pad_exact(kMaxFramePayloadSize - overhead, 'x');
  const json payload_exact{{"k", pad_exact}};
  CHECK(payload_exact.dump().size() == kMaxFramePayloadSize);  // 4,096 bytes

  const std::string pad_above(kMaxFramePayloadSize + 1U - overhead, 'x');
  const json payload_above{{"k", pad_above}};
  CHECK(payload_above.dump().size() == kMaxFramePayloadSize + 1U);  // 4,097 bytes

  // 1. Immediately below limit (4,095 bytes): round-trips through serialize, parse, and decoder
  const Frame frame_below(payload_below);
  const auto bytes_below = frame_below.serialize();
  CHECK(bytes_below.size() == kMaxFramePayloadSize - 1U + 4U);
  std::size_t consumed_below = 0;
  const auto parsed_below = Frame::parse(bytes_below, consumed_below);
  CHECK(parsed_below.has_value() && parsed_below->payload() == payload_below);
  CHECK(consumed_below == bytes_below.size());

  FrameDecoder decoder_below;
  const auto fed_below = decoder_below.feed(std::span<const char>(bytes_below.data(), bytes_below.size()));
  CHECK(fed_below.size() == 1U && fed_below[0].payload() == payload_below);
  CHECK(decoder_below.buffered_bytes() == 0U);

  // 2. Exactly at limit (4,096 bytes): round-trips through serialize, parse, and decoder
  const Frame frame_exact(payload_exact);
  const auto bytes_exact = frame_exact.serialize();
  CHECK(bytes_exact.size() == kMaxFramePayloadSize + 4U);
  std::size_t consumed_exact = 0;
  const auto parsed_exact = Frame::parse(bytes_exact, consumed_exact);
  CHECK(parsed_exact.has_value() && parsed_exact->payload() == payload_exact);
  CHECK(consumed_exact == bytes_exact.size());

  FrameDecoder decoder_exact;
  const auto fed_exact = decoder_exact.feed(std::span<const char>(bytes_exact.data(), bytes_exact.size()));
  CHECK(fed_exact.size() == 1U && fed_exact[0].payload() == payload_exact);
  CHECK(decoder_exact.buffered_bytes() == 0U);

  // 3. Immediately above limit (4,097 bytes): outbound serialization MUST reject
  const Frame frame_above(payload_above);
  bool serialize_above_rejected = false;
  try {
    (void)frame_above.serialize();
  } catch (const std::length_error&) {
    serialize_above_rejected = true;
  }
  CHECK(serialize_above_rejected);

  // 4. Inbound declared length immediately above limit (4,097 bytes) MUST be rejected before accumulating payload
  const std::string header_above{"\0\0\x10\x01", 4U};  // length 4,097
  std::size_t consumed_above = 0;
  bool parse_above_rejected = false;
  try {
    (void)Frame::parse(header_above, consumed_above);
  } catch (const std::length_error&) {
    parse_above_rejected = true;
  }
  CHECK(parse_above_rejected);

  FrameDecoder decoder_above;
  bool feed_above_rejected = false;
  try {
    (void)decoder_above.feed(std::span<const char>(header_above.data(), header_above.size()));
  } catch (const std::length_error&) {
    feed_above_rejected = true;
  }
  CHECK(feed_above_rejected);
}

void test_malformed_and_oversized_frames() {
  FrameDecoder malformed;
  const std::string bad_json{"\0\0\0\1{", 5U};
  bool malformed_rejected = false;
  try { (void)malformed.feed(std::span<const char>(bad_json.data(), bad_json.size())); }
  catch (const std::invalid_argument&) { malformed_rejected = true; }
  CHECK(malformed_rejected);

  // Zero declared length
  const std::array<char, 4> zero{0, 0, 0, 0};
  FrameDecoder empty;
  bool zero_rejected = false;
  try { (void)empty.feed(zero); }
  catch (const std::length_error&) { zero_rejected = true; }
  CHECK(zero_rejected);

  std::size_t consumed_zero = 0;
  bool parse_zero_rejected = false;
  try { (void)Frame::parse(std::string(zero.data(), zero.size()), consumed_zero); }
  catch (const std::length_error&) { parse_zero_rejected = true; }
  CHECK(parse_zero_rejected);

  // Oversized declared lengths: 4,097 (boundary + 1), 65,536, 1,048,577, and 0xFFFFFFFF
  const std::vector<std::string> oversized_headers{
      std::string{"\0\0\x10\x01", 4U},      // 4,097
      std::string{"\0\x01\0\0", 4U},        // 65,536
      std::string{"\0\x10\0\x01", 4U},      // 1,048,577
      std::string{"\xff\xff\xff\xff", 4U},  // 4,294,967,295
  };
  for (const auto& header : oversized_headers) {
    FrameDecoder dec;
    bool dec_rejected = false;
    try { (void)dec.feed(std::span<const char>(header.data(), header.size())); }
    catch (const std::length_error&) { dec_rejected = true; }
    CHECK(dec_rejected);

    std::size_t consumed = 0;
    bool parse_rejected = false;
    try { (void)Frame::parse(header, consumed); }
    catch (const std::length_error&) { parse_rejected = true; }
    CHECK(parse_rejected);
  }
}

void test_partial_output_and_queue_bound() {
  OutputQueue output;
  CHECK(output.size() == 0U);
  CHECK(output.pending().empty());

  CHECK(output.append("abcdef"));
  CHECK(output.pending() == "abcdef");
  output.consume(2U);
  CHECK(output.pending() == "cdef");
  CHECK(output.append("gh"));
  CHECK(output.pending() == "cdefgh");
  output.consume(6U);
  CHECK(output.size() == 0U);
  CHECK(output.pending().empty());

  bool past_end_rejected = false;
  try { output.consume(1U); } catch (const std::out_of_range&) { past_end_rejected = true; }
  CHECK(past_end_rejected);

  // Fill output queue exactly to its limit (64 KiB = 65,536 bytes)
  const std::string full_chunk(kMaxQueuedBytes, 'x');
  CHECK(output.append(full_chunk));
  CHECK(output.size() == kMaxQueuedBytes);
  CHECK(output.pending() == full_chunk);

  // Overflow by one byte must be rejected without altering queue size or content
  CHECK(!output.append("y"));
  CHECK(output.size() == kMaxQueuedBytes);
  CHECK(output.pending() == full_chunk);

  // Repeated attempts to append after overflow must also fail cleanly
  CHECK(!output.append("z"));
  CHECK(!output.append(std::string(100, '!')));
  CHECK(output.size() == kMaxQueuedBytes);
  CHECK(output.pending() == full_chunk);

  // Partial consumption frees space and allows appending up to exact boundary
  output.consume(1U);
  CHECK(output.size() == kMaxQueuedBytes - 1U);
  CHECK(!output.append("ab"));  // 2 bytes would overflow (65,535 + 2 = 65,537 > 65,536)
  CHECK(output.size() == kMaxQueuedBytes - 1U);
  CHECK(output.append("c"));    // 1 byte fits exactly
  CHECK(output.size() == kMaxQueuedBytes);

  // Completely consume and verify empty state
  output.consume(kMaxQueuedBytes);
  CHECK(output.size() == 0U);
  CHECK(output.pending().empty());
}

void test_fragmented_and_coalesced_max_frames() {
  // Fragmented maximum payload frame (4,096 bytes) fed byte-by-byte
  const auto overhead = json{{"k", ""}}.dump().size();
  const std::string pad(kMaxFramePayloadSize - overhead, 'm');
  const auto max_frame = Frame(json{{"k", pad}}).serialize();
  CHECK(max_frame.size() == kMaxFramePayloadSize + 4U);

  FrameStream stream;
  Frame decoded(json{});
  for (std::size_t i = 0; i < max_frame.size(); ++i) {
    stream.feed(std::span<const char>(max_frame.data() + i, 1U));
    if (i + 1U < max_frame.size()) {
      CHECK(!stream.next(decoded));
    }
  }
  CHECK(stream.next(decoded));
  CHECK(decoded.payload()["k"] == pad);
  CHECK(!stream.next(decoded));

  // Coalesced stream: maximum-size frame + small frame in single feed
  const auto small_frame = Frame(json{{"s", 1}}).serialize();
  const std::string coalesced = max_frame + small_frame;
  FrameDecoder decoder;
  const auto frames = decoder.feed(std::span<const char>(coalesced.data(), coalesced.size()));
  CHECK(frames.size() == 2U);
  CHECK(frames[0].payload()["k"] == pad);
  CHECK(frames[1].payload()["s"] == 1);
  CHECK(decoder.buffered_bytes() == 0U);
}

}  // namespace

int main() {
  test_frame_golden_and_round_trip();
  test_fragmented_and_coalesced_stream();
  test_persistent_frame_stream_queue();
  test_payload_boundary_limits();
  test_malformed_and_oversized_frames();
  test_partial_output_and_queue_bound();
  test_fragmented_and_coalesced_max_frames();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "All network protocol tests passed.\n";
  return EXIT_SUCCESS;
}
