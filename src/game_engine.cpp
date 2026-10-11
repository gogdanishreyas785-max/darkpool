#include "darkpool/game_engine.hpp"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace darkpool {
namespace {

std::int32_t sign(std::int32_t value) noexcept {
  return (value > 0) - (value < 0);
}

std::int32_t clamp_value(std::int32_t value, std::int32_t low, std::int32_t high) noexcept {
  return std::clamp(value, low, high);
}

}  // namespace

std::optional<SupplyPlan> supply_plan(std::size_t seated_players) noexcept {
  if (seated_players < kPlayerCount || seated_players > 64U) return std::nullopt;
  const auto players = static_cast<Shares>(seated_players);
  const auto held = players * kStartingShares;
  return SupplyPlan{held * 2U, held, held};
}

Match::Match(const std::vector<PlayerSetup>& roster) {
  if (roster.size() != kPlayerCount) throw std::invalid_argument("roster must contain exactly five players");
  std::unordered_set<PlayerId> ids;
  std::uint32_t insiders = 0;
  std::uint32_t investigators = 0;
  std::uint32_t employees = 0;
  for (const auto& seat : roster) {
    if (!ids.insert(seat.id).second) throw std::invalid_argument("player IDs must be unique");
    switch (seat.role) {
      case Role::Insider: ++insiders; break;
      case Role::Investigator: ++investigators; break;
      case Role::Employee: ++employees; break;
    }
    state_.players.push_back(PlayerState{seat.id, seat.role});
  }
  if (insiders != 1 || investigators != 1 || employees != 3) {
    throw std::invalid_argument("roster requires one Insider, one Investigator, and three Employees");
  }
  const auto supply = supply_plan(kPlayerCount).value();
  state_.total_supply = supply.total;
  state_.reserve = supply.reserve;
  events_.push_back("init|players=5|supply=100|reserve=50|health=5|price=10");
}

Match::Match(MatchState state, bool validate) : state_(std::move(state)) {
  if (validate) {
    std::string why;
    if (!validate_state(why)) throw std::invalid_argument("invalid match state: " + why);
  }
  events_.push_back("fixture|validated=true");
}

Match Match::from_state(MatchState state) { return Match(std::move(state), true); }

bool Match::validate_state(std::string& why) const {
  if (state_.players.size() != kPlayerCount) { why = "five players required"; return false; }
  const auto expected_supply = supply_plan(kPlayerCount).value().total;
  if (state_.total_supply != expected_supply) { why = "total supply does not match five-player economy"; return false; }
  std::unordered_set<PlayerId> ids;
  std::uint32_t insiders = 0, investigators = 0, employees = 0;
  std::uint64_t held = 0;
  for (const auto& p : state_.players) {
    if (!ids.insert(p.id).second) { why = "duplicate player ID"; return false; }
    if (p.credits > kCreditBalanceCap) { why = "credit balance exceeds cap"; return false; }
    held += p.shares;
    if ((p.order.has_value() && !p.action_submitted) ||
        (p.vote_target.has_value() && !p.vote_submitted)) {
      why = "submission state is inconsistent"; return false;
    }
    switch (p.role) {
      case Role::Insider: ++insiders; break;
      case Role::Investigator: ++investigators; break;
      case Role::Employee: ++employees; break;
    }
  }
  if (insiders != 1 || investigators != 1 || employees != 3) { why = "role counts invalid"; return false; }
  if (held + state_.reserve != state_.total_supply) { why = "share supply is not conserved"; return false; }
  if (state_.company.health < kMinHealth || state_.company.health > kMaxHealth ||
      state_.company.price < kMinPrice || state_.company.price > kMaxPrice) {
    why = "company value outside inclusive bounds"; return false;
  }
  if (state_.round > kRoundCount || state_.event_impact < -2 || state_.event_impact > 2 ||
      state_.seat_priority >= state_.players.size() ||
      state_.eligible_voters_at_open > state_.players.size() ||
      (state_.phase == Phase::Lobby && state_.round != 0U) ||
      (state_.phase != Phase::Lobby && state_.round == 0U)) {
    why = "round/event value outside bounds"; return false;
  }
  return true;
}

