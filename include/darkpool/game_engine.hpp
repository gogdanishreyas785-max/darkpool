#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace darkpool {

using PlayerId = std::uint32_t;
using Credits = std::uint32_t;
using Shares = std::uint32_t;

inline constexpr std::size_t kPlayerCount = 5;
inline constexpr std::uint32_t kRoundCount = 3;
inline constexpr Credits kStartingCredits = 100;
inline constexpr Shares kStartingShares = 10;
inline constexpr Credits kCreditBalanceCap = 1'000'000;
inline constexpr std::int32_t kInitialHealth = 5;
inline constexpr std::int32_t kMinHealth = 0;
inline constexpr std::int32_t kMaxHealth = 10;
inline constexpr std::int32_t kInitialPrice = 10;
inline constexpr std::int32_t kMinPrice = 1;
inline constexpr std::int32_t kMaxPrice = 100;

enum class Role { Insider, Investigator, Employee };
enum class Phase { Lobby, AwaitingEvent, Discussion, Trading, Clue, Voting, Finished };
enum class TradeAction { Buy, Hold, Sell };
enum class Outcome { None, InsiderWin, EmployeesWin };

enum class ErrorCode {
  None,
  WrongPhase,
  UnknownPlayer,
  InactivePlayer,
  InvalidRoster,
  InvalidEventImpact,
  InvalidAction,
  ActionAlreadySubmitted,
  InsufficientCredits,
  NoSharesToSell,
  CreditCapExceeded,
  NotInvestigator,
  InvalidClueTarget,
  ClueAlreadyUsed,
  InvalidVoteTarget,
  VoteAlreadySubmitted,
  InvalidState
};

struct PlayerSetup {
  PlayerId id{};
  Role role{Role::Employee};
};

struct PlayerState {
  PlayerId id{};
  Role role{Role::Employee};
  Credits credits{kStartingCredits};
  Shares shares{kStartingShares};
  bool active{true};
  bool action_submitted{false};
  std::optional<TradeAction> order;
  bool vote_submitted{false};
  std::optional<PlayerId> vote_target;  // null means abstention
};

struct CompanyState {
  std::int32_t health{kInitialHealth};
  std::int32_t price{kInitialPrice};
};

struct SupplyPlan {
  Shares total{};
  Shares player_held{};
  Shares reserve{};
};

// Economy arithmetic is useful for larger-seat planning without approving a
// larger-match role policy or enabling >5-player matches in this milestone.
[[nodiscard]] std::optional<SupplyPlan> supply_plan(std::size_t seated_players) noexcept;

struct MatchState {
  std::vector<PlayerState> players;
  Shares reserve{};
  Shares total_supply{};
  CompanyState company{};
  Phase phase{Phase::Lobby};
  std::uint32_t round{0};
  std::int32_t event_impact{0};
  std::uint32_t eligible_voters_at_open{0};
  std::uint32_t seat_priority{0};
  bool clue_used{false};
  std::optional<PlayerId> clue_target;
  std::optional<bool> clue_result;
  Outcome outcome{Outcome::None};
};

struct OperationResult {
  ErrorCode error{ErrorCode::None};
  std::string detail;
  [[nodiscard]] explicit operator bool() const noexcept { return error == ErrorCode::None; }
};

enum class TradeDisposition { Executed, RejectedInsufficientReserve, NoOrder };
struct TradeReceipt {
  PlayerId player{};
  TradeDisposition disposition{TradeDisposition::NoOrder};
};

struct MarketResult {
  std::vector<TradeReceipt> receipts;
  std::int32_t opening_health{};
  std::int32_t opening_price{};
  std::int32_t event_impact{};
  std::uint32_t executed_buys{};
  std::uint32_t executed_sells{};
  std::int32_t closing_health{};
  std::int32_t closing_price{};
  Shares closing_reserve{};
  Outcome outcome{Outcome::None};
};

struct VoteResult {
  std::optional<PlayerId> eliminated;
  std::uint32_t eligible_voters{};
  std::uint32_t votes_received{};
  Outcome outcome{Outcome::None};
};

class Match {
 public:
  // Roles are injected explicitly: S1 has no approved role-shuffling policy.
  explicit Match(const std::vector<PlayerSetup>& roster);

  // Allows deterministic, invariant-checked fixtures such as reserve=1.
  [[nodiscard]] static Match from_state(MatchState state);

  [[nodiscard]] const MatchState& state() const noexcept { return state_; }
  [[nodiscard]] const std::vector<std::string>& event_log() const noexcept { return events_; }

  [[nodiscard]] OperationResult start();
  [[nodiscard]] OperationResult publish_event(std::int32_t impact);
  [[nodiscard]] OperationResult open_trading();
  [[nodiscard]] OperationResult submit_trade(PlayerId player, TradeAction action);
  [[nodiscard]] std::optional<MarketResult> resolve_market();
  [[nodiscard]] std::optional<bool> use_clue(PlayerId investigator, PlayerId target,
                                               OperationResult& result);
  [[nodiscard]] OperationResult open_voting();
  [[nodiscard]] OperationResult submit_vote(PlayerId voter, std::optional<PlayerId> target);
  [[nodiscard]] std::optional<VoteResult> resolve_votes();

  // The impact is private game information; callers must apply audience authorization.
  [[nodiscard]] std::optional<std::int32_t> insider_hint(PlayerId player,
                                                          OperationResult& result) const;
  // Internal canonical views include secrets (roles, clue, and event impact); never use as player output.
  [[nodiscard]] std::string canonical_state() const;
  [[nodiscard]] std::string canonical_events() const;

  [[nodiscard]] static std::string_view to_string(Role value) noexcept;
  [[nodiscard]] static std::string_view to_string(Phase value) noexcept;
  [[nodiscard]] static std::string_view to_string(Outcome value) noexcept;
  [[nodiscard]] static std::string_view to_string(ErrorCode value) noexcept;

 private:
  explicit Match(MatchState state, bool validate);
  [[nodiscard]] PlayerState* find_player(PlayerId id) noexcept;
  [[nodiscard]] const PlayerState* find_player(PlayerId id) const noexcept;
  [[nodiscard]] OperationResult fail(ErrorCode code, std::string detail);
  void record(std::string entry);
  [[nodiscard]] bool validate_state(std::string& why) const;
  void finish(Outcome outcome);

  MatchState state_;
  std::vector<std::string> events_;
};

}  // namespace darkpool

