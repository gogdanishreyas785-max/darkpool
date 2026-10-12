#include "darkpool/network.hpp"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using darkpool::network::Frame;
using darkpool::network::json;

bool parse_port(const char* text, std::uint16_t& port) {
  unsigned int value = 0;
  const std::string input(text);
  const auto result = std::from_chars(input.data(), input.data() + input.size(), value);
  if (result.ec != std::errc{} || result.ptr != input.data() + input.size() || value == 0U || value > 65535U) return false;
  port = static_cast<std::uint16_t>(value);
  return true;
}

bool write_all(int fd, const std::string& bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = send(fd, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
    if (count < 0) { if (errno == EINTR) continue; return false; }
    if (count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

bool read_exact(int fd, char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto count = recv(fd, data + offset, size - offset, 0);
    if (count < 0) { if (errno == EINTR) continue; return false; }
    if (count == 0) return false;
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

bool receive_frame(int fd, Frame& frame) {
  char header[4];
  if (!read_exact(fd, header, sizeof(header))) return false;
  const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
  const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
  const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
  const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
  if (length == 0U || length > darkpool::network::kMaxMessageSize) return false;
  std::string bytes(header, sizeof(header));
  bytes.resize(static_cast<std::size_t>(length) + 4U);
  if (!read_exact(fd, bytes.data() + 4, static_cast<std::size_t>(length))) return false;
  std::size_t consumed = 0;
  auto parsed = Frame::parse(bytes, consumed);
  if (!parsed || consumed != bytes.size()) return false;
  frame = std::move(*parsed);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::uint16_t port = 0;
  if (argc != 4 || !parse_port(argv[2], port) || (std::string(argv[3]) != "player" && std::string(argv[3]) != "moderator")) {
    std::cerr << "usage: darkpool_client <host> <port> <player|moderator>\n"
                 "Enter one JSON value per line; the server echoes each valid message.\n";
    return 2;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* addresses = nullptr;
  const auto service = std::to_string(port);
  const int lookup = getaddrinfo(argv[1], service.c_str(), &hints, &addresses);
  if (lookup != 0) { std::cerr << "getaddrinfo: " << gai_strerror(lookup) << '\n'; return 1; }
  int fd = -1;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd >= 0 && connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
    if (fd >= 0) (void)close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  if (fd < 0) { std::cerr << "connect: " << std::strerror(errno) << '\n'; return 1; }

  const auto exchange = [fd](const Frame& outgoing) {
    if (!write_all(fd, outgoing.serialize())) return false;
    Frame incoming(json{});
    if (!receive_frame(fd, incoming)) return false;
    std::cout << incoming.payload().dump() << '\n';
    return true;
  };
  if (!exchange(Frame(json{{"type", "connect"}, {"role", argv[3]}}))) {
    std::cerr << "server rejected connection or closed it\n"; (void)close(fd); return 1;
  }
  std::string line;
  while (std::getline(std::cin, line)) {
    try {
      if (!exchange(Frame(line))) { std::cerr << "connection closed while awaiting echo\n"; (void)close(fd); return 1; }
    } catch (const std::exception& error) {
      std::cerr << "invalid JSON or oversized message: " << error.what() << '\n';
    }
  }
  (void)close(fd);
  return 0;
}
