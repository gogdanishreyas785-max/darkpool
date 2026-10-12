#include "darkpool/cli.hpp"
#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

using darkpool::network::Frame;
using darkpool::network::FrameDecoder;
using darkpool::network::json;

bool write_all(int fd, const std::string& buffer) {
  std::size_t offset = 0;
  while (offset < buffer.size()) {
    const auto sent = send(fd, buffer.data() + offset, buffer.size() - offset, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (sent == 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

bool receive_frame(int fd, Frame& out_frame) {
  FrameDecoder decoder;
  char buffer[4096];
  while (true) {
    const auto count = recv(fd, buffer, sizeof(buffer), 0);
    if (count <= 0) {
      if (count < 0 && errno == EINTR) continue;
      return false;
    }
    const auto frames = decoder.feed(std::span<const char>(buffer, static_cast<std::size_t>(count)));
    if (!frames.empty()) {
      out_frame = frames.front();
      return true;
    }
  }
}

bool parse_port(const char* text, std::uint16_t& port) {
  char* end = nullptr;
  const auto value = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value == 0UL || value > 65535UL) return false;
  port = static_cast<std::uint16_t>(value);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "usage: darkpool_client <host> <port> <player|moderator>\n";
    return 2;
  }

  std::uint16_t port = 0;
  if (!parse_port(argv[2], port)) {
    std::cerr << "error: invalid port: " << argv[2] << '\n';
    return 2;
  }

  if (std::string(argv[3]) != "player" && std::string(argv[3]) != "moderator") {
    std::cerr << "error: role must be 'player' or 'moderator', got: " << argv[3] << '\n';
    return 2;
  }

  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

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

  const darkpool::cli::ClientRole client_role = (std::string(argv[3]) == "moderator")
      ? darkpool::cli::ClientRole::Moderator
      : darkpool::cli::ClientRole::Player;
  darkpool::cli::ClientState state(client_role);

  const auto exchange = [fd, &state](const Frame& outgoing) -> bool {
    if (!write_all(fd, outgoing.serialize())) return false;
    Frame incoming(json{});
    if (!receive_frame(fd, incoming)) return false;
    std::cout << state.format_response(incoming.payload()) << '\n';
    return true;
  };

  json connect_payload{{"type", "connect"}, {"role", argv[3]}};
  if (client_role == darkpool::cli::ClientRole::Moderator) {
    const char* token = std::getenv("DARKPOOL_MODERATOR_TOKEN");
    if (!token || token[0] == '\0') {
      std::cerr << "error: DARKPOOL_MODERATOR_TOKEN environment variable is required for moderator\n";
      (void)close(fd);
      return 1;
    }
    connect_payload["token"] = token;
  }

  if (!exchange(Frame(connect_payload))) {
    std::cerr << "server rejected connection or closed it\n";
    (void)close(fd);
    return 1;
  }

  const bool is_interactive = isatty(fileno(stdin)) != 0;
  if (is_interactive) {
    std::cout << "Type 'help' for commands, 'status' for game state.\n";
  }

  std::string line;
  while (true) {
    if (is_interactive) {
      std::cout << state.prompt() << std::flush;
    }
    if (!std::getline(std::cin, line)) break;

    auto cmd = state.process_input(line);
    while (cmd.prompt_for_input) {
      if (is_interactive) {
        std::cout << *cmd.prompt_for_input << std::flush;
      }
      std::string prompt_response;
      if (!std::getline(std::cin, prompt_response)) {
        cmd = darkpool::cli::CommandResult{};
        break;
      }
      cmd = state.process_prompt_response(prompt_response);
    }

    if (!cmd.local_message.empty()) {
      std::cout << cmd.local_message << '\n';
    }
    if (cmd.should_exit) {
      break;
    }
    if (cmd.send_frame) {
      try {
        if (!exchange(Frame(cmd.payload))) {
          std::cerr << "connection closed while awaiting response\n";
          (void)close(fd);
          return 1;
        }
      } catch (const std::exception& error) {
        std::cerr << "network error: " << error.what() << '\n';
      }
    }
  }

  (void)close(fd);
  return 0;
}
