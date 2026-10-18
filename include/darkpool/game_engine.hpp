#pragma once

#include <cstdint>
#include <cstddef>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace darkpool {

using PlayerId = std::uint32_t;
using Credits = std::uint32_t;
using Shares = std::uint32_t;

inline constexpr std::size_t kPlayerCount = 5;
inline constexpr std::size_t kMinPlayerCount = 3;
inline constexpr std::size_t kMaxPlayerCount = 64;
inline constexpr std::uint32_t kRoundCount = 3;
inline constexpr Credits kStartingCredits = 100;
inline constexpr Shares kStartingShares = 10;
inline constexpr Credits kCreditBalanceCap = 1'000'000;
inline constexpr Credits kResearchCost = 10;
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
  InvalidState,
  InvalidQuantity,
  ResearchAlreadyUsed
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
  Shares order_quantity{0};
  bool vote_submitted{false};
  std::optional<PlayerId> vote_target;  // null means abstention
  bool clue_used{false};
  std::optional<PlayerId> clue_target;
  std::optional<bool> clue_result;
  bool research_used{false};
  std::optional<std::string> research_direction;
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

struct RoleCounts {
  std::size_t insiders{};
  std::size_t investigators{};
  std::size_t employees{};
};

struct MarketModelCoefficients {
  // Candidate coefficients pending explicit user review (documented in docs/market-model-proposal.md)
  std::int32_t event_weight{2};        // Approved: business event impact multiplier
  std::int32_t demand_weight{1};       // Approved: net demand direction multiplier sign(B - S)
  std::int32_t volume_weight{0};       // Candidate: excess quantity impact: volume_weight * sign(D) * floor((|D| - 1) / 2)
  std::int32_t volatility_weight{0};   // Candidate: volatility magnification: volatility_weight * dir_market * floor(V / 4)
  std::int32_t health_weight{0};       // Candidate: fundamental valuation impact: (H' - 5) * health_weight
  std::int32_t sentiment_weight{0};    // Candidate: market sentiment adjustment: sentiment_weight * floor(sentiment / 2)
  std::int32_t liquidity_penalty{0};   // Candidate: reserve depletion scarcity surcharge (+w_l) when buying and reserve < 20%

  [[nodiscard]] static constexpr MarketModelCoefficients approved_baseline() noexcept {
    return MarketModelCoefficients{2, 1, 0, 0, 0, 0, 0};
  }

  [[nodiscard]] static constexpr MarketModelCoefficients candidate_proposal() noexcept {
    return MarketModelCoefficients{2, 1, 1, 1, 1, 1, 2};
  }
};

struct ResearchOutcome {
  std::string direction;
  bool already_used{false};
  Credits credits_remaining{};
};

// Five seats remain the default; variable-size games support 3..64 active players.
[[nodiscard]] std::optional<SupplyPlan> supply_plan(std::size_t seated_players) noexcept;
[[nodiscard]] std::optional<RoleCounts> role_counts(std::size_t seated_players) noexcept;

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
  std::int32_t sentiment{0};
  std::uint32_t volatility{0};
  std::uint32_t liquidity{50};
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
  Shares quantity{0};
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
  std::int32_t sentiment{0};
  std::uint32_t volatility{0};
  std::uint32_t liquidity{50};
  Outcome outcome{Outcome::None};
};

struct MarketResolution {
  OperationResult status;
  std::optional<MarketResult> value;
  [[nodiscard]] bool has_value() const noexcept { return static_cast<bool>(status) && value.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] MarketResult* operator->() noexcept { return value ? &*value : nullptr; }
  [[nodiscard]] const MarketResult* operator->() const noexcept { return value ? &*value : nullptr; }
};

struct VoteResult {
  std::optional<PlayerId> eliminated;
  std::uint32_t eligible_voters{};
  std::uint32_t votes_cast{};
  std::uint32_t votes_abstained{};
  std::uint32_t votes_missing{};
  std::uint32_t votes_received{};
  Outcome outcome{Outcome::None};
};

struct VoteResolution {
  OperationResult status;
  std::optional<VoteResult> value;
  [[nodiscard]] bool has_value() const noexcept { return static_cast<bool>(status) && value.has_value(); }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }
  [[nodiscard]] VoteResult* operator->() noexcept { return value ? &*value : nullptr; }
  [[nodiscard]] const VoteResult* operator->() const noexcept { return value ? &*value : nullptr; }
};

