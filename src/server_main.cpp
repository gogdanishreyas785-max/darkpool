#include "darkpool/match_coordinator.hpp"
#include "darkpool/network.hpp"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::FrameDecoder;
using darkpool::network::json;
using darkpool::network::OutputQueue;

constexpr std::uint16_t kDefaultPort = 4782;
constexpr std::size_t kMaxConnections = 6;
constexpr std::size_t kMaxPlayers = 5;
constexpr std::size_t kMaxModerators = 1;
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr int kEpollTimeoutMs = 100;

enum class ClientKind { Unassigned, Player, Moderator };
struct Connection {
  FrameDecoder decoder;
  OutputQueue output;
  ClientKind kind{ClientKind::Unassigned};
  bool peer_closed{false};
  std::chrono::steady_clock::time_point connected_at{
      std::chrono::steady_clock::now()};
};

bool set_events(int epoll_fd, int fd, bool reading, bool writing) {
  epoll_event event{};
  event.events = 0U;
  if (reading)
    event.events |= static_cast<std::uint32_t>(EPOLLIN) |
                    static_cast<std::uint32_t>(EPOLLRDHUP);
  if (writing)
    event.events |= static_cast<std::uint32_t>(EPOLLOUT);
  event.data.fd = fd;
  return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event) == 0;
}

void close_connection(int epoll_fd, int fd, std::map<int, Connection> &clients,
                      std::size_t &players, std::size_t &moderators,
                      darkpool::server::MatchCoordinator &coordinator) {
  const auto found = clients.find(fd);
  if (found == clients.end())
    return;
  if (found->second.kind == ClientKind::Player)
    --players;
  if (found->second.kind == ClientKind::Moderator)
    --moderators;
  coordinator.unregister_session(static_cast<std::uint64_t>(fd));
  (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
  (void)close(fd);
  clients.erase(found);
}

bool enqueue_echo(int epoll_fd, int fd, Connection &connection,
                  const Frame &frame) {
  try {
    if (!connection.output.append(frame.serialize()))
      return false;
    return set_events(epoll_fd, fd, !connection.peer_closed, true);
  } catch (const std::exception &) {
    return false;
  }
}

bool constant_time_equals(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) {
    return false;
  }
  unsigned char diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    diff |= static_cast<unsigned char>(static_cast<unsigned char>(a[i]) ^
                                       static_cast<unsigned char>(b[i]));
  }
  return diff == 0;
}

bool process_frame(int epoll_fd, int fd, Connection &connection,
                   const Frame &frame, std::size_t &players,
                   std::size_t &moderators,
                   darkpool::server::MatchCoordinator &coordinator) {
  if (connection.kind == ClientKind::Unassigned) {
    const auto &payload = frame.payload();
    if (!payload.is_object() ||
        payload.value("type", std::string{}) != "connect") {
      return false;
    }
    const auto role = payload.value("role", std::string{});
    if (role == "player") {
      if (players >= kMaxPlayers)
        return false;
      connection.kind = ClientKind::Player;
      ++players;
      coordinator.register_session(static_cast<std::uint64_t>(fd),
                                   darkpool::server::SessionRole::Player);
      return enqueue_echo(epoll_fd, fd, connection,
                          Frame(json{{"type", "connect"}, {"role", "player"}}));
    } else if (role == "moderator") {
      const char *server_token = std::getenv("DARKPOOL_MODERATOR_TOKEN");
      if (!server_token || server_token[0] == '\0') {
        return false;
      }
      if (!payload.contains("token") || !payload["token"].is_string()) {
        return false;
      }
      const auto token = payload["token"].get<std::string>();
      if (!constant_time_equals(token, server_token)) {
        return false;
      }
      if (moderators >= kMaxModerators)
        return false;
      connection.kind = ClientKind::Moderator;
      ++moderators;
      coordinator.register_session(static_cast<std::uint64_t>(fd),
                                   darkpool::server::SessionRole::Moderator);
      return enqueue_echo(
          epoll_fd, fd, connection,
          Frame(json{{"type", "connect"}, {"role", "moderator"}}));
    } else {
      return false;
    }
  }

  // Already assigned connection: reject any subsequent connect frame
  if (frame.payload().is_object() &&
      frame.payload().value("type", std::string{}) == "connect") {
    return false;
  }

  const auto response =
      coordinator.handle(static_cast<std::uint64_t>(fd), frame.payload());
  return enqueue_echo(epoll_fd, fd, connection,
                      response ? Frame(*response) : frame);
}

bool read_client(int epoll_fd, int fd, Connection &connection,
                 std::size_t &players, std::size_t &moderators,
                 darkpool::server::MatchCoordinator &coordinator) {
  char buffer[4096];
  while (true) {
    const auto count = recv(fd, buffer, sizeof(buffer), 0);
    if (count == 0) {
      connection.peer_closed = true;
      return set_events(epoll_fd, fd, false,
                        !connection.output.pending().empty());
    }
    if (count < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return true;
      return false;
    }
    try {
      const auto frames = connection.decoder.feed(
          std::span<const char>(buffer, static_cast<std::size_t>(count)));
      for (const auto &frame : frames) {
        if (!process_frame(epoll_fd, fd, connection, frame, players, moderators,
                           coordinator))
          return false;
      }
    } catch (const std::exception &) {
      return false;
    }
  }
}

