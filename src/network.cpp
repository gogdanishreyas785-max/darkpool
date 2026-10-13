#include "darkpool/network.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace darkpool::network {

void Frame::encode_length(std::string& out, std::uint32_t len) {
  out.push_back(static_cast<char>((len >> 24U) & 0xffU));
  out.push_back(static_cast<char>((len >> 16U) & 0xffU));
  out.push_back(static_cast<char>((len >> 8U) & 0xffU));
  out.push_back(static_cast<char>(len & 0xffU));
}

std::optional<std::uint32_t> Frame::decode_length(const std::string& buffer) {
  if (buffer.size() < 4U) return std::nullopt;
  const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[0]));
  const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[1]));
  const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[2]));
  const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer[3]));
  return (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
}

Frame::Frame(const json& payload) : payload_(payload) {}

Frame::Frame(const std::string& json_str) : payload_(json::parse(json_str)) {}

std::string Frame::serialize() const {
  const auto body = payload_.dump();
  if (body.empty() || body.size() > kMaxMessageSize ||
      body.size() > static_cast<std::size_t>(std::numeric_limits<std::uint32_t>::max())) {
    throw std::length_error("JSON frame payload length is outside the allowed range");
  }
  std::string result;
  result.reserve(body.size() + 4U);
  encode_length(result, static_cast<std::uint32_t>(body.size()));
  result.append(body);
  return result;
}

std::optional<Frame> Frame::parse(const std::string& buffer, std::size_t& bytes_consumed) {
  bytes_consumed = 0;
  const auto length = decode_length(buffer);
  if (!length) return std::nullopt;
  if (*length == 0U || *length > kMaxMessageSize) {
    throw std::length_error("invalid JSON frame length");
  }
  const auto frame_size = static_cast<std::size_t>(*length) + 4U;
  if (buffer.size() < frame_size) return std::nullopt;
  try {
    Frame parsed(buffer.substr(4U, static_cast<std::size_t>(*length)));
    bytes_consumed = frame_size;
    return parsed;
  } catch (const json::exception& error) {
    throw std::invalid_argument(std::string("malformed JSON frame: ") + error.what());
  }
}

std::vector<Frame> FrameDecoder::feed(std::span<const char> bytes) {
  std::vector<Frame> frames;
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    if (buffer_.size() < 4U) {
      const auto needed = 4U - buffer_.size();
      const auto count = std::min(needed, bytes.size() - offset);
      buffer_.append(bytes.data() + offset, count);
      offset += count;
      if (buffer_.size() < 4U) break;
    }
    const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[0]));
    const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[1]));
    const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[2]));
    const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(buffer_[3]));
    const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
    if (length == 0U || length > kMaxMessageSize) throw std::length_error("invalid JSON frame length");
    const auto frame_size = static_cast<std::size_t>(length) + 4U;
    const auto needed = frame_size - buffer_.size();
    const auto count = std::min(needed, bytes.size() - offset);
    buffer_.append(bytes.data() + offset, count);
    offset += count;
    if (buffer_.size() < frame_size) break;
    std::size_t consumed = 0;
    auto frame = Frame::parse(buffer_, consumed);
    if (!frame || consumed != frame_size) throw std::logic_error("frame decoder lost stream alignment");
    frames.push_back(std::move(*frame));
    buffer_.clear();
  }
  return frames;
}

void FrameStream::feed(std::span<const char> bytes) {
  auto decoded = decoder_.feed(bytes);
  for (auto& frame : decoded) frames_.push_back(std::move(frame));
}

bool FrameStream::next(Frame& out_frame) {
  if (frames_.empty()) return false;
  out_frame = std::move(frames_.front());
  frames_.pop_front();
  return true;
}

bool OutputQueue::append(const std::string& bytes) {
  if (bytes.size() > kMaxQueuedBytes || size() > kMaxQueuedBytes - bytes.size()) return false;
  if (offset_ != 0U && offset_ >= bytes_.size() / 2U) {
    bytes_.erase(0, offset_);
    offset_ = 0;
  }
  bytes_.append(bytes);
  return true;
}

std::string_view OutputQueue::pending() const noexcept {
  return std::string_view(bytes_).substr(offset_);
}

void OutputQueue::consume(std::size_t bytes) {
  if (bytes > size()) throw std::out_of_range("cannot consume beyond queued output");
  offset_ += bytes;
  if (offset_ == bytes_.size()) {
    bytes_.clear();
    offset_ = 0;
  }
}

MessageType get_message_type(const json& payload) {
  if (!payload.is_object() || !payload.contains("type") || !payload["type"].is_string()) return MessageType::Unknown;
  const auto type = payload["type"].get<std::string>();
  if (type == "login") return MessageType::Login;
  if (type == "client_ready") return MessageType::ClientReady;
  if (type == "submit_action") return MessageType::SubmitAction;
  if (type == "phase_update") return MessageType::PhaseUpdate;
  if (type == "game_state") return MessageType::GameState;
  if (type == "error") return MessageType::Error;
  if (type == "heartbeat") return MessageType::Heartbeat;
  return MessageType::Unknown;
}

Frame make_error_frame(const std::string& detail) { return Frame(json{{"type", "error"}, {"detail", detail}}); }
Frame make_ack_frame(const std::string& message) { return Frame(json{{"type", "ack"}, {"message", message}}); }

}  // namespace darkpool::network