PlayerState* Match::find_player(PlayerId id) noexcept {
  const auto it = std::find_if(state_.players.begin(), state_.players.end(),
                               [id](const PlayerState& p) { return p.id == id; });
  return it == state_.players.end() ? nullptr : &*it;
}

const PlayerState* Match::find_player(PlayerId id) const noexcept {
  const auto it = std::find_if(state_.players.begin(), state_.players.end(),
                               [id](const PlayerState& p) { return p.id == id; });
  return it == state_.players.end() ? nullptr : &*it;
}

void Match::record(std::string entry) { events_.push_back(std::move(entry)); }

OperationResult Match::fail(ErrorCode code, std::string detail) {
  record("reject|" + std::string(to_string(code)) + "|" + detail);
  return {code, std::move(detail)};
}

void Match::finish(Outcome outcome) {
  state_.outcome = outcome;
  state_.phase = Phase::Finished;
  record("finish|" + std::string(to_string(outcome)));
}

OperationResult Match::start() {
  if (state_.phase != Phase::Lobby || state_.round != 0) return fail(ErrorCode::WrongPhase, "start requires lobby");
  state_.round = 1;
  state_.phase = Phase::AwaitingEvent;
  record("start|round=1");
  return {};
}

OperationResult Match::publish_event(std::int32_t impact) {
  if (state_.phase != Phase::AwaitingEvent) return fail(ErrorCode::WrongPhase, "event requires awaiting-event phase");
  if (impact < -2 || impact > 2) return fail(ErrorCode::InvalidEventImpact, "impact must be in [-2,2]");
  state_.event_impact = impact;
  state_.phase = Phase::Discussion;
  record("event|round=" + std::to_string(state_.round) + "|impact=" + std::to_string(impact));
  return {};
}

OperationResult Match::open_trading() {
  if (state_.phase != Phase::Discussion) return fail(ErrorCode::WrongPhase, "trading requires discussion phase");
  state_.phase = Phase::Trading;
  state_.seat_priority = (state_.round - 1U) % static_cast<std::uint32_t>(state_.players.size());
  for (auto& player : state_.players) {
    player.action_submitted = false;
    player.order.reset();
  }
  record("trading_open|round=" + std::to_string(state_.round) + "|priority=" + std::to_string(state_.seat_priority));
  return {};
}

OperationResult Match::submit_trade(PlayerId id, TradeAction action) {
  if (state_.phase != Phase::Trading) return fail(ErrorCode::WrongPhase, "trade requires trading phase");
  auto* player = find_player(id);
  if (player == nullptr) return fail(ErrorCode::UnknownPlayer, "unknown player");
  if (!player->active) return fail(ErrorCode::InactivePlayer, "inactive player");
  if (player->action_submitted) return fail(ErrorCode::ActionAlreadySubmitted, "one action per player per round");

  // The approved action-slot rule consumes the slot even when validation rejects the order.
  player->action_submitted = true;
  const auto reject_order = [this](ErrorCode code, std::string detail) {
    record("trade_rejected|" + std::string(to_string(code)) + "|" + detail);
    return OperationResult{code, std::move(detail)};
  };
  if (action != TradeAction::Buy && action != TradeAction::Hold && action != TradeAction::Sell) {
    return reject_order(ErrorCode::InvalidAction, "unknown trade action");
  }
  if (action == TradeAction::Buy && player->credits < static_cast<Credits>(state_.company.price)) {
    return reject_order(ErrorCode::InsufficientCredits, "BUY requires opening price credits");
  }
  if (action == TradeAction::Sell && player->shares == 0) {
    return reject_order(ErrorCode::NoSharesToSell, "SELL requires an owned share");
  }
  if (action == TradeAction::Sell &&
      player->credits > kCreditBalanceCap - static_cast<Credits>(state_.company.price)) {
    return reject_order(ErrorCode::CreditCapExceeded, "SELL would exceed per-player credit cap");
  }
  player->order = action;
  record("trade_submitted|player=" + std::to_string(id) + "|action=" + std::string(action == TradeAction::Buy ? "BUY" : action == TradeAction::Sell ? "SELL" : "HOLD"));
  return {};
}

