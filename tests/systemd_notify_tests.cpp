#include "darkpool/systemd_notify.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    ++failures;
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

using Clock = darkpool::systemd::Notifier::Clock;

class ScopedEnvironment {
 public:
  ScopedEnvironment(const char* name, const char* value) : name_(name) {
    const char* previous = std::getenv(name);
    if (previous != nullptr) {
      previous_ = previous;
    }
    if (value == nullptr) {
      CHECK(::unsetenv(name_.c_str()) == 0);
    } else {
      CHECK(::setenv(name_.c_str(), value, 1) == 0);
    }
  }

  ~ScopedEnvironment() {
    if (previous_.has_value()) {
      (void)::setenv(name_.c_str(), previous_->c_str(), 1);
    } else {
      (void)::unsetenv(name_.c_str());
    }
  }

  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  std::string name_;
  std::optional<std::string> previous_;
};

bool make_test_address(std::string_view name, sockaddr_un& address,
                       socklen_t& length) {
  address = {};
  address.sun_family = AF_UNIX;
  const auto offset = offsetof(sockaddr_un, sun_path);
  if (name.starts_with('@')) {
    const auto abstract_name = name.substr(1U);
    if (abstract_name.empty() || abstract_name.size() >= sizeof(address.sun_path)) {
      return false;
    }
    std::memcpy(address.sun_path + 1, abstract_name.data(), abstract_name.size());
    length = static_cast<socklen_t>(offset + 1U + abstract_name.size());
    return true;
  }
  if (name.empty() || name.front() != '/' || name.size() >= sizeof(address.sun_path)) {
    return false;
  }
  std::memcpy(address.sun_path, name.data(), name.size());
  address.sun_path[name.size()] = '\0';
  length = static_cast<socklen_t>(offset + name.size() + 1U);
  return true;
}

class NotificationReceiver {
 public:
  explicit NotificationReceiver(bool use_abstract) {
    const auto unique = std::to_string(::getpid()) + "-" +
        std::to_string(Clock::now().time_since_epoch().count());
    if (use_abstract) {
      name_ = "@darkpool-systemd-test-" + unique;
    } else {
      path_ = (std::filesystem::temp_directory_path() /
               ("darkpool-systemd-test-" + unique + ".sock")).string();
      name_ = path_;
    }

    sockaddr_un address{};
    socklen_t address_length = 0;
    CHECK(make_test_address(name_, address, address_length));
    socket_fd_ = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    CHECK(socket_fd_ >= 0);
    if (socket_fd_ >= 0) {
      CHECK(::bind(socket_fd_, reinterpret_cast<const sockaddr*>(&address), address_length) == 0);
    }
  }

  ~NotificationReceiver() {
    if (socket_fd_ >= 0) {
      (void)::close(socket_fd_);
    }
    if (!path_.empty()) {
      (void)::unlink(path_.c_str());
    }
  }

  NotificationReceiver(const NotificationReceiver&) = delete;
  NotificationReceiver& operator=(const NotificationReceiver&) = delete;

  [[nodiscard]] const std::string& name() const noexcept { return name_; }

  std::optional<std::string> receive(std::chrono::milliseconds timeout) const {
    if (socket_fd_ < 0) {
      return std::nullopt;
    }
    pollfd descriptor{.fd = socket_fd_, .events = POLLIN, .revents = 0};
    int ready = 0;
    do {
      ready = ::poll(&descriptor, 1U, static_cast<int>(timeout.count()));
    } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || (descriptor.revents & POLLIN) == 0) {
      return std::nullopt;
    }
    std::array<char, 128> buffer{};
    const auto count = ::recv(socket_fd_, buffer.data(), buffer.size(), 0);
    if (count < 0) {
      return std::nullopt;
    }
    return std::string(buffer.data(), static_cast<std::size_t>(count));
  }

 private:
  int socket_fd_{-1};
  std::string name_;
  std::string path_;
};

void test_absent_environment_is_disabled() {
  ScopedEnvironment notify_socket("NOTIFY_SOCKET", nullptr);
  ScopedEnvironment watchdog_usec("WATCHDOG_USEC", nullptr);
  ScopedEnvironment watchdog_pid("WATCHDOG_PID", nullptr);
  darkpool::systemd::Notifier notifier;
  CHECK(!notifier.watchdog_enabled());
  notifier.ready();
  notifier.check_watchdog(Clock::time_point::max());
  notifier.stopping();
}

