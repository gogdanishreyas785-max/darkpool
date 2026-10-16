#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "darkpool_server requires Linux (epoll/signalfd/unix sockets)\n";
  return 1;
}

#else

#include "darkpool/match_coordinator.hpp"
#include "darkpool/network.hpp"
#include "darkpool/tls_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ifaddrs.h>
#include <iostream>
#include <map>
#include <netinet/tcp.h>
#include <netinet/in.h>
#include <string>
#include <string_view>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using darkpool::network::Frame;
using darkpool::network::FrameDecoder;
using darkpool::network::json;
using darkpool::network::OutputQueue;

constexpr std::uint16_t kDefaultPort = 4782;
constexpr std::uint16_t kDefaultTlsPort = 4783;
constexpr std::size_t kMaxMatches = 4;
constexpr std::size_t kMaxPlayersPerMatch = darkpool::kMaxPlayerCount;
constexpr std::size_t kMaxPlayerConnections = kMaxMatches * kMaxPlayersPerMatch;
constexpr std::size_t kMaxModerators = kMaxMatches;
constexpr std::size_t kMaxConnections = kMaxPlayerConnections + kMaxModerators;
constexpr auto kHandshakeTimeout = std::chrono::seconds(5);
constexpr int kEpollTimeoutMs = 100;

enum class ClientKind { Unassigned, Player, Moderator };
enum class TransportType { Plaintext, Tls };

struct TokenBucketRateLimiter {
  static constexpr double kRate = 20.0;     // 20 requests per second (DEC-11)
  static constexpr double kBurst = 40.0;    // burst capacity 40 (DEC-11)
  double tokens{kBurst};
  std::chrono::steady_clock::time_point last_refill{std::chrono::steady_clock::now()};

  bool try_acquire() {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(now - last_refill).count();
    last_refill = now;
    tokens = std::min(kBurst, tokens + elapsed * kRate);
    if (tokens >= 1.0) {
      tokens -= 1.0;
      return true;
    }
    return false;
  }
};

struct Connection {
  FrameDecoder decoder;
  OutputQueue output;
  TokenBucketRateLimiter rate_limiter;
  ClientKind kind{ClientKind::Unassigned};
  bool peer_closed{false};
  std::chrono::steady_clock::time_point connected_at{
      std::chrono::steady_clock::now()};

  TransportType transport{TransportType::Plaintext};
  darkpool::transport::SslPtr ssl{nullptr};
  bool tls_handshake_done{true};
  bool tls_handshake_want_read{false};
  bool tls_handshake_want_write{false};

  bool tls_read_want_read{false};
  bool tls_read_want_write{false};

