#include <darkpool/server_worker_channel.hpp>
#include <darkpool/worker_process_manager.hpp>
#include <darkpool/worker_protocol.hpp>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace darkpool::ipc::test {

static void cleanup_shm(const std::string& name) {
  ::shm_unlink(name.c_str());
}

static void require(bool cond, const char* msg) {
  if (!cond) {
    std::cerr << "Assertion failure: " << msg << std::endl;
    std::abort();
  }
}

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

  // Client connects
  auto client_chan = WorkerClientChannel::open(
      req_shm, resp_shm, srv_chan->worker_notify_fd(), srv_chan->server_notify_fd(), err);
  require(client_chan.has_value(), "open client_chan");

  // Server sends ping
  WorkerRequestMessage req{};
  req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
  req.request_id = 1;
  req.price = 100;

  require(srv_chan->send_request(req, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "send_request");

  // Worker consumes notification and pops request
  auto notif = client_chan->consume_worker_notification();
  require(notif.has_value() && *notif > 0, "consume_worker_notification");

  WorkerRequestMessage popped_req{};
  require(client_chan->pop_request(popped_req, std::chrono::milliseconds{500}) == QueueOpStatus::Success,
          "pop_request");
  require(popped_req.request_id == 1, "popped_req id");
  require(popped_req.opcode == static_cast<int32_t>(WorkerOpcode::Ping), "popped_req opcode");
  require(popped_req.price == 100, "popped_req price");

  // Worker sends pong response
  WorkerResponseMessage resp{};
  resp.request_id = popped_req.request_id;
  resp.opcode = popped_req.opcode;
  resp.status = static_cast<int32_t>(WorkerStatusCode::Success);
  resp.projected_price = 100;

  require(client_chan->send_response(resp, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "send_response");

  // Server consumes notification and drains response
  auto srv_notif = srv_chan->consume_server_notification();
  require(srv_notif.has_value() && *srv_notif > 0, "consume_server_notification");

  size_t drained = 0;
  WorkerResponseMessage popped_resp{};
  drained = srv_chan->drain_responses([&](const WorkerResponseMessage& r) {
    popped_resp = r;
  });
  require(drained == 1, "drained count");
  require(popped_resp.request_id == 1, "resp id");
  require(popped_resp.status == static_cast<int32_t>(WorkerStatusCode::Success), "resp status");
  require(popped_resp.projected_price == 100, "resp price");

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

  // Capacity 4
  ServerWorkerChannelConfig cfg{req_shm, resp_shm, 4};
  std::string err;
  auto srv_chan = ServerWorkerChannel::create(cfg, err);
  require(srv_chan.has_value(), "create srv_chan");

  // Fill request queue (capacity is 4)
  for (uint64_t i = 0; i < 4; ++i) {
    WorkerRequestMessage req{};
    req.request_id = i + 1;
    req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
    require(srv_chan->send_request(req, std::chrono::milliseconds{10}) == QueueOpStatus::Success,
            "send_request");
  }

  // 5th push should fail with backpressure / timeout
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

  // Send Ping
  WorkerRequestMessage req{};
  req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
  req.request_id = 100;
  req.price = 50;
  require(mgr.send_request(req, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "mgr send_request");

  // Poll for response
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

  // Send EvaluateMarketModel
  WorkerRequestMessage eval_req{};
  eval_req.opcode = static_cast<int32_t>(WorkerOpcode::EvaluateMarketModel);
  eval_req.request_id = 101;
  eval_req.price = 100;
  eval_req.health = 80;
  eval_req.event_impact = 10;
  eval_req.demand = 5;
  eval_req.volume = 20;
  eval_req.sentiment = 15;
  eval_req.total_supply = 1000;
  eval_req.reserve = 500;

  require(mgr.send_request(eval_req, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "mgr send eval_req");

  got_resp = false;
  for (int retry = 0; retry < 50; ++retry) {
    if (mgr.drain_responses([&](const WorkerResponseMessage& r) {
          resp = r;
          got_resp = true;
        }) > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  require(got_resp, "got eval response");
  require(resp.request_id == 101, "resp id 101");
  require(resp.status == static_cast<int32_t>(WorkerStatusCode::Success), "resp status success");
  require(resp.projected_price > 0, "projected price > 0");
  std::cout << "Model eval result: projected price=" << resp.projected_price
            << " delta=" << resp.price_delta << std::endl;

  // Clean shutdown
  require(mgr.shutdown(std::chrono::milliseconds{1000}), "mgr shutdown");
  require(!mgr.is_alive(), "mgr not alive");
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);
  std::cout << "[TEST] ProcessManager end-to-end passed." << std::endl;
}

static void test_worker_crash_recovery(const std::string& worker_bin) {
  std::cout << "[TEST] Worker crash recovery & restart..." << std::endl;
  std::string req_shm = "/dp_test_crash_req_" + std::to_string(::getpid());
  std::string resp_shm = "/dp_test_crash_resp_" + std::to_string(::getpid());
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);

  WorkerProcessConfig cfg;
  cfg.worker_binary_path = worker_bin;
  cfg.request_shm_name = req_shm;
  cfg.response_shm_name = resp_shm;
  cfg.queue_capacity = 16;

  std::string err;
  auto mgr_opt = WorkerProcessManager::launch(cfg, err);
  require(mgr_opt.has_value(), "launch mgr");
  pid_t worker_pid = mgr_opt->worker_pid();
  require(worker_pid > 0, "worker_pid > 0");

  // Send SIGKILL directly to worker to simulate abnormal termination
  std::cout << "Simulating sudden crash by SIGKILL on worker pid: " << worker_pid << std::endl;
  ::kill(worker_pid, SIGKILL);

  // Wait a brief moment and verify try_reap identifies worker exit
  bool reaped = false;
  for (int i = 0; i < 50; ++i) {
    if (mgr_opt->try_reap()) {
      reaped = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  require(reaped, "reaped worker");
  require(!mgr_opt->is_alive(), "mgr not alive after kill");

  // Destroy previous manager and launch new worker instance cleanly
  mgr_opt.reset();
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);

  auto restarted_mgr = WorkerProcessManager::launch(cfg, err);
  require(restarted_mgr.has_value(), "relaunch worker");
  require(restarted_mgr->is_alive(), "restarted is_alive");
  require(restarted_mgr->worker_pid() != worker_pid, "new worker_pid");

  // Verify worker processes requests again
  WorkerRequestMessage req{};
  req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
  req.request_id = 200;
  require(restarted_mgr->send_request(req, std::chrono::milliseconds{100}) == QueueOpStatus::Success,
          "restarted send_request");

  WorkerResponseMessage resp{};
  bool got_resp = false;
  for (int retry = 0; retry < 50; ++retry) {
    if (restarted_mgr->drain_responses([&](const WorkerResponseMessage& r) {
          resp = r;
          got_resp = true;
        }) > 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  require(got_resp, "got resp after restart");
  require(resp.request_id == 200, "resp id 200");

  restarted_mgr->shutdown();
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);
  std::cout << "[TEST] Worker crash recovery passed." << std::endl;
}

static void test_stale_shm_cleanup(const std::string& worker_bin) {
  std::cout << "[TEST] Stale shared memory cleanup on launch..." << std::endl;
  std::string req_shm = "/dp_test_stale_req_" + std::to_string(::getpid());
  std::string resp_shm = "/dp_test_stale_resp_" + std::to_string(::getpid());

  // Create orphaned / stale POSIX shm files directly
  int fd1 = ::shm_open(req_shm.c_str(), O_CREAT | O_RDWR, 0660);
  int fd2 = ::shm_open(resp_shm.c_str(), O_CREAT | O_RDWR, 0660);
  if (fd1 >= 0) ::close(fd1);
  if (fd2 >= 0) ::close(fd2);

  WorkerProcessConfig cfg;
  cfg.worker_binary_path = worker_bin;
  cfg.request_shm_name = req_shm;
  cfg.response_shm_name = resp_shm;
  cfg.queue_capacity = 16;

  // WorkerProcessManager should unlink stale shm and succeed
  std::string err;
  auto mgr_opt = WorkerProcessManager::launch(cfg, err);
  require(mgr_opt.has_value(), "launch with stale shm");
  require(mgr_opt->is_alive(), "is_alive with stale shm");

  mgr_opt->shutdown();
  cleanup_shm(req_shm);
  cleanup_shm(resp_shm);
  std::cout << "[TEST] Stale shared memory cleanup passed." << std::endl;
}

}  // namespace darkpool::ipc::test

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: darkpool_worker_integration_tests <darkpool_worker_path> [server_path]" << std::endl;
    return 1;
  }
  std::string worker_bin = argv[1];

  try {
    darkpool::ipc::test::test_worker_channel_direct();
    darkpool::ipc::test::test_queue_bounds_and_backpressure();
    darkpool::ipc::test::test_process_manager_end_to_end(worker_bin);
    darkpool::ipc::test::test_worker_crash_recovery(worker_bin);
    darkpool::ipc::test::test_stale_shm_cleanup(worker_bin);
  } catch (const std::exception& ex) {
    std::cerr << "Test failed with exception: " << ex.what() << std::endl;
    return 1;
  }

  std::cout << "\n>>> ALL IPC WORKER INTEGRATION TESTS PASSED <<<" << std::endl;
  return 0;
}
