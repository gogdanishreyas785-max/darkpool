#include "darkpool/posix_shm_telemetry.hpp"

#if defined(__linux__)

#include "darkpool/network.hpp"
#include "darkpool/posix_mq_control.hpp"
#include "load_test_harness_common.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
using darkpool::network::Frame;
using darkpool::network::json;
using darkpool::telemetry::ServerState;
using darkpool::telemetry::TelemetryObserver;
using darkpool::telemetry::TelemetryPublisher;
using darkpool::telemetry::TelemetrySharedMemory;

int g_failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    ++g_failures;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

std::string unique_token() {
  return std::to_string(::getpid()) + "_" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

bool send_bytes(int fd, std::string_view data) {
  std::size_t offset = 0U;
  while (offset < data.size()) {
    const auto sent = ::send(fd, data.data() + offset, data.size() - offset, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (sent == 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

bool read_exact(int fd, char* output, std::size_t size,
                std::chrono::milliseconds timeout = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::size_t offset = 0U;
  while (offset < size) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return false;
    const auto remain = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&descriptor, 1U, static_cast<int>(remain));
    if (ready < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (ready == 0 || (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) return false;
    const auto received = ::recv(fd, output + offset, size - offset, 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (received == 0) return false;
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

std::optional<json> receive_frame(int fd) {
  std::array<char, 4> header{};
  if (!read_exact(fd, header.data(), header.size())) return std::nullopt;
  const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
  const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
  const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
  const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  const std::uint32_t length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
  if (length == 0U || length > darkpool::network::kMaxMessageSize) return std::nullopt;
  std::string bytes(header.data(), header.size());
  bytes.resize(header.size() + static_cast<std::size_t>(length));
  if (!read_exact(fd, bytes.data() + header.size(), static_cast<std::size_t>(length))) return std::nullopt;
  std::size_t consumed = 0U;
  try {
    const auto frame = Frame::parse(bytes, consumed);
    if (!frame || consumed != bytes.size()) return std::nullopt;
    return frame->payload();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

class ChildProcess {
 public:
  ChildProcess() noexcept = default;
  explicit ChildProcess(pid_t pid) noexcept : pid_(pid) {}
  ~ChildProcess() { terminate(SIGKILL); }

  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept : pid_(other.pid_) { other.pid_ = -1; }
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      terminate(SIGKILL);
      pid_ = other.pid_;
      other.pid_ = -1;
    }
    return *this;
  }

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }

  [[nodiscard]] std::optional<int> wait_for(std::chrono::milliseconds timeout = 0ms) {
    if (pid_ < 0) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
      int status = 0;
      const pid_t result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return status;
      }
      if (result < 0 && errno == ECHILD) {
        pid_ = -1;
        return std::nullopt;
      }
      if (timeout > 0ms) {
        (void)::poll(nullptr, 0U, 10);
      }
    } while (std::chrono::steady_clock::now() < deadline);
    return std::nullopt;
  }

  void terminate(int signal = SIGTERM) noexcept {
    if (pid_ >= 0) {
      (void)::kill(pid_, signal);
      int status = 0;
      for (int i = 0; i < 20; ++i) {
        const pid_t result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
          pid_ = -1;
          return;
        }
        (void)::poll(nullptr, 0U, 10);
      }
      (void)::kill(pid_, SIGKILL);
      (void)::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
  }

 private:
  pid_t pid_{-1};
};

struct ExecResult {
  int exit_code{-1};
  std::string output;
};

struct CliChild {
  ChildProcess process;
  int output_fd{-1};
};

CliChild launch_cmd(const std::vector<std::string>& args) {
  int pipe_fds[2] = {-1, -1};
  CHECK(::pipe2(pipe_fds, O_CLOEXEC) == 0);
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    (void)::close(pipe_fds[0]);
    (void)::dup2(pipe_fds[1], STDOUT_FILENO);
    (void)::dup2(pipe_fds[1], STDERR_FILENO);
    (void)::close(pipe_fds[1]);
    std::vector<char*> argv;
    for (const auto& a : args) {
      argv.push_back(const_cast<char*>(a.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    _exit(127);
  }
  (void)::close(pipe_fds[1]);
  return CliChild{ChildProcess(child), pipe_fds[0]};
}

ExecResult collect_cmd(CliChild& child, std::chrono::milliseconds timeout = 5s) {
  ExecResult result;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<char, 1024> buffer{};
  bool pipe_closed = false;
  bool process_exited = false;

  while (std::chrono::steady_clock::now() < deadline) {
    if (!pipe_closed && child.output_fd >= 0) {
      pollfd descriptor{.fd = child.output_fd, .events = POLLIN | POLLHUP, .revents = 0};
      const int ready = ::poll(&descriptor, 1U, 50);
      if (ready > 0) {
        const auto count = ::read(child.output_fd, buffer.data(), buffer.size());
        if (count > 0) {
          result.output.append(buffer.data(), static_cast<std::size_t>(count));
          if (result.output.size() > 16384U) break;
        } else if (count == 0) {
          pipe_closed = true;
          (void)::close(child.output_fd);
          child.output_fd = -1;
        } else if (errno != EAGAIN && errno != EINTR) {
          pipe_closed = true;
        }
      }
    }
    const auto status = child.process.wait_for(10ms);
    if (status) {
      result.exit_code = *status;
      process_exited = true;
    }
    if (process_exited && (pipe_closed || child.output_fd < 0)) return result;
  }
  child.process.terminate(SIGKILL);
  if (child.output_fd >= 0) {
    (void)::close(child.output_fd);
    child.output_fd = -1;
  }
  return result;
}

void test_unit_and_edge_cases() {
  std::cerr << "[shm test] running unit and edge case tests\n";

  // Name validation
  CHECK(!darkpool::telemetry::valid_ipc_name(""));
  CHECK(!darkpool::telemetry::valid_ipc_name("no_leading_slash"));
  CHECK(!darkpool::telemetry::valid_ipc_name("/has/sub/slash"));
  CHECK(!darkpool::telemetry::valid_ipc_name("/invalid char space"));
  CHECK(darkpool::telemetry::valid_ipc_name("/valid_name-1.0"));

  // Check struct layout
  static_assert(std::is_standard_layout_v<TelemetrySharedMemory>);
  CHECK(sizeof(TelemetrySharedMemory) >= 64U);

  // Exclusive creation
  const auto token = unique_token();
  const auto shm_name = "/darkpool_test_shm_" + token;
  const auto sem_name = "/darkpool_test_sem_" + token;

  darkpool::telemetry::TelemetryInput init_input{
      .active_matches = 0U,
      .connected_players = 0U,
      .connected_moderators = 0U,
      .match_capacity = 4U,
      .players_per_match_capacity = 64U,
      .player_connection_capacity = 256U,
  };

  TelemetryPublisher pub1(shm_name, sem_name);
  std::string err1;
  CHECK(pub1.start(init_input, err1));
  CHECK(pub1.running());

  // Second publisher with same names must fail (EEXIST)
  TelemetryPublisher pub2(shm_name, sem_name);
  std::string err2;
  CHECK(!pub2.start(init_input, err2));
  CHECK(err2.find("already exists") != std::string::npos);

  // Observer can open valid publisher
  TelemetryObserver obs;
  std::string obs_err;
  CHECK(obs.open(shm_name, sem_name, obs_err));

  // Initial snapshot has nonzero sequence and running state
  std::string read_err;
  auto init_snapshot = obs.read_snapshot(1000ms, read_err);
  CHECK(init_snapshot.has_value());
  if (init_snapshot) {
    CHECK(init_snapshot->sequence == 1U);
    CHECK(init_snapshot->server_state == ServerState::Running);
  }

  // C. Unchanged snapshot: publishing same input does NOT increment sequence
  CHECK(pub1.publish(init_input));
  auto snap_same = obs.read_snapshot(1000ms, read_err);
  CHECK(snap_same.has_value());
  if (snap_same) {
    CHECK(snap_same->sequence == 1U);
  }

  // Publication with changed input advances sequence and updates integrity
  darkpool::telemetry::TelemetryInput input{
      .active_matches = 2U,
      .connected_players = 8U,
      .connected_moderators = 1U,
      .match_capacity = 4U,
      .players_per_match_capacity = 64U,
      .player_connection_capacity = 256U,
  };
  CHECK(pub1.publish(input));

  // Observer reads updated snapshot
  auto snapshot = obs.read_snapshot(1000ms, read_err);
  CHECK(snapshot.has_value());
  if (snapshot) {
    CHECK(snapshot->sequence == 2U);
    CHECK(snapshot->active_matches == 2U);
    CHECK(snapshot->connected_players == 8U);
    CHECK(snapshot->connected_moderators == 1U);
    CHECK(snapshot->server_state == ServerState::Running);
  }

  // B. Robust mutex owner death recovery: test in separate child process
  std::cerr << "[shm test] testing robust mutex owner death recovery...\n";
  const pid_t locker_pid = ::fork();
  CHECK(locker_pid >= 0);
  if (locker_pid == 0) {
    // Child opens existing shm, locks mutex, and dies without unlocking
    TelemetryObserver child_obs;
    std::string c_err;
    if (child_obs.open(shm_name, sem_name, c_err)) {
      int test_shm_fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
      if (test_shm_fd >= 0) {
        auto* raw = static_cast<TelemetrySharedMemory*>(
            ::mmap(nullptr, sizeof(TelemetrySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, test_shm_fd, 0));
        if (raw != MAP_FAILED) {
          ::pthread_mutex_lock(&raw->mutex);
          // Die while holding lock!
          ::_exit(42);
        }
      }
    }
    ::_exit(1);
  }

  int locker_status = 0;
  ::waitpid(locker_pid, &locker_status, 0);

  // Observer reading after owner death recovers with EOWNERDEAD and accepts valid committed snapshot
  auto snap_after_death = obs.read_snapshot(1000ms, read_err);
  CHECK(snap_after_death.has_value());
  if (snap_after_death) {
    CHECK(snap_after_death->sequence == 2U);
  }

  // Test that invalid integrity is rejected
  int direct_shm = ::shm_open(shm_name.c_str(), O_RDWR, 0);
  CHECK(direct_shm >= 0);
  if (direct_shm >= 0) {
    auto* raw_shm = static_cast<TelemetrySharedMemory*>(
        ::mmap(nullptr, sizeof(TelemetrySharedMemory), PROT_READ | PROT_WRITE, MAP_SHARED, direct_shm, 0));
    CHECK(raw_shm != MAP_FAILED);
    if (raw_shm != MAP_FAILED) {
      ::pthread_mutex_lock(&raw_shm->mutex);
      const auto saved_integrity = raw_shm->payload.integrity;
      raw_shm->payload.integrity = 0xDEADBEEFU;  // Corrupt integrity!
      ::pthread_mutex_unlock(&raw_shm->mutex);

      std::string corrupt_err;
      auto corrupt_snap = obs.read_snapshot(1000ms, corrupt_err);
      CHECK(!corrupt_snap.has_value());
      CHECK(corrupt_err.find("integrity") != std::string::npos);

      // Restore integrity
      ::pthread_mutex_lock(&raw_shm->mutex);
      raw_shm->payload.integrity = saved_integrity;
      ::pthread_mutex_unlock(&raw_shm->mutex);
      (void)::munmap(raw_shm, sizeof(TelemetrySharedMemory));
    }
    (void)::close(direct_shm);
  }

  // Clean shutdown cleans up IPC objects
  obs.close();
  pub1.stop();

  // Observer opening now must fail with missing object
  std::string open_err2;
  CHECK(!obs.open(shm_name, sem_name, open_err2));
  CHECK(open_err2.find("does not exist") != std::string::npos);

  std::cerr << "[shm test] unit and edge case tests passed\n";
}

void test_server_and_watch_integration(const char* server_bin, const char* watch_bin,
                                       const char* ctl_bin) {
  std::cerr << "[shm test] running full server & watch integration test\n";

  const auto token = unique_token();
  const auto shm_name = "/darkpool_test_srv_shm_" + token;
  const auto sem_name = "/darkpool_test_srv_sem_" + token;
  const auto queue_name = darkpool::control::ControlServer::default_queue_name() + "_srv_" + token;
  const auto socket_path = "/tmp/darkpool_shm_srv_" + token + ".sock";
  const std::string mod_token = "mod_test_token_" + token;

  ::setenv("DARKPOOL_MODERATOR_TOKEN", mod_token.c_str(), 1);

  // 1. Launch real server
  std::vector<std::string> server_cmd = {
      server_bin,
      "--socket", socket_path,
      "--no-tcp",
      "--control-queue", queue_name,
      "--telemetry-shm", shm_name,
      "--telemetry-sem", sem_name,
  };

  const pid_t server_pid = ::fork();
  CHECK(server_pid >= 0);
  if (server_pid == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      (void)::dup2(devnull, STDOUT_FILENO);
      (void)::dup2(devnull, STDERR_FILENO);
      (void)::close(devnull);
    }
    std::vector<char*> argv;
    for (const auto& a : server_cmd) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(argv[0], argv.data());
    _exit(127);
  }
  ChildProcess server_child(server_pid);

  // Wait for server to create shm and sem
  const auto start_deadline = std::chrono::steady_clock::now() + 5s;
  bool objects_created = false;
  while (std::chrono::steady_clock::now() < start_deadline) {
    int fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
    if (fd >= 0) {
      (void)::close(fd);
      sem_t* sem = ::sem_open(sem_name.c_str(), 0);
      if (sem != SEM_FAILED) {
        (void)::sem_close(sem);
        objects_created = true;
        break;
      }
    }
    (void)::poll(nullptr, 0U, 10);
  }
  CHECK(objects_created);
  std::cerr << "[shm test] server created shm and semaphore\n";

  // 2. Observer can read initial valid snapshot via `darkpool_watch --once`
  auto watch_once = launch_cmd({watch_bin, "--shm", shm_name, "--sem", sem_name, "--once"});
  auto watch_res = collect_cmd(watch_once);
  CHECK(watch_res.exit_code == 0);
  const auto init_json = json::parse(watch_res.output, nullptr, false);
  CHECK(!init_json.is_discarded());
  CHECK(init_json.value("server_state", "") == "running");
  CHECK(init_json.value("active_matches", 1U) == 0U);
  CHECK(init_json.value("connected_players", 1U) == 0U);
  CHECK(init_json.value("connected_moderators", 1U) == 0U);
  const auto seq_init = init_json.value("sequence", 0ULL);
  CHECK(seq_init >= 1ULL);
  std::cerr << "[shm test] initial snapshot verified: seq=" << seq_init << '\n';

  // 7. Launch `darkpool_watch --follow` in background
  auto watch_follow = launch_cmd({watch_bin, "--shm", shm_name, "--sem", sem_name, "--follow"});
  CHECK(watch_follow.process.pid() >= 0);

  // Connect player via UNIX socket
  const int player_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(player_fd >= 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
  CHECK(::connect(player_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

  // Authenticate player
  CHECK(send_bytes(player_fd, Frame(json{{"type", "connect"}, {"role", "player"}}).serialize()));
  const auto player_ack = receive_frame(player_fd);
  CHECK(player_ack.has_value());
  CHECK(player_ack->value("type", "") == "connect" && player_ack->value("role", "") == "player");

  // 3. Player connection changes connected-player count
  auto watch_player = launch_cmd({watch_bin, "--shm", shm_name, "--sem", sem_name, "--once"});
  auto player_snap = collect_cmd(watch_player);
  CHECK(player_snap.exit_code == 0);
  const auto player_json = json::parse(player_snap.output, nullptr, false);
  CHECK(!player_json.is_discarded());
  CHECK(player_json.value("connected_players", 0U) == 1U);
  const auto seq_player = player_json.value("sequence", 0ULL);
  CHECK(seq_player > seq_init);
  std::cerr << "[shm test] player connection verified: players=1, seq=" << seq_player << '\n';

  // Connect moderator via UNIX socket
  const int mod_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(mod_fd >= 0);
  CHECK(::connect(mod_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);

  // 4. Authenticate moderator
  CHECK(send_bytes(mod_fd, Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", mod_token}}).serialize()));
  const auto mod_ack = receive_frame(mod_fd);
  CHECK(mod_ack.has_value());
  CHECK(mod_ack->value("type", "") == "connect" && mod_ack->value("role", "") == "moderator");

  auto watch_mod = launch_cmd({watch_bin, "--shm", shm_name, "--sem", sem_name, "--once"});
  auto mod_snap = collect_cmd(watch_mod);
  CHECK(mod_snap.exit_code == 0);
  const auto mod_json = json::parse(mod_snap.output, nullptr, false);
  CHECK(!mod_json.is_discarded());
  CHECK(mod_json.value("connected_moderators", 0U) == 1U);
  const auto seq_mod = mod_json.value("sequence", 0ULL);
  CHECK(seq_mod > seq_player);
  std::cerr << "[shm test] moderator connection verified: mods=1, seq=" << seq_mod << '\n';

  // 5. Match creation changes active-match count
  CHECK(send_bytes(mod_fd, Frame(json{{"type", "create_match"}, {"player_count", 3}}).serialize()));
  const auto create_ack = receive_frame(mod_fd);
  CHECK(create_ack.has_value());
  CHECK(create_ack->value("type", "") == "match_created");

  auto watch_match = launch_cmd({watch_bin, "--shm", shm_name, "--sem", sem_name, "--once"});
  auto match_snap = collect_cmd(watch_match);
  CHECK(match_snap.exit_code == 0);
  const auto match_json = json::parse(match_snap.output, nullptr, false);
  CHECK(!match_json.is_discarded());
  CHECK(match_json.value("active_matches", 0U) == 1U);
  const auto seq_match = match_json.value("sequence", 0ULL);
  // 6. Verify strictly increasing sequence numbers
  CHECK(seq_match > seq_mod);
  std::cerr << "[shm test] match creation verified: matches=1, seq=" << seq_match << '\n';

  // 9. Existing POSIX MQ ping and status continue to work
  auto ctl_ping = launch_cmd({ctl_bin, "--queue", queue_name, "ping"});
  auto ping_res = collect_cmd(ctl_ping);
  CHECK(ping_res.exit_code == 0);
  const auto ping_json = json::parse(ping_res.output, nullptr, false);
  CHECK(ping_json.value("ok", false));

  auto ctl_status = launch_cmd({ctl_bin, "--queue", queue_name, "status"});
  auto status_res = collect_cmd(ctl_status);
  CHECK(status_res.exit_code == 0);
  const auto status_json = json::parse(status_res.output, nullptr, false);
  CHECK(status_json.value("ok", false));
  CHECK(status_json["result"].value("active_matches", 0U) == 1U);
  CHECK(status_json["result"].value("connected_players", 0U) == 1U);
  std::cerr << "[shm test] POSIX MQ control ping and status verified\n";

  // 10. Gameplay socket communication remains functional
  const json echo_msg{{"type", "gameplay_probe"}, {"nonce", token}};
  CHECK(send_bytes(player_fd, Frame(echo_msg).serialize()));
  const auto echo_res = receive_frame(player_fd);
  CHECK(echo_res.has_value() && *echo_res == echo_msg);
  std::cerr << "[shm test] gameplay echo frame verified\n";

  // Close client sockets
  (void)::close(player_fd);
  (void)::close(mod_fd);

  // 8. Orderly shutdown via darkpool_ctl shutdown publishes stopping state and cleans up owned IPC resources
  std::cerr << "[shm test] sending shutdown command via darkpool_ctl\n";
  auto ctl_shutdown = launch_cmd({ctl_bin, "--queue", queue_name, "shutdown"});
  auto shutdown_res = collect_cmd(ctl_shutdown);
  CHECK(shutdown_res.exit_code == 0);
  const auto shutdown_json = json::parse(shutdown_res.output, nullptr, false);
  CHECK(shutdown_json.value("ok", false) == true);
  CHECK(shutdown_json.value("operation", std::string{}) == "shutdown");
  CHECK(shutdown_json["result"].value("shutdown_requested", false) == true);

  // Wait for `darkpool_watch --follow` to detect stopping state and exit cleanly (code 0)
  auto follow_res = collect_cmd(watch_follow, 5s);
  std::cerr << "[shm test] follow exit_code=" << follow_res.exit_code << '\n';
  CHECK(follow_res.exit_code == 0);
  CHECK(follow_res.output.find("stopping") != std::string::npos);

  // D. Verify follow mode observed intermediate updates (e.g. sequence changes, player/match counts)
  CHECK(follow_res.output.find("\"connected_players\": 1") != std::string::npos ||
        follow_res.output.find("\"active_matches\": 1") != std::string::npos);

  // Ensure server exits gracefully after SIGTERM
  (void)server_child.wait_for(5s);

  // Verify IPC cleanup
  errno = 0;
  int test_shm = ::shm_open(shm_name.c_str(), O_RDWR, 0);
  CHECK(test_shm < 0 && errno == ENOENT);
  if (test_shm >= 0) (void)::close(test_shm);

  errno = 0;
  sem_t* test_sem = ::sem_open(sem_name.c_str(), 0);
  CHECK(test_sem == SEM_FAILED && errno == ENOENT);
  if (test_sem != SEM_FAILED) (void)::sem_close(test_sem);

  (void)::unlink(socket_path.c_str());
  std::cerr << "[shm test] integration test completed successfully\n";
}


void test_load_harness_cleanup_after_forced_server_exit(const std::string& server_bin) {
  using darkpool::load_test::connect_unix;
  using darkpool::load_test::generate_unique_socket_path;
  using darkpool::load_test::spawn_server_unix;
  using darkpool::load_test::stop_server_unix;
  using darkpool::load_test::telemetry_sem_for_test_server;
  using darkpool::load_test::telemetry_shm_for_test_server;

  const auto socket_path = generate_unique_socket_path();
  const auto server_pid = spawn_server_unix(server_bin, socket_path, "test-telemetry-cleanup-token");
  CHECK(server_pid > 0);
  if (server_pid <= 0) {
    return;
  }

  const auto shm_name = telemetry_shm_for_test_server(server_pid);
  const auto sem_name = telemetry_sem_for_test_server(server_pid);

  // Listener readiness also proves telemetry initialization has completed.
  const int probe_fd = connect_unix(socket_path);
  CHECK(probe_fd >= 0);
  if (probe_fd >= 0) {
    (void)::close(probe_fd);
  }

  errno = 0;
  const int shm_fd = ::shm_open(shm_name.c_str(), O_RDWR, 0);
  CHECK(shm_fd >= 0);
  if (shm_fd >= 0) {
    (void)::close(shm_fd);
  }

  errno = 0;
  sem_t* semaphore = ::sem_open(sem_name.c_str(), 0);
  CHECK(semaphore != SEM_FAILED);
  if (semaphore != SEM_FAILED) {
    (void)::sem_close(semaphore);
  }

  // Simulate the abrupt process death exercised by the L6 cleanup regression.
  CHECK(::kill(server_pid, SIGKILL) == 0);
  stop_server_unix(server_pid, socket_path);

  errno = 0;
  const int stale_shm = ::shm_open(shm_name.c_str(), O_RDWR, 0);
  CHECK(stale_shm < 0 && errno == ENOENT);
  if (stale_shm >= 0) {
    (void)::close(stale_shm);
  }

  errno = 0;
  sem_t* stale_sem = ::sem_open(sem_name.c_str(), 0);
  CHECK(stale_sem == SEM_FAILED && errno == ENOENT);
  if (stale_sem != SEM_FAILED) {
    (void)::sem_close(stale_sem);
  }
  std::cerr << "[shm test] forced-exit test-server IPC cleanup verified\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::cerr << "Usage: darkpool_posix_shm_tests <server_bin> <watch_bin> <ctl_bin>\n";
    return 2;
  }

  test_unit_and_edge_cases();
  test_server_and_watch_integration(argv[1], argv[2], argv[3]);
  test_load_harness_cleanup_after_forced_server_exit(argv[1]);

  if (g_failures == 0) {
    std::cout << "POSIX shared memory telemetry tests passed.\n";
    return 0;
  }

  std::cerr << "POSIX shared memory telemetry tests failed with " << g_failures << " failures.\n";
  return 1;
}

#else

#include <iostream>

int main() {
  std::cerr << "darkpool_posix_shm_tests requires Linux\n";
  return 1;
}

#endif