MarketResolution Match::resolve_market() {
  if (state_.phase != Phase::Trading) return {fail(ErrorCode::WrongPhase, "market resolution requires trading phase"), std::nullopt};
  MarketResult result;
  result.opening_health = state_.company.health;
  result.opening_price = state_.company.price;
  result.event_impact = state_.event_impact;
  result.closing_reserve = state_.reserve;
  result.receipts.reserve(state_.players.size());
  const auto count = static_cast<std::uint32_t>(state_.players.size());
  std::vector<bool> buy_accepted(state_.players.size(), false);

  // Allocate BUYs from opening reserve in deterministic rotating seat order.
  Shares inventory = state_.reserve;
  for (std::uint32_t offset = 0; offset < count; ++offset) {
    const auto seat = (state_.seat_priority + offset) % count;
    const auto& p = state_.players[seat];
    if (p.active && p.order == TradeAction::Buy && inventory > 0) {
      buy_accepted[seat] = true;
      --inventory;
    }
  }

  for (std::size_t i = 0; i < state_.players.size(); ++i) {
    auto& p = state_.players[i];
    TradeDisposition disposition = TradeDisposition::NoOrder;
    if (p.active && p.order == TradeAction::Buy) {
      if (!buy_accepted[i]) {
        disposition = TradeDisposition::RejectedInsufficientReserve;
      } else {
        p.credits -= static_cast<Credits>(state_.company.price);
        ++p.shares;
        --state_.reserve;
        ++result.executed_buys;
        disposition = TradeDisposition::Executed;
      }
    } else if (p.active && p.order == TradeAction::Sell) {
      p.credits += static_cast<Credits>(state_.company.price);
      --p.shares;
      ++state_.reserve;
      ++result.executed_sells;
      disposition = TradeDisposition::Executed;
    }
    result.receipts.push_back({p.id, disposition});
    const auto disposition_text = disposition == TradeDisposition::Executed
        ? "executed"
        : disposition == TradeDisposition::RejectedInsufficientReserve ? "rejected_insufficient_reserve" : "no_order";
    record("market_order|player=" + std::to_string(p.id) + "|result=" + disposition_text);
  }

  const auto demand = static_cast<std::int32_t>(result.executed_buys) - static_cast<std::int32_t>(result.executed_sells);
  const auto demand_sign = sign(demand);
  state_.company.health = clamp_value(result.opening_health + result.event_impact + demand_sign, kMinHealth, kMaxHealth);
  state_.company.price = clamp_value(result.opening_price + 2 * result.event_impact + demand_sign, kMinPrice, kMaxPrice);
  result.closing_health = state_.company.health;
  result.closing_price = state_.company.price;
  result.closing_reserve = state_.reserve;
  state_.phase = Phase::Clue;
  record("market|round=" + std::to_string(state_.round) + "|B=" + std::to_string(result.executed_buys) + "|S=" + std::to_string(result.executed_sells) + "|H=" + std::to_string(state_.company.health) + "|P=" + std::to_string(state_.company.price) + "|reserve=" + std::to_string(state_.reserve));
  if (state_.company.health == 0) finish(Outcome::InsiderWin);
  result.outcome = state_.outcome;
  return {{}, result};
}

std::optional<bool> Match::use_clue(PlayerId investigator, PlayerId target, OperationResult& result) {
  if (state_.phase != Phase::Clue) { result = fail(ErrorCode::WrongPhase, "clue requires clue phase"); return std::nullopt; }
  const auto* requester = find_player(investigator);
  const auto* target_player = find_player(target);
  if (requester == nullptr || target_player == nullptr) { result = fail(ErrorCode::UnknownPlayer, "unknown player"); return std::nullopt; }
  if (!requester->active) { result = fail(ErrorCode::InactivePlayer, "inactive investigator"); return std::nullopt; }
  if (requester->role != Role::Investigator) { result = fail(ErrorCode::NotInvestigator, "only Investigator may request clue"); return std::nullopt; }
  if (investigator == target || !target_player->active) { result = fail(ErrorCode::InvalidClueTarget, "target must be another active player"); return std::nullopt; }
  if (state_.clue_used) {
    if (state_.clue_target == target) { result = {}; return state_.clue_result; }
    result = fail(ErrorCode::ClueAlreadyUsed, "clue already used on another target");
    return std::nullopt;
  }
  const auto answer = target_player->role == Role::Insider;
  state_.clue_used = true;
  state_.clue_target = target;
  state_.clue_result = answer;
  record("clue_used|requester=" + std::to_string(investigator) + "|target=" + std::to_string(target));
  result = {};
  return answer;
}

