#include "darkpool/match_coordinator.hpp"
#include "darkpool/network.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::FrameDecoder;
using darkpool::network::json;
using darkpool::network::OutputQueue;

constexpr std::uint16_t kDefaultPort = 4782;
constexpr std::size_t kMaxMatches = 4;
constexpr std::size_t kMaxPlayersPerMatch = darkpool::kMaxPlayerCount;
constexpr std::size_t kMaxPlayerConnections = kMaxMatches * kMaxPlayersPerMatch;
constexpr std::size_t kMaxModerators = 1;
constexpr std::size_t kMaxConnections = kMaxPlayerConnections + kMaxModerators;
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

struct SocketFileGuard {
  std::string path;
  bool active{false};

  void disarm() noexcept { active = false; }

  ~SocketFileGuard() {
    if (active && !path.empty()) {
      struct stat st {};
      if (::lstat(path.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) {
        (void)::unlink(path.c_str());
      }
    }
  }
};

bool prepare_unix_socket_path(const std::string &path) {
  if (path.empty()) {
    std::cerr << "error: unix socket path cannot be empty\n";
    return false;
  }
  sockaddr_un test_addr{};
  if (path.size() >= sizeof(test_addr.sun_path)) {
    std::cerr << "error: unix socket path exceeds maximum length ("
              << sizeof(test_addr.sun_path) - 1 << " bytes)\n";
    return false;
  }

  struct stat st {};
  if (::lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      std::cerr << "error: refusing to remove non-socket file at '" << path << "'\n";
      return false;
    }

    const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) {
      std::cerr << "error: cannot probe existing socket at '" << path
                << "': " << std::strerror(errno) << '\n';
      return false;
    }

    sockaddr_un probe_addr{};
    probe_addr.sun_family = AF_UNIX;
    std::strncpy(probe_addr.sun_path, path.c_str(), sizeof(probe_addr.sun_path) - 1);
    const int res = connect(probe, reinterpret_cast<sockaddr *>(&probe_addr), sizeof(probe_addr));
    const int connect_err = (res < 0) ? errno : 0;
    (void)close(probe);

    if (res == 0) {
      std::cerr << "error: another server is actively listening on " << path << '\n';
      return false;
    }

    if (connect_err == ENOENT) {
      // Path disappeared between lstat and connect (benign race condition)
      return true;
    }

    if (connect_err != ECONNREFUSED) {
      std::cerr << "error: cannot verify if socket '" << path
                << "' is stale (connect failed: " << std::strerror(connect_err) << ")\n";
      return false;
    }

    // Connect was specifically refused (ECONNREFUSED), establishing that the socket is stale
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "error: failed to unlink stale socket file '" << path
                << "': " << std::strerror(errno) << '\n';
      return false;
    }
  }

  return true;
}

int create_unix_listener(const std::string &path) {
  if (!prepare_unix_socket_path(path)) {
    return -1;
  }

  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    std::cerr << "unix socket: " << std::strerror(errno) << '\n';
    return -1;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  const mode_t old_mask = ::umask(0077);
  const int bind_res = bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
  ::umask(old_mask);

  if (bind_res < 0) {
    std::cerr << "unix bind: " << std::strerror(errno) << '\n';
    (void)close(listener);
    return -1;
  }

  if (listen(listener, 16) < 0) {
    std::cerr << "unix listen: " << std::strerror(errno) << '\n';
    (void)close(listener);
    (void)::unlink(path.c_str());
    return -1;
  }

  return listener;
}

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
      if (players >= kMaxPlayerConnections)
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

void accept_clients(int listener, int epoll_fd,
                    std::map<int, Connection> &clients) {
  while (true) {
    const auto client =
        accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
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
}

struct ServerConfig {
  bool enable_tcp{false};
  std::uint16_t tcp_port{kDefaultPort};
  bool enable_unix{false};
  std::string unix_path;
};

bool parse_server_args(int argc, char **argv, ServerConfig &config) {
  if (argc == 1) {
    config.enable_tcp = true;
    config.tcp_port = kDefaultPort;
    config.enable_unix = true;
    const char *env_path = std::getenv("DARKPOOL_SOCKET_PATH");
    config.unix_path =
        (env_path && env_path[0] != '\0') ? env_path : "/tmp/darkpool.sock";
    return true;
  }

  bool explicit_unix = false;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--no-tcp") {
      config.enable_tcp = false;
    } else if (arg == "--no-unix") {
      config.enable_unix = false;
      explicit_unix = true;
    } else if (arg == "--port" || arg == "-p") {
      if (i + 1 >= argc)
        return false;
      if (!parse_port(argv[++i], config.tcp_port))
        return false;
      config.enable_tcp = true;
    } else if (arg == "--socket" || arg == "-s") {
      if (i + 1 >= argc)
        return false;
      config.unix_path = argv[++i];
      config.enable_unix = true;
      explicit_unix = true;
    } else if (arg.rfind("--socket=", 0) == 0) {
      config.unix_path = arg.substr(9);
      config.enable_unix = true;
      explicit_unix = true;
    } else if (arg.rfind("--port=", 0) == 0) {
      std::uint16_t p = 0;
      if (!parse_port(std::string(arg.substr(7)).c_str(), p))
        return false;
      config.tcp_port = p;
      config.enable_tcp = true;
    } else {
      std::uint16_t p = 0;
      if (parse_port(argv[i], p)) {
        config.tcp_port = p;
        config.enable_tcp = true;
      } else {
        config.unix_path = argv[i];
        config.enable_unix = true;
        explicit_unix = true;
      }
    }
  }

  const char *env_path = std::getenv("DARKPOOL_SOCKET_PATH");
  if (!explicit_unix && env_path && env_path[0] != '\0') {
    config.enable_unix = true;
    config.unix_path = env_path;
  }

  return config.enable_tcp || config.enable_unix;
}

} // namespace