  bool tls_write_want_read{false};
  bool tls_write_want_write{false};
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

bool update_connection_events(int epoll_fd, int fd, Connection &connection) {
  if (connection.transport == TransportType::Plaintext) {
    const bool reading = !connection.peer_closed;
    const bool writing = !connection.output.pending().empty();
    return set_events(epoll_fd, fd, reading, writing);
  }

  // Handshake phase
  if (!connection.tls_handshake_done) {
    return set_events(epoll_fd, fd, connection.tls_handshake_want_read,
                      connection.tls_handshake_want_write);
  }

  bool reading = false;
  bool writing = false;

  // Read readiness requirements
  if (!connection.peer_closed) {
    if (connection.tls_read_want_write) {
      writing = true; // SSL_read needs write readiness to resume
    } else {
      reading = true; // normal read or tls_read_want_read
    }
  }

  // Write readiness requirements
  if (!connection.output.pending().empty()) {
    if (connection.tls_write_want_read) {
      reading = true; // SSL_write needs read readiness to resume; DO NOT set writing (prevents spin)
    } else {
      writing = true; // normal write or tls_write_want_write
    }
  }

  return set_events(epoll_fd, fd, reading, writing);
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
  if (found->second.ssl != nullptr) {
    (void)SSL_shutdown(found->second.ssl.get());
    found->second.ssl.reset();
  }
  (void)close(fd);
  clients.erase(found);
}

bool enqueue_echo(int epoll_fd, int fd, Connection &connection,
                  const Frame &frame) {
  try {
    if (!connection.output.append(frame.serialize()))
      return false;
    return update_connection_events(epoll_fd, fd, connection);
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

  // Rate limiting enforcement per connection (DEC-11: 20 req/s, burst capacity 40)
  if (!connection.rate_limiter.try_acquire()) {
    return enqueue_echo(
        epoll_fd, fd, connection,
        Frame(json{{"type", "error"}, {"error", "rate limit exceeded"}}));
  }

  const auto response =
      coordinator.handle(static_cast<std::uint64_t>(fd), frame.payload());
  return enqueue_echo(epoll_fd, fd, connection,
                      response ? Frame(*response) : frame);
}

bool read_client(int epoll_fd, int fd, Connection &connection,
                 std::size_t &players, std::size_t &moderators,
                 darkpool::server::MatchCoordinator &coordinator) {
  if (connection.transport == TransportType::Tls) {
    char buffer[4096];
    while (true) {
      std::size_t bytes_read = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          connection.ssl.get(), buffer, sizeof(buffer), bytes_read, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        connection.tls_read_want_read = false;
        connection.tls_read_want_write = false;
        try {
          const auto frames = connection.decoder.feed(
              std::span<const char>(buffer, bytes_read));
          for (const auto &frame : frames) {
            if (!process_frame(epoll_fd, fd, connection, frame, players, moderators,
                               coordinator))
              return false;
          }
        } catch (const std::exception &) {
          return false;
        }
      } else if (status == darkpool::transport::TlsIoStatus::PeerClosed) {
        connection.peer_closed = true;
        connection.tls_read_want_read = false;
        connection.tls_read_want_write = false;
        return update_connection_events(epoll_fd, fd, connection);
      } else if (status == darkpool::transport::TlsIoStatus::WantRead) {
        connection.tls_read_want_read = true;
        connection.tls_read_want_write = false;
        return update_connection_events(epoll_fd, fd, connection);
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite) {
        connection.tls_read_want_read = false;
        connection.tls_read_want_write = true;
        return update_connection_events(epoll_fd, fd, connection);
      } else {
        return false;
      }
    }
  }

  char buffer[4096];
  while (true) {
    const auto count = recv(fd, buffer, sizeof(buffer), 0);
    if (count == 0) {
      connection.peer_closed = true;
      return update_connection_events(epoll_fd, fd, connection);
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
  if (connection.transport == TransportType::Tls) {
    while (!connection.output.pending().empty()) {
      const auto pending = connection.output.pending();
      std::size_t bytes_written = 0;
      std::string err;
      const auto status = darkpool::transport::tls_write(
          connection.ssl.get(), pending.data(), pending.size(), bytes_written, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        connection.output.consume(bytes_written);
        connection.tls_write_want_read = false;
        connection.tls_write_want_write = false;
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite) {
        connection.tls_write_want_write = true;
        connection.tls_write_want_read = false;
        return update_connection_events(epoll_fd, fd, connection);
      } else if (status == darkpool::transport::TlsIoStatus::WantRead) {
        connection.tls_write_want_read = true;
        connection.tls_write_want_write = false;
        return update_connection_events(epoll_fd, fd, connection);
      } else {
        return false;
      }
    }
    connection.tls_write_want_write = false;
    connection.tls_write_want_read = false;
    return update_connection_events(epoll_fd, fd, connection);
  }

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
  return update_connection_events(epoll_fd, fd, connection);
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
                    std::map<int, Connection> &clients,
                    bool is_tls,
                    SSL_CTX *server_ssl_ctx) {
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
    int sndbuf = 16384;
    (void)setsockopt(client, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int one = 1;
    (void)setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    if (is_tls) {
      if (server_ssl_ctx == nullptr) {
        (void)close(client);
        continue;
      }
      SSL *raw_ssl = SSL_new(server_ssl_ctx);
      if (raw_ssl == nullptr) {
        (void)close(client);
        continue;
      }
      darkpool::transport::SslPtr ssl(raw_ssl);
      if (SSL_set_fd(ssl.get(), client) != 1) {
        (void)close(client);
        continue;
      }
      SSL_set_accept_state(ssl.get());

      epoll_event client_event{};
      client_event.events = static_cast<std::uint32_t>(EPOLLIN) |
                            static_cast<std::uint32_t>(EPOLLRDHUP);
      client_event.data.fd = client;
      if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client, &client_event) == 0) {
        Connection conn;
        conn.transport = TransportType::Tls;
        conn.ssl = std::move(ssl);
        conn.tls_handshake_done = false;
        conn.tls_handshake_want_read = true;
        conn.tls_handshake_want_write = false;
        clients.emplace(client, std::move(conn));
      } else {
        (void)close(client);
      }
    } else {
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
}

bool validate_bind_address(const std::string& ip_str, std::string& error_msg) {
  if (ip_str.empty()) {
    error_msg = "bind address cannot be empty";
    return false;
  }
  if (ip_str.find(':') != std::string::npos) {
    error_msg = "IPv6 addresses are not supported in this IPv4-only milestone; must specify an IPv4 address";
    return false;
  }
  struct in_addr in {};
  if (inet_pton(AF_INET, ip_str.c_str(), &in) <= 0) {
    error_msg = "malformed IPv4 address '" + ip_str + "'";
    return false;
  }
  char canonical[INET_ADDRSTRLEN];
  if (inet_ntop(AF_INET, &in, canonical, sizeof(canonical)) == nullptr || ip_str != canonical) {
    error_msg = "malformed or non-canonical IPv4 address format '" + ip_str + "'";
    return false;
  }
  const std::uint32_t host_ip = ntohl(in.s_addr);
  if (host_ip == 0U) {
    error_msg = "unspecified address 0.0.0.0 is not allowed; must specify an assigned unicast IPv4 address";
    return false;
  }
  // Multicast: 224.0.0.0/4 (224.0.0.0 - 239.255.255.255)
  if ((host_ip >> 28U) == 0xEU) {
    error_msg = "multicast address is not allowed";
    return false;
  }
  // Broadcast or reserved: 240.0.0.0/4 (240.0.0.0 - 255.255.255.255), including 255.255.255.255
  if ((host_ip >> 28U) == 0xFU) {
    error_msg = "broadcast or reserved address is not allowed";
    return false;
  }
  // Check loopback: 127.0.0.0/8
  const bool is_loopback = ((host_ip >> 24U) == 127U);
  // Check RFC1918 private IPv4:
  // 10.0.0.0/8 (10.0.0.0 - 10.255.255.255)
  const bool is_10_net = ((host_ip >> 24U) == 10U);
  // 172.16.0.0/12 (172.16.0.0 - 172.31.255.255) -> host_ip between 0xAC100000 and 0xAC1FFFFF
  const bool is_172_net = (host_ip >= 0xAC100000U && host_ip <= 0xAC1FFFFFU);
  // 192.168.0.0/16 (192.168.0.0 - 192.168.255.255) -> (host_ip >> 16U) == 0xC0A8U
  const bool is_192_net = ((host_ip >> 16U) == 0xC0A8U);

  if (!is_loopback && !is_10_net && !is_172_net && !is_192_net) {
    error_msg = "bind address must be an RFC1918 private IPv4 address (10.0.0.0/8, 172.16.0.0/12, or 192.168.0.0/16) or loopback (127.0.0.0/8); public or non-LAN address '" + ip_str + "' is rejected";
    return false;
  }
  return true;
}

bool is_address_assigned_to_local_interface(const std::string& ip_str) {
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return false;
  }
  bool found = false;
  for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if (ifa->ifa_addr->sa_family == AF_INET) {
      char host[INET_ADDRSTRLEN];
      auto *sa = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
      if (inet_ntop(AF_INET, &(sa->sin_addr), host, sizeof(host)) != nullptr) {
        if (ip_str == host) {
          found = true;
          break;
        }
      }
    }
  }
  freeifaddrs(ifaddr);
  return found;
}

struct ServerConfig {
  bool enable_tcp{false};
  std::uint16_t tcp_port{kDefaultPort};
  std::string bind_address{"127.0.0.1"};
  bool explicit_bind_address{false};
  bool enable_unix{false};
  std::string unix_path;