void test_notifications_and_schedule(bool use_abstract) {
  NotificationReceiver receiver(use_abstract);
  ScopedEnvironment notify_socket("NOTIFY_SOCKET", receiver.name().c_str());
  ScopedEnvironment watchdog_usec("WATCHDOG_USEC", "1000000");
  ScopedEnvironment watchdog_pid("WATCHDOG_PID", nullptr);
  darkpool::systemd::Notifier notifier;
  CHECK(notifier.watchdog_enabled());
  CHECK(notifier.max_poll_timeout_ms(100) == 100);

  const auto start = Clock::time_point{};
  notifier.ready(start);
  CHECK(receiver.receive(std::chrono::milliseconds(100)) == "READY=1");

  constexpr auto half_interval = std::chrono::milliseconds(500);
  notifier.check_watchdog(start + half_interval - Clock::duration(1));
  CHECK(!receiver.receive(std::chrono::milliseconds(20)).has_value());

  notifier.check_watchdog(start + half_interval);
  CHECK(receiver.receive(std::chrono::milliseconds(100)) == "WATCHDOG=1");
  notifier.check_watchdog(start + half_interval);
  CHECK(!receiver.receive(std::chrono::milliseconds(20)).has_value());

  notifier.check_watchdog(start + half_interval * 2);
  CHECK(receiver.receive(std::chrono::milliseconds(100)) == "WATCHDOG=1");
  notifier.stopping();
  CHECK(receiver.receive(std::chrono::milliseconds(100)) == "STOPPING=1");
  notifier.stopping();
  CHECK(!receiver.receive(std::chrono::milliseconds(20)).has_value());
}

void test_invalid_watchdog_intervals() {
  NotificationReceiver receiver(false);
  ScopedEnvironment notify_socket("NOTIFY_SOCKET", receiver.name().c_str());
  ScopedEnvironment watchdog_pid("WATCHDOG_PID", nullptr);
  constexpr std::array<const char*, 4> invalid_intervals{
      "not-a-number", "0", "18446744073709551616", "18446744073709551615"};

  for (const char* interval : invalid_intervals) {
    ScopedEnvironment watchdog_usec("WATCHDOG_USEC", interval);
    darkpool::systemd::Notifier notifier;
    CHECK(!notifier.watchdog_enabled());
    notifier.ready();
    CHECK(receiver.receive(std::chrono::milliseconds(100)) == "READY=1");
    notifier.check_watchdog(Clock::time_point::max());
    CHECK(!receiver.receive(std::chrono::milliseconds(20)).has_value());
    notifier.stopping();
    CHECK(receiver.receive(std::chrono::milliseconds(100)) == "STOPPING=1");
  }
}

void test_watchdog_pid_mismatch_and_match() {
  NotificationReceiver receiver(false);
  ScopedEnvironment notify_socket("NOTIFY_SOCKET", receiver.name().c_str());
  ScopedEnvironment watchdog_usec("WATCHDOG_USEC", "1000000");
  const auto other_pid = std::to_string(static_cast<unsigned long long>(::getpid()) + 1ULL);
  {
    ScopedEnvironment watchdog_pid("WATCHDOG_PID", other_pid.c_str());
    darkpool::systemd::Notifier notifier;
    CHECK(!notifier.watchdog_enabled());
    notifier.ready();
    CHECK(receiver.receive(std::chrono::milliseconds(100)) == "READY=1");
    notifier.check_watchdog(Clock::time_point::max());
    CHECK(!receiver.receive(std::chrono::milliseconds(20)).has_value());
  }
  {
    const auto own_pid = std::to_string(static_cast<unsigned long long>(::getpid()));
    ScopedEnvironment short_watchdog_usec("WATCHDOG_USEC", "100000");
    ScopedEnvironment watchdog_pid("WATCHDOG_PID", own_pid.c_str());
    darkpool::systemd::Notifier notifier;
    CHECK(notifier.watchdog_enabled());
    CHECK(notifier.max_poll_timeout_ms(100) == 50);
    const auto start = Clock::time_point{};
    notifier.ready(start);
    CHECK(receiver.receive(std::chrono::milliseconds(100)) == "READY=1");
    notifier.check_watchdog(start + std::chrono::milliseconds(500));
    CHECK(receiver.receive(std::chrono::milliseconds(100)) == "WATCHDOG=1");
  }
}

class ChildProcess {
 public:
  explicit ChildProcess(pid_t pid) : pid_(pid) {}
  ~ChildProcess() { terminate(); }
  ChildProcess(const ChildProcess&) = delete;
  ChildProcess& operator=(const ChildProcess&) = delete;

