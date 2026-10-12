#pragma once

#include <cstdint>
#include <string>
#include <optional>
#include <stdexcept>
#include <sstream>
#include <nlohmann/json.hpp>

namespace darkpool::network {

using json = nlohmann::json;

// Maximum message payload size (1 MB for safety)
inline constexpr std::size_t kMaxMessageSize = 1024 * 1024;

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

// Message helper: extract and validate message type from JSON
[[nodiscard]] MessageType get_message_type(const json& payload);

// Message helper: create a simple response frame
[[nodiscard]] Frame make_error_frame(const std::string& error_detail);
[[nodiscard]] Frame make_ack_frame(const std::string& message);

}  // namespace darkpool::network
