#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

namespace {

using darkpool::network::Frame;
using darkpool::network::FrameDecoder;
using darkpool::network::OutputQueue;
using darkpool::network::json;

constexpr std::uint16_t kDefaultPort = 4782;
constexpr std::size_t kMaxConnections = 6;
constexpr std::size_t kMaxPlayers = 5;
constexpr std::size_t kMaxModerators = 1;

enum class ClientKind { Unassigned, Player, Moderator };
struct Connection {
  FrameDecoder decoder;
  OutputQueue output;
  ClientKind kind{ClientKind::Unassigned};
};

bool set_events(int epoll_fd, int fd, bool writing) {
  epoll_event event{};
  event.events = static_cast<std::uint32_t>(EPOLLIN | EPOLLRDHUP | (writing ? EPOLLOUT : 0));
  event.data.fd = fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == 0;
}

void close_connection(int epoll_fd, int fd, std::map<int, Connection>& clients,
                      std::size_t& players, std::size_t& moderators) {
  const auto found = clients.find(fd);
  if (found == clients.end()) return;
  if (found->second.kind == ClientKind::Player) --players;
  if (found->second.kind == ClientKind::Moderator) --moderators;
  (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
  (void)close(fd);
  clients.erase(found);
}

bool enqueue_echo(int epoll_fd, int fd, Connection& connection, const Frame& frame) {
  if (!connection.output.append(frame.serialize())) return false;
  return set_events(epoll_fd, fd, true);
}

bool process_frame(int epoll_fd, int fd, Connection& connection, const Frame& frame,
                   std::size_t& players, std::size_t& moderators) {
  if (connection.kind == ClientKind::Unassigned) {
    const auto& payload = frame.payload();
    if (!payload.is_object() || payload.value("type", std::string{}) != "connect" ||
        !payload.contains("role") || !payload["role"].is_string()) return false;
    const auto role = payload["role"].get<std::string>();
    if (role == "player") {
      if (players >= kMaxPlayers) return false;
      connection.kind = ClientKind::Player;
      ++players;
    } else if (role == "moderator") {
      if (moderators >= kMaxModerators) return false;
      connection.kind = ClientKind::Moderator;
      ++moderators;
    } else {
      return false;
    }
  }
  return enqueue_echo(epoll_fd, fd, connection, frame);
}

bool read_client(int epoll_fd, int fd, Connection& connection,
                 std::size_t& players, std::size_t& moderators) {
  char buffer[4096];
  while (true) {
    const auto count = recv(fd, buffer, sizeof(buffer), 0);
    if (count == 0) return false;
    if (count < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      return false;
    }
    try {
      const auto frames = connection.decoder.feed(std::span<const char>(buffer, static_cast<std::size_t>(count)));
      for (const auto& frame : frames) {
        if (!process_frame(epoll_fd, fd, connection, frame, players, moderators)) return false;
      }
    } catch (const std::exception&) {
      return false;
    }
  }
}

bool write_client(int epoll_fd, int fd, Connection& connection) {
  while (!connection.output.pending().empty()) {
    const auto pending = connection.output.pending();
    const auto count = send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
      return false;
    }
    if (count == 0) return false;
    connection.output.consume(static_cast<std::size_t>(count));
  }
  return set_events(epoll_fd, fd, false);
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
  std::uint16_t port = kDefaultPort;
  if (argc > 2 || (argc == 2 && !parse_port(argv[1], port))) {
    std::cerr << "usage: darkpool_server [port]\n";
    return 2;
  }
  const int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) { std::cerr << "socket: " << std::strerror(errno) << '\n'; return 1; }
  int reuse = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(listener, 16) < 0) {
    std::cerr << "bind/listen: " << std::strerror(errno) << '\n'; (void)close(listener); return 1;
  }
  const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) { std::cerr << "epoll_create1: " << std::strerror(errno) << '\n'; (void)close(listener); return 1; }
  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = listener;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener, &event) < 0) {
    std::cerr << "epoll_ctl: " << std::strerror(errno) << '\n'; (void)close(epoll_fd); (void)close(listener); return 1;
  }
  std::map<int, Connection> clients;
  std::size_t players = 0;
  std::size_t moderators = 0;
  std::cout << "DARKPOOL echo server listening on 127.0.0.1:" << port << '\n';
  epoll_event events[64]{};
  bool running = true;
  while (running) {
    const int count = epoll_wait(epoll_fd, events, 64, -1);
    if (count < 0) { if (errno == EINTR) continue; std::cerr << "epoll_wait: " << std::strerror(errno) << '\n'; break; }
    for (int i = 0; i < count; ++i) {
      const int fd = events[i].data.fd;
      if (fd == listener) {
        while (true) {
          const int client = accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (client < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            std::cerr << "accept4: " << std::strerror(errno) << '\n'; break;
          }
          if (clients.size() >= kMaxConnections) { (void)close(client); continue; }
          epoll_event client_event{};
          client_event.events = EPOLLIN | EPOLLRDHUP;
          client_event.data.fd = client;
          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &client_event) < 0) { (void)close(client); continue; }
          clients.emplace(client, Connection{});
        }
        continue;
      }
      const auto found = clients.find(fd);
      if (found == clients.end()) continue;
      bool keep = true;
      const auto flags = events[i].events;
      if ((flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U) keep = false;
      if (keep && (flags & EPOLLIN) != 0U) keep = read_client(epoll_fd, fd, found->second, players, moderators);
      if (keep && (flags & EPOLLOUT) != 0U) keep = write_client(epoll_fd, fd, found->second);
      if (!keep) close_connection(epoll_fd, fd, clients, players, moderators);
    }
  }
  for (auto& [fd, connection] : clients) { (void)connection; (void)close(fd); }
  (void)close(epoll_fd);
  (void)close(listener);
  return running ? 1 : 0;
}