class Match {
 public:
  // Role assignment is deterministic and validated against role_counts(N).
  explicit Match(const std::vector<PlayerSetup>& roster,
                 MarketModelCoefficients coeffs = MarketModelCoefficients::approved_baseline());

  // Allows deterministic, invariant-checked fixtures such as reserve=1.
  [[nodiscard]] static Match from_state(MatchState state,
                                       MarketModelCoefficients coeffs = MarketModelCoefficients::approved_baseline());

  [[nodiscard]] const MatchState& state() const noexcept { return state_; }
  [[nodiscard]] const std::vector<std::string>& event_log() const noexcept { return events_; }

  void set_market_coefficients(const MarketModelCoefficients& coeffs) noexcept { coefficients_ = coeffs; }
  [[nodiscard]] const MarketModelCoefficients& market_coefficients() const noexcept { return coefficients_; }

  [[nodiscard]] OperationResult start();
  [[nodiscard]] OperationResult publish_event(std::int32_t impact);
  [[nodiscard]] OperationResult open_trading();
  [[nodiscard]] OperationResult submit_trade(PlayerId player, TradeAction action, Shares quantity = 1);
  [[nodiscard]] MarketResolution resolve_market();
  [[nodiscard]] std::optional<bool> use_clue(PlayerId investigator, PlayerId target,
                                               OperationResult& result);
  [[nodiscard]] std::optional<ResearchOutcome> conduct_research(PlayerId player,
                                                               OperationResult& result);
  [[nodiscard]] OperationResult open_voting();
  [[nodiscard]] OperationResult submit_vote(PlayerId voter, std::optional<PlayerId> target);
  [[nodiscard]] VoteResolution resolve_votes();

  // Safe mathematical integer floor division helper: floor(a / b)
  // Valid domain: b != 0. Avoids undefined behavior on INT32_MIN / -1 by clamping or using 64-bit arithmetic.
  [[nodiscard]] static constexpr std::int32_t floor_div(std::int32_t a, std::int32_t b) {
    if (b == 0) {
      throw std::invalid_argument("floor_div: division by zero");
    }
    const std::int64_t a64 = a;
    const std::int64_t b64 = b;
    std::int64_t q = a64 / b64;
    const std::int64_t r = a64 % b64;
    if (r != 0 && ((a64 < 0) ^ (b64 < 0))) {
      --q;
    }
    if (q > std::numeric_limits<std::int32_t>::max()) {
      return std::numeric_limits<std::int32_t>::max();
    }
    if (q < std::numeric_limits<std::int32_t>::min()) {
      return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(q);
  }

  // Price calculation under specific market model coefficients
  [[nodiscard]] static std::int32_t calculate_price(std::int32_t opening_price,
                                                    std::int32_t closing_health,
                                                    std::int32_t event_impact,
                                                    std::int32_t demand,
                                                    std::int32_t sentiment,
                                                    std::uint32_t liquidity,
                                                    const MarketModelCoefficients& coeffs = MarketModelCoefficients::approved_baseline()) noexcept;

  [[nodiscard]] static std::int32_t calculate_price(std::int32_t opening_price,
                                                    std::int32_t closing_health,
                                                    std::int32_t event_impact,
                                                    std::int32_t demand,
                                                    std::int32_t sentiment,
                                                    std::uint32_t liquidity,
                                                    std::uint32_t volatility,
                                                    const MarketModelCoefficients& coeffs = MarketModelCoefficients::approved_baseline()) noexcept;

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
  explicit Match(MatchState state, bool validate,
                 MarketModelCoefficients coeffs = MarketModelCoefficients::approved_baseline());
  [[nodiscard]] PlayerState* find_player(PlayerId id) noexcept;
  [[nodiscard]] const PlayerState* find_player(PlayerId id) const noexcept;
  [[nodiscard]] OperationResult fail(ErrorCode code, std::string detail);
  void record(std::string entry);
  [[nodiscard]] bool validate_state(std::string& why) const;
  void finish(Outcome outcome);

  MatchState state_;
  std::vector<std::string> events_;
  MarketModelCoefficients coefficients_{MarketModelCoefficients::approved_baseline()};
};

}  // namespace darkpool

