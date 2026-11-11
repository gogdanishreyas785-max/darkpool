#include "darkpool/posix_mq_control.hpp"

#if defined(__linux__)

#include <charconv>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>

namespace {

volatile std::sig_atomic_t interrupted = 0;

void handle_signal(int) { interrupted = 1; }

void print_usage(std::ostream& stream) {
  stream << "Usage: darkpool_ctl [--queue NAME] [--timeout-ms N] <ping|status|shutdown>\n";
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

}  // namespace

int main(int argc, char** argv) {
  auto queue_name = darkpool::control::ControlServer::default_queue_name();
  auto timeout = std::chrono::milliseconds(2000);
  std::string_view operation;

  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--help" || argument == "-h") {
      print_usage(std::cout);
      return 0;
    }
    if (argument == "--queue") {
      if (i + 1 >= argc) {
        print_usage(std::cerr);
        return 2;
      }
      queue_name = argv[++i];
      continue;
    }
    if (argument == "--timeout-ms") {
      if (i + 1 >= argc || !parse_timeout(argv[++i], timeout)) {
        std::cerr << "error: timeout must be an integer from 1 through 60000 milliseconds\n";
        return 2;
      }
      continue;
    }
    if (!operation.empty()) {
      print_usage(std::cerr);
      return 2;
    }
    operation = argument;
  }

  if (operation.empty() || (operation != "ping" && operation != "status" && operation != "shutdown") ||
      !darkpool::control::ControlServer::valid_server_queue_name(queue_name)) {
    print_usage(std::cerr);
    return 2;
  }

  struct sigaction action {};
  action.sa_handler = handle_signal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = 0;
  (void)::sigaction(SIGINT, &action, nullptr);
  (void)::sigaction(SIGTERM, &action, nullptr);

  std::string error_message;
  const auto response = darkpool::control::request(
      queue_name, operation, timeout, error_message, &interrupted);
  if (!response) {
    if (interrupted != 0) {
      std::cerr << "interrupted\n";
      return 130;
    }
    std::cerr << "error: " << error_message << '\n';
    return error_message == "control request timed out" ? 4 : 3;
  }

  std::cout << response->message.dump(2) << '\n';
  return response->message.value("ok", false) ? 0 : 5;
}

#else

#include <iostream>

int main() {
  std::cerr << "darkpool_ctl requires Linux POSIX message queues\n";
  return 1;
}

#endif