  bool enable_tls{false};
  std::uint16_t tls_port{kDefaultTlsPort};
  std::string tls_bind_address{"127.0.0.1"};
  bool explicit_tls_bind_address{false};
  std::string tls_cert_path;
  std::string tls_key_path;
};

bool parse_server_args(int argc, char **argv, ServerConfig &config, std::string &error_msg) {
  if (argc == 1) {
    config.enable_tcp = true;
    config.tcp_port = kDefaultPort;
    config.bind_address = "127.0.0.1";
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
    } else if (arg == "--no-tls") {
      config.enable_tls = false;
    } else if (arg == "--port" || arg == "-p") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --port";
        return false;
      }
      if (!parse_port(argv[++i], config.tcp_port)) {
        error_msg = "invalid port argument";
        return false;
      }
      config.enable_tcp = true;
    } else if (arg.rfind("--port=", 0) == 0) {
      std::uint16_t p = 0;
      if (!parse_port(std::string(arg.substr(7)).c_str(), p)) {
        error_msg = "invalid port argument";
        return false;
      }
      config.tcp_port = p;
      config.enable_tcp = true;
    } else if (arg == "--bind-address" || arg == "-b") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --bind-address";
        return false;
      }
      const std::string addr = argv[++i];
      if (!validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.bind_address = addr;
      config.explicit_bind_address = true;
      config.enable_tcp = true;
    } else if (arg.rfind("--bind-address=", 0) == 0) {
      const std::string addr = std::string(arg.substr(15));
      if (!validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.bind_address = addr;
      config.explicit_bind_address = true;
      config.enable_tcp = true;
    } else if (arg.rfind("-b=", 0) == 0) {
      const std::string addr = std::string(arg.substr(3));
      if (!validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.bind_address = addr;
      config.explicit_bind_address = true;
      config.enable_tcp = true;
    } else if (arg == "--socket" || arg == "-s") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --socket";
        return false;
      }
      config.unix_path = argv[++i];
      config.enable_unix = true;
      explicit_unix = true;
    } else if (arg.rfind("--socket=", 0) == 0) {
      config.unix_path = arg.substr(9);
      config.enable_unix = true;
      explicit_unix = true;
    } else if (arg == "--tls-bind-address") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --tls-bind-address";
        return false;
      }
      const std::string addr = argv[++i];
      if (!validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.tls_bind_address = addr;
      config.explicit_tls_bind_address = true;
      config.enable_tls = true;
    } else if (arg.rfind("--tls-bind-address=", 0) == 0) {
      const std::string addr = std::string(arg.substr(19));
      if (!validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.tls_bind_address = addr;
      config.explicit_tls_bind_address = true;
      config.enable_tls = true;
    } else if (arg == "--tls-port") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --tls-port";
        return false;
      }
      if (!parse_port(argv[++i], config.tls_port)) {
        error_msg = "invalid port argument for --tls-port";
        return false;
      }
      config.enable_tls = true;
    } else if (arg.rfind("--tls-port=", 0) == 0) {
      std::uint16_t p = 0;
      if (!parse_port(std::string(arg.substr(11)).c_str(), p)) {
        error_msg = "invalid port argument for --tls-port";
        return false;
      }
      config.tls_port = p;
      config.enable_tls = true;
    } else if (arg == "--tls-cert") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --tls-cert";
        return false;
      }
      config.tls_cert_path = argv[++i];
      config.enable_tls = true;
    } else if (arg.rfind("--tls-cert=", 0) == 0) {
      config.tls_cert_path = std::string(arg.substr(11));
      config.enable_tls = true;
    } else if (arg == "--tls-key") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --tls-key";
        return false;
      }
      config.tls_key_path = argv[++i];
      config.enable_tls = true;
    } else if (arg.rfind("--tls-key=", 0) == 0) {
      config.tls_key_path = std::string(arg.substr(10));
      config.enable_tls = true;
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

  if (config.enable_tls) {
    if (config.tls_cert_path.empty() || config.tls_key_path.empty()) {
      error_msg = "--tls-cert and --tls-key must be provided together when TLS is enabled";
      return false;
    }
  }

  const char *env_path = std::getenv("DARKPOOL_SOCKET_PATH");
  if (!explicit_unix && env_path && env_path[0] != '\0') {
    config.enable_unix = true;
    config.unix_path = env_path;
  }

  return config.enable_tcp || config.enable_unix || config.enable_tls;
}

} // namespace

