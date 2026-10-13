#pragma once

#include <cstdint>
#include <cstddef>
#include <span>
#include <string>
#include <optional>
#include <string_view>
#include <vector>
#include <deque>
#include <stdexcept>
#include <sstream>
#include <nlohmann/json.hpp>

namespace darkpool::network {

using json = nlohmann::json;

// Maximum message payload size (1 MB for safety)
inline constexpr std::size_t kMaxMessageSize = 1024 * 1024;
inline constexpr std::size_t kMaxQueuedBytes = 2 * kMaxMessageSize;

// Message types
enum class MessageType {
  Login,
  ClientReady,
  SubmitAction,
  PhaseUpdate,
  GameState,
  Error,
  Heartbeat,
  Unknown
};

// Frame structure: 4-byte big-endian length + JSON payload
class Frame {
 public:
  explicit Frame(const json& payload);
  explicit Frame(const std::string& json_str);

  // Serialize to bytes: length prefix + JSON payload
  [[nodiscard]] std::string serialize() const;

  // Parse frame from a buffer; returns the frame if complete, nullopt if incomplete.
  // Updates 'bytes_consumed' on success.
  [[nodiscard]] static std::optional<Frame> parse(
      const std::string& buffer,
      std::size_t& bytes_consumed);

  [[nodiscard]] const json& payload() const { return payload_; }

 private:
  json payload_;

  // Helper: write 4-byte big-endian length to buffer
  static void encode_length(std::string& out, std::uint32_t len);

  // Helper: read 4-byte big-endian length from buffer
  static std::optional<std::uint32_t> decode_length(const std::string& buffer);
};

// Incremental stream decoder. feed() accepts arbitrary TCP read boundaries and
// may return multiple complete frames. Invalid JSON and invalid lengths throw.
class FrameDecoder {
 public:
  [[nodiscard]] std::vector<Frame> feed(std::span<const char> bytes);
  [[nodiscard]] std::size_t buffered_bytes() const noexcept { return buffer_.size(); }
 private:
  std::string buffer_;
};

// Maintains decoded frames across TCP reads, including multiple frames returned
// by one read. The caller owns one FrameStream per connection.
class FrameStream {
 public:
  void feed(std::span<const char> bytes);
  [[nodiscard]] bool next(Frame& out_frame);
  [[nodiscard]] std::size_t pending_frames() const noexcept { return frames_.size(); }
 private:
  FrameDecoder decoder_;
  std::deque<Frame> frames_;
};

// Bounded FIFO for nonblocking socket writes. The caller consumes exactly the
// number of bytes accepted by send(), allowing partial writes without copying.
class OutputQueue {
 public:
  [[nodiscard]] bool append(const std::string& bytes);
  [[nodiscard]] std::string_view pending() const noexcept;
  void consume(std::size_t bytes);
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size() - offset_; }
 private:
  std::string bytes_;
  std::size_t offset_{0};
};

// Message helper: extract and validate message type from JSON
[[nodiscard]] MessageType get_message_type(const json& payload);

// Message helper: create a simple response frame
[[nodiscard]] Frame make_error_frame(const std::string& error_detail);
[[nodiscard]] Frame make_ack_frame(const std::string& message);

}  // namespace darkpool::network
