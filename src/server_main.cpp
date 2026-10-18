#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "darkpool_server requires Linux (epoll/signalfd/unix sockets)\n";
  return 1;
}

#else

#include "darkpool/match_coordinator.hpp"
#include "darkpool/network.hpp"
#include "darkpool/server_runtime.hpp"
#include "darkpool/socket_primitives.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <string>
#include <string_view>

namespace {

using darkpool::network::Frame;
using darkpool::network::json;

constexpr std::uint16_t kDefaultPort = 4782;
constexpr std::uint16_t kDefaultTlsPort = 4783;
constexpr std::size_t kMaxMatches = 4;
constexpr std::size_t kMaxPlayersPerMatch = darkpool::kMaxPlayerCount;
constexpr std::size_t kMaxPlayerConnections = kMaxMatches * kMaxPlayersPerMatch;
constexpr std::size_t kMaxModerators = kMaxMatches;
constexpr std::size_t kMaxConnections = kMaxPlayerConnections + kMaxModerators;

enum class ClientKind { Unassigned, Player, Moderator };

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

bool parse_server_args(int argc, char** argv, ServerConfig& config, std::string& error_msg) {
  if (argc == 1) {
    config.enable_tcp = true;
    config.tcp_port = kDefaultPort;
    config.bind_address = "127.0.0.1";
    config.enable_unix = true;
    const char* env_path = std::getenv("DARKPOOL_SOCKET_PATH");
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
      if (!darkpool::network::parse_port(argv[++i], config.tcp_port)) {
        error_msg = "invalid port argument";
        return false;
      }
      config.enable_tcp = true;
    } else if (arg.rfind("--port=", 0) == 0) {
      std::uint16_t p = 0;
      if (!darkpool::network::parse_port(std::string(arg.substr(7)).c_str(), p)) {
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
      if (!darkpool::network::validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.bind_address = addr;
      config.explicit_bind_address = true;
      config.enable_tcp = true;
    } else if (arg.rfind("--bind-address=", 0) == 0) {
      const std::string addr = std::string(arg.substr(15));
      if (!darkpool::network::validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.bind_address = addr;
      config.explicit_bind_address = true;
      config.enable_tcp = true;
    } else if (arg.rfind("-b=", 0) == 0) {
      const std::string addr = std::string(arg.substr(3));
      if (!darkpool::network::validate_bind_address(addr, error_msg)) {
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
      if (!darkpool::network::validate_bind_address(addr, error_msg)) {
        return false;
      }
      config.tls_bind_address = addr;
      config.explicit_tls_bind_address = true;
      config.enable_tls = true;
    } else if (arg.rfind("--tls-bind-address=", 0) == 0) {
      const std::string addr = std::string(arg.substr(19));
      if (!darkpool::network::validate_bind_address(addr, error_msg)) {
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
      if (!darkpool::network::parse_port(argv[++i], config.tls_port)) {
        error_msg = "invalid port argument for --tls-port";
        return false;
      }
      config.enable_tls = true;
    } else if (arg.rfind("--tls-port=", 0) == 0) {
      std::uint16_t p = 0;
      if (!darkpool::network::parse_port(std::string(arg.substr(11)).c_str(), p)) {
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
      if (darkpool::network::parse_port(argv[i], p)) {
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

  const char* env_path = std::getenv("DARKPOOL_SOCKET_PATH");
  if (!explicit_unix && env_path && env_path[0] != '\0') {
    config.enable_unix = true;
    config.unix_path = env_path;
  }

  return config.enable_tcp || config.enable_unix || config.enable_tls;
}

}  // namespace

int main(int argc, char** argv) {
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
    if (!darkpool::network::is_address_assigned_to_local_interface(config.bind_address)) {
      std::cerr << "error: requested bind address " << config.bind_address
                << " is not assigned to any local network interface\n";
      return 1;
    }
  }

  if (config.enable_tls) {
    if (!darkpool::network::is_address_assigned_to_local_interface(config.tls_bind_address)) {
      std::cerr << "error: requested TLS bind address " << config.tls_bind_address
                << " is not assigned to any local network interface\n";
      return 1;
    }
  }

  darkpool::network::ServerRuntimeConfig runtime_cfg;
  runtime_cfg.enable_tcp = config.enable_tcp;
  runtime_cfg.tcp_port = config.tcp_port;
  runtime_cfg.bind_address = config.bind_address;

  runtime_cfg.enable_unix = config.enable_unix;
  runtime_cfg.unix_path = config.unix_path;

  runtime_cfg.enable_tls = config.enable_tls;
  runtime_cfg.tls_port = config.tls_port;
  runtime_cfg.tls_bind_address = config.tls_bind_address;
  runtime_cfg.tls_cert_path = config.tls_cert_path;
  runtime_cfg.tls_key_path = config.tls_key_path;

  runtime_cfg.max_connections = kMaxConnections;

  darkpool::network::ServerRuntime runtime(std::move(runtime_cfg));

  std::map<std::uint64_t, ClientKind> session_kinds;
  darkpool::server::MatchCoordinator coordinator;
  std::size_t players = 0;
  std::size_t moderators = 0;

  runtime.set_disconnect_handler([&](std::uint64_t session_id) {
    const auto found = session_kinds.find(session_id);
    if (found != session_kinds.end()) {
      if (found->second == ClientKind::Player) {
        --players;
      } else if (found->second == ClientKind::Moderator) {
        --moderators;
      }
      session_kinds.erase(found);
    }
    coordinator.unregister_session(session_id);
  });

  runtime.set_message_handler([&](std::uint64_t session_id, const Frame& frame) -> bool {
    auto& kind = session_kinds[session_id];  // Defaults to ClientKind::Unassigned

    if (kind == ClientKind::Unassigned) {
      const auto& payload = frame.payload();
      if (!payload.is_object() ||
          payload.value("type", std::string{}) != "connect") {
        return false;
      }
      const auto role = payload.value("role", std::string{});
      if (role == "player") {
        if (players >= kMaxPlayerConnections) {
          return false;
        }
        kind = ClientKind::Player;
        ++players;
        coordinator.register_session(session_id, darkpool::server::SessionRole::Player);
        runtime.set_authenticated(session_id, true);
        return runtime.send_message(session_id, Frame(json{{"type", "connect"}, {"role", "player"}}));
      } else if (role == "moderator") {
        const char* server_token = std::getenv("DARKPOOL_MODERATOR_TOKEN");
        if (!server_token || server_token[0] == '\0') {
          return false;
        }
        if (!payload.contains("token") || !payload["token"].is_string()) {
          return false;
        }
        const auto token = payload["token"].get<std::string>();
        if (!darkpool::network::constant_time_equals(token, server_token)) {
          return false;
        }
        if (moderators >= kMaxModerators) {
          return false;
        }
        kind = ClientKind::Moderator;
        ++moderators;
        coordinator.register_session(session_id, darkpool::server::SessionRole::Moderator);
        runtime.set_authenticated(session_id, true);
        return runtime.send_message(session_id, Frame(json{{"type", "connect"}, {"role", "moderator"}}));
      } else {
        return false;
      }
    }

    // Already assigned connection: reject any subsequent connect frame
    if (frame.payload().is_object() &&
        frame.payload().value("type", std::string{}) == "connect") {
      return false;
    }

    const auto response = coordinator.handle(session_id, frame.payload());
    const bool ok = runtime.send_message(session_id, response ? Frame(*response) : frame);
    const auto notifications = coordinator.drain_notifications();
    for (const auto& notif : notifications) {
      for (const auto recipient_id : notif.recipients) {
        runtime.send_message(recipient_id, Frame(notif.payload));
      }
    }
    return ok;
  });

  runtime.set_tick_handler([&]() {
    coordinator.tick();
    const auto notifications = coordinator.drain_notifications();
    for (const auto& notif : notifications) {
      for (const auto recipient_id : notif.recipients) {
        runtime.send_message(recipient_id, Frame(notif.payload));
      }
    }
  });

  std::string start_err;
  if (!runtime.start(start_err)) {
    std::cerr << "error: server startup failed: " << start_err << '\n';
    return 1;
  }

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

  return runtime.run();
}

#endif  // !defined(__linux__)
