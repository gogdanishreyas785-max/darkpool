#include "darkpool/cli.hpp"
#include "darkpool/client_runtime.hpp"
#include "darkpool/network.hpp"
#include "darkpool/socket_primitives.hpp"
#include "darkpool/tls_transport.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <poll.h>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::json;

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
    if (!args.ca_file.empty() && !args.fingerprint_hex.empty()) {
      std::cerr << "error: mutually exclusive verification options: cannot specify both --ca-cert and --server-fingerprint\n";
      return 2;
    }
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
      if (!darkpool::network::parse_port(positional[1].c_str(), args.port)) {
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
      if (!darkpool::network::parse_port(positional[1].c_str(), args.port)) {
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

  darkpool::network::ClientConnection conn;
  std::string conn_err;

  if (args.use_tls) {
    darkpool::transport::ClientTlsConfig tls_cfg;
    tls_cfg.mode = args.verify_mode;
    tls_cfg.ca_file = args.ca_file;
    tls_cfg.pinned_fingerprint_hex = args.fingerprint_hex;
    tls_cfg.target_ip = args.host;

    if (!conn.connect_tls(args.host, args.port, tls_cfg, conn_err)) {
      std::cerr << "error: " << conn_err << '\n';
      return 1;
    }
  } else if (args.is_unix) {
    if (!conn.connect_unix(args.unix_path, conn_err)) {
      std::cerr << "error: " << conn_err << '\n';
      return 1;
    }
  } else {
    if (!conn.connect_tcp(args.host, args.port, conn_err)) {
      std::cerr << "error: " << conn_err << '\n';
      return 1;
    }
  }

  const darkpool::cli::ClientRole client_role = (args.role_str == "moderator")
      ? darkpool::cli::ClientRole::Moderator
      : darkpool::cli::ClientRole::Player;
  darkpool::cli::ClientState state(client_role);

  const auto is_push_notification = [](const json& payload) {
    if (!payload.is_object()) return false;
    const auto type = payload.value("type", std::string{});
    return type == "server_event" || type == "market_snapshot";
  };

  const auto exchange = [&conn, &state, &is_push_notification](const Frame& outgoing) -> bool {
    if (!conn.send_frame(outgoing)) return false;
    while (true) {
      Frame incoming(json{});
      if (!conn.receive_frame(incoming)) return false;
      const auto& payload = incoming.payload();
      const auto formatted = state.format_response(payload);
      if (!formatted.empty()) {
        std::cout << formatted << std::endl;
      }
      if (!is_push_notification(payload)) {
        return true;
      }
    }
  };

  json connect_payload{{"type", "connect"}, {"role", args.role_str}};
  if (client_role == darkpool::cli::ClientRole::Moderator) {
    const char* token = std::getenv("DARKPOOL_MODERATOR_TOKEN");
    if (!token || token[0] == '\0') {
      std::cerr << "error: DARKPOOL_MODERATOR_TOKEN environment variable is required for moderator\n";
      conn.close();
      return 1;
    }
    connect_payload["token"] = token;
  }

  if (!exchange(Frame(connect_payload))) {
    std::cerr << "server rejected connection or closed it\n";
    conn.close();
    return 1;
  }

  const bool is_interactive = isatty(fileno(stdin)) != 0;
  if (is_interactive) {
    std::cout << "Type 'help' for commands, 'status' for game state.\n";
  }

  std::string input_buffer;
  bool stdin_eof = false;

  const auto drain_incoming_notifications = [&]() {
    Frame incoming(json{});
    while (conn.is_open()) {
      if (conn.has_pending_frame()) {
        if (!conn.receive_frame(incoming)) break;
      } else if (conn.poll_frame(incoming, std::chrono::milliseconds(0))) {
        // frame received
      } else {
        break;
      }
      const auto formatted = state.format_response(incoming.payload());
      if (!formatted.empty()) {
        std::cout << formatted << std::endl;
        if (is_interactive) {
          std::cout << state.prompt() << input_buffer << std::flush;
        }
      }
    }
  };

  const auto read_next_line = [&](std::string& out_line) -> bool {
    while (true) {
      const auto nl = input_buffer.find('\n');
      if (nl != std::string::npos) {
        out_line = input_buffer.substr(0, nl);
        if (!out_line.empty() && out_line.back() == '\r') {
          out_line.pop_back();
        }
        input_buffer.erase(0, nl + 1);
        return true;
      }
      if (stdin_eof) {
        if (!input_buffer.empty()) {
          out_line = input_buffer;
          if (!out_line.empty() && out_line.back() == '\r') {
            out_line.pop_back();
          }
          input_buffer.clear();
          return true;
        }
        return false;
      }

      drain_incoming_notifications();

      nfds_t nfds = 1;
      pollfd pfds[2];
      pfds[0].fd = fileno(stdin);
      pfds[0].events = POLLIN;
      if (conn.is_open()) {
        pfds[1].fd = conn.native_handle();
        pfds[1].events = POLLIN;
        nfds = 2;
      }

      int timeout_ms = conn.has_pending_data() ? 0 : 50;
      const int pr = ::poll(pfds, nfds, timeout_ms);
      if (pr < 0) {
        if (errno == EINTR) continue;
        return false;
      }

      if (nfds == 2 && (pfds[1].revents & (POLLIN | POLLERR | POLLHUP))) {
        drain_incoming_notifications();
        if (!conn.is_open()) {
          return false;
        }
      }

      if (pfds[0].revents & (POLLIN | POLLERR | POLLHUP)) {
        char buf[512];
        const auto count = ::read(fileno(stdin), buf, sizeof(buf));
        if (count > 0) {
          input_buffer.append(buf, static_cast<std::size_t>(count));
        } else if (count == 0) {
          stdin_eof = true;
        } else {
          if (errno != EINTR) {
            stdin_eof = true;
          }
        }
      }
    }
  };

  drain_incoming_notifications();

  std::string line;
  while (true) {
    if (is_interactive) {
      std::cout << state.prompt() << std::flush;
    }
    if (!read_next_line(line)) break;

    auto cmd = state.process_input(line);
    while (cmd.prompt_for_input) {
      if (is_interactive) {
        std::cout << *cmd.prompt_for_input << std::flush;
      }
      std::string prompt_response;
      if (!read_next_line(prompt_response)) {
        cmd = darkpool::cli::CommandResult{};
        break;
      }
      cmd = state.process_prompt_response(prompt_response);
    }

    if (!cmd.local_message.empty()) {
      std::cout << cmd.local_message << std::endl;
    }
    if (cmd.should_exit) {
      break;
    }
    if (cmd.send_frame) {
      try {
        if (!exchange(Frame(cmd.payload))) {
          std::cerr << "connection closed while awaiting response\n";
          conn.close();
          return 1;
        }
      } catch (const std::exception& error) {
        std::cerr << "network error: " << error.what() << '\n';
      }
    }
  }

  conn.close();
  return 0;
}
