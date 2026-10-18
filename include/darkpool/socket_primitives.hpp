#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#if defined(__linux__)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace darkpool::network {

// Constant-time string comparison for secret credentials (moderator tokens)
[[nodiscard]] bool constant_time_equals(std::string_view a, std::string_view b) noexcept;

// Safe port string parser (1 - 65535)
[[nodiscard]] bool parse_port(const char* text, std::uint16_t& port);

#if defined(__linux__)

// Validates that an IPv4 address is canonical and is an RFC1918 private address or loopback (127.0.0.0/8).
[[nodiscard]] bool validate_bind_address(const std::string& ip_str, std::string& error_msg);

// Checks if the given IPv4 address is assigned to any local network interface.
[[nodiscard]] bool is_address_assigned_to_local_interface(const std::string& ip_str);

// Verifies and prepares a UNIX socket path, removing a stale socket if connect fails with ECONNREFUSED.
[[nodiscard]] bool prepare_unix_socket_path(const std::string& path);

// RAII guard to remove a UNIX socket file on destruction unless disarmed.
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

// Creates a listening nonblocking TCP socket bound to bind_address:port.
[[nodiscard]] int create_tcp_listener(const std::string& bind_address, std::uint16_t port, std::string& error_msg);

// Creates a listening nonblocking UNIX-domain socket at path.
[[nodiscard]] int create_unix_listener(const std::string& path, std::string& error_msg);

// Connects to a remote TCP host:port with nonblocking flags or TCP_NODELAY. Returns connected fd or -1.
[[nodiscard]] int connect_tcp(const std::string& host, std::uint16_t port, std::string& error_msg);

// Connects to a local UNIX domain socket at path. Returns connected fd or -1.
[[nodiscard]] int connect_unix(const std::string& path, std::string& error_msg);

#endif  // defined(__linux__)

}  // namespace darkpool::network
