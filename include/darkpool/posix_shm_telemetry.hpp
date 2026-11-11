#pragma once

#if defined(__linux__)

#include <chrono>
#include <csignal>
#include <cstdint>
#include <fcntl.h>
#include <functional>
#include <optional>
#include <pthread.h>
#include <semaphore.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <type_traits>
#include <unistd.h>

namespace darkpool::telemetry {

inline constexpr std::uint32_t kTelemetryMagic = 0x44505445U;  // 'DPTE' (DarkPool TElemetry)
inline constexpr std::uint32_t kTelemetryAbiVersion = 1U;

enum class ServerState : std::uint32_t {
  Uninitialized = 0U,
  Running = 1U,
  Stopping = 2U
};

[[nodiscard]] inline std::string_view server_state_to_string(ServerState state) noexcept {
  switch (state) {
    case ServerState::Running:
      return "running";
    case ServerState::Stopping:
      return "stopping";
    case ServerState::Uninitialized:
    default:
      return "uninitialized";
  }
}

[[nodiscard]] inline ServerState server_state_from_uint32(std::uint32_t value) noexcept {
  switch (value) {
    case 1U:
      return ServerState::Running;
    case 2U:
      return ServerState::Stopping;
    default:
      return ServerState::Uninitialized;
  }
}

#pragma pack(push, 8)
struct TelemetryPayload {
  std::uint64_t sequence;
  std::uint64_t timestamp_ns;
  std::int64_t server_pid;
  std::uint32_t server_state;
  std::uint32_t reserved;
  std::uint64_t active_matches;
  std::uint64_t connected_players;
  std::uint64_t connected_moderators;
  std::uint64_t match_capacity;
  std::uint64_t players_per_match_capacity;
  std::uint64_t player_connection_capacity;
};

struct TelemetrySharedMemory {
  std::uint32_t magic;
  std::uint32_t abi_version;
  std::uint32_t struct_size;
  std::uint32_t owner_uid;
  std::int64_t server_pid;
  std::uint32_t state;
  std::uint32_t reserved;
  pthread_mutex_t mutex;
  TelemetryPayload payload;
};
#pragma pack(pop)

static_assert(std::is_trivial_v<TelemetryPayload>,
              "TelemetryPayload must be trivial");
static_assert(std::is_trivial_v<TelemetrySharedMemory>,
              "TelemetrySharedMemory must be trivial");
static_assert(std::is_standard_layout_v<TelemetrySharedMemory>,
              "TelemetrySharedMemory must have standard layout");

struct TelemetrySnapshot {
  std::uint64_t sequence{0U};
  std::uint64_t timestamp_ns{0U};
  pid_t server_pid{0};
  ServerState server_state{ServerState::Uninitialized};
  std::uint64_t active_matches{0U};
  std::uint64_t connected_players{0U};
  std::uint64_t connected_moderators{0U};
  std::uint64_t match_capacity{0U};
  std::uint64_t players_per_match_capacity{0U};
  std::uint64_t player_connection_capacity{0U};
};

struct TelemetryInput {
  std::uint64_t active_matches{0U};
  std::uint64_t connected_players{0U};
  std::uint64_t connected_moderators{0U};
  std::uint64_t match_capacity{4U};
  std::uint64_t players_per_match_capacity{64U};
  std::uint64_t player_connection_capacity{256U};
};

[[nodiscard]] std::string default_shm_name(uid_t uid = ::geteuid());
[[nodiscard]] std::string default_sem_name(uid_t uid = ::geteuid());
[[nodiscard]] bool valid_ipc_name(std::string_view name) noexcept;

class TelemetryPublisher {
 public:
  TelemetryPublisher(std::string shm_name = default_shm_name(),
                     std::string sem_name = default_sem_name());
  ~TelemetryPublisher();

  TelemetryPublisher(const TelemetryPublisher&) = delete;
  TelemetryPublisher& operator=(const TelemetryPublisher&) = delete;
  TelemetryPublisher(TelemetryPublisher&&) = delete;
  TelemetryPublisher& operator=(TelemetryPublisher&&) = delete;

  [[nodiscard]] bool start(std::string& error_message);
  void stop() noexcept;

  // Nonblocking publication from the event loop thread using trylock.
  // Returns true if publication succeeded, false if lock was busy or unavailable.
  [[nodiscard]] bool publish(const TelemetryInput& input, bool is_stopping = false) noexcept;

  [[nodiscard]] const std::string& shm_name() const noexcept { return shm_name_; }
  [[nodiscard]] const std::string& sem_name() const noexcept { return sem_name_; }
  [[nodiscard]] bool running() const noexcept { return running_; }

 private:
  std::string shm_name_;
  std::string sem_name_;
  int shm_fd_{-1};
  TelemetrySharedMemory* shm_{nullptr};
  sem_t* sem_{SEM_FAILED};
  bool owns_shm_{false};
  bool owns_sem_{false};
  bool mutex_initialized_{false};
  bool running_{false};
  std::uint64_t sequence_{0U};
};

class TelemetryObserver {
 public:
  TelemetryObserver() noexcept = default;
  ~TelemetryObserver();

  TelemetryObserver(const TelemetryObserver&) = delete;
  TelemetryObserver& operator=(const TelemetryObserver&) = delete;
  TelemetryObserver(TelemetryObserver&& other) noexcept;
  TelemetryObserver& operator=(TelemetryObserver&& other) noexcept;

  // Open existing shared memory and semaphore objects (does NOT create them).
  [[nodiscard]] bool open(std::string_view shm_name, std::string_view sem_name,
                          std::string& error_message);
  void close() noexcept;

  // Acquire process-shared robust mutex with bounded timeout and copy snapshot.
  [[nodiscard]] std::optional<TelemetrySnapshot> read_snapshot(
      std::chrono::milliseconds timeout, std::string& error_message) const;

  // Wait for notification on semaphore with bounded timeout.
  // Returns true if notified, false if timed out or interrupted.
  [[nodiscard]] bool wait_for_notification(
      std::chrono::milliseconds timeout,
      const volatile sig_atomic_t* interrupted = nullptr) const noexcept;

  [[nodiscard]] bool is_open() const noexcept { return shm_ != nullptr; }

 private:
  std::string shm_name_;
  std::string sem_name_;
  int shm_fd_{-1};
  TelemetrySharedMemory* shm_{nullptr};
  sem_t* sem_{SEM_FAILED};
};

}  // namespace darkpool::telemetry

#endif  // defined(__linux__)
