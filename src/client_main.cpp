#include "darkpool/cli.hpp"
#include "darkpool/network.hpp"
#include "darkpool/tls_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::FrameDecoder;
using darkpool::network::FrameStream;
using darkpool::network::json;

bool write_all(int fd, SSL* ssl, const std::string& buffer) {
  if (ssl != nullptr) {
    std::size_t offset = 0;
    while (offset < buffer.size()) {
      std::size_t written = 0;
      std::string err;
      const auto status = darkpool::transport::tls_write(
          ssl, buffer.data() + offset, buffer.size() - offset, written, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        offset += written;
      } else if (status == darkpool::transport::TlsIoStatus::WantWrite ||
                 status == darkpool::transport::TlsIoStatus::WantRead) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = (status == darkpool::transport::TlsIoStatus::WantWrite) ? POLLOUT : POLLIN;
        (void)poll(&pfd, 1, 1000);
      } else {
        return false;
      }
    }
    return true;
  }

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

bool receive_frame(int fd, SSL* ssl, FrameStream& stream, Frame& out_frame) {
  if (stream.next(out_frame)) return true;

  char buffer[4096];
  while (true) {
    if (ssl != nullptr) {
      std::size_t bytes_read = 0;
      std::string err;
      const auto status = darkpool::transport::tls_read(
          ssl, buffer, sizeof(buffer), bytes_read, err);
      if (status == darkpool::transport::TlsIoStatus::Ok) {
        try {
          stream.feed(std::span<const char>(buffer, bytes_read));
        } catch (const std::exception& error) {
          std::cerr << "protocol error: invalid server frame: " << error.what() << '\n';
          return false;
        }
        if (stream.next(out_frame)) return true;
      } else if (status == darkpool::transport::TlsIoStatus::WantRead ||
                 status == darkpool::transport::TlsIoStatus::WantWrite) {
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = (status == darkpool::transport::TlsIoStatus::WantRead) ? POLLIN : POLLOUT;
        (void)poll(&pfd, 1, 1000);
      } else {
        return false;
      }
    } else {
      const auto count = recv(fd, buffer, sizeof(buffer), 0);
      if (count <= 0) {
        if (count < 0 && errno == EINTR) continue;
        return false;
      }
      try {
        stream.feed(std::span<const char>(buffer, static_cast<std::size_t>(count)));
      } catch (const std::exception& error) {
        std::cerr << "protocol error: invalid server frame: " << error.what() << '\n';
        return false;
      }
      if (stream.next(out_frame)) return true;
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

int connect_unix(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "unix socket: " << std::strerror(errno) << '\n';
    return -1;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    std::cerr << "error: unix socket path exceeds maximum length ("
              << sizeof(address.sun_path) - 1 << " bytes)\n";
    (void)close(fd);
    return -1;
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "connect (" << path << "): " << std::strerror(errno) << '\n';
    (void)close(fd);
    return -1;
  }
  return fd;
}

int connect_tcp(const std::string& host, std::uint16_t port) {
  addrinfo hints{};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* addresses = nullptr;
  const auto service = std::to_string(port);
  const int lookup = getaddrinfo(host.c_str(), service.c_str(), &hints, &addresses);
  if (lookup != 0) {
    std::cerr << "getaddrinfo: " << gai_strerror(lookup) << '\n';
    return -1;
  }
  int fd = -1;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd >= 0 && connect(fd, address->ai_addr, address->ai_addrlen) == 0) break;
    if (fd >= 0) (void)close(fd);
    fd = -1;
  }
  freeaddrinfo(addresses);
  if (fd < 0) {
    std::cerr << "connect (" << host << ":" << port << "): " << std::strerror(errno) << '\n';
    return -1;
  }
  return fd;
}

