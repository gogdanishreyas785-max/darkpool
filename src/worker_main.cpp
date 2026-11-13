#include "darkpool/server_worker_channel.hpp"
#include "darkpool/worker_protocol.hpp"

#if defined(__linux__)

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using darkpool::ipc::WorkerClientChannel;
using darkpool::ipc::WorkerOpcode;
using darkpool::ipc::WorkerRequestMessage;
using darkpool::ipc::WorkerResponseMessage;
using darkpool::ipc::WorkerStatusCode;

volatile std::sig_atomic_t g_stop_requested = 0;

void signal_handler(int) {
  g_stop_requested = 1;
}

void print_usage(const char* prog) {
  std::cerr << "Usage: " << prog << " --request-shm <NAME> --response-shm <NAME> "
            << "--worker-eventfd <FD> --server-eventfd <FD> [--bench-mode]\n";
}

}  // namespace

int main(int argc, char** argv) {
  // This worker has no need for server authentication material. Refuse to run
  // if launched with credential-bearing environment variables rather than
  // silently retaining secrets in its process environment.
  static constexpr const char* kForbiddenEnvironment[] = {
      "DARKPOOL_MODERATOR_TOKEN",
      "DARKPOOL_MATCH_JOIN_TOKEN",
      "DARKPOOL_PLAYER_TOKEN",
      "DARKPOOL_JOIN_TOKEN",
      "DARKPOOL_ENROLLMENT_TOKEN",
      "DARKPOOL_PLAYER_SECRET",
      "DARKPOOL_PLAYER_RESUME_TOKEN",
      "DARKPOOL_TOKEN_DIR"
  };
  for (const char* name : kForbiddenEnvironment) {
    const char* value = std::getenv(name);
    if (value != nullptr && value[0] != '\0') {
      std::cerr << "[darkpool_worker] refusing credential-bearing environment\n";
      return 2;
    }
  }

  std::string req_shm;
  std::string resp_shm;
  int worker_efd = -1;
  int server_efd = -1;
  bool bench_mode = false;
  (void)bench_mode;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--request-shm" && i + 1 < argc) {
      req_shm = argv[++i];
    } else if (arg == "--response-shm" && i + 1 < argc) {
      resp_shm = argv[++i];
    } else if (arg == "--worker-eventfd" && i + 1 < argc) {
      worker_efd = std::atoi(argv[++i]);
    } else if (arg == "--server-eventfd" && i + 1 < argc) {
      server_efd = std::atoi(argv[++i]);
    } else if (arg == "--bench-mode") {
      bench_mode = true;
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
  }

  if (req_shm.empty() || resp_shm.empty() || worker_efd < 0 || server_efd < 0) {
    print_usage(argv[0]);
    return 2;
  }

  // Register clean signal handling
  struct sigaction sa{};
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);

  std::string open_err;
  auto channel_opt = WorkerClientChannel::open(
      req_shm, resp_shm, worker_efd, server_efd, open_err);
  if (!channel_opt.has_value()) {
    std::cerr << "[darkpool_worker] Failed to connect channel: " << open_err << '\n';
    return 1;
  }

  auto& channel = *channel_opt;
  const auto my_pid = static_cast<std::uint32_t>(::getpid());

  // Send initial Ready handshake message to server
  {
    WorkerResponseMessage ready_resp{};
    ready_resp.opcode = static_cast<std::int32_t>(WorkerOpcode::Ready);
    ready_resp.status = static_cast<std::int32_t>(WorkerStatusCode::Success);
    ready_resp.worker_pid = my_pid;
    channel.send_response(ready_resp);
  }

  // Main worker loop: wait on requests from the queue / eventfd
  while (!g_stop_requested) {
    // Consume worker-side eventfd notification if signaled
    (void)channel.consume_worker_notification();

    WorkerRequestMessage req{};
    std::string pop_err;
    // Bounded timeout wait for requests (1000ms check allows responsive signal handling)
    const auto op_st = channel.pop_request(req, std::chrono::milliseconds{1000}, &pop_err);

    if (op_st == darkpool::ipc::QueueOpStatus::Timeout) {
      continue;
    }

    if (op_st == darkpool::ipc::QueueOpStatus::Closed ||
        op_st == darkpool::ipc::QueueOpStatus::Faulted) {
      // Channel was closed by server or faulted; exit cleanly
      break;
    }

    if (op_st != darkpool::ipc::QueueOpStatus::Success) {
      continue;
    }

    // Process request
    WorkerResponseMessage resp{};
    resp.request_id = req.request_id;
    resp.client_session_id = req.client_session_id;
    resp.opcode = req.opcode;
    resp.worker_pid = my_pid;

    if (req.opcode == static_cast<std::int32_t>(WorkerOpcode::Shutdown)) {
      resp.status = static_cast<std::int32_t>(WorkerStatusCode::Success);
      channel.send_response(resp);
      break;
    }

    if (req.opcode == static_cast<std::int32_t>(WorkerOpcode::Ping)) {
      resp.status = static_cast<std::int32_t>(WorkerStatusCode::Success);
      channel.send_response(resp);
      continue;
    }

    if (req.opcode == static_cast<std::int32_t>(WorkerOpcode::EvaluateMarketModel)) {
      // Perform advisory market model sensitivity / projection evaluation
      // Safely widen to int64_t to prevent signed overflow
      const std::int64_t price = req.price;
      const std::int64_t health = req.health;
      const std::int64_t e = req.event_impact;
      const std::int64_t s = (req.demand > 0) ? 1 : ((req.demand < 0) ? -1 : 0);

      const std::int64_t raw_baseline_p = price + (2 * e) + s;
      const std::int64_t raw_baseline_h = health + e + s;
      const std::int32_t baseline_p = static_cast<std::int32_t>(std::clamp<std::int64_t>(raw_baseline_p, 1, 100));
      const std::int32_t baseline_h = static_cast<std::int32_t>(std::clamp<std::int64_t>(raw_baseline_h, 0, 10));

      // Candidate multi-factor evaluation with safe int64_t arithmetic
      std::int64_t raw_candidate_p = price + (2 * e) + (2 * s);
      if (req.volume > 10) {
        raw_candidate_p += 1;
      }
      const std::int32_t candidate_p = static_cast<std::int32_t>(std::clamp<std::int64_t>(raw_candidate_p, 1, 100));

      resp.status = static_cast<std::int32_t>(WorkerStatusCode::Success);
      resp.projected_price = candidate_p;
      resp.projected_health = baseline_h;
      resp.price_delta = static_cast<std::int32_t>(candidate_p - req.price);
      resp.baseline_price = baseline_p;

      channel.send_response(resp);
      continue;
    }

    // Unrecognized opcode
    resp.status = static_cast<std::int32_t>(WorkerStatusCode::InvalidPayload);
    channel.send_response(resp);
  }

  channel.close();
  return 0;
}

#else

int main() {
  return 0;
}

#endif  // defined(__linux__)

