#include <darkpool/network.hpp>
#include <darkpool/server_worker_channel.hpp>
#include <darkpool/worker_process_manager.hpp>
#include <darkpool/worker_protocol.hpp>

#include <arpa/inet.h>
#include <cassert>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mqueue.h>
#include <netinet/in.h>
#include <semaphore.h>
#include <string>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace darkpool::ipc::test {

using darkpool::network::Frame;
using darkpool::network::json;

static void cleanup_shm(const std::string& name) {
  ::shm_unlink(name.c_str());
}

static void require(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "Assertion failure: " << msg << std::endl;
    std::abort();
  }
}

static bool send_all(int fd, const char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto sent = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
    if (sent < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (sent == 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

static bool receive_exact(int fd, char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto received = ::recv(fd, data + offset, size - offset, 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (received == 0) return false;
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

static std::optional<json> read_single_frame(int fd) {
  char header[4];
  if (!receive_exact(fd, header, sizeof(header))) return std::nullopt;
  const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
  const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
  const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
  const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
  if (length == 0U || length > darkpool::network::kMaxMessageSize) return std::nullopt;
  std::string bytes(header, sizeof(header));
  bytes.resize(static_cast<std::size_t>(length) + sizeof(header));
  if (!receive_exact(fd, bytes.data() + sizeof(header), static_cast<std::size_t>(length))) return std::nullopt;
  std::size_t consumed = 0;
  try {
    const auto response = Frame::parse(bytes, consumed);
    if (!response || consumed != bytes.size()) return std::nullopt;
    return response->payload();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

static std::optional<json> request_json(int fd, const json& payload) {
  const auto outgoing = Frame(payload).serialize();
  if (!send_all(fd, outgoing.data(), outgoing.size())) return std::nullopt;
  return read_single_frame(fd);
}

static int connect_tcp(uint16_t port) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

  // Set timeout
  struct timeval tv{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// -------------------------------------------------------------
// Manager-level unit tests
// -------------------------------------------------------------

static void test_worker_channel_direct() {
  std::cout << "[TEST] Direct channel push/pop across threads..." << std::endl;
  std::string req_shm = "/dp_test_dir_req_" + std::to_string(::getpid());
  std::string resp_shm = "/dp_test_dir_resp_" + std::to_string(::getpid());
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);

  ServerWorkerChannelConfig cfg{req_shm, resp_shm, 16};
  std::string err;
  auto srv_chan = ServerWorkerChannel::create(cfg, err);
  require(srv_chan.has_value(), "create srv_chan");

  auto client_chan = WorkerClientChannel::open(
      req_shm, resp_shm, srv_chan->worker_notify_fd(), srv_chan->server_notify_fd(), err);
  require(client_chan.has_value(), "open client_chan");

  WorkerRequestMessage req{};
  req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
  req.request_id = 1;
  req.price = 100;

  require(srv_chan->send_request(req, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "send_request");

  auto notif = client_chan->consume_worker_notification();
  require(notif.has_value() && *notif > 0, "consume_worker_notification");

  WorkerRequestMessage popped_req{};
  require(client_chan->pop_request(popped_req, std::chrono::milliseconds{500}) == QueueOpStatus::Success,
          "pop_request");
  require(popped_req.request_id == 1, "popped_req id");

  WorkerResponseMessage resp{};
  resp.request_id = popped_req.request_id;
  resp.opcode = popped_req.opcode;
  resp.status = static_cast<int32_t>(WorkerStatusCode::Success);
  resp.projected_price = 100;

  require(client_chan->send_response(resp, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "send_response");

  auto srv_notif = srv_chan->consume_server_notification();
  require(srv_notif.has_value() && *srv_notif > 0, "consume_server_notification");

  size_t drained = 0;
  WorkerResponseMessage popped_resp{};
  drained = srv_chan->drain_responses([&](const WorkerResponseMessage& r) {
    popped_resp = r;
  });
  require(drained == 1, "drained count");
  require(popped_resp.request_id == 1, "resp id");

  srv_chan->close_channel();
  client_chan->close();
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);
  std::cout << "[TEST] Direct channel test passed." << std::endl;
}

static void test_queue_bounds_and_backpressure() {
  std::cout << "[TEST] Queue bounds and backpressure..." << std::endl;
  std::string req_shm = "/dp_test_bound_req_" + std::to_string(::getpid());
  std::string resp_shm = "/dp_test_bound_resp_" + std::to_string(::getpid());
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);

  ServerWorkerChannelConfig cfg{req_shm, resp_shm, 4};
  std::string err;
  auto srv_chan = ServerWorkerChannel::create(cfg, err);
  require(srv_chan.has_value(), "create srv_chan");

  for (uint64_t i = 0; i < 4; ++i) {
    WorkerRequestMessage req{};
    req.request_id = i + 1;
    req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
    require(srv_chan->send_request(req, std::chrono::milliseconds{10}) == QueueOpStatus::Success,
            "send_request");
  }

  WorkerRequestMessage overflow_req{};
  overflow_req.request_id = 5;
  QueueOpStatus st = srv_chan->send_request(overflow_req, std::chrono::milliseconds{10});
  require(st == QueueOpStatus::Timeout, "overflow timeout");

  srv_chan->close_channel();
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);
  std::cout << "[TEST] Queue bounds and backpressure passed." << std::endl;
}

static void test_process_manager_end_to_end(const std::string& worker_bin) {
  std::cout << "[TEST] ProcessManager end-to-end with worker binary: " << worker_bin << std::endl;
  std::string req_shm = "/dp_test_mgr_req_" + std::to_string(::getpid());
  std::string resp_shm = "/dp_test_mgr_resp_" + std::to_string(::getpid());
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);

  WorkerProcessConfig cfg;
  cfg.worker_binary_path = worker_bin;
  cfg.request_shm_name = req_shm;
  cfg.response_shm_name = resp_shm;
  cfg.queue_capacity = 32;

  std::string err;
  auto mgr_opt = WorkerProcessManager::launch(cfg, err);
  if (!mgr_opt) {
    std::cerr << "Failed to launch worker: " << err << std::endl;
  }
  require(mgr_opt.has_value(), "launch worker");
  auto& mgr = *mgr_opt;

  require(mgr.is_alive(), "worker is_alive");
  std::cout << "Worker launched with PID: " << mgr.worker_pid() << std::endl;

  WorkerRequestMessage req{};
  req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
  req.request_id = 100;
  req.price = 50;
  require(mgr.send_request(req, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "mgr send_request");

  bool got_resp = false;
  WorkerResponseMessage resp{};
  for (int retry = 0; retry < 50; ++retry) {
    if (mgr.drain_responses([&](const WorkerResponseMessage& r) {
          resp = r;
          got_resp = true;
        }) > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  require(got_resp, "got ping response");
  require(resp.request_id == 100, "resp id 100");
  require(resp.status == static_cast<int32_t>(WorkerStatusCode::Success), "resp status success");

  require(mgr.shutdown(std::chrono::milliseconds{1000}), "mgr shutdown");
  require(!mgr.is_alive(), "mgr not alive");
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);
  std::cout << "[TEST] ProcessManager end-to-end passed." << std::endl;
}

// -------------------------------------------------------------
// Real Server–Worker Production End-to-End Integration Tests
// -------------------------------------------------------------

struct ServerProcessFixture {
  pid_t pid{-1};
  uint16_t port{0};
  std::string unix_path;
  std::string req_shm;
  std::string resp_shm;

  std::string ctl_queue;
  std::string telem_shm;
  std::string telem_sem;

  ServerProcessFixture(const std::string& server_bin,
                       const std::string& worker_bin,
                       uint16_t target_port,
                       bool enable_worker = true) {
    port = target_port;
    unix_path = "/tmp/dp_srv_test_" + std::to_string(::getpid()) + "_" + std::to_string(port) + ".sock";
    req_shm = "/dp_srv_test_req_" + std::to_string(::getpid()) + "_" + std::to_string(port);
    resp_shm = "/dp_srv_test_resp_" + std::to_string(::getpid()) + "_" + std::to_string(port);
    ctl_queue = "/darkpool_ctl_" + std::to_string(::geteuid()) + "_" + std::to_string(::getpid()) + "_" + std::to_string(port);
    telem_shm = "/darkpool_telemetry_" + std::to_string(::geteuid()) + "_" + std::to_string(::getpid()) + "_" + std::to_string(port);
    telem_sem = "/darkpool_telemetry_sem_" + std::to_string(::geteuid()) + "_" + std::to_string(::getpid()) + "_" + std::to_string(port);

    cleanup_shm(req_shm);
    cleanup_shm(resp_shm);
    cleanup_shm(telem_shm);
    ::mq_unlink(ctl_queue.c_str());
    ::sem_unlink(telem_sem.c_str());
    ::unlink(unix_path.c_str());

    pid = ::fork();
    if (pid == 0) {
      std::vector<std::string> args = {
          server_bin,
          "--port", std::to_string(port),
          "--socket", unix_path,
          "--control-queue", ctl_queue,
          "--telemetry-shm", telem_shm,
          "--telemetry-sem", telem_sem,
          "--open-lobby",
          "--no-tls"
      };
      if (enable_worker) {
        args.push_back("--worker");
        args.push_back("--worker-binary");
        args.push_back(worker_bin);
        args.push_back("--worker-req-shm");
        args.push_back(req_shm);
        args.push_back("--worker-resp-shm");
        args.push_back(resp_shm);
      }

      std::vector<char*> c_args;
      for (auto& a : args) c_args.push_back(const_cast<char*>(a.c_str()));
      c_args.push_back(nullptr);

      ::execv(server_bin.c_str(), c_args.data());
      ::_exit(127);
    }

    // Wait for server to accept TCP connections
    bool ready = false;
    for (int retry = 0; retry < 50; ++retry) {
      int fd = connect_tcp(port);
      if (fd >= 0) {
        ::close(fd);
        ready = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    require(ready, "server process failed to start listening");
  }

  ~ServerProcessFixture() {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      int status = 0;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
      bool reaped = false;
      while (std::chrono::steady_clock::now() < deadline) {
        if (::waitpid(pid, &status, WNOHANG) == pid) {
          reaped = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      if (!reaped) {
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
      }
      pid = -1;
    }
    ::unlink(unix_path.c_str());
    cleanup_shm(req_shm);
    cleanup_shm(resp_shm);
    cleanup_shm(telem_shm);
    ::mq_unlink(ctl_queue.c_str());
    ::sem_unlink(telem_sem.c_str());
  }
};

static void test_server_worker_eval_model_e2e(const std::string& server_bin, const std::string& worker_bin) {
  std::cout << "[TEST] Real server–worker epoll integration: eval_model..." << std::endl;
  ServerProcessFixture server(server_bin, worker_bin, 49152);

  int client_fd = connect_tcp(server.port);
  require(client_fd >= 0, "client connected");

  // Connect handshake as player
  auto connect_resp = request_json(client_fd, json{{"type", "connect"}, {"role", "player"}});
  require(connect_resp.has_value(), "connect response received");
  require((*connect_resp)["type"] == "connect", "connect success");

  // Send eval_model request
  json eval_req{
      {"type", "eval_model"},
      {"request_id", "req-client-42"},
      {"price", 100},
      {"health", 5},
      {"event_impact", 1},
      {"demand", 10},
      {"volume", 20},
      {"sentiment", 5},
      {"total_supply", 1000},
      {"reserve", 500}
  };

  auto eval_resp = request_json(client_fd, eval_req);
  require(eval_resp.has_value(), "eval_model response received via epoll");
  require((*eval_resp)["type"] == "eval_model_response", "eval_model_response type");
  require((*eval_resp)["status"] == "success", "status success");
  require((*eval_resp)["request_id"] == "req-client-42", "request_id correlated");
  require((*eval_resp)["projected_price"].get<int>() > 0, "projected_price positive");
  require((*eval_resp)["worker_pid"].get<int>() > 0, "worker_pid present");

  ::close(client_fd);
  std::cout << "[TEST] Real server–worker eval_model e2e passed." << std::endl;
}

static void test_server_worker_input_validation(const std::string& server_bin, const std::string& worker_bin) {
  std::cout << "[TEST] Server worker input validation and range checking..." << std::endl;
  ServerProcessFixture server(server_bin, worker_bin, 49153);

  int client_fd = connect_tcp(server.port);
  require(client_fd >= 0, "client connected");

  auto connect_resp = request_json(client_fd, json{{"type", "connect"}, {"role", "player"}});
  require(connect_resp.has_value() && (*connect_resp)["type"] == "connect", "connected");

  // Negative price check (invalid range)
  json bad_req{
      {"type", "eval_model"},
      {"request_id", "bad-price-1"},
      {"price", -10}
  };
  auto resp = request_json(client_fd, bad_req);
  require(resp.has_value(), "got validation response");
  require((*resp)["status"] == "failed", "status failed");
  require((*resp)["request_id"] == "bad-price-1", "request_id correlated");

  // Out-of-range health check
  json bad_health{
      {"type", "eval_model"},
      {"request_id", "bad-health-1"},
      {"health", 99999}
  };
  resp = request_json(client_fd, bad_health);
  require(resp.has_value(), "got validation response");
  require((*resp)["status"] == "failed", "status failed");
  require((*resp)["request_id"] == "bad-health-1", "request_id correlated");

  ::close(client_fd);
  std::cout << "[TEST] Server worker input validation passed." << std::endl;
}

static void test_server_worker_crash_and_recovery_e2e(const std::string& server_bin, const std::string& worker_bin) {
  std::cout << "[TEST] Real server mid-request worker SIGKILL and recovery..." << std::endl;
  ServerProcessFixture server(server_bin, worker_bin, 49154);

  int client_fd = connect_tcp(server.port);
  require(client_fd >= 0, "client connected");

  auto connect_resp = request_json(client_fd, json{{"type", "connect"}, {"role", "player"}});
  require(connect_resp.has_value() && (*connect_resp)["type"] == "connect", "connected");

  // Initial successful request
  json eval_req{
      {"type", "eval_model"},
      {"request_id", "req-1"},
      {"price", 50}
  };
  auto resp = request_json(client_fd, eval_req);
  require(resp.has_value() && (*resp)["status"] == "success", "first eval success");
  pid_t worker_pid = (*resp)["worker_pid"].get<pid_t>();
  require(worker_pid > 0, "valid worker pid");

  // Kill worker process directly with SIGKILL
  std::cout << "Killing active worker PID: " << worker_pid << " with SIGKILL" << std::endl;
  ::kill(worker_pid, SIGKILL);

  // Allow server tick / detection to notice worker death or restart
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Submit next request: server should detect worker failure, recover, and either fail pending or restart
  json eval_req2{
      {"type", "eval_model"},
      {"request_id", "req-2"},
      {"price", 60}
  };

  // Poll until recovery completes
  bool recovered = false;
  for (int retry = 0; retry < 50; ++retry) {
    auto resp2 = request_json(client_fd, eval_req2);
    if (resp2.has_value()) {
      if ((*resp2)["status"] == "success") {
        pid_t new_worker_pid = (*resp2)["worker_pid"].get<pid_t>();
        require(new_worker_pid != worker_pid, "worker recovered with new PID");
        recovered = true;
        break;
      } else {
        // May receive failed response while worker is relaunching
        std::cout << "Received intermediate status: " << (*resp2).dump() << ", retrying..." << std::endl;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  require(recovered, "server successfully recovered worker and serviced requests");

  ::close(client_fd);
  std::cout << "[TEST] Mid-request worker crash and recovery e2e passed." << std::endl;
}

static void test_server_worker_true_inflight_crash(const std::string& server_bin, const std::string& worker_bin) {
  std::cout << "[TEST] True in-flight crash test: SIGSTOP worker, send request, verify pending, SIGKILL..." << std::endl;
  ServerProcessFixture server(server_bin, worker_bin, 49156);

  int client_fd = connect_tcp(server.port);
  require(client_fd >= 0, "client connected");

  auto connect_resp = request_json(client_fd, json{{"type", "connect"}, {"role", "player"}});
  require(connect_resp.has_value() && (*connect_resp)["type"] == "connect", "connected");

  // Initial request to get active worker PID
  json eval_req0{
      {"type", "eval_model"},
      {"request_id", "req-init-1"},
      {"price", 50}
  };
  auto resp0 = request_json(client_fd, eval_req0);
  require(resp0.has_value() && (*resp0)["status"] == "success", "init eval success");
  pid_t worker_pid = (*resp0)["worker_pid"].get<pid_t>();
  require(worker_pid > 0, "valid worker pid");

  // 1. Freeze worker with SIGSTOP so it cannot pop or process incoming requests
  std::cout << "Freezing worker (PID: " << worker_pid << ") with SIGSTOP..." << std::endl;
  ::kill(worker_pid, SIGSTOP);

  // 2. Submit in-flight request while worker is frozen
  json inflight_req{
      {"type", "eval_model"},
      {"request_id", "req-inflight-99"},
      {"price", 75}
  };
  const auto outgoing = Frame(inflight_req).serialize();
  require(send_all(client_fd, outgoing.data(), outgoing.size()), "sent in-flight request");

  // 3. Verify request remains pending and has not received an immediate response
  // Set short timeout to verify no premature frame arrives
  struct timeval tv{0, 200'000}; // 200ms
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  auto premature_resp = read_single_frame(client_fd);
  require(!premature_resp.has_value(), "request correctly remained pending while worker was frozen");

  // Restore 3-second read timeout
  struct timeval tv_normal{3, 0};
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv_normal, sizeof(tv_normal));

  // 4. Kill the frozen worker with SIGKILL while request is truly in-flight
  std::cout << "Killing frozen worker with SIGKILL..." << std::endl;
  ::kill(worker_pid, SIGKILL);

  // 5. The server must detect worker termination and return failure for that exact in-flight request
  auto inflight_resp = read_single_frame(client_fd);
  require(inflight_resp.has_value(), "received failure response for in-flight request");
  require((*inflight_resp)["type"] == "eval_model_response", "eval_model_response type");
  require((*inflight_resp)["status"] == "failed", "status failed for in-flight request");
  require((*inflight_resp)["request_id"] == "req-inflight-99", "correlation ID matched exact in-flight request");
  std::cout << "In-flight failure verified: " << (*inflight_resp).dump() << std::endl;

  // 6. Verify server recovers and successfully services subsequent request
  json subsequent_req{
      {"type", "eval_model"},
      {"request_id", "req-subsequent-100"},
      {"price", 80}
  };

  bool recovered = false;
  for (int retry = 0; retry < 50; ++retry) {
    auto sub_resp = request_json(client_fd, subsequent_req);
    if (sub_resp.has_value() && (*sub_resp)["status"] == "success") {
      require((*sub_resp)["request_id"] == "req-subsequent-100", "subsequent request_id correlated");
      pid_t new_pid = (*sub_resp)["worker_pid"].get<pid_t>();
      require(new_pid != worker_pid, "new worker process spawned with new PID");
      recovered = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  require(recovered, "subsequent request processed successfully by recovered worker");

  // 7. Verify no duplicate or trailing frames in socket buffer
  setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  auto trailing_resp = read_single_frame(client_fd);
  require(!trailing_resp.has_value(), "no trailing or duplicate frames in stream");

  ::close(client_fd);
  std::cout << "[TEST] True in-flight crash test passed." << std::endl;
}

static void test_server_worker_startup_failure(const std::string& server_bin) {
  std::cout << "[TEST] Server behavior when worker fails startup..." << std::endl;
  // Non-existent worker binary path
  ServerProcessFixture server(server_bin, "/nonexistent/path/darkpool_worker", 49155);

  int client_fd = connect_tcp(server.port);
  require(client_fd >= 0, "client connected");

  auto connect_resp = request_json(client_fd, json{{"type", "connect"}, {"role", "player"}});
  require(connect_resp.has_value() && (*connect_resp)["type"] == "connect", "connected");

  json eval_req{
      {"type", "eval_model"},
      {"request_id", "req-unavailable"},
      {"price", 50}
  };
  auto resp = request_json(client_fd, eval_req);
  require(resp.has_value(), "got response");
  require((*resp)["status"] == "failed", "status failed when worker unavailable");
  require((*resp)["request_id"] == "req-unavailable", "request_id preserved");

  ::close(client_fd);
  std::cout << "[TEST] Server worker startup failure handling passed." << std::endl;
}

} // namespace darkpool::ipc::test

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: darkpool_worker_integration_tests <darkpool_worker_path> [server_path]" << std::endl;
    return 1;
  }
  std::string worker_bin = argv[1];
  std::string server_bin = (argc >= 3) ? argv[2] : "./build/darkpool_server";

  try {
    darkpool::ipc::test::test_worker_channel_direct();
    darkpool::ipc::test::test_queue_bounds_and_backpressure();
    darkpool::ipc::test::test_process_manager_end_to_end(worker_bin);
    darkpool::ipc::test::test_server_worker_eval_model_e2e(server_bin, worker_bin);
    darkpool::ipc::test::test_server_worker_input_validation(server_bin, worker_bin);
    darkpool::ipc::test::test_server_worker_crash_and_recovery_e2e(server_bin, worker_bin);
    darkpool::ipc::test::test_server_worker_true_inflight_crash(server_bin, worker_bin);
    darkpool::ipc::test::test_server_worker_startup_failure(server_bin);
  } catch (const std::exception& ex) {
    std::cerr << "Test failed with exception: " << ex.what() << std::endl;
    return 1;
  }

  std::cout << "\n>>> ALL IPC WORKER INTEGRATION TESTS PASSED <<<" << std::endl;
  return 0;
}