OperationResult Match::open_voting() {
  if (state_.phase != Phase::Clue) return fail(ErrorCode::WrongPhase, "voting requires clue phase");
  state_.phase = Phase::Voting;
  state_.eligible_voters_at_open = 0;
  for (auto& player : state_.players) {
    player.vote_submitted = false;
    player.vote_target.reset();
    if (player.active) ++state_.eligible_voters_at_open;
  }
  record("voting_open|eligible=" + std::to_string(state_.eligible_voters_at_open));
  return {};
}

OperationResult Match::submit_vote(PlayerId voter_id, std::optional<PlayerId> target_id) {
  if (state_.phase != Phase::Voting) return fail(ErrorCode::WrongPhase, "vote requires voting phase");
  auto* voter = find_player(voter_id);
  if (voter == nullptr) return fail(ErrorCode::UnknownPlayer, "unknown voter");
  if (!voter->active) return fail(ErrorCode::InactivePlayer, "inactive voter");
  if (voter->vote_submitted) return fail(ErrorCode::VoteAlreadySubmitted, "one vote per eligible player");
  if (target_id.has_value()) {
    const auto* target = find_player(*target_id);
    if (target == nullptr || *target_id == voter_id || !target->active) {
      return fail(ErrorCode::InvalidVoteTarget, "target must be another active player");
    }
  }
  voter->vote_submitted = true;
  voter->vote_target = target_id;
  record("vote_submitted|voter=" + std::to_string(voter_id) + "|target=" + (target_id ? std::to_string(*target_id) : "abstain"));
  return {};
}

VoteResolution Match::resolve_votes() {
  if (state_.phase != Phase::Voting) return {fail(ErrorCode::WrongPhase, "vote resolution requires voting phase"), std::nullopt};
  VoteResult result;
  result.eligible_voters = state_.eligible_voters_at_open;
  std::vector<std::uint32_t> counts(state_.players.size(), 0);
  for (const auto& voter : state_.players) {
    if (!voter.active || !voter.vote_submitted || !voter.vote_target) continue;  // missing is abstain
    const auto target = std::find_if(state_.players.begin(), state_.players.end(),
                                     [&voter](const PlayerState& p) { return p.id == *voter.vote_target; });
    if (target != state_.players.end()) ++counts[static_cast<std::size_t>(std::distance(state_.players.begin(), target))];
  }
  const auto threshold = result.eligible_voters / 2U;
  bool insider_eliminated = false;
  for (std::size_t i = 0; i < counts.size(); ++i) {
    if (state_.players[i].active && counts[i] > threshold) {
      result.eliminated = state_.players[i].id;
      result.votes_received = counts[i];
      state_.players[i].active = false;
      insider_eliminated = state_.players[i].role == Role::Insider && state_.company.health > 0;
      break;
    }
  }
  record("vote_resolved|eligible=" + std::to_string(result.eligible_voters) + "|eliminated=" + (result.eliminated ? std::to_string(*result.eliminated) : "none") + "|votes=" + std::to_string(result.votes_received));
  if (insider_eliminated) finish(Outcome::EmployeesWin);
  else if (state_.round == kRoundCount) finish(Outcome::InsiderWin);
  else {
    ++state_.round;
    state_.phase = Phase::AwaitingEvent;
  }
  result.outcome = state_.outcome;
  return {{}, result};
}

std::optional<std::int32_t> Match::insider_hint(PlayerId id, OperationResult& result) const {
  if (state_.phase == Phase::Lobby || state_.phase == Phase::AwaitingEvent || state_.phase == Phase::Finished) {
    result = {ErrorCode::WrongPhase, "hint is available only during an active round"}; return std::nullopt;
  }
  const auto* player = find_player(id);
  if (player == nullptr) { result = {ErrorCode::UnknownPlayer, "unknown player"}; return std::nullopt; }
  if (!player->active) { result = {ErrorCode::InactivePlayer, "inactive player"}; return std::nullopt; }
  if (player->role != Role::Insider) { result = {ErrorCode::NotInvestigator, "event impact is private to Insider"}; return std::nullopt; }
  result = {};
  return state_.event_impact;
}