int main(int argc, char **argv) {
  ServerConfig config;
  if (!parse_server_args(argc, argv, config)) {
    std::cerr << "usage: darkpool_server [--port <port>] [--socket <path>] "
                 "[--no-tcp] [--no-unix] [port] [socket_path]\n";
    return 2;
  }

  int tcp_listener = -1;
  if (config.enable_tcp) {
    tcp_listener =
        socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (tcp_listener < 0) {
      std::cerr << "tcp socket: " << std::strerror(errno) << '\n';
      return 1;
    }
    int reuse = 1;
    (void)setsockopt(tcp_listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.tcp_port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(tcp_listener, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0 ||
        listen(tcp_listener, 16) < 0) {
      std::cerr << "tcp bind/listen: " << std::strerror(errno) << '\n';
      (void)close(tcp_listener);
      return 1;
    }
  }

  int unix_listener = -1;
  SocketFileGuard socket_guard;
  if (config.enable_unix) {
    unix_listener = create_unix_listener(config.unix_path);
    if (unix_listener < 0) {
      if (tcp_listener >= 0)
        (void)close(tcp_listener);
      return 1;
    }
    socket_guard.path = config.unix_path;
    socket_guard.active = true;
  }

  const int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0) {
    std::cerr << "epoll_create1: " << std::strerror(errno) << '\n';
    if (tcp_listener >= 0)
      (void)close(tcp_listener);
    if (unix_listener >= 0)
      (void)close(unix_listener);
    return 1;
  }

  if (tcp_listener >= 0) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = tcp_listener;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tcp_listener, &event) < 0) {
      std::cerr << "epoll_ctl tcp listener: " << std::strerror(errno) << '\n';
      (void)close(epoll_fd);
      (void)close(tcp_listener);
      if (unix_listener >= 0)
        (void)close(unix_listener);
      return 1;
    }
  }

  if (unix_listener >= 0) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = unix_listener;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, unix_listener, &event) < 0) {
      std::cerr << "epoll_ctl unix listener: " << std::strerror(errno) << '\n';
      (void)close(epoll_fd);
      if (tcp_listener >= 0)
        (void)close(tcp_listener);
      (void)close(unix_listener);
      return 1;
    }
  }

  sigset_t sig_mask;
  sigemptyset(&sig_mask);
  sigaddset(&sig_mask, SIGINT);
  sigaddset(&sig_mask, SIGTERM);
  (void)sigprocmask(SIG_BLOCK, &sig_mask, nullptr);
  const int signal_fd = signalfd(-1, &sig_mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (signal_fd >= 0) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = signal_fd;
    (void)epoll_ctl(epoll_fd, EPOLL_CTL_ADD, signal_fd, &event);
  }

  std::map<int, Connection> clients;
  darkpool::server::MatchCoordinator coordinator;
  std::size_t players = 0;
  std::size_t moderators = 0;

  if (config.enable_tcp) {
    std::cout << "darkpool_server listening on port " << config.tcp_port
              << '\n';
  }
  if (config.enable_unix) {
    std::cout << "darkpool_server listening on unix socket " << config.unix_path
              << '\n';
  }

  int exit_status = 0;
  bool should_terminate = false;
  while (!should_terminate) {
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
      if (signal_fd >= 0 && fd == signal_fd) {
        signalfd_siginfo info{};
        const auto read_bytes = read(signal_fd, &info, sizeof(info));
        (void)read_bytes;
        should_terminate = true;
        break;
      }
      if ((tcp_listener >= 0 && fd == tcp_listener) ||
          (unix_listener >= 0 && fd == unix_listener)) {
        accept_clients(fd, epoll_fd, clients);
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
  if (signal_fd >= 0)
    (void)close(signal_fd);
  (void)close(epoll_fd);
  if (tcp_listener >= 0)
    (void)close(tcp_listener);
  if (unix_listener >= 0)
    (void)close(unix_listener);
  return exit_status;
}
