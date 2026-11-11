#include "darkpool/posix_shm_telemetry.hpp"

#if defined(__linux__)

#include "darkpool/network.hpp"

#include <charconv>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using darkpool::network::json;
using darkpool::telemetry::ServerState;
using darkpool::telemetry::TelemetryObserver;
using darkpool::telemetry::TelemetrySnapshot;

volatile std::sig_atomic_t g_interrupted = 0;

void handle_signal(int) {
  g_interrupted = 1;
}

void print_usage(std::ostream& stream) {
  stream << "Usage: darkpool_watch [--shm NAME] [--sem NAME] [--timeout-ms N] [--once | --follow]\n"
         << "Modes:\n"
         << "  --once    Print one validated JSON telemetry snapshot and exit (default)\n"
         << "  --follow  Continuously stream snapshots on update until stopped\n";
}

bool parse_timeout(std::string_view text, std::chrono::milliseconds& timeout) {
  unsigned value = 0U;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      value == 0U || value > 60'000U) {
    return false;
  }
  timeout = std::chrono::milliseconds(value);
  return true;
}

json snapshot_to_json(const TelemetrySnapshot& s) {
  return json{
      {"sequence", s.sequence},
      {"timestamp_ns", s.timestamp_ns},
      {"server_pid", static_cast<long long>(s.server_pid)},
      {"server_state", darkpool::telemetry::server_state_to_string(s.server_state)},
      {"active_matches", s.active_matches},
      {"connected_players", s.connected_players},
      {"connected_moderators", s.connected_moderators},
      {"match_capacity", s.match_capacity},
      {"players_per_match_capacity", s.players_per_match_capacity},
      {"player_connection_capacity", s.player_connection_capacity},
  };
}

}  // namespace

int main(int argc, char** argv) {
  std::string shm_name = darkpool::telemetry::default_shm_name();
  std::string sem_name = darkpool::telemetry::default_sem_name();
  std::chrono::milliseconds timeout(2000);
  bool follow_mode = false;
  bool mode_specified = false;

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg(argv[i]);
    if (arg == "--help" || arg == "-h") {
      print_usage(std::cout);
      return 0;
    }
    if (arg == "--once") {
      if (mode_specified && follow_mode) {
        std::cerr << "error: cannot specify both --once and --follow\n";
        return 2;
      }
      follow_mode = false;
      mode_specified = true;
      continue;
    }
    if (arg == "--follow") {
      if (mode_specified && !follow_mode) {
        std::cerr << "error: cannot specify both --once and --follow\n";
        return 2;
      }
      follow_mode = true;
      mode_specified = true;
      continue;
    }
    if (arg == "--shm") {
      if (i + 1 >= argc) {
        print_usage(std::cerr);
        return 2;
      }
      shm_name = argv[++i];
      continue;
    }
    if (arg == "--sem") {
      if (i + 1 >= argc) {
        print_usage(std::cerr);
        return 2;
      }
      sem_name = argv[++i];
      continue;
    }
    if (arg == "--timeout-ms") {
      if (i + 1 >= argc || !parse_timeout(argv[++i], timeout)) {
        std::cerr << "error: timeout must be an integer from 1 through 60000 milliseconds\n";
        return 2;
      }
      continue;
    }
    print_usage(std::cerr);
    return 2;
  }

  struct sigaction action {};
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  (void)::sigaction(SIGINT, &action, nullptr);
  (void)::sigaction(SIGTERM, &action, nullptr);

  TelemetryObserver observer;
  std::string open_error;
  if (!observer.open(shm_name, sem_name, open_error)) {
    std::cerr << "error: " << open_error << '\n';
    return 3;
  }

  std::string read_error;
  auto snapshot = observer.read_snapshot(timeout, read_error);
  if (!snapshot) {
    std::cerr << "error: " << read_error << '\n';
    return (read_error.find("timed out") != std::string::npos) ? 4 : 3;
  }

  // Print the current snapshot
  std::cout << snapshot_to_json(*snapshot).dump(2) << '\n';

  if (!follow_mode) {
    return 0;
  }

  if (snapshot->server_state == ServerState::Stopping) {
    return 0;
  }

  std::uint64_t last_sequence = snapshot->sequence;

  while (g_interrupted == 0) {
    // Wait on semaphore for update notification or 500ms timeout to poll sequence
    (void)observer.wait_for_notification(
        std::chrono::milliseconds(500), &g_interrupted);
    if (g_interrupted != 0) {
      break;
    }

    std::string update_error;
    auto updated_snapshot = observer.read_snapshot(timeout, update_error);
    if (!updated_snapshot) {
      if (g_interrupted != 0) {
        break;
      }
      // If server shut down and unlinked or mutex destroyed, exit cleanly
      break;
    }

    if (updated_snapshot->sequence != last_sequence) {
      std::cout << snapshot_to_json(*updated_snapshot).dump(2) << '\n';
      last_sequence = updated_snapshot->sequence;
    }

    if (updated_snapshot->server_state == ServerState::Stopping) {
      return 0;
    }
  }

  return 0;
}

#else

#include <iostream>

int main() {
  std::cerr << "darkpool_watch requires Linux POSIX shared memory and semaphores\n";
  return 1;
}

#endif