std::string Match::canonical_state() const {
  std::ostringstream out;
  out << "round=" << state_.round << ";phase=" << to_string(state_.phase)
      << ";health=" << state_.company.health << ";price=" << state_.company.price
      << ";reserve=" << state_.reserve << ";supply=" << state_.total_supply
      << ";event=" << state_.event_impact << ";clue_used=" << (state_.clue_used ? 1 : 0)
      << ";voters_at_open=" << state_.eligible_voters_at_open
      << ";seat_priority=" << state_.seat_priority
      << ";clue_target=" << (state_.clue_target ? std::to_string(*state_.clue_target) : "-")
      << ";clue_result=" << (state_.clue_result ? (*state_.clue_result ? "1" : "0") : "-")
      << ";outcome=" << to_string(state_.outcome);
  for (std::size_t i = 0; i < state_.players.size(); ++i) {
    const auto& p = state_.players[i];
    out << "\nseat" << i << '=' << p.id << ',' << to_string(p.role) << ',' << p.credits << ',' << p.shares
        << ',' << (p.active ? 1 : 0) << ',' << (p.action_submitted ? 1 : 0) << ','
        << (p.order ? (*p.order == TradeAction::Buy ? "BUY" : *p.order == TradeAction::Sell ? "SELL" : "HOLD") : "-")
        << ',' << (p.vote_submitted ? 1 : 0) << ',' << (p.vote_target ? std::to_string(*p.vote_target) : "-");
  }
  return out.str();
}

std::string Match::canonical_events() const {
  std::ostringstream out;
  for (std::size_t i = 0; i < events_.size(); ++i) {
    if (i != 0) out << '\n';
    out << events_[i];
  }
  return out.str();
}

std::string_view Match::to_string(Role value) noexcept {
  switch (value) { case Role::Insider: return "Insider"; case Role::Investigator: return "Investigator"; case Role::Employee: return "Employee"; }
  return "UnknownRole";
}
std::string_view Match::to_string(Phase value) noexcept {
  switch (value) { case Phase::Lobby: return "Lobby"; case Phase::AwaitingEvent: return "AwaitingEvent"; case Phase::Discussion: return "Discussion"; case Phase::Trading: return "Trading"; case Phase::Clue: return "Clue"; case Phase::Voting: return "Voting"; case Phase::Finished: return "Finished"; }
  return "UnknownPhase";
}
std::string_view Match::to_string(Outcome value) noexcept {
  switch (value) { case Outcome::None: return "None"; case Outcome::InsiderWin: return "InsiderWin"; case Outcome::EmployeesWin: return "EmployeesWin"; }
  return "UnknownOutcome";
}
std::string_view Match::to_string(ErrorCode value) noexcept {
  switch (value) {
    case ErrorCode::None: return "None"; case ErrorCode::WrongPhase: return "WrongPhase";
    case ErrorCode::UnknownPlayer: return "UnknownPlayer"; case ErrorCode::InactivePlayer: return "InactivePlayer";
    case ErrorCode::InvalidRoster: return "InvalidRoster"; case ErrorCode::InvalidEventImpact: return "InvalidEventImpact";
    case ErrorCode::InvalidAction: return "InvalidAction";
    case ErrorCode::ActionAlreadySubmitted: return "ActionAlreadySubmitted"; case ErrorCode::InsufficientCredits: return "InsufficientCredits";
    case ErrorCode::NoSharesToSell: return "NoSharesToSell"; case ErrorCode::CreditCapExceeded: return "CreditCapExceeded";
    case ErrorCode::NotInvestigator: return "NotInvestigator"; case ErrorCode::InvalidClueTarget: return "InvalidClueTarget";
    case ErrorCode::ClueAlreadyUsed: return "ClueAlreadyUsed"; case ErrorCode::InvalidVoteTarget: return "InvalidVoteTarget";
    case ErrorCode::VoteAlreadySubmitted: return "VoteAlreadySubmitted"; case ErrorCode::InvalidState: return "InvalidState";
  }
  return "UnknownError";
}

}  // namespace darkpool

