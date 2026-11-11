#include "darkpool/posix_mq_control.hpp"

#if defined(__linux__)

#include "darkpool/network.hpp"

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
#include <utility>
#include <unistd.h>
#include <vector>

namespace {

using namespace std::chrono_literals;
using darkpool::control::ControlServer;
using darkpool::control::MessageQueue;
using darkpool::control::QueueLimits;
using darkpool::network::Frame;
using darkpool::network::json;

int failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

std::string unique_token() {
  return std::to_string(::getpid()) + "_" +
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string server_queue_name(std::string_view suffix) {
  return ControlServer::default_queue_name() + "_" + std::string(suffix) + "_" + unique_token();
}

std::string reply_queue_name(std::string_view request_id, pid_t pid = ::getpid()) {
  return "/darkpool_ctl_reply_" + std::to_string(::geteuid()) + "_" +
      std::to_string(static_cast<long long>(pid)) + "_" + std::string(request_id);
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

void send_request(MessageQueue& server, MessageQueue& reply, std::string_view id,
                  std::string_view operation, std::optional<int> version = 1,
                  bool add_unknown_field = false) {
  json request{{"request_id", id}, {"reply_to", reply.name()}, {"operation", operation}};
  if (version) request["version"] = *version;
  if (add_unknown_field) request["unused"] = "ignored";
  int error_number = 0;
  CHECK(server.send(request.dump(), 0U, error_number));
}

std::optional<json> receive_json(MessageQueue& reply, std::chrono::milliseconds timeout = 500ms) {
  pollfd descriptor{.fd = reply.descriptor(), .events = POLLIN, .revents = 0};
  const int ready = ::poll(&descriptor, 1U, static_cast<int>(timeout.count()));
  if (ready <= 0 || (descriptor.revents & POLLIN) == 0) return std::nullopt;
  unsigned priority = 0U;
  int error_number = 0;
  const auto bytes = reply.receive(priority, error_number);
  if (!bytes) return std::nullopt;
  auto response = json::parse(bytes->begin(), bytes->end(), nullptr, false);
  if (response.is_discarded()) return std::nullopt;
  return response;
}

void test_queue_ownership_nonblocking_and_priority() {
  std::string error;
  const auto name = "/darkpool_queue_test_" + unique_token();
  auto queue = MessageQueue::create_exclusive(name, O_RDWR, QueueLimits{1, 16}, error);
  CHECK(queue.has_value());
  if (!queue) return;
  CHECK(queue->has_permissions(::geteuid(), 0600));
  mq_attr attributes{};
  int error_number = 0;
  CHECK(queue->get_attributes(attributes, error_number));
  CHECK(attributes.mq_maxmsg == 1);
  CHECK(attributes.mq_msgsize == 16);
  CHECK((attributes.mq_flags & O_NONBLOCK) != 0);

  CHECK(queue->send("first", 0U, error_number));
  CHECK(!queue->send("second", 0U, error_number));
  CHECK(error_number == EAGAIN || error_number == EWOULDBLOCK);
  unsigned priority = 0U;
  CHECK(queue->receive(priority, error_number) == "first");
  CHECK(!queue->receive(priority, error_number));
  CHECK(error_number == EAGAIN || error_number == EWOULDBLOCK);
  CHECK(!queue->send(std::string(17U, 'x'), 0U, error_number));
  CHECK(error_number == EMSGSIZE);

  const auto ordered_name = "/darkpool_queue_order_" + unique_token();
  auto ordered = MessageQueue::create_exclusive(ordered_name, O_RDWR, QueueLimits{4, 32}, error);
  CHECK(ordered.has_value());
  if (!ordered) return;
  CHECK(ordered->send("low-one", 1U, error_number));
  CHECK(ordered->send("high", 7U, error_number));
  CHECK(ordered->send("low-two", 1U, error_number));
  CHECK(ordered->receive(priority, error_number) == "high" && priority == 7U);
  CHECK(ordered->receive(priority, error_number) == "low-one" && priority == 1U);
  CHECK(ordered->receive(priority, error_number) == "low-two" && priority == 1U);
}

void test_control_server_validation_and_duplicate_requests() {
  const auto name = server_queue_name("unit");
  ControlServer server(name);
  std::string error;
  CHECK(server.start(error));
  CHECK(MessageQueue::valid_name(name));
  CHECK(ControlServer::valid_server_queue_name(name));
  CHECK(!ControlServer::valid_server_queue_name("/someone_elses_queue"));
  CHECK(!ControlServer::valid_reply_queue_name("/darkpool_ctl_reply_0_1_not-a-nonce"));

  ControlServer collision(name);
  CHECK(!collision.start(error));
  auto inbox = MessageQueue::open_existing(name, O_WRONLY, error);
  CHECK(inbox.has_value());
  if (!inbox) return;
  const std::string id(32U, 'a');
  auto reply = MessageQueue::create_exclusive(reply_queue_name(id), O_RDONLY,
                                               QueueLimits{1, darkpool::control::kMaxMessageBytes}, error);
  CHECK(reply.has_value());
  if (!reply) return;

  send_request(*inbox, *reply, id, "ping");
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const auto ping = receive_json(*reply);
  CHECK(ping.has_value());
  CHECK(ping && (*ping)["ok"] == true);
  CHECK(ping && (*ping)["request_id"] == id);
  CHECK(ping && (*ping)["result"]["server_available"] == true);

  send_request(*inbox, *reply, id, "ping");
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const auto duplicate = receive_json(*reply);
  CHECK(duplicate == ping);

  send_request(*inbox, *reply, id, "status");
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const auto conflict = receive_json(*reply);
  CHECK(conflict && (*conflict)["error"] == "request_id_conflict");

  const std::string status_id(32U, 'b');
  auto status_reply = MessageQueue::create_exclusive(reply_queue_name(status_id), O_RDONLY,
                                                      QueueLimits{1, darkpool::control::kMaxMessageBytes}, error);
  CHECK(status_reply.has_value());
  if (!status_reply) return;
  send_request(*inbox, *status_reply, status_id, "status");
  server.process_readable([] {
    return darkpool::control::StatusSnapshot{.active_matches = 2,
        .connected_players = 17, .match_capacity = 4,
        .players_per_match_capacity = 64, .player_connection_capacity = 256};
  });
  const auto status = receive_json(*status_reply);
  CHECK(status && (*status)["result"]["active_matches"] == 2U);
  CHECK(status && (*status)["result"]["connected_players"] == 17U);
  if (status && status->contains("result")) {
    const auto& result = (*status)["result"];
    CHECK(result.size() == 8U);
    CHECK(result.contains("server_available") && result.contains("active_matches") &&
          result.contains("connected_players") && result.contains("match_capacity") &&
          result.contains("players_per_match_capacity") &&
          result.contains("player_connection_capacity") &&
          result.contains("control_channel") && result.contains("transport"));
  }

  const std::string bad_version_id(32U, 'c');
  auto version_reply = MessageQueue::create_exclusive(reply_queue_name(bad_version_id), O_RDONLY,
                                                       QueueLimits{1, darkpool::control::kMaxMessageBytes}, error);
  CHECK(version_reply.has_value());
  if (!version_reply) return;
  send_request(*inbox, *version_reply, bad_version_id, "ping", 99);
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const auto unsupported = receive_json(*version_reply);
  CHECK(unsupported && (*unsupported)["error"] == "unsupported_version");

  const std::string unknown_id(32U, 'd');
  auto unknown_reply = MessageQueue::create_exclusive(reply_queue_name(unknown_id), O_RDONLY,
                                                       QueueLimits{1, darkpool::control::kMaxMessageBytes}, error);
  CHECK(unknown_reply.has_value());
  if (!unknown_reply) return;
  send_request(*inbox, *unknown_reply, unknown_id, "execute_shell");
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const auto invalid_operation = receive_json(*unknown_reply);
  CHECK(invalid_operation && (*invalid_operation)["error"] == "unsupported_operation");

  const std::string malformed_id(32U, 'e');
  auto malformed_reply = MessageQueue::create_exclusive(reply_queue_name(malformed_id), O_RDONLY,
                                                         QueueLimits{1, darkpool::control::kMaxMessageBytes}, error);
  CHECK(malformed_reply.has_value());
  if (!malformed_reply) return;
  int error_number = 0;
  CHECK(inbox->send("{malformed", 0U, error_number));
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  mq_attr malformed_attributes{};
  CHECK(malformed_reply->get_attributes(malformed_attributes, error_number));
  CHECK(malformed_attributes.mq_curmsgs == 0);

  const std::string extra_id(32U, 'f');
  auto extra_reply = MessageQueue::create_exclusive(reply_queue_name(extra_id), O_RDONLY,
                                                     QueueLimits{1, darkpool::control::kMaxMessageBytes}, error);
  CHECK(extra_reply.has_value());
  if (!extra_reply) return;
  send_request(*inbox, *extra_reply, extra_id, "ping", 1, true);
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const auto extra = receive_json(*extra_reply);
  CHECK(extra && (*extra)["error"] == "invalid_request");

  const std::string missing_id(32U, '1');
  const json missing{{"version", 1}, {"request_id", missing_id}, {"operation", "ping"},
                     {"reply_to", reply_queue_name(missing_id)}};
  CHECK(inbox->send(missing.dump(), 0U, error_number));
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const json oversized{{"pad", std::string(2000U, 'x')}};
  CHECK(!inbox->send(oversized.dump(), 0U, error_number));
  CHECK(error_number == EMSGSIZE);

  const std::string bad_destination_id(32U, '2');
  const json bad_destination{{"version", 1}, {"request_id", bad_destination_id},
      {"operation", "ping"}, {"reply_to", "/arbitrary_destination"}};
  CHECK(inbox->send(bad_destination.dump(), 0U, error_number));
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  const std::string valid_id(32U, '3');
  auto valid_reply = MessageQueue::create_exclusive(reply_queue_name(valid_id), O_RDONLY,
                                                     QueueLimits{1, darkpool::control::kMaxMessageBytes}, error);
  CHECK(valid_reply.has_value());
  if (!valid_reply) return;
  send_request(*inbox, *valid_reply, valid_id, "ping");
  server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
  CHECK(receive_json(*valid_reply).has_value());

  if (::geteuid() != 0) {
    const std::string private_id(32U, '4');
    const auto private_name = reply_queue_name(private_id);
    mq_attr attr{};
    attr.mq_flags = O_NONBLOCK;
    attr.mq_maxmsg = 1;
    attr.mq_msgsize = darkpool::control::kMaxMessageBytes;
    const mqd_t private_queue = ::mq_open(private_name.c_str(), O_CREAT | O_EXCL | O_RDONLY | O_NONBLOCK,
                                          0600, &attr);
    CHECK(private_queue != static_cast<mqd_t>(-1));
    if (private_queue != static_cast<mqd_t>(-1)) {
      CHECK(::fchmod(static_cast<int>(private_queue), 0000) == 0);
      // Rewrite the destination to the deliberately inaccessible queue.
      const json private_request{{"version", 1}, {"request_id", private_id},
          {"operation", "ping"}, {"reply_to", private_name}};
      CHECK(inbox->send(private_request.dump(), 0U, error_number));
      server.process_readable([] { return darkpool::control::StatusSnapshot{}; });
      CHECK(::fchmod(static_cast<int>(private_queue), 0600) == 0);
      mq_attr private_attributes{};
      CHECK(::mq_getattr(private_queue, &private_attributes) == 0);
      CHECK(private_attributes.mq_curmsgs == 0);
      (void)::mq_close(private_queue);
      CHECK(::mq_unlink(private_name.c_str()) == 0);
    }
  }
}

class ChildProcess {
 public:
  explicit ChildProcess(pid_t pid) : pid_(pid) {}
  ~ChildProcess() { terminate(); }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;
  ChildProcess(ChildProcess&& other) noexcept : pid_(std::exchange(other.pid_, -1)) {}
  ChildProcess& operator=(ChildProcess&& other) noexcept {
    if (this != &other) {
      terminate();
      pid_ = std::exchange(other.pid_, -1);
    }
    return *this;
  }

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }

  std::optional<int> wait_for(std::chrono::milliseconds timeout) {
    if (pid_ < 0) return std::nullopt;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        return WIFEXITED(status) ? std::optional<int>(WEXITSTATUS(status))
                                : std::optional<int>(128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
      }
      if (result < 0 && errno != EINTR) return std::nullopt;
      (void)::poll(nullptr, 0U, 10);
    }
    return std::nullopt;
  }

  void terminate(int signal = SIGTERM) noexcept {
    if (pid_ < 0) return;
    (void)::kill(pid_, signal);
    int status = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_ || (result < 0 && errno == ECHILD)) {
        pid_ = -1;
        return;
      }
      (void)::poll(nullptr, 0U, 10);
    }
    (void)::kill(pid_, SIGKILL);
    (void)::waitpid(pid_, &status, 0);
    pid_ = -1;
  }

 private:
  pid_t pid_{-1};
};

