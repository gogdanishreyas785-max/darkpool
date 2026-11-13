#include <darkpool/server_worker_channel.hpp>
#include <darkpool/worker_process_manager.hpp>
#include <darkpool/worker_protocol.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string>
#include <sys/mman.h>
#include <vector>

namespace darkpool::ipc::bench {

struct BenchmarkMetrics {
  std::string name;
  uint64_t total_messages{0};
  double elapsed_sec{0.0};
  double throughput_msg_sec{0.0};
  uint64_t min_ns{0};
  uint64_t p50_ns{0};
  uint64_t p90_ns{0};
  uint64_t p99_ns{0};
  uint64_t max_ns{0};
};

static void print_metrics(const BenchmarkMetrics& m) {
  std::cout << "\n======================================================\n";
  std::cout << " Benchmark: " << m.name << "\n";
  std::cout << " Total Messages: " << m.total_messages << "\n";
  std::cout << " Elapsed Time:   " << std::fixed << std::setprecision(4) << m.elapsed_sec << " s\n";
  std::cout << " Throughput:     " << std::fixed << std::setprecision(2) << m.throughput_msg_sec << " msg/sec\n";
  std::cout << " Round-trip Latency (ns):\n";
  std::cout << "   Min: " << m.min_ns << " ns\n";
  std::cout << "   p50: " << m.p50_ns << " ns\n";
  std::cout << "   p90: " << m.p90_ns << " ns\n";
  std::cout << "   p99: " << m.p99_ns << " ns\n";
  std::cout << "   Max: " << m.max_ns << " ns\n";
  std::cout << "======================================================\n";
}

static BenchmarkMetrics benchmark_ping_pong(WorkerProcessManager& mgr, uint64_t iterations) {
  std::vector<uint64_t> latencies;
  latencies.reserve(iterations);

  // Warm-up
  for (uint64_t i = 0; i < 100; ++i) {
    WorkerRequestMessage req{};
    req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
    req.request_id = i + 1;
    if (mgr.send_request(req, std::chrono::milliseconds{100}) != QueueOpStatus::Success) {
      std::cerr << "Warmup ping send failed\n";
      std::abort();
    }

    const auto warm_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
    bool got_warm = false;
    while (std::chrono::steady_clock::now() < warm_deadline) {
      if (mgr.drain_responses([](const WorkerResponseMessage&) {}) > 0) {
        got_warm = true;
        break;
      }
      struct timespec ts{0, 100'000L};
      ::nanosleep(&ts, nullptr);
    }
    if (!got_warm) {
      std::cerr << "Warmup ping response timeout\n";
      std::abort();
    }
  }

  auto start_time = std::chrono::steady_clock::now();
  uint64_t completed = 0;

  for (uint64_t i = 0; i < iterations; ++i) {
    WorkerRequestMessage req{};
    req.opcode = static_cast<int32_t>(WorkerOpcode::Ping);
    req.request_id = i + 1;

    auto t0 = std::chrono::steady_clock::now();
    if (mgr.send_request(req, std::chrono::milliseconds{1000}) != QueueOpStatus::Success) {
      std::cerr << "Ping send failed at iteration " << i << '\n';
      std::abort();
    }

    const auto ping_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{2000};
    bool got_resp = false;
    while (std::chrono::steady_clock::now() < ping_deadline) {
      size_t drained = mgr.drain_responses([&](const WorkerResponseMessage& resp) {
        if (resp.request_id != (i + 1) || resp.status != static_cast<int32_t>(WorkerStatusCode::Success)) {
          std::cerr << "Mismatched ping response ID or status! Expected " << (i + 1)
                    << ", got " << resp.request_id << " status " << resp.status << '\n';
          std::abort();
        }
        got_resp = true;
      });
      if (drained > 0 && got_resp) {
        break;
      }
      struct timespec ts{0, 50'000L};
      ::nanosleep(&ts, nullptr);
    }

    if (!got_resp) {
      std::cerr << "FATAL: Ping response timed out at iteration " << i << '\n';
      std::abort();
    }

    auto t1 = std::chrono::steady_clock::now();
    uint64_t duration_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    latencies.push_back(duration_ns);
    completed++;
  }

  auto end_time = std::chrono::steady_clock::now();
  double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

  if (completed != iterations) {
    std::cerr << "FATAL: Completed count " << completed << " != requested iterations " << iterations << '\n';
    std::abort();
  }

  std::sort(latencies.begin(), latencies.end());

  BenchmarkMetrics m;
  m.name = "Worker Process-Shared Cross-Process Ping-Pong";
  m.total_messages = completed;
  m.elapsed_sec = elapsed_sec;
  m.throughput_msg_sec = static_cast<double>(completed) / elapsed_sec;
  m.min_ns = latencies.front();
  m.p50_ns = latencies[completed * 50 / 100];
  m.p90_ns = latencies[completed * 90 / 100];
  m.p99_ns = latencies[completed * 99 / 100];
  m.max_ns = latencies.back();

  return m;
}

static BenchmarkMetrics benchmark_pipelined_eval(WorkerProcessManager& mgr, uint64_t total_items, uint64_t batch_size) {
  std::vector<uint64_t> latencies;
  latencies.reserve(total_items);

  auto start_time = std::chrono::steady_clock::now();

  uint64_t sent = 0;
  uint64_t received = 0;

  std::vector<std::chrono::steady_clock::time_point> sent_times(total_items);
  std::vector<bool> received_ids(total_items, false);

  const auto pipeline_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};

  while (received < total_items) {
    if (std::chrono::steady_clock::now() > pipeline_deadline) {
      std::cerr << "FATAL: Pipelined benchmark timed out! Sent: " << sent << ", Received: " << received << '\n';
      std::abort();
    }

    // Send up to batch_size in-flight
    while (sent < total_items && (sent - received) < batch_size) {
      WorkerRequestMessage req{};
      req.opcode = static_cast<int32_t>(WorkerOpcode::EvaluateMarketModel);
      req.request_id = sent + 1;
      req.price = 100;
      req.health = 50;
      req.event_impact = 5;
      req.demand = 10;
      req.volume = 15;
      req.sentiment = 10;
      req.total_supply = 1000;
      req.reserve = 500;

      sent_times[sent] = std::chrono::steady_clock::now();
      if (mgr.send_request(req, std::chrono::milliseconds{0}) != QueueOpStatus::Success) {
        break;
      }
      sent++;
    }

    // Drain available responses
    size_t drained = mgr.drain_responses([&](const WorkerResponseMessage& resp) {
      auto now = std::chrono::steady_clock::now();
      if (resp.request_id < 1 || resp.request_id > total_items) {
        std::cerr << "FATAL: Received out-of-range request_id in response: " << resp.request_id << '\n';
        std::abort();
      }
      if (resp.status != static_cast<int32_t>(WorkerStatusCode::Success)) {
        std::cerr << "FATAL: Response status failure: " << resp.status << '\n';
        std::abort();
      }
      const size_t idx = static_cast<size_t>(resp.request_id - 1);
      if (received_ids[idx]) {
        std::cerr << "FATAL: Duplicate response received for ID " << resp.request_id << '\n';
        std::abort();
      }
      received_ids[idx] = true;

      uint64_t latency = static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now - sent_times[idx]).count());
      latencies.push_back(latency);
      received++;
    });

    if (drained == 0 && sent < total_items) {
      struct timespec ts{0, 10'000L};
      ::nanosleep(&ts, nullptr);
    }
  }

  auto end_time = std::chrono::steady_clock::now();
  double elapsed_sec = std::chrono::duration<double>(end_time - start_time).count();

  if (received != total_items || sent != total_items) {
    std::cerr << "FATAL: Mismatched counts! Sent: " << sent << ", Received: " << received
              << ", Expected: " << total_items << '\n';
    std::abort();
  }

  std::sort(latencies.begin(), latencies.end());

  BenchmarkMetrics m;
  m.name = "Worker Process-Shared Batch Pipeline (EvaluateMarketModel)";
  m.total_messages = received;
  m.elapsed_sec = elapsed_sec;
  m.throughput_msg_sec = static_cast<double>(received) / elapsed_sec;
  m.min_ns = latencies.empty() ? 0 : latencies.front();
  m.p50_ns = latencies.empty() ? 0 : latencies[latencies.size() * 50 / 100];
  m.p90_ns = latencies.empty() ? 0 : latencies[latencies.size() * 90 / 100];
  m.p99_ns = latencies.empty() ? 0 : latencies[latencies.size() * 99 / 100];
  m.max_ns = latencies.empty() ? 0 : latencies.back();

  return m;
}

}  // namespace darkpool::ipc::bench

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: darkpool_worker_benchmark <darkpool_worker_binary> [iterations]" << std::endl;
    return 1;
  }
  std::string worker_bin = argv[1];
  uint64_t iterations = (argc >= 3) ? std::stoull(argv[2]) : 10000;

  std::string req_shm = "/dp_bench_req_" + std::to_string(::getpid());
  std::string resp_shm = "/dp_bench_resp_" + std::to_string(::getpid());
  ::shm_unlink(req_shm.c_str());
  ::shm_unlink(resp_shm.c_str());

  darkpool::ipc::WorkerProcessConfig cfg;
  cfg.worker_binary_path = worker_bin;
  cfg.request_shm_name = req_shm;
  cfg.response_shm_name = resp_shm;
  cfg.queue_capacity = 256;

  std::string err;
  auto mgr_opt = darkpool::ipc::WorkerProcessManager::launch(cfg, err);
  if (!mgr_opt) {
    std::cerr << "Failed to start worker process: " << err << std::endl;
    return 1;
  }

  std::cout << ">>> Running Darkpool Server-Worker IPC Benchmark (" << iterations << " iterations) <<<\n";
  auto ping_pong_metrics = darkpool::ipc::bench::benchmark_ping_pong(*mgr_opt, iterations);
  darkpool::ipc::bench::print_metrics(ping_pong_metrics);

  auto pipelined_metrics = darkpool::ipc::bench::benchmark_pipelined_eval(*mgr_opt, iterations, 128);
  darkpool::ipc::bench::print_metrics(pipelined_metrics);

  mgr_opt->shutdown();
  ::shm_unlink(req_shm.c_str());
  ::shm_unlink(resp_shm.c_str());
  return 0;
}
