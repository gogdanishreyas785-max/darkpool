#if !defined(__linux__)

#include <iostream>

int main() {
  std::cerr << "darkpool_server requires Linux (epoll/signalfd/unix sockets)\n";
  return 1;
}

#else

#include "darkpool/match_coordinator.hpp"
#include "darkpool/network.hpp"
#include "darkpool/posix_mq_control.hpp"
#include "darkpool/posix_shm_telemetry.hpp"
#include "darkpool/server_runtime.hpp"
#include "darkpool/socket_primitives.hpp"
#include "darkpool/systemd_notify.hpp"
#include "darkpool/worker_process_manager.hpp"

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <sys/epoll.h>

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
  std::string control_mq_queue;
  std::string telemetry_shm;
  std::string telemetry_sem;
  bool open_lobby{false};
  bool enable_worker{false};
  std::string worker_binary;
  std::string worker_req_shm;
  std::string worker_resp_shm;
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
    } else if (arg == "--open-lobby") {
      config.open_lobby = true;
    } else if (arg == "--control-queue") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --control-queue";
        return false;
      }
      config.control_mq_queue = argv[++i];
    } else if (arg == "--telemetry-shm") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --telemetry-shm";
        return false;
      }
      config.telemetry_shm = argv[++i];
    } else if (arg == "--telemetry-sem") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --telemetry-sem";
        return false;
      }
      config.telemetry_sem = argv[++i];
    } else if (arg == "--worker") {
      config.enable_worker = true;
    } else if (arg == "--worker-binary") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --worker-binary";
        return false;
      }
      config.worker_binary = argv[++i];
      config.enable_worker = true;
    } else if (arg == "--worker-req-shm") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --worker-req-shm";
        return false;
      }
      config.worker_req_shm = argv[++i];
      config.enable_worker = true;
    } else if (arg == "--worker-resp-shm") {
      if (i + 1 >= argc) {
        error_msg = "missing argument for --worker-resp-shm";
        return false;
      }
      config.worker_resp_shm = argv[++i];
      config.enable_worker = true;
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
                 "[--control-queue <name>] [--telemetry-shm <name>] [--telemetry-sem <name>] "
                 "[--open-lobby] [--no-tcp] [--no-unix] [port] [socket_path]\n";
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

  darkpool::control::ControlServer control_server(
      config.control_mq_queue.empty() ? darkpool::control::ControlServer::default_queue_name()
                                      : config.control_mq_queue);
  std::string control_error;
  if (!control_server.start(control_error)) {
    std::cerr << "error: control channel startup failed: " << control_error << '\n';
    return 1;
  }

  darkpool::telemetry::TelemetryPublisher telemetry_publisher(
      config.telemetry_shm.empty() ? darkpool::telemetry::default_shm_name()
                                   : config.telemetry_shm,
      config.telemetry_sem.empty() ? darkpool::telemetry::default_sem_name()
                                   : config.telemetry_sem);
  std::string telemetry_error;
  if (!telemetry_publisher.start(
          darkpool::telemetry::TelemetryInput{
              .active_matches = 0U,
              .connected_players = 0U,
              .connected_moderators = 0U,
              .match_capacity = kMaxMatches,
              .players_per_match_capacity = kMaxPlayersPerMatch,
              .player_connection_capacity = kMaxPlayerConnections,
          },
          telemetry_error)) {
    std::cerr << "error: telemetry startup failed: " << telemetry_error << '\n';
    return 1;
  }

  darkpool::systemd::Notifier systemd_notifier;
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
  runtime_cfg.epoll_timeout_ms =
      systemd_notifier.max_poll_timeout_ms(runtime_cfg.epoll_timeout_ms);

  darkpool::network::ServerRuntime runtime(std::move(runtime_cfg));

  std::map<std::uint64_t, ClientKind> session_kinds;
  darkpool::server::MatchCoordinator coordinator;
  if (config.open_lobby) {
    coordinator.set_default_open_lobby(true);
  }
  const char* fast_timers_env = std::getenv("DARKPOOL_FAST_TIMERS");
  if (fast_timers_env && (std::string(fast_timers_env) == "1" || std::string(fast_timers_env) == "true")) {
    coordinator.set_phase_durations(darkpool::server::PhaseDurations{
      .discussion = std::chrono::seconds(2),
      .trading = std::chrono::seconds(2),
      .voting = std::chrono::seconds(2),
      .transition = std::chrono::seconds(1),
      .clue = std::chrono::seconds(1),
    });
  }
  std::size_t players = 0;
  std::size_t moderators = 0;

  // Initialize WorkerProcessManager if requested
  std::unique_ptr<darkpool::ipc::WorkerProcessManager> worker_mgr;
  darkpool::ipc::WorkerProcessConfig w_cfg{};
  std::uint64_t worker_req_id_gen = 0;

  // Track pending client requests routed to worker:
  // correlation_id (request_id) -> struct PendingWorkerRequest
  struct PendingWorkerRequest {
    std::uint64_t client_session_id{0};
    std::string client_request_id;
    std::chrono::steady_clock::time_point submitted_at;
  };
  std::unordered_map<std::uint64_t, PendingWorkerRequest> pending_worker_requests;

  auto fail_all_pending_requests = [&](std::string_view reason) {
    for (const auto& [req_id, pending] : pending_worker_requests) {
      if (runtime.has_session(pending.client_session_id)) {
        json resp_json{
            {"type", "eval_model_response"},
            {"status", "failed"},
            {"error", std::string(reason)}
        };
        if (!pending.client_request_id.empty()) {
          resp_json["request_id"] = pending.client_request_id;
        }
        (void)runtime.send_message(pending.client_session_id, Frame(resp_json));
      }
    }
    pending_worker_requests.clear();
  };

  auto ensure_worker_running = [&]() -> bool {
    if (!config.enable_worker) return false;
    if (worker_mgr && worker_mgr->is_alive() && !worker_mgr->is_faulted()) {
      return true;
    }

    // Worker died or faulted: fail all pending requests first
    fail_all_pending_requests("worker process terminated abnormally");

    if (worker_mgr) {
      if (worker_mgr->server_notify_fd() >= 0) {
        runtime.remove_epoll_event_source(worker_mgr->server_notify_fd());
      }
      worker_mgr->shutdown(std::chrono::milliseconds{200});
      worker_mgr.reset();
    }

    std::string w_err;
    auto mgr_opt = darkpool::ipc::WorkerProcessManager::launch(w_cfg, w_err);
    if (mgr_opt.has_value()) {
      worker_mgr = std::make_unique<darkpool::ipc::WorkerProcessManager>(std::move(*mgr_opt));
      std::cout << "[darkpool_server] (Re)started worker process (PID: " << worker_mgr->worker_pid() << ")\n";

      // Register new worker notify eventfd in epoll
      if (worker_mgr->server_notify_fd() >= 0) {
        std::string epoll_err;
        runtime.add_epoll_event_source(
            worker_mgr->server_notify_fd(),
            [&](std::uint32_t events) {
              if ((events & static_cast<std::uint32_t>(EPOLLIN)) != 0U) {
                if (worker_mgr) {
                  worker_mgr->consume_server_notification();
                  worker_mgr->drain_responses([&](const darkpool::ipc::WorkerResponseMessage& resp) {
                    auto it = pending_worker_requests.find(resp.request_id);
                    if (it != pending_worker_requests.end()) {
                      const auto pending = it->second;
                      pending_worker_requests.erase(it);

                      if (runtime.has_session(pending.client_session_id)) {
                        json resp_json{
                            {"type", "eval_model_response"},
                            {"status", resp.status == 0 ? "success" : "failed"},
                            {"projected_price", resp.projected_price},
                            {"projected_health", resp.projected_health},
                            {"price_delta", resp.price_delta},
                            {"baseline_price", resp.baseline_price},
                            {"worker_pid", resp.worker_pid}
                        };
                        if (!pending.client_request_id.empty()) {
                          resp_json["request_id"] = pending.client_request_id;
                        }
                        runtime.send_message(pending.client_session_id, Frame(resp_json));
                      }
                    }
                  });
                }
              }
            },
            epoll_err);
      }
      return true;
    } else {
      std::cerr << "warning: failed to launch/recover worker process: " << w_err << '\n';
      return false;
    }
  };

  std::string start_err;
  if (!runtime.start(start_err)) {
    std::cerr << "error: server startup failed: " << start_err << '\n';
    return 1;
  }

  if (config.enable_worker) {
    w_cfg = darkpool::ipc::WorkerProcessConfig{
        .worker_binary_path = config.worker_binary.empty() ? "./darkpool_worker" : config.worker_binary,
        .request_shm_name = config.worker_req_shm.empty() ? "/dp_srv_worker_req" : config.worker_req_shm,
        .response_shm_name = config.worker_resp_shm.empty() ? "/dp_srv_worker_resp" : config.worker_resp_shm,
        .queue_capacity = darkpool::ipc::kWorkerQueueCapacity
    };
    ensure_worker_running();
  }

  auto publish_telemetry = [&](bool is_stopping = false) {
    (void)telemetry_publisher.publish(
        darkpool::telemetry::TelemetryInput{
            .active_matches = static_cast<std::uint64_t>(coordinator.active_match_count()),
            .connected_players = static_cast<std::uint64_t>(players),
            .connected_moderators = static_cast<std::uint64_t>(moderators),
            .match_capacity = static_cast<std::uint64_t>(kMaxMatches),
            .players_per_match_capacity = static_cast<std::uint64_t>(kMaxPlayersPerMatch),
            .player_connection_capacity = static_cast<std::uint64_t>(kMaxPlayerConnections),
        },
        is_stopping);
  };

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

    // Clean up any pending worker requests for disconnected session
    for (auto it = pending_worker_requests.begin(); it != pending_worker_requests.end(); ) {
      if (it->second.client_session_id == session_id) {
        it = pending_worker_requests.erase(it);
      } else {
        ++it;
      }
    }

    publish_telemetry();
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
        publish_telemetry();
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
        publish_telemetry();
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

    // Already assigned connection: check for offloaded worker request
    if (frame.payload().is_object() &&
        frame.payload().value("type", std::string{}) == "eval_model") {
      const auto& p = frame.payload();
      std::string client_req_id;
      if (p.contains("request_id")) {
        if (p["request_id"].is_string()) {
          client_req_id = p["request_id"].get<std::string>();
        } else if (p["request_id"].is_number()) {
          client_req_id = p["request_id"].dump();
        }
      }

      auto send_client_error = [&](std::string_view err_msg) {
        json err_resp{
            {"type", "eval_model_response"},
            {"status", "failed"},
            {"error", std::string(err_msg)}
        };
        if (!client_req_id.empty()) {
          err_resp["request_id"] = client_req_id;
        }
        return runtime.send_message(session_id, Frame(err_resp));
      };

      // Input range validation
      if (p.contains("price") && (!p["price"].is_number_integer() || p["price"].get<std::int64_t>() < 1 || p["price"].get<std::int64_t>() > 100000)) {
        return send_client_error("invalid price: must be integer between 1 and 100000");
      }
      if (p.contains("health") && (!p["health"].is_number_integer() || p["health"].get<std::int64_t>() < 0 || p["health"].get<std::int64_t>() > 100)) {
        return send_client_error("invalid health: must be integer between 0 and 100");
      }
      if (p.contains("event_impact") && (!p["event_impact"].is_number_integer() || p["event_impact"].get<std::int64_t>() < -100 || p["event_impact"].get<std::int64_t>() > 100)) {
        return send_client_error("invalid event_impact: must be integer between -100 and 100");
      }
      if (p.contains("demand") && (!p["demand"].is_number_integer() || p["demand"].get<std::int64_t>() < -10000 || p["demand"].get<std::int64_t>() > 10000)) {
        return send_client_error("invalid demand: must be integer between -10000 and 10000");
      }
      if (p.contains("volume") && (!p["volume"].is_number_integer() || p["volume"].get<std::int64_t>() < 0 || p["volume"].get<std::int64_t>() > 1000000)) {
        return send_client_error("invalid volume: must be non-negative integer");
      }
      if (p.contains("sentiment") && (!p["sentiment"].is_number_integer() || p["sentiment"].get<std::int64_t>() < -1000 || p["sentiment"].get<std::int64_t>() > 1000)) {
        return send_client_error("invalid sentiment: must be integer between -1000 and 1000");
      }

      if (!config.enable_worker || !ensure_worker_running()) {
        return send_client_error("worker unavailable");
      }

      const std::uint64_t correlation_id = ++worker_req_id_gen;
      darkpool::ipc::WorkerRequestMessage req{};
      req.request_id = correlation_id;
      req.client_session_id = session_id;
      req.opcode = static_cast<std::int32_t>(darkpool::ipc::WorkerOpcode::EvaluateMarketModel);
      req.price = p.value("price", 10);
      req.health = p.value("health", 5);
      req.event_impact = p.value("event_impact", 0);
      req.demand = p.value("demand", 0);
      req.volume = p.value("volume", 0);
      req.sentiment = p.value("sentiment", 0);
      req.total_supply = p.value("total_supply", 100U);
      req.reserve = p.value("reserve", 50U);

      // Record pending request before sending
      pending_worker_requests[correlation_id] = PendingWorkerRequest{
          .client_session_id = session_id,
          .client_request_id = client_req_id,
          .submitted_at = std::chrono::steady_clock::now()
      };

      std::string send_err;
      const auto send_st = worker_mgr->send_request(req, std::chrono::milliseconds{10}, &send_err);
      if (send_st == darkpool::ipc::QueueOpStatus::Success) {
        // Pending worker response will be delivered asynchronously via epoll
        return true;
      }

      // If push failed, erase pending record and return error
      pending_worker_requests.erase(correlation_id);
      return send_client_error("worker queue full or write timeout");
    }

    const auto response = coordinator.handle(session_id, frame.payload());
    const bool ok = runtime.send_message(session_id, response ? Frame(*response) : frame);
    const auto notifications = coordinator.drain_notifications();
    for (const auto& notif : notifications) {
      for (const auto recipient_id : notif.recipients) {
        runtime.send_message(recipient_id, Frame(notif.payload));
      }
    }
    publish_telemetry();
    return ok;
  });

  runtime.set_tick_handler([&]() {
    systemd_notifier.check_watchdog();
    coordinator.tick();

    // Check worker health & handle timeouts
    if (config.enable_worker && worker_mgr) {
      if (!worker_mgr->is_alive() || worker_mgr->is_faulted()) {
        ensure_worker_running();
      } else if (!pending_worker_requests.empty()) {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = pending_worker_requests.begin(); it != pending_worker_requests.end(); ) {
          if (now - it->second.submitted_at > std::chrono::seconds{5}) {
            const auto pending = it->second;
            it = pending_worker_requests.erase(it);
            if (runtime.has_session(pending.client_session_id)) {
              json err_resp{
                  {"type", "eval_model_response"},
                  {"status", "failed"},
                  {"error", "worker request timed out"}
              };
              if (!pending.client_request_id.empty()) {
                err_resp["request_id"] = pending.client_request_id;
              }
              runtime.send_message(pending.client_session_id, Frame(err_resp));
            }
          } else {
            ++it;
          }
        }
      }
    }

    const auto notifications = coordinator.drain_notifications();
    for (const auto& notif : notifications) {
      for (const auto recipient_id : notif.recipients) {
        runtime.send_message(recipient_id, Frame(notif.payload));
      }
    }
    publish_telemetry();
  });

  std::string epoll_source_error;
  if (!runtime.add_epoll_event_source(
          control_server.descriptor(),
          [&](std::uint32_t events) {
            if ((events & static_cast<std::uint32_t>(EPOLLIN)) != 0U) {
              const bool shutdown_requested = control_server.process_readable([&]() {
                return darkpool::control::StatusSnapshot{
                    .active_matches = coordinator.active_match_count(),
                    .connected_players = players,
                    .match_capacity = kMaxMatches,
                    .players_per_match_capacity = kMaxPlayersPerMatch,
                    .player_connection_capacity = kMaxPlayerConnections,
                };
              });
              if (shutdown_requested) {
                runtime.request_shutdown();
              }
            }
          },
          epoll_source_error)) {
    std::cerr << "error: control channel epoll registration failed: " << epoll_source_error << '\n';
    runtime.stop();
    return 1;
  }
  systemd_notifier.ready();
  publish_telemetry();

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

  const int run_result = runtime.run();
  if (worker_mgr) {
    worker_mgr->shutdown(std::chrono::milliseconds{1000});
  }
  runtime.stop();
  systemd_notifier.stopping();
  telemetry_publisher.stop();
  return run_result;
}

#endif  // !defined(__linux__)
