#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::json;

int failures = 0;
void check(bool condition, const char* expression, int line) {
  if (!condition) { ++failures; std::cerr << "FAIL line " << line << ": " << expression << '\n'; }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

bool send_all(int fd, const char* data, std::size_t size, bool fragment) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto request = fragment ? 1U : size - offset;
    const auto sent = send(fd, data + offset, request, MSG_NOSIGNAL);
    if (sent < 0) { if (errno == EINTR) continue; return false; }
    if (sent == 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

bool receive_exact(int fd, char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto received = recv(fd, data + offset, size - offset, 0);
    if (received < 0) { if (errno == EINTR) continue; return false; }
    if (received == 0) return false;
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

bool receive_echo(int fd, const json& payload) {
  char header[4];
  if (!receive_exact(fd, header, sizeof(header))) return false;
  const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
  const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
  const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
  const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
  if (length == 0U || length > darkpool::network::kMaxMessageSize) return false;
  std::string bytes(header, sizeof(header));
  bytes.resize(static_cast<std::size_t>(length) + sizeof(header));
  if (!receive_exact(fd, bytes.data() + sizeof(header), static_cast<std::size_t>(length))) return false;
  std::size_t consumed = 0;
  const auto echoed = Frame::parse(bytes, consumed);
  return echoed && consumed == bytes.size() && echoed->payload() == payload;
}

bool exchange(int fd, const json& payload, bool fragment) {
  const auto outgoing = Frame(payload).serialize();
  return send_all(fd, outgoing.data(), outgoing.size(), fragment) && receive_echo(fd, payload);
}

int connect_to(std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
  (void)close(fd);
  return -1;
}

bool rejected_by_server(int fd) {
  timeval timeout{1, 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char byte = 0;
  const auto received = recv(fd, &byte, 1U, 0);
  (void)close(fd);
  return received == 0 || (received < 0 && (errno == ECONNRESET || errno == EPIPE));
}

void test_server_capacity_and_echo(const char* server_path) {
  const int probe = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(probe >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  CHECK(bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
  socklen_t address_length = sizeof(address);
  CHECK(getsockname(probe, reinterpret_cast<sockaddr*>(&address), &address_length) == 0);
  const auto port = ntohs(address.sin_port);
  (void)close(probe);

  const auto child = fork();
  CHECK(child >= 0);
  if (child < 0) return;
  if (child == 0) {
    const auto port_text = std::to_string(port);
    execl(server_path, server_path, port_text.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  std::vector<int> peers;
  int moderator = -1;
  int duplicate_moderator = -1;
  bool startup = false;
  for (int attempt = 0; attempt < 80; ++attempt) {
    const int fd = connect_to(port);
    if (fd >= 0) { peers.push_back(fd); startup = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  CHECK(startup);
  if (!startup) { (void)kill(child, SIGTERM); (void)waitpid(child, nullptr, 0); return; }

  CHECK(exchange(peers[0], json{{"type", "connect"}, {"role", "player"}}, true));
  CHECK(exchange(peers[0], json{{"echo", "fragmented hello"}}, true));

  const int half_closed = peers[0];
  const json last_message{{"half_close", "echo before eof"}};
  const auto final_frame = Frame(last_message).serialize();
  CHECK(send_all(half_closed, final_frame.data(), final_frame.size(), false));
  CHECK(shutdown(half_closed, SHUT_WR) == 0);
  CHECK(receive_echo(half_closed, last_message));
  char trailing = 0;
  CHECK(recv(half_closed, &trailing, 1U, 0) == 0);
  (void)close(half_closed);
  peers.erase(peers.begin());

  for (int i = 0; i < 5; ++i) {
    const int fd = connect_to(port);
    CHECK(fd >= 0);
    if (fd < 0) continue;
    peers.push_back(fd);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
  }
  moderator = connect_to(port);
  CHECK(moderator >= 0);
  if (moderator >= 0) CHECK(exchange(moderator, json{{"type", "connect"}, {"role", "moderator"}}, false));

  const int excess = connect_to(port);
  CHECK(excess >= 0);
  if (excess >= 0) CHECK(rejected_by_server(excess));

  if (!peers.empty()) { (void)close(peers.back()); peers.pop_back(); }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  duplicate_moderator = connect_to(port);
  CHECK(duplicate_moderator >= 0);
  if (duplicate_moderator >= 0) {
    const auto hello = Frame(json{{"type", "connect"}, {"role", "moderator"}}).serialize();
    CHECK(send_all(duplicate_moderator, hello.data(), hello.size(), false));
    CHECK(rejected_by_server(duplicate_moderator));
  }

  const int replacement = connect_to(port);
  CHECK(replacement >= 0);
  if (replacement >= 0) {
    CHECK(exchange(replacement, json{{"type", "connect"}, {"role", "player"}}, false));
    peers.push_back(replacement);
  }
  for (const auto fd : peers) (void)close(fd);
  if (moderator >= 0) (void)close(moderator);
  (void)kill(child, SIGTERM);
  (void)waitpid(child, nullptr, 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "expected server executable path\n"; return EXIT_FAILURE; }
  test_server_capacity_and_echo(argv[1]);
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Server echo and connection-capacity integration tests passed.\n";
  return EXIT_SUCCESS;
}