bool write_client(int epoll_fd, int fd, Connection &connection) {
  while (!connection.output.pending().empty()) {
    const auto pending = connection.output.pending();
    const auto count = send(fd, pending.data(), pending.size(), MSG_NOSIGNAL);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        return true;
      return false;
    }
    if (count == 0)
      return false;
    connection.output.consume(static_cast<std::size_t>(count));
  }
  return set_events(epoll_fd, fd, !connection.peer_closed, false);
}

bool parse_port(const char *text, std::uint16_t &port) {
  char *end = nullptr;
  const auto value = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value == 0UL || value > 65535UL)
    return false;
  port = static_cast<std::uint16_t>(value);
  return true;
}

} // namespace

int main(int argc, char **argv) {
  std::uint16_t port = kDefaultPort;
  if (argc > 2 || (argc == 2 && !parse_port(argv[1], port))) {
    std::cerr << "usage: darkpool_server [port]\n";
    return 2;
  }
  const int listener =
      socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    std::cerr << "socket: " << std::strerror(errno) << '\n';
    return 1;
  }
  int reuse = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) <
          0 ||
      listen(listener, 16) < 0) {
    std::cerr << "bind/listen: " << std::strerror(errno) << '\n';
    (void)close(listener);
    return 1;
  }
  const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    std::cerr << "epoll_create1: " << std::strerror(errno) << '\n';
    (void)close(listener);
    return 1;
  }
  epoll_event event{};
  event.events = static_cast<std::uint32_t>(EPOLLIN);
  event.data.fd = listener;
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listener, &event) < 0) {
    std::cerr << "epoll_ctl listener: " << std::strerror(errno) << '\n';
    (void)close(epoll_fd);
    (void)close(listener);
    return 1;
  }
  std::map<int, Connection> clients;
  darkpool::server::MatchCoordinator coordinator;
  std::size_t players = 0;
  std::size_t moderators = 0;
  std::cout << "darkpool_server listening on port " << port << '\n';

  int exit_status = 0;
  while (true) {
    epoll_event events[16];
    const auto ready = epoll_wait(epoll_fd, events, 16, kEpollTimeoutMs);
    if (ready < 0) {
      if (errno == EINTR)
        continue;
      std::cerr << "epoll_wait: " << std::strerror(errno) << '\n';
      exit_status = 1;
      break;
    }
    for (int i = 0; i < ready; ++i) {
      const auto fd = events[i].data.fd;
      if (fd == listener) {
        while (true) {
          sockaddr_in peer{};
          socklen_t peer_len = sizeof(peer);
          const auto client =
              accept4(listener, reinterpret_cast<sockaddr *>(&peer), &peer_len,
                      SOCK_NONBLOCK | SOCK_CLOEXEC);
          if (client < 0) {
            if (errno == EINTR)
              continue;
            break;
          }
          if (clients.size() >= kMaxConnections) {
            (void)close(client);
            continue;
          }
          epoll_event client_event{};
          client_event.events = static_cast<std::uint32_t>(EPOLLIN) |
                                static_cast<std::uint32_t>(EPOLLRDHUP);
          client_event.data.fd = client;
          if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &client_event) == 0) {
            clients.emplace(client, Connection{});
          } else {
            (void)close(client);
          }
        }
        continue;
      }
      const auto found = clients.find(fd);
      if (found == clients.end())
        continue;
      const auto flags = events[i].events;
      if ((flags & EPOLLERR) != 0U) {
        close_connection(epoll_fd, fd, clients, players, moderators,
                         coordinator);
        continue;
      }
      bool keep = true;
      const auto read_or_hangup = static_cast<std::uint32_t>(EPOLLIN) |
                                   static_cast<std::uint32_t>(EPOLLRDHUP) |
                                   static_cast<std::uint32_t>(EPOLLHUP);
      if ((flags & read_or_hangup) != 0U) {
        keep = read_client(epoll_fd, fd, found->second, players, moderators,
                           coordinator);
      }
      if (keep && (flags & (EPOLLRDHUP | EPOLLHUP)) != 0U &&
          !found->second.peer_closed) {
        found->second.peer_closed = true;
        keep = set_events(epoll_fd, fd, false,
                          !found->second.output.pending().empty());
      }
      if (keep && ((flags & EPOLLOUT) != 0U || found->second.peer_closed) &&
          !found->second.output.pending().empty()) {
        keep = write_client(epoll_fd, fd, found->second);
      }
      if (keep && found->second.peer_closed &&
          found->second.output.pending().empty())
        keep = false;
      if (!keep)
        close_connection(epoll_fd, fd, clients, players, moderators,
                         coordinator);
    }
    const auto now = std::chrono::steady_clock::now();
    std::vector<int> expired;
    for (const auto &[client_fd, conn] : clients) {
      if (conn.kind == ClientKind::Unassigned &&
          now - conn.connected_at >= kHandshakeTimeout) {
        expired.push_back(client_fd);
      }
    }
    for (const auto client_fd : expired) {
      close_connection(epoll_fd, client_fd, clients, players, moderators,
                       coordinator);
    }
  }
  for (const auto &[client_fd, _] : clients) {
    (void)close(client_fd);
  }
  (void)close(epoll_fd);
  (void)close(listener);
  return exit_status;
}