int main(int argc, char **argv) {
  ServerConfig config;
  std::string arg_error;
  if (!parse_server_args(argc, argv, config, arg_error)) {
    if (!arg_error.empty()) {
      std::cerr << "error: " << arg_error << '\n';
    }
    std::cerr << "usage: darkpool_server [--bind-address <IPv4>] [--port <port>] [--socket <path>] "
                 "[--tls-bind-address <IPv4>] [--tls-port <port>] [--tls-cert <cert.pem>] [--tls-key <key.pem>] "
                 "[--no-tcp] [--no-unix] [port] [socket_path]\n";
    return 2;
  }

  if (config.enable_tcp) {
    if (!is_address_assigned_to_local_interface(config.bind_address)) {
      std::cerr << "error: requested bind address " << config.bind_address
                << " is not assigned to any local network interface\n";
      return 1;
    }
  }

  if (config.enable_tls) {
    if (!is_address_assigned_to_local_interface(config.tls_bind_address)) {
      std::cerr << "error: requested TLS bind address " << config.tls_bind_address
                << " is not assigned to any local network interface\n";
      return 1;
    }
  }

  darkpool::transport::SslCtxPtr server_ssl_ctx;
  if (config.enable_tls) {
    darkpool::transport::ServerTlsConfig tls_cfg;
    tls_cfg.cert_file = config.tls_cert_path;
    tls_cfg.key_file = config.tls_key_path;
    tls_cfg.bind_address = config.tls_bind_address;
    tls_cfg.port = config.tls_port;

    std::string tls_init_err;
    server_ssl_ctx = darkpool::transport::create_server_ssl_ctx(tls_cfg, tls_init_err);
    if (server_ssl_ctx == nullptr) {
      std::cerr << "error: TLS initialization failed: " << tls_init_err << '\n';
      return 1;
    }
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
    if (inet_pton(AF_INET, config.bind_address.c_str(), &address.sin_addr) <= 0) {
      std::cerr << "tcp invalid address: " << config.bind_address << '\n';
      (void)close(tcp_listener);
      return 1;
    }
    if (bind(tcp_listener, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0 ||
        listen(tcp_listener, 16) < 0) {
      std::cerr << "tcp bind/listen on " << config.bind_address << ":" << config.tcp_port
                << ": " << std::strerror(errno) << '\n';
      (void)close(tcp_listener);
      return 1;
    }
  }

  int tls_listener = -1;
  if (config.enable_tls) {
    tls_listener =
        socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (tls_listener < 0) {
      std::cerr << "tls socket: " << std::strerror(errno) << '\n';
      if (tcp_listener >= 0) (void)close(tcp_listener);
      return 1;
    }
    int reuse = 1;
    (void)setsockopt(tls_listener, SOL_SOCKET, SO_REUSEADDR, &reuse,
                     sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.tls_port);
    if (inet_pton(AF_INET, config.tls_bind_address.c_str(), &address.sin_addr) <= 0) {
      std::cerr << "tls invalid address: " << config.tls_bind_address << '\n';
      (void)close(tls_listener);
      if (tcp_listener >= 0) (void)close(tcp_listener);
      return 1;
    }
    if (bind(tls_listener, reinterpret_cast<sockaddr *>(&address),
             sizeof(address)) < 0 ||
        listen(tls_listener, 16) < 0) {
      std::cerr << "tls bind/listen on " << config.tls_bind_address << ":" << config.tls_port
                << ": " << std::strerror(errno) << '\n';
      (void)close(tls_listener);
      if (tcp_listener >= 0) (void)close(tcp_listener);
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
      if (tls_listener >= 0)
        (void)close(tls_listener);
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
    if (tls_listener >= 0)
      (void)close(tls_listener);
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
      if (tls_listener >= 0)
        (void)close(tls_listener);
      if (unix_listener >= 0)
        (void)close(unix_listener);
      return 1;
    }
  }

  if (tls_listener >= 0) {
    epoll_event event{};
    event.events = static_cast<std::uint32_t>(EPOLLIN);
    event.data.fd = tls_listener;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tls_listener, &event) < 0) {
      std::cerr << "epoll_ctl tls listener: " << std::strerror(errno) << '\n';
      (void)close(epoll_fd);
      if (tcp_listener >= 0)
        (void)close(tcp_listener);
      (void)close(tls_listener);
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
      if (tls_listener >= 0)
        (void)close(tls_listener);
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
    if (config.bind_address != "127.0.0.1") {
      std::cout << "WARNING: LAN binding enabled on " << config.bind_address
                << ". The LAN transport is plaintext and unencrypted; do NOT expose this server to untrusted networks or the public internet!\n";
    }
    std::cout << "darkpool_server listening on " << config.bind_address << ":" << config.tcp_port
              << '\n';
  }
  if (config.enable_tls) {
    std::cout << "darkpool_server listening on TLS 1.3 " << config.tls_bind_address << ":" << config.tls_port
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
      if (tcp_listener >= 0 && fd == tcp_listener) {
        accept_clients(fd, epoll_fd, clients, false, nullptr);
        continue;
      }
      if (tls_listener >= 0 && fd == tls_listener) {
        accept_clients(fd, epoll_fd, clients, true, server_ssl_ctx.get());
        continue;
      }
      if (unix_listener >= 0 && fd == unix_listener) {
        accept_clients(fd, epoll_fd, clients, false, nullptr);
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

      // If TLS handshake is still in progress, drive it forward
      if (found->second.transport == TransportType::Tls && !found->second.tls_handshake_done) {
        bool want_read = false;
        bool want_write = false;
        std::string err;
        const auto status = darkpool::transport::step_tls_handshake(
            found->second.ssl.get(), true, want_read, want_write, err);
        if (status == darkpool::transport::TlsHandshakeStatus::Established) {
          found->second.tls_handshake_done = true;
          found->second.tls_handshake_want_read = false;
          found->second.tls_handshake_want_write = false;
          (void)update_connection_events(epoll_fd, fd, found->second);
        } else if (status == darkpool::transport::TlsHandshakeStatus::InProgress) {
          found->second.tls_handshake_want_read = want_read;
          found->second.tls_handshake_want_write = want_write;
          (void)update_connection_events(epoll_fd, fd, found->second);
          continue;
        } else {
          // Handshake failed (e.g. plaintext sent to TLS port or bad client alert)
          close_connection(epoll_fd, fd, clients, players, moderators, coordinator);
          continue;
        }
      }

      bool keep = true;
      const auto read_or_hangup = static_cast<std::uint32_t>(EPOLLIN) |
                                   static_cast<std::uint32_t>(EPOLLRDHUP) |
                                   static_cast<std::uint32_t>(EPOLLHUP);

      const bool initial_write_want_read = (found->second.transport == TransportType::Tls) && found->second.tls_write_want_read;
      const bool initial_read_want_write = (found->second.transport == TransportType::Tls) && found->second.tls_read_want_write;

      // Cross-direction resume:
      // If a pending TLS write was waiting for read readiness (SSL_ERROR_WANT_READ) and read readiness fired:
      if (initial_write_want_read && (flags & read_or_hangup) != 0U) {
        keep = write_client(epoll_fd, fd, found->second);
      }

      // Normal read: only if not waiting for cross-direction readiness
      if (keep && !initial_write_want_read && !initial_read_want_write && (flags & read_or_hangup) != 0U) {
        keep = read_client(epoll_fd, fd, found->second, players, moderators,
                           coordinator);
      }

      // Cross-direction resume:
      // If a pending TLS read was waiting for write readiness (SSL_ERROR_WANT_WRITE) and write readiness fired:
      if (keep && initial_read_want_write && (flags & EPOLLOUT) != 0U) {
        keep = read_client(epoll_fd, fd, found->second, players, moderators,
                           coordinator);
      }

      if (keep && (flags & (EPOLLRDHUP | EPOLLHUP)) != 0U &&
          !found->second.peer_closed) {
        found->second.peer_closed = true;
        keep = update_connection_events(epoll_fd, fd, found->second);
      }

      // Normal write: only if not waiting for cross-direction readiness
      if (keep && !initial_read_want_write && !initial_write_want_read &&
          ((flags & EPOLLOUT) != 0U || found->second.peer_closed) &&
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
      const bool handshake_incomplete =
          (conn.transport == TransportType::Tls && !conn.tls_handshake_done) ||
          (conn.kind == ClientKind::Unassigned);
      if (handshake_incomplete && now - conn.connected_at >= kHandshakeTimeout) {
        expired.push_back(client_fd);
      }
    }
    for (const auto client_fd : expired) {
      close_connection(epoll_fd, client_fd, clients, players, moderators,
                       coordinator);
    }
  }
  for (auto &[client_fd, conn] : clients) {
    if (conn.ssl != nullptr) {
      (void)SSL_shutdown(conn.ssl.get());
      conn.ssl.reset();
    }
    (void)close(client_fd);
  }
  clients.clear();
  if (signal_fd >= 0)
    (void)close(signal_fd);
  (void)close(epoll_fd);
  if (tcp_listener >= 0)
    (void)close(tcp_listener);
  if (tls_listener >= 0)
    (void)close(tls_listener);
  if (unix_listener >= 0)
    (void)close(unix_listener);
  return exit_status;
}

#endif  // !defined(__linux__)