bool perform_client_tls_handshake(SSL* ssl, int fd, std::chrono::seconds timeout) {
  const int flags = fcntl(fd, F_GETFL, 0);
  (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  const auto start = std::chrono::steady_clock::now();
  while (true) {
    if (std::chrono::steady_clock::now() - start >= timeout) {
      std::cerr << "error: TLS handshake timed out after " << timeout.count() << " seconds\n";
      return false;
    }
    bool want_read = false;
    bool want_write = false;
    std::string err;
    const auto status = darkpool::transport::step_tls_handshake(
        ssl, false, want_read, want_write, err);
    if (status == darkpool::transport::TlsHandshakeStatus::Established) {
      break;
    }
    if (status == darkpool::transport::TlsHandshakeStatus::Failed) {
      std::cerr << "error: TLS handshake failed: " << err << '\n';
      return false;
    }
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = (want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0);
    const int pret = poll(&pfd, 1, 500);
    if (pret < 0 && errno != EINTR) {
      std::cerr << "error: poll during TLS handshake: " << std::strerror(errno) << '\n';
      return false;
    }
  }
  (void)fcntl(fd, F_SETFL, flags);
  return true;
}

struct ParsedClientArgs {
  bool use_tls{false};
  darkpool::transport::ClientVerifyMode verify_mode{darkpool::transport::ClientVerifyMode::LocalCa};
  bool has_explicit_mode{false};
  std::string ca_file;
  std::string fingerprint_hex;

  bool is_unix{false};
  std::string unix_path;

  std::string host;
  std::uint16_t port{0};

  std::string role_str;
};

}  // namespace

