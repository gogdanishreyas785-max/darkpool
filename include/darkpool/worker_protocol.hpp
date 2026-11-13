#pragma once

#if defined(__linux__)

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace darkpool::ipc {

inline constexpr std::size_t kWorkerQueueCapacity = 256U;
inline constexpr std::uint32_t kWorkerProtocolVersion = 1U;

enum class WorkerOpcode : std::int32_t {
  Ping = 1,
  EvaluateMarketModel = 2,
  Shutdown = 3
};

enum class WorkerStatusCode : std::int32_t {
  Success = 0,
  InvalidPayload = 1,
  QueueFull = 2,
  TimedOut = 3,
  WorkerFaulted = 4,
  WorkerExited = 5
};

#pragma pack(push, 8)
struct WorkerRequestMessage {
  std::uint64_t request_id{0};
  std::uint64_t client_session_id{0};
  std::int32_t opcode{static_cast<std::int32_t>(WorkerOpcode::Ping)};
  std::int32_t price{10};
  std::int32_t health{5};
  std::int32_t event_impact{0};
  std::int32_t demand{0};
  std::int32_t volume{0};
  std::int32_t sentiment{0};
  std::uint32_t total_supply{100};
  std::uint32_t reserve{50};
  std::uint32_t flags{0};
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<WorkerRequestMessage>, "WorkerRequestMessage must be standard layout");
static_assert(sizeof(WorkerRequestMessage) <= 256U, "WorkerRequestMessage must fit in QueueMessageSlot");

#pragma pack(push, 8)
struct WorkerResponseMessage {
  std::uint64_t request_id{0};
  std::uint64_t client_session_id{0};
  std::int32_t opcode{0};
  std::int32_t status{0};            // WorkerStatusCode
  std::int32_t projected_price{0};
  std::int32_t projected_health{0};
  std::int32_t price_delta{0};
  std::int32_t baseline_price{0};
  std::uint32_t flags{0};
  std::uint32_t worker_pid{0};
};
#pragma pack(pop)

static_assert(std::is_standard_layout_v<WorkerResponseMessage>, "WorkerResponseMessage must be standard layout");
static_assert(sizeof(WorkerResponseMessage) <= 256U, "WorkerResponseMessage must fit in QueueMessageSlot");

}  // namespace darkpool::ipc

#endif  // defined(__linux__)

