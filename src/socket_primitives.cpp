#include "darkpool/socket_primitives.hpp"

#include <cstdlib>
#include <cstring>

#if defined(__linux__)
#include <arpa/inet.h>
#include <cerrno>
#include <ifaddrs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace darkpool::network {

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

bool parse_port(const char* text, std::uint16_t& port) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  char* end = nullptr;
  const auto value = std::strtoul(text, &end, 10);
  if (end == text || *end != '\0' || value == 0UL || value > 65535UL) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

#if defined(__linux__)

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
  struct ifaddrs* ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == -1) {
    return false;
  }
  bool found = false;
  for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
    if (ifa->ifa_addr == nullptr) continue;
    if (ifa->ifa_addr->sa_family == AF_INET) {
      char host[INET_ADDRSTRLEN];
      auto* sa = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
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

bool prepare_unix_socket_path(const std::string& path) {
  if (path.empty()) {
    return false;
  }
  sockaddr_un test_addr{};
  if (path.size() >= sizeof(test_addr.sun_path)) {
    return false;
  }

  struct stat st {};
  if (::lstat(path.c_str(), &st) == 0) {
    if (!S_ISSOCK(st.st_mode)) {
      return false;
    }

    const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) {
      return false;
    }

    sockaddr_un probe_addr{};
    probe_addr.sun_family = AF_UNIX;
    std::strncpy(probe_addr.sun_path, path.c_str(), sizeof(probe_addr.sun_path) - 1);
    const int res = connect(probe, reinterpret_cast<sockaddr*>(&probe_addr), sizeof(probe_addr));
    const int connect_err = (res < 0) ? errno : 0;
    (void)::close(probe);

    if (res == 0) {
      // Another server is actively listening
      return false;
    }

    if (connect_err == ENOENT) {
      return true;
    }

    if (connect_err != ECONNREFUSED) {
      return false;
    }

    // Stale socket, remove it
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
      return false;
    }
  }

  return true;
}

int create_tcp_listener(const std::string& bind_address, std::uint16_t port, std::string& error_msg) {
  const int listener = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    error_msg = std::string("tcp socket: ") + std::strerror(errno);
    return -1;
  }

  int reuse = 1;
  (void)setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) <= 0) {
    error_msg = std::string("tcp invalid address: ") + bind_address;
    (void)::close(listener);
    return -1;
  }

  if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 ||
      listen(listener, 16) < 0) {
    error_msg = std::string("tcp bind/listen on ") + bind_address + ":" + std::to_string(port) + ": " + std::strerror(errno);
    (void)::close(listener);
    return -1;
  }

  return listener;
}

int create_unix_listener(const std::string& path, std::string& error_msg) {
  if (!prepare_unix_socket_path(path)) {
    error_msg = std::string("failed to prepare unix socket path '") + path + "'";
    return -1;
  }

  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (listener < 0) {
    error_msg = std::string("unix socket: ") + std::strerror(errno);
    return -1;
  }

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);

  const mode_t old_mask = ::umask(0077);
  const int bind_res = bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  ::umask(old_mask);

  if (bind_res < 0) {
    error_msg = std::string("unix bind: ") + std::strerror(errno);
    (void)::close(listener);
    return -1;
  }

  if (listen(listener, 16) < 0) {
    error_msg = std::string("unix listen: ") + std::strerror(errno);
    (void)::close(listener);
    (void)::unlink(path.c_str());
    return -1;
  }

  return listener;
}

int connect_tcp(const std::string& host, std::uint16_t port, std::string& error_msg) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* addresses = nullptr;
  const auto service = std::to_string(port);
  const int lookup = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (lookup != 0) {
    error_msg = std::string("getaddrinfo: ") + gai_strerror(lookup);
    return -1;
  }

  int fd = -1;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd >= 0) {
      int one = 1;
      (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
      if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
        break;
      }
      (void)::close(fd);
      fd = -1;
    }
  }
  freeaddrinfo(addresses);

  if (fd < 0) {
    error_msg = std::string("connect (") + host + ":" + std::to_string(port) + "): " + std::strerror(errno);
    return -1;
  }
  return fd;
}

int connect_unix(const std::string& path, std::string& error_msg) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    error_msg = std::string("unix socket: ") + std::strerror(errno);
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    error_msg = std::string("unix socket path exceeds maximum length (") +
                std::to_string(sizeof(address.sun_path) - 1) + " bytes)";
    (void)::close(fd);
    return -1;
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    error_msg = std::string("connect (") + path + "): " + std::strerror(errno);
    (void)::close(fd);
    return -1;
  }
  return fd;
}

#endif  // defined(__linux__)

}  // namespace darkpool::network