  std::optional<int> wait_for(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
      int status = 0;
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        if (WIFEXITED(status)) {
          return WEXITSTATUS(status);
        }
        return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
      }
      if (result < 0 && errno != EINTR) {
        return std::nullopt;
      }
      (void)::poll(nullptr, 0U, 10);
    }
    return std::nullopt;
  }

  bool request_stop() noexcept {
    return pid_ >= 0 && ::kill(pid_, SIGTERM) == 0;
  }

  void terminate() noexcept {
    if (pid_ < 0) {
      return;
    }
    (void)::kill(pid_, SIGTERM);
    int status = 0;
    for (int attempt = 0; attempt < 100; ++attempt) {
      const auto result = ::waitpid(pid_, &status, WNOHANG);
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

 private:
  pid_t pid_{-1};
};

ChildProcess start_server(const char* executable, const std::string& notify_socket,
                          const std::string& unix_socket,
                          const char* watchdog_usec, bool force_startup_failure) {
  const pid_t child = ::fork();
  CHECK(child >= 0);
  if (child == 0) {
    (void)::setenv("NOTIFY_SOCKET", notify_socket.c_str(), 1);
    if (watchdog_usec != nullptr) {
      (void)::setenv("WATCHDOG_USEC", watchdog_usec, 1);
    } else {
      (void)::unsetenv("WATCHDOG_USEC");
    }
    (void)::unsetenv("WATCHDOG_PID");
    if (force_startup_failure) {
      ::execl(executable, executable, "--tls-cert", "/not/a/certificate.pem",
              "--tls-key", "/not/a/private-key.pem", "--no-tcp", "--no-unix",
              static_cast<char*>(nullptr));
    } else {
      ::execl(executable, executable, "--socket", unix_socket.c_str(), "--no-tcp",
              static_cast<char*>(nullptr));
    }
    _exit(127);
  }
  return ChildProcess(child);
}

bool wait_for_unix_server(const std::string& path,
                          std::chrono::milliseconds timeout) {
  const auto deadline = Clock::now() + timeout;
  while (Clock::now() < deadline) {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      return false;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1U);
    const auto result = ::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                                  sizeof(address));
    const int connect_error = result < 0 ? errno : 0;
    (void)::close(fd);
    if (result == 0) {
      return true;
    }
    if (connect_error != ENOENT && connect_error != ECONNREFUSED) {
      return false;
    }
    (void)::poll(nullptr, 0U, 10);
  }
  return false;
}

void test_server_lifecycle(const char* server_executable) {
  NotificationReceiver receiver(false);
  const auto socket_path = (std::filesystem::temp_directory_path() /
      ("darkpool-server-notify-" + std::to_string(::getpid()) + ".sock")).string();
  (void)::unlink(socket_path.c_str());
  auto child = start_server(server_executable, receiver.name(), socket_path, "200000", false);

  CHECK(receiver.receive(std::chrono::seconds(3)) == "READY=1");
  CHECK(receiver.receive(std::chrono::seconds(2)) == "WATCHDOG=1");
  CHECK(child.request_stop());
  const auto result = child.wait_for(std::chrono::seconds(3));
  CHECK(result.has_value() && *result == 0);
  CHECK(receiver.receive(std::chrono::seconds(1)) == "STOPPING=1");
}

void test_server_startup_failure_never_reports_ready(const char* server_executable) {
  NotificationReceiver receiver(false);
  auto child = start_server(server_executable, receiver.name(), std::string{}, nullptr, true);
  const auto result = child.wait_for(std::chrono::seconds(3));
  CHECK(result.has_value());
  CHECK(result.has_value() && *result != 0);
  CHECK(!receiver.receive(std::chrono::milliseconds(100)).has_value());
}

void test_notification_failure_does_not_block_server(const char* server_executable) {
  const auto socket_path = (std::filesystem::temp_directory_path() /
      ("darkpool-server-notify-failure-" + std::to_string(::getpid()) + ".sock")).string();
  (void)::unlink(socket_path.c_str());
  const auto missing_notify_socket = (std::filesystem::temp_directory_path() /
      ("darkpool-missing-notify-dir-" + std::to_string(::getpid()) + "/notify.sock")).string();
  auto child = start_server(server_executable, missing_notify_socket, socket_path, "200000", false);
  CHECK(wait_for_unix_server(socket_path, std::chrono::seconds(3)));
  child.terminate();
  CHECK(!std::filesystem::exists(socket_path));
}

}  // namespace

int main(int argc, char** argv) {
  test_absent_environment_is_disabled();
  test_notifications_and_schedule(false);
  test_notifications_and_schedule(true);
  test_invalid_watchdog_intervals();
  test_watchdog_pid_mismatch_and_match();
  if (argc > 1) {
    test_server_lifecycle(argv[1]);
    test_server_startup_failure_never_reports_ready(argv[1]);
    test_notification_failure_does_not_block_server(argv[1]);
  } else {
    CHECK(false);
  }

  if (failures != 0) {
    return EXIT_FAILURE;
  }
  std::cout << "Systemd notification tests passed.\n";
  return EXIT_SUCCESS;
}