int main(int argc, char** argv) {
  ParsedClientArgs args;
  std::vector<std::string> positional;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--tls") {
      args.use_tls = true;
    } else if (arg == "--ca-cert") {
      if (i + 1 >= argc) {
        std::cerr << "error: missing argument for --ca-cert\n";
        return 2;
      }
      args.ca_file = argv[++i];
      args.verify_mode = darkpool::transport::ClientVerifyMode::LocalCa;
      args.has_explicit_mode = true;
      args.use_tls = true;
    } else if (arg.rfind("--ca-cert=", 0) == 0) {
      args.ca_file = std::string(arg.substr(10));
      args.verify_mode = darkpool::transport::ClientVerifyMode::LocalCa;
      args.has_explicit_mode = true;
      args.use_tls = true;
    } else if (arg == "--server-fingerprint") {
      if (i + 1 >= argc) {
        std::cerr << "error: missing argument for --server-fingerprint\n";
        return 2;
      }
      args.fingerprint_hex = argv[++i];
      args.verify_mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
      args.has_explicit_mode = true;
      args.use_tls = true;
    } else if (arg.rfind("--server-fingerprint=", 0) == 0) {
      args.fingerprint_hex = std::string(arg.substr(21));
      args.verify_mode = darkpool::transport::ClientVerifyMode::FingerprintPinning;
      args.has_explicit_mode = true;
      args.use_tls = true;
    } else if (arg == "--socket" || arg == "-s") {
      if (i + 1 >= argc) {
        std::cerr << "error: missing argument for --socket\n";
        return 2;
      }
      args.unix_path = argv[++i];
      args.is_unix = true;
    } else if (arg.rfind("--socket=", 0) == 0) {
      args.unix_path = std::string(arg.substr(9));
      args.is_unix = true;
    } else {
      positional.emplace_back(argv[i]);
    }
  }

  if (args.use_tls) {
    if (!args.has_explicit_mode) {
      std::cerr << "error: TLS client requires either --ca-cert <path> or --server-fingerprint <sha256>\n";
      return 2;
    }
    if (args.is_unix) {
      std::cerr << "error: TLS is not applicable to UNIX-domain sockets\n";
      return 2;
    }
    if (positional.size() == 3) {
      args.host = positional[0];
      if (!parse_port(positional[1].c_str(), args.port)) {
        std::cerr << "error: invalid port: " << positional[1] << '\n';
        return 2;
      }
      args.role_str = positional[2];
    } else if (positional.size() == 2) {
      args.host = positional[0];
      args.port = 4783;
      args.role_str = positional[1];
    } else {
      std::cerr << "usage: darkpool_client --tls --ca-cert <ca.pem> <host> <port> <player|moderator>\n"
                << "   or: darkpool_client --tls --server-fingerprint <sha256> <host> <port> <player|moderator>\n";
      return 2;
    }
  } else if (args.is_unix) {
    if (positional.size() == 1) {
      args.role_str = positional[0];
    } else if (positional.size() == 2 && args.unix_path.empty()) {
      args.unix_path = positional[0];
      args.role_str = positional[1];
    } else {
      std::cerr << "usage: darkpool_client [--socket <path>] <player|moderator>\n";
      return 2;
    }
  } else {
    if (positional.size() == 3) {
      args.host = positional[0];
      if (!parse_port(positional[1].c_str(), args.port)) {
        std::cerr << "error: invalid port: " << positional[1] << '\n';
        return 2;
      }
      args.role_str = positional[2];
    } else if (positional.size() == 2) {
      args.is_unix = true;
      args.unix_path = positional[0];
      args.role_str = positional[1];
    } else if (positional.size() == 1 && (positional[0] == "player" || positional[0] == "moderator")) {
      args.is_unix = true;
      const char* env_path = std::getenv("DARKPOOL_SOCKET_PATH");
      args.unix_path = (env_path && env_path[0] != '\0') ? env_path : "/tmp/darkpool.sock";
      args.role_str = positional[0];
    } else {
      std::cerr << "usage: darkpool_client <socket_path> <player|moderator>\n"
                << "   or: darkpool_client <host> <port> <player|moderator>\n"
                << "   or: darkpool_client [--socket <path>] <player|moderator>\n"
                << "   or: darkpool_client --tls --ca-cert <ca.pem> <host> <port> <player|moderator>\n"
                << "   or: darkpool_client --tls --server-fingerprint <sha256> <host> <port> <player|moderator>\n";
      return 2;
    }
  }

  if (args.role_str != "player" && args.role_str != "moderator") {
    std::cerr << "error: role must be 'player' or 'moderator', got: " << args.role_str << '\n';
    return 2;
  }

  int fd = -1;
  darkpool::transport::SslCtxPtr client_ssl_ctx;
  darkpool::transport::SslPtr ssl;
  darkpool::transport::ClientTlsConfig tls_cfg;

  if (args.use_tls) {
    tls_cfg.mode = args.verify_mode;
    tls_cfg.ca_file = args.ca_file;
    tls_cfg.pinned_fingerprint_hex = args.fingerprint_hex;
    tls_cfg.target_ip = args.host;

    std::string tls_init_err;
    client_ssl_ctx = darkpool::transport::create_client_ssl_ctx(tls_cfg, tls_init_err);
    if (client_ssl_ctx == nullptr) {
      std::cerr << "error: client TLS initialization failed: " << tls_init_err << '\n';
      return 1;
    }

    fd = connect_tcp(args.host, args.port);
    if (fd < 0) {
      return 1;
    }

    SSL* raw_ssl = SSL_new(client_ssl_ctx.get());
    if (raw_ssl == nullptr) {
      std::cerr << "error: failed to allocate client SSL session\n";
      (void)close(fd);
      return 1;
    }
    ssl.reset(raw_ssl);
    SSL_set_app_data(ssl.get(), &tls_cfg);
    if (SSL_set_fd(ssl.get(), fd) != 1) {
      std::cerr << "error: failed to attach socket to SSL session\n";
      (void)close(fd);
      return 1;
    }

    if (!perform_client_tls_handshake(ssl.get(), fd, std::chrono::seconds(5))) {
      (void)close(fd);
      return 1;
    }

    if (args.verify_mode == darkpool::transport::ClientVerifyMode::FingerprintPinning) {
      X509* peer_cert = SSL_get_peer_certificate(ssl.get());
      if (peer_cert == nullptr) {
        std::cerr << "error: server presented no certificate in fingerprint pinning mode\n";
        (void)close(fd);
        return 1;
      }
      darkpool::transport::X509Ptr cert_guard(peer_cert);
      std::string err;
      if (!darkpool::transport::verify_cert_time_validity(peer_cert, err)) {
        std::cerr << "error: certificate validity check failed: " << err << '\n';
        (void)close(fd);
        return 1;
      }
      if (!darkpool::transport::verify_cert_ip_san(peer_cert, args.host, err)) {
        std::cerr << "error: IP SAN verification failed: " << err << '\n';
        (void)close(fd);
        return 1;
      }
      const std::string actual_fp = darkpool::transport::compute_cert_fingerprint_sha256(peer_cert);
      std::array<std::uint8_t, 32> exp_digest{};
      if (!darkpool::transport::parse_fingerprint_sha256(args.fingerprint_hex, exp_digest, err)) {
        std::cerr << "error: invalid pinned fingerprint: " << err << '\n';
        (void)close(fd);
        return 1;
      }
      std::ostringstream oss;
      oss << std::hex << std::setfill('0');
      for (const auto b : exp_digest) {
        oss << std::setw(2) << static_cast<int>(b);
      }
      if (actual_fp != oss.str()) {
        std::cerr << "error: server certificate fingerprint mismatch (pinned "
                  << oss.str() << ", presented " << actual_fp << ")\n";
        (void)close(fd);
        return 1;
      }
    } else if (args.verify_mode == darkpool::transport::ClientVerifyMode::LocalCa) {
      const long verify_res = SSL_get_verify_result(ssl.get());
      if (verify_res != X509_V_OK) {
        std::cerr << "error: certificate verification failed: "
                  << X509_verify_cert_error_string(verify_res) << '\n';
        (void)close(fd);
        return 1;
      }
    }
  } else if (args.is_unix) {
    fd = connect_unix(args.unix_path);
  } else {
    fd = connect_tcp(args.host, args.port);
  }

  if (fd < 0) {
    return 1;
  }

  const darkpool::cli::ClientRole client_role = (args.role_str == "moderator")
      ? darkpool::cli::ClientRole::Moderator
      : darkpool::cli::ClientRole::Player;
  darkpool::cli::ClientState state(client_role);

  FrameStream response_stream;
  const auto exchange = [fd, ssl_ptr = ssl.get(), &state, &response_stream](const Frame& outgoing) -> bool {
    if (!write_all(fd, ssl_ptr, outgoing.serialize())) return false;
    Frame incoming(json{});
    if (!receive_frame(fd, ssl_ptr, response_stream, incoming)) return false;
    std::cout << state.format_response(incoming.payload()) << '\n';
    return true;
  };

  json connect_payload{{"type", "connect"}, {"role", args.role_str}};
  if (client_role == darkpool::cli::ClientRole::Moderator) {
    const char* token = std::getenv("DARKPOOL_MODERATOR_TOKEN");
    if (!token || token[0] == '\0') {
      std::cerr << "error: DARKPOOL_MODERATOR_TOKEN environment variable is required for moderator\n";
      if (ssl != nullptr) {
        (void)SSL_shutdown(ssl.get());
      }
      (void)close(fd);
      return 1;
    }
    connect_payload["token"] = token;
  }

  if (!exchange(Frame(connect_payload))) {
    std::cerr << "server rejected connection or closed it\n";
    if (ssl != nullptr) {
      (void)SSL_shutdown(ssl.get());
    }
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
          if (ssl != nullptr) {
            (void)SSL_shutdown(ssl.get());
          }
          (void)close(fd);
          return 1;
        }
      } catch (const std::exception& error) {
        std::cerr << "network error: " << error.what() << '\n';
      }
    }
  }

  if (ssl != nullptr) {
    (void)SSL_shutdown(ssl.get());
  }
  (void)close(fd);
  return 0;
}
