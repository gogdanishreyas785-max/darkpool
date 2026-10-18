#pragma once

#include "darkpool/game_engine.hpp"
#include "darkpool/network.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace darkpool::server {

enum class SessionRole { Player, Moderator };

struct OutgoingNotification {
  std::vector<std::uint64_t> recipients;
  network::json payload;
};

struct PhaseDurations {
  std::chrono::seconds discussion{180};
  std::chrono::seconds trading{120};
  std::chrono::seconds voting{60};
  std::chrono::seconds transition{10};
  std::chrono::seconds clue{30};
};

struct MatchTimer {
  bool active{false};
  bool paused{false};
  Phase phase{Phase::Lobby};
  bool in_transition{false};
  Phase upcoming_phase{Phase::Lobby};
  std::chrono::seconds total_duration{0};
  std::chrono::steady_clock::time_point deadline{};
  std::chrono::milliseconds paused_remaining{0};
  bool warning_30_fired{false};
  bool warning_10_fired{false};
};

class MatchCoordinator {
 public:
  MatchCoordinator() = default;
  ~MatchCoordinator() = default;
  void register_session(std::uint64_t session, SessionRole role);
  void unregister_session(std::uint64_t session) noexcept;

  // Returns nullopt for messages outside the coordinator command set (echo path).
  [[nodiscard]] std::optional<network::json> handle(
      std::uint64_t session, const network::json& command);

  [[nodiscard]] std::vector<OutgoingNotification> drain_notifications();

  void tick(std::optional<std::chrono::steady_clock::time_point> now = std::nullopt);
  void set_simulated_time(std::optional<std::chrono::steady_clock::time_point> now);
  void set_phase_durations(PhaseDurations durations);
  [[nodiscard]] PhaseDurations phase_durations() const noexcept { return phase_durations_; }
  void set_default_market_coefficients(MarketModelCoefficients coeffs) noexcept { default_market_coefficients_ = coeffs; }
  [[nodiscard]] const MarketModelCoefficients& default_market_coefficients() const noexcept { return default_market_coefficients_; }
  void set_default_open_lobby(bool open_lobby) noexcept { default_open_lobby_ = open_lobby; }
  [[nodiscard]] bool default_open_lobby() const noexcept { return default_open_lobby_; }

 private:
  struct Session {
    SessionRole role{SessionRole::Player};
    std::optional<std::uint64_t> player_match;
    std::optional<PlayerId> player_id;
    std::vector<std::uint64_t> moderator_matches;
  };
  struct MatchRecord {
    std::unique_ptr<Match> match;
    std::uint64_t moderator{};
    std::vector<std::optional<std::uint64_t>> player_sessions;
    std::optional<MarketResult> last_market;
    std::optional<VoteResult> last_vote;
    std::uint64_t state_version{1};
    bool is_aborted{false};
    MatchTimer timer{};
    std::optional<std::string> join_token;
  };

  OperationResult apply_open_trading(std::uint64_t match_id, MatchRecord& record,
                                     std::chrono::steady_clock::time_point now);
  MarketResolution apply_resolve_market(std::uint64_t match_id, MatchRecord& record,
                                       std::chrono::steady_clock::time_point now);
  OperationResult apply_open_voting(std::uint64_t match_id, MatchRecord& record,
                                    std::chrono::steady_clock::time_point now);
  VoteResolution apply_resolve_votes(std::uint64_t match_id, MatchRecord& record,
                                     std::chrono::steady_clock::time_point now);
  void start_transition_interval(std::uint64_t match_id, MatchRecord& record, Phase upcoming,
                                 std::chrono::steady_clock::time_point now);
  [[nodiscard]] std::chrono::steady_clock::time_point now_() const;

  [[nodiscard]] bool is_member(std::uint64_t session, std::uint64_t match_id) const;
  [[nodiscard]] static std::vector<std::uint64_t> get_match_members(const MatchRecord& record);
  std::map<std::uint64_t, Session> sessions_;
  std::map<std::uint64_t, MatchRecord> matches_;
  std::vector<OutgoingNotification> pending_notifications_;
  std::uint64_t next_match_id_{1};
  std::optional<std::chrono::steady_clock::time_point> simulated_time_{std::nullopt};
  PhaseDurations phase_durations_{};
  MarketModelCoefficients default_market_coefficients_{MarketModelCoefficients::approved_baseline()};
  bool default_open_lobby_{false};
};

}  // namespace darkpool::server