struct CliChild {
  ChildProcess process;
  int output_fd{-1};
  CliChild(pid_t pid, int fd) : process(pid), output_fd(fd) {}
  CliChild(CliChild&& other) noexcept
      : process(std::move(other.process)), output_fd(other.output_fd) { other.output_fd = -1; }
  ~CliChild() { if (output_fd >= 0) (void)::close(output_fd); }
  CliChild(const CliChild&) = delete;
  CliChild& operator=(const CliChild&) = delete;
};

CliChild launch_cli(const char* executable, const std::string& queue_name,
                    std::string_view operation) {
  int pipes[2] = {-1, -1};
  if (::pipe2(pipes, O_CLOEXEC | O_NONBLOCK) != 0) return CliChild(-1, -1);
  const pid_t child = ::fork();
  if (child < 0) {
    (void)::close(pipes[0]);
    (void)::close(pipes[1]);
    return CliChild(-1, -1);
  }
  if (child == 0) {
    (void)::close(pipes[0]);
    (void)::dup2(pipes[1], STDOUT_FILENO);
    (void)::dup2(pipes[1], STDERR_FILENO);
    (void)::close(pipes[1]);
    ::execl(executable, executable, "--queue", queue_name.c_str(), "--timeout-ms", "5000",
            std::string(operation).c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  (void)::close(pipes[1]);
  return CliChild(child, pipes[0]);
}

struct CliResult {
  int exit_code{-1};
  std::string output;
};

CliResult collect_cli(CliChild& child, std::chrono::milliseconds timeout = 8s) {
  CliResult result;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<char, 1024> buffer{};
  bool pipe_closed = false;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!pipe_closed && child.output_fd >= 0) {
      pollfd descriptor{.fd = child.output_fd, .events = POLLIN | POLLHUP, .revents = 0};
      const int ready = ::poll(&descriptor, 1U, 50);
      if (ready > 0) {
        const auto count = ::read(child.output_fd, buffer.data(), buffer.size());
        if (count > 0) {
          result.output.append(buffer.data(), static_cast<std::size_t>(count));
          if (result.output.size() > 8192U) break;
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
      if (pipe_closed || child.output_fd < 0) return result;
    }
  }
  child.process.terminate(SIGKILL);
  if (child.output_fd >= 0) {
    (void)::close(child.output_fd);
    child.output_fd = -1;
  }
  return result;
}

void wait_for_server_queue(const std::string& queue_name, ChildProcess& server) {
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  while (std::chrono::steady_clock::now() < deadline) {
    const mqd_t queue = ::mq_open(queue_name.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (queue != static_cast<mqd_t>(-1)) {
      (void)::mq_close(queue);
      return;
    }
    CHECK(server.pid() >= 0);
    (void)::poll(nullptr, 0U, 10);
  }
  CHECK(false && "server control queue did not become available");
}

ChildProcess launch_server(const char* executable, const std::string& queue_name,
                           const std::string& socket_path, bool fail_startup = false) {
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    const int devnull = ::open("/dev/null", O_WRONLY);
    if (devnull >= 0) {
      (void)::dup2(devnull, STDOUT_FILENO);
      (void)::dup2(devnull, STDERR_FILENO);
      (void)::close(devnull);
    }
    if (fail_startup) {
      ::execl(executable, executable, "--socket", socket_path.c_str(), "--no-tcp",
              "--control-queue", queue_name.c_str(), "--tls-cert", "/not/a/cert.pem",
              "--tls-key", "/not/a/key.pem", static_cast<char*>(nullptr));
    } else {
      ::execl(executable, executable, "--socket", socket_path.c_str(), "--no-tcp",
              "--control-queue", queue_name.c_str(), static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  return ChildProcess(child);
}

void assert_queue_absent(const std::string& queue_name) {
  errno = 0;
  const mqd_t descriptor = ::mq_open(queue_name.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  CHECK(descriptor == static_cast<mqd_t>(-1));
  CHECK(errno == ENOENT);
  if (descriptor != static_cast<mqd_t>(-1)) (void)::mq_close(descriptor);
}

void test_real_server_cli_concurrency_and_cleanup(const char* server_executable,
                                                   const char* cli_executable) {
  std::cerr << "[mq test] starting server integration\n";
  const auto token = unique_token();
  const auto queue_name = server_queue_name("integration");
  const auto socket_path = "/tmp/darkpool_mq_" + token + ".sock";
  auto server = launch_server(server_executable, queue_name, socket_path);
  CHECK(server.pid() >= 0);
  if (server.pid() < 0) return;
  wait_for_server_queue(queue_name, server);
  std::cerr << "[mq test] server queue ready\n";

  auto ping_child = launch_cli(cli_executable, queue_name, "ping");
  auto ping = collect_cli(ping_child);
  std::cerr << "[mq test] ping CLI completed with " << ping.exit_code << '\n';
  CHECK(ping.exit_code == 0);
  const auto ping_json = json::parse(ping.output, nullptr, false);
  CHECK(!ping_json.is_discarded());
  CHECK(ping_json.is_object() && ping_json.value("ok", false));
  CHECK(ping_json.is_object() && ping_json.value("operation", std::string{}) == "ping");
  CHECK(ping_json.is_object() && ping_json.value("request_id", std::string{}).size() == 32U);

  auto status_child = launch_cli(cli_executable, queue_name, "status");
  auto status = collect_cli(status_child);
  std::cerr << "[mq test] status CLI completed with " << status.exit_code << '\n';
  CHECK(status.exit_code == 0);
  const auto status_json = json::parse(status.output, nullptr, false);
  CHECK(!status_json.is_discarded());
  CHECK(status_json.is_object() && status_json.value("operation", std::string{}) == "status");
  CHECK(status_json.is_object() && status_json["result"].value("match_capacity", 0U) == 4U);
  CHECK(status_json.is_object() && status_json["result"].value("player_connection_capacity", 0U) == 256U);
  CHECK(status_json.is_object() && status_json["result"].size() == 8U);

  std::vector<CliChild> concurrent;
  for (int i = 0; i < 12; ++i) concurrent.push_back(launch_cli(cli_executable, queue_name, "ping"));
  std::cerr << "[mq test] launched concurrent clients\n";

  const int network_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  CHECK(network_fd >= 0);
  if (network_fd >= 0) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1U);
    CHECK(::connect(network_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    const auto connect_frame = Frame(json{{"type", "connect"}, {"role", "player"}}).serialize();
    CHECK(send_bytes(network_fd, connect_frame));
    const auto connected = receive_frame(network_fd);
    CHECK(connected && connected->value("type", std::string{}) == "connect");
    const json echo{{"type", "ipc_network_regression_probe"}, {"nonce", token}};
    CHECK(send_bytes(network_fd, Frame(echo).serialize()));
    const auto echo_response = receive_frame(network_fd);
    CHECK(echo_response && *echo_response == echo);
    auto connected_status_child = launch_cli(cli_executable, queue_name, "status");
    const auto connected_status = collect_cli(connected_status_child);
    CHECK(connected_status.exit_code == 0);
    const auto connected_status_json = json::parse(connected_status.output, nullptr, false);
    CHECK(!connected_status_json.is_discarded());
    CHECK(connected_status_json.is_object() &&
          connected_status_json["result"].value("connected_players", 0U) == 1U);
    (void)::close(network_fd);
  }

  for (auto& child : concurrent) {
    const auto result = collect_cli(child);
    std::cerr << "[mq test] concurrent CLI completed with " << result.exit_code << '\n';
    CHECK(result.exit_code == 0);
  }

  server.terminate(SIGTERM);
  CHECK(server.wait_for(0ms).has_value() || server.pid() < 0);
  assert_queue_absent(queue_name);
  CHECK(::access(socket_path.c_str(), F_OK) != 0);
}

void test_partial_startup_cleanup(const char* server_executable) {
  std::cerr << "[mq test] starting partial-startup cleanup test\n";
  const auto queue_name = server_queue_name("startup_failure");
  const auto socket_path = "/tmp/darkpool_mq_fail_" + unique_token() + ".sock";
  auto server = launch_server(server_executable, queue_name, socket_path, true);
  CHECK(server.pid() >= 0);
  const auto status = server.wait_for(5s);
  CHECK(status.has_value() && *status != 0);
  assert_queue_absent(queue_name);
  CHECK(::access(socket_path.c_str(), F_OK) != 0);
}

}  // namespace

int main(int argc, char** argv) {
  std::cerr << "[mq test] starting queue unit tests\n";
  test_queue_ownership_nonblocking_and_priority();
  test_control_server_validation_and_duplicate_requests();
  std::cerr << "[mq test] queue unit tests complete\n";
  if (argc >= 3) {
    test_real_server_cli_concurrency_and_cleanup(argv[1], argv[2]);
    test_partial_startup_cleanup(argv[1]);
  } else {
    CHECK(false && "server and darkpool_ctl executable paths are required");
  }

  if (failures != 0) {
    std::cerr << failures << " POSIX MQ check(s) failed.\n";
    return EXIT_FAILURE;
  }
  std::cout << "POSIX MQ control tests passed.\n";
  return EXIT_SUCCESS;
}

#else

int main() { return 0; }

#endif
