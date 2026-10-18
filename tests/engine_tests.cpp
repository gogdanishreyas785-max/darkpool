#include "darkpool/game_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace darkpool;

namespace {

int failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}

#define CHECK(x) check(static_cast<bool>(x), #x, __LINE__)

std::vector<PlayerSetup> roster() {
  return {{10, Role::Insider}, {20, Role::Investigator}, {30, Role::Employee},
          {40, Role::Employee}, {50, Role::Employee}};
}

std::vector<PlayerSetup> scaled_roster(std::size_t count) {
  const auto counts = role_counts(count);
  if (!counts) return {};
  std::vector<PlayerSetup> players;
  players.reserve(count);
  for (std::size_t seat = 0; seat < count; ++seat) {
    const auto role = seat < counts->insiders ? Role::Insider
        : seat < counts->insiders + counts->investigators ? Role::Investigator : Role::Employee;
    players.push_back({static_cast<PlayerId>(seat + 1U), role});
  }
  return players;
}

PlayerState initial_player(PlayerId id, Role role) {
  PlayerState player{};
  player.id = id;
  player.role = role;
  return player;
}

MatchState base_state() {
  MatchState state;
  state.players = {initial_player(10, Role::Insider), initial_player(20, Role::Investigator),
                   initial_player(30, Role::Employee), initial_player(40, Role::Employee),
                   initial_player(50, Role::Employee)};
  state.reserve = 50;
  state.total_supply = 100;
  state.company = {5, 10};
  state.phase = Phase::Trading;
  state.round = 1;
  return state;
}

Match market_fixture(std::int32_t health, std::int32_t price, std::int32_t event,
                     std::vector<std::pair<std::size_t, TradeAction>> actions,
                     Shares reserve = 50) {
  auto state = base_state();
  state.company = {health, price};
  state.event_impact = event;
  state.reserve = reserve;
  std::uint64_t held = 0;
  for (const auto& [seat, action] : actions) {
    state.players.at(seat).action_submitted = true;
    state.players.at(seat).order = action;
    if (action == TradeAction::Sell) ++state.players.at(seat).shares;
  }
  for (const auto& player : state.players) held += player.shares;
  if (held + state.reserve != state.total_supply) {
    const auto delta = static_cast<std::int64_t>(state.total_supply) - static_cast<std::int64_t>(held + state.reserve);
    if (delta >= 0) state.reserve += static_cast<Shares>(delta);
    else state.reserve -= static_cast<Shares>(-delta);
  }
  return Match::from_state(std::move(state));
}

void start_to_trading(Match& match, std::int32_t event) {
  CHECK(match.start());
  CHECK(match.publish_event(event));
  CHECK(match.open_trading());
}

void round_to_voting(Match& match, std::int32_t event) {
  CHECK(match.publish_event(event));
  CHECK(match.open_trading());
  const auto market = match.resolve_market();
  CHECK(market.has_value());
  if (match.state().phase == Phase::Clue) CHECK(match.open_voting());
}

void finish_round_without_elimination(Match& match) {
  for (const auto& p : match.state().players) {
    if (p.active) CHECK(match.submit_vote(p.id, std::nullopt));
  }
  CHECK(match.resolve_votes().has_value());
}

void test_state_fixture_invariants() {
  const auto rejected = [](MatchState state) {
    try { (void)Match::from_state(std::move(state)); }
    catch (const std::invalid_argument&) { return true; }
    return false;
  };

  auto finished_without_outcome = base_state();
  finished_without_outcome.phase = Phase::Finished;
  CHECK(rejected(std::move(finished_without_outcome)));

  auto live_with_outcome = base_state();
  live_with_outcome.outcome = Outcome::InsiderWin;
  CHECK(rejected(std::move(live_with_outcome)));

  auto zero_health_live = base_state();
  zero_health_live.company.health = 0;
  CHECK(rejected(std::move(zero_health_live)));

  auto zero_health_wrong_winner = base_state();
  zero_health_wrong_winner.phase = Phase::Finished;
  zero_health_wrong_winner.company.health = 0;
  zero_health_wrong_winner.players[0].active = false;
  zero_health_wrong_winner.outcome = Outcome::EmployeesWin;
  CHECK(rejected(std::move(zero_health_wrong_winner)));

  auto premature_insider_win = base_state();
  premature_insider_win.phase = Phase::Finished;
  premature_insider_win.outcome = Outcome::InsiderWin;
  premature_insider_win.round = 2;
  CHECK(rejected(std::move(premature_insider_win)));

  auto employees_win_with_insider_alive = base_state();
  employees_win_with_insider_alive.phase = Phase::Finished;
  employees_win_with_insider_alive.outcome = Outcome::EmployeesWin;
  CHECK(rejected(std::move(employees_win_with_insider_alive)));

  auto trade_in_lobby = base_state();
  trade_in_lobby.phase = Phase::Lobby;
  trade_in_lobby.round = 0;
  trade_in_lobby.players[0].action_submitted = true;
  trade_in_lobby.players[0].order = TradeAction::Hold;
  CHECK(rejected(std::move(trade_in_lobby)));

  auto trade_before_trading = base_state();
  trade_before_trading.phase = Phase::Discussion;
  trade_before_trading.players[0].action_submitted = true;
  trade_before_trading.players[0].order = TradeAction::Hold;
  CHECK(rejected(std::move(trade_before_trading)));

  auto vote_before_voting = base_state();
  vote_before_voting.phase = Phase::Discussion;
  vote_before_voting.players[0].vote_submitted = true;
  vote_before_voting.players[0].vote_target = 20;
  CHECK(rejected(std::move(vote_before_voting)));

  auto clue_before_clue_phase = base_state();
  clue_before_clue_phase.clue_used = true;
  clue_before_clue_phase.clue_target = 30;
  clue_before_clue_phase.clue_result = false;
  CHECK(rejected(std::move(clue_before_clue_phase)));

  auto inactive_first_round_clue_target = base_state();
  inactive_first_round_clue_target.phase = Phase::Clue;
  inactive_first_round_clue_target.clue_used = true;
  inactive_first_round_clue_target.clue_target = 50;
  inactive_first_round_clue_target.clue_result = false;
  inactive_first_round_clue_target.players[1].clue_used = true;
  inactive_first_round_clue_target.players[1].clue_target = 50;
  inactive_first_round_clue_target.players[1].clue_result = false;
  inactive_first_round_clue_target.players[4].active = false;
  CHECK(rejected(std::move(inactive_first_round_clue_target)));

  auto fabricated_finished_activity = base_state();
  fabricated_finished_activity.phase = Phase::Finished;
  fabricated_finished_activity.players[0].active = false;
  fabricated_finished_activity.players[2].active = false;
  fabricated_finished_activity.outcome = Outcome::EmployeesWin;
  CHECK(rejected(std::move(fabricated_finished_activity)));

  auto inactive_vote_target_without_majority = base_state();
  inactive_vote_target_without_majority.phase = Phase::AwaitingEvent;
  inactive_vote_target_without_majority.round = 2;
  inactive_vote_target_without_majority.eligible_voters_at_open = 5;
  inactive_vote_target_without_majority.players[4].active = false;
  inactive_vote_target_without_majority.players[0].vote_submitted = true;
  inactive_vote_target_without_majority.players[0].vote_target = 50;
  CHECK(rejected(std::move(inactive_vote_target_without_majority)));

  auto valid_resolved_vote_to_eliminated_player = base_state();
  valid_resolved_vote_to_eliminated_player.phase = Phase::AwaitingEvent;
  valid_resolved_vote_to_eliminated_player.round = 2;
  valid_resolved_vote_to_eliminated_player.eligible_voters_at_open = 5;
  valid_resolved_vote_to_eliminated_player.players[4].active = false;
  for (std::size_t i = 0; i < 3; ++i) {
    valid_resolved_vote_to_eliminated_player.players[i].vote_submitted = true;
    valid_resolved_vote_to_eliminated_player.players[i].vote_target = 50;
  }
  (void)Match::from_state(std::move(valid_resolved_vote_to_eliminated_player));

  auto vote_count_mismatch = base_state();
  vote_count_mismatch.phase = Phase::Voting;
  vote_count_mismatch.eligible_voters_at_open = 4;
  CHECK(rejected(std::move(vote_count_mismatch)));

  auto inactive_voter = base_state();
  inactive_voter.phase = Phase::Voting;
  inactive_voter.players[4].active = false;
  inactive_voter.eligible_voters_at_open = 4;
  inactive_voter.players[4].vote_submitted = true;
  CHECK(rejected(std::move(inactive_voter)));

  auto clue_without_result = base_state();
  clue_without_result.clue_used = true;
  clue_without_result.clue_target = 30;
  CHECK(rejected(std::move(clue_without_result)));

  auto incorrect_clue_result = base_state();
  incorrect_clue_result.clue_used = true;
  incorrect_clue_result.clue_target = 30;
  incorrect_clue_result.clue_result = true; // target 30 is an Employee
  CHECK(rejected(std::move(incorrect_clue_result)));

  // A once-valid clue target can later be eliminated; preserve that valid historical state.
  auto used_clue_target_later_eliminated = base_state();
  used_clue_target_later_eliminated.phase = Phase::Voting;
  used_clue_target_later_eliminated.round = 2;
  used_clue_target_later_eliminated.players[4].active = false;
  used_clue_target_later_eliminated.eligible_voters_at_open = 4;
  used_clue_target_later_eliminated.clue_used = true;
  used_clue_target_later_eliminated.clue_target = 50;
  used_clue_target_later_eliminated.clue_result = false;
  (void)Match::from_state(std::move(used_clue_target_later_eliminated));

  auto valid_health_win = base_state();
  valid_health_win.phase = Phase::Finished;
  valid_health_win.company.health = 0;
  valid_health_win.outcome = Outcome::InsiderWin;
  (void)Match::from_state(std::move(valid_health_win));

  auto invalid_final_win_without_vote = base_state();
  invalid_final_win_without_vote.phase = Phase::Finished;
  invalid_final_win_without_vote.round = kRoundCount;
  invalid_final_win_without_vote.outcome = Outcome::InsiderWin;
  CHECK(rejected(std::move(invalid_final_win_without_vote)));

  auto valid_final_survival = base_state();
  valid_final_survival.phase = Phase::Finished;
  valid_final_survival.round = kRoundCount;
  valid_final_survival.eligible_voters_at_open = 5;
  valid_final_survival.outcome = Outcome::InsiderWin;
  for (auto& player : valid_final_survival.players) player.vote_submitted = true;
  (void)Match::from_state(std::move(valid_final_survival));

  auto invalid_insider_elimination_without_vote = base_state();
  invalid_insider_elimination_without_vote.phase = Phase::Finished;
  invalid_insider_elimination_without_vote.players[0].active = false;
  invalid_insider_elimination_without_vote.outcome = Outcome::EmployeesWin;
  CHECK(rejected(std::move(invalid_insider_elimination_without_vote)));

  auto valid_insider_elimination = base_state();
  valid_insider_elimination.phase = Phase::Finished;
  valid_insider_elimination.players[0].active = false;
  valid_insider_elimination.eligible_voters_at_open = 5;
  valid_insider_elimination.outcome = Outcome::EmployeesWin;
  for (std::size_t i = 1; i <= 3; ++i) {
    valid_insider_elimination.players[i].vote_submitted = true;
    valid_insider_elimination.players[i].vote_target = 10;
  }
  (void)Match::from_state(std::move(valid_insider_elimination));
}

void test_initialization_and_supply() {
  Match match(roster());
  CHECK(match.state().players.size() == 5U);
  CHECK(match.state().reserve == 50U);
  CHECK(match.state().total_supply == 100U);
  CHECK(match.state().company.health == 5);
  CHECK(match.state().company.price == 10);
  CHECK(match.state().phase == Phase::Lobby);
  std::uint32_t insiders = 0, investigators = 0, employees = 0;
  for (const auto& player : match.state().players) {
    CHECK(player.credits == 100U && player.shares == 10U && player.active);
    if (player.role == Role::Insider) ++insiders;
    else if (player.role == Role::Investigator) ++investigators;
    else ++employees;
  }
  CHECK(insiders == 1U && investigators == 1U && employees == 3U);
  CHECK(match.publish_event(0).error == ErrorCode::WrongPhase);
  CHECK(match.start());
  CHECK(match.publish_event(3).error == ErrorCode::InvalidEventImpact);
  CHECK(match.publish_event(-2));
  CHECK(match.state().phase == Phase::Discussion);
  const auto wrong_market_phase = match.resolve_market();
  CHECK(wrong_market_phase.status.error == ErrorCode::WrongPhase && !wrong_market_phase.has_value());
  const auto five = supply_plan(5);
  const auto sixty_four = supply_plan(64);
  CHECK(five && five->total == 100U && five->player_held == 50U && five->reserve == 50U);
  CHECK(sixty_four && sixty_four->total == 1280U && sixty_four->player_held == 640U && sixty_four->reserve == 640U);
  CHECK(!supply_plan(2));
  CHECK(!supply_plan(65));

  bool rejected = false;
  try { Match invalid({{1, Role::Insider}, {2, Role::Investigator}, {3, Role::Employee}, {4, Role::Employee}, {5, Role::Insider}}); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);

  auto duplicate_ids = roster();
  duplicate_ids[4].id = duplicate_ids[3].id;
  rejected = false;
  try { Match invalid(duplicate_ids); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);

  auto zero_id = roster();
  zero_id[0].id = 0;
  rejected = false;
  try { Match invalid(zero_id); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);

  auto invalid_role = roster();
  invalid_role[4].role = static_cast<Role>(99);
  rejected = false;
  try { Match invalid(invalid_role); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
}

void test_variable_rosters_and_supply() {
  struct Expected { std::size_t n; std::size_t insiders; std::size_t investigators; std::size_t employees; };
  constexpr Expected expected[]{{3,1,1,1},{4,1,1,2},{5,1,1,3},{8,1,1,6},
      {9,2,1,6},{16,2,1,13},{24,3,2,19},{32,4,2,26},{64,8,4,52}};
  for (const auto& item : expected) {
    const auto counts = role_counts(item.n);
    const auto supply = supply_plan(item.n);
    CHECK(counts && counts->insiders == item.insiders &&
          counts->investigators == item.investigators && counts->employees == item.employees);
    CHECK(counts && counts->employees >= 1U);
    CHECK(supply && supply->total == 20U * item.n && supply->player_held == 10U * item.n &&
          supply->reserve == 10U * item.n);
    Match match(scaled_roster(item.n));
    CHECK(match.state().players.size() == item.n);
    CHECK(match.state().total_supply == 20U * item.n && match.state().reserve == 10U * item.n);
    std::uint64_t held = 0;
    for (const auto& player : match.state().players) {
      CHECK(player.credits == kStartingCredits && player.shares == kStartingShares);
      held += player.shares;
    }
    CHECK(held + match.state().reserve == match.state().total_supply);
  }
  CHECK(!role_counts(2));
  CHECK(!role_counts(65));
  for (const auto bad_size : {std::size_t{2}, std::size_t{65}}) {
    bool rejected = false;
    try { Match invalid(scaled_roster(bad_size)); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
  }
  auto missing_investigator = scaled_roster(9);
  missing_investigator[2].role = Role::Employee;
  bool invalid_distribution = false;
  try { Match invalid(missing_investigator); }
  catch (const std::invalid_argument&) { invalid_distribution = true; }
  CHECK(invalid_distribution);
}

void test_role_distribution_boundaries() {
  // Explicitly verify N=2 (unsupported lower boundary: < kMinPlayerCount)
  CHECK(!role_counts(2));
  CHECK(!supply_plan(2));

  // Explicitly verify N=3 (minimum supported boundary: 1 Insider, 1 Investigator, 1 Employee)
  const auto c3 = role_counts(3);
  CHECK(c3.has_value());
  CHECK(c3->insiders == 1U);
  CHECK(c3->investigators == 1U);
  CHECK(c3->employees == 1U);

  // Explicitly verify N=5 (baseline: 1 Insider, 1 Investigator, 3 Employees)
  const auto c5 = role_counts(5);
  CHECK(c5.has_value());
  CHECK(c5->insiders == 1U);
  CHECK(c5->investigators == 1U);
  CHECK(c5->employees == 3U);

  // Explicitly verify N=32 (L3: 4 Insiders, 2 Investigators, 26 Employees)
  const auto c32 = role_counts(32);
  CHECK(c32.has_value());
  CHECK(c32->insiders == 4U);
  CHECK(c32->investigators == 2U);
  CHECK(c32->employees == 26U);

  // Explicitly verify N=64 (L4/maximum: 8 Insiders, 4 Investigators, 52 Employees)
  const auto c64 = role_counts(64);
  CHECK(c64.has_value());
  CHECK(c64->insiders == 8U);
  CHECK(c64->investigators == 4U);
  CHECK(c64->employees == 52U);

  // Explicitly verify N=65 (unsupported upper boundary: > kMaxPlayerCount)
  CHECK(!role_counts(65));
  CHECK(!supply_plan(65));

  // Verify formula invariance for all valid N in [3, 64]
  for (std::size_t n = 3; n <= 64; ++n) {
    const auto counts = role_counts(n);
    CHECK(counts.has_value());
    const auto expected_insiders = 1U + (n - 1U) / 8U;
    const auto expected_investigators = 1U + (n - 1U) / 16U;
    const auto expected_employees = n - expected_insiders - expected_investigators;
    CHECK(counts->insiders == expected_insiders);
    CHECK(counts->investigators == expected_investigators);
    CHECK(counts->employees == expected_employees);
    CHECK(counts->employees >= 1U);
    CHECK(counts->insiders + counts->investigators + counts->employees == n);
  }
}

void test_variable_market_conservation_and_determinism() {
  for (const auto count : {std::size_t{3}, std::size_t{16}, std::size_t{64}}) {
    Match first(scaled_roster(count));
    Match second(scaled_roster(count));
    start_to_trading(first, -1);
    start_to_trading(second, -1);
    for (std::size_t seat = 0; seat < count; ++seat) {
      const auto action = seat % 3U == 0U ? TradeAction::Buy
          : seat % 3U == 1U ? TradeAction::Sell : TradeAction::Hold;
      const auto player = first.state().players[seat].id;
      CHECK(first.submit_trade(player, action));
      CHECK(second.submit_trade(player, action));
    }
    const auto first_market = first.resolve_market();
    const auto second_market = second.resolve_market();
    CHECK(first_market && second_market);
    CHECK(first.canonical_state() == second.canonical_state());
    CHECK(first.canonical_events() == second.canonical_events());
    std::uint64_t held = 0;
    for (const auto& player : first.state().players) held += player.shares;
    CHECK(held + first.state().reserve == first.state().total_supply);
  }
}

void test_normal_five_player_worked_example() {
  Match match(roster());
  start_to_trading(match, -1);
  OperationResult result;
  CHECK(match.insider_hint(10, result) == -1 && result);
  CHECK(!match.insider_hint(20, result) && result.error == ErrorCode::NotInvestigator);
  CHECK(match.submit_trade(10, TradeAction::Buy));
  CHECK(match.submit_trade(20, TradeAction::Buy));
  CHECK(match.submit_trade(30, TradeAction::Sell));
  CHECK(match.submit_trade(40, TradeAction::Hold));
  CHECK(match.submit_trade(50, TradeAction::Hold));
  const auto market = match.resolve_market();
  CHECK(market.has_value());
  if (!market) return;
  CHECK(market->executed_buys == 2U && market->executed_sells == 1U);
  CHECK(market->closing_reserve == 49U);
  CHECK(market->closing_health == 5 && market->closing_price == 9);
  std::uint32_t held = 0;
  for (const auto& player : match.state().players) held += player.shares;
  CHECK(held + match.state().reserve == 100U && held == 51U);
  CHECK(match.state().players[0].credits == 90U);
  CHECK(match.state().players[1].credits == 90U);
  CHECK(match.state().players[2].credits == 110U);
}

void test_reserve_shortage_and_conservation() {
  auto state = base_state();
  state.reserve = 1;
  state.players[0].shares += 49;  // held=99; total remains 100 including reserve
  state.players[0].action_submitted = true;
  state.players[0].order = TradeAction::Buy;
  state.players[1].action_submitted = true;
  state.players[1].order = TradeAction::Buy;
  Match match = Match::from_state(std::move(state));
  const auto result = match.resolve_market();
  CHECK(result.has_value());
  if (!result) return;
  CHECK(result->receipts[0].disposition == TradeDisposition::Executed);
  CHECK(result->receipts[1].disposition == TradeDisposition::RejectedInsufficientReserve);
  CHECK(result->executed_buys == 1U && result->closing_reserve == 0U);
  std::uint32_t held = 0;
  for (const auto& p : match.state().players) held += p.shares;
  CHECK(held == 100U && held + match.state().reserve == match.state().total_supply);

  auto same_round_state = base_state();
  same_round_state.reserve = 1;
  same_round_state.players[0].shares += 49;
  same_round_state.players[0].action_submitted = true;
  same_round_state.players[0].order = TradeAction::Buy;
  same_round_state.players[1].action_submitted = true;
  same_round_state.players[1].order = TradeAction::Buy;
  same_round_state.players[2].action_submitted = true;
  same_round_state.players[2].order = TradeAction::Sell;
  Match same_round = Match::from_state(std::move(same_round_state));
  const auto batch = same_round.resolve_market();
  CHECK(batch && batch->executed_buys == 1U && batch->executed_sells == 1U);
  CHECK(batch && batch->receipts[1].disposition == TradeDisposition::RejectedInsufficientReserve);
  CHECK(batch && batch->closing_reserve == 1U); // the SELL replenishes only after BUY allocation
}

void test_invalid_trades_and_credit_cap() {
  auto state = base_state();
  state.players[0].credits = 5;
  state.players[0].shares = 0;
  state.players[1].shares += 10;
  state.players[1].credits = kCreditBalanceCap;
  Match match = Match::from_state(std::move(state));
  const auto before_credits = match.state().players[0].credits;
  const auto before_shares = match.state().players[0].shares;
  CHECK(match.submit_trade(10, TradeAction::Buy).error == ErrorCode::InsufficientCredits);
  CHECK(match.state().players[0].credits == before_credits && match.state().players[0].shares == before_shares);
  CHECK(match.submit_trade(10, TradeAction::Hold).error == ErrorCode::ActionAlreadySubmitted);
  CHECK(match.submit_trade(20, TradeAction::Sell).error == ErrorCode::CreditCapExceeded);
  CHECK(match.submit_trade(40, static_cast<TradeAction>(99)).error == ErrorCode::InvalidAction);
  CHECK(match.state().players[1].credits == kCreditBalanceCap && match.state().players[1].shares == 20U);
  CHECK(match.submit_trade(30, TradeAction::Sell));
  const auto result = match.resolve_market();
  CHECK(result.has_value());
  CHECK(match.state().players[2].credits == 110U);

  auto fixture = base_state();
  fixture.players[0].shares = 0;
  fixture.players[1].shares = 20;
  Match zero_share_match = Match::from_state(std::move(fixture));
  CHECK(zero_share_match.submit_trade(10, TradeAction::Sell).error == ErrorCode::NoSharesToSell);
}

void test_trade_order_preconditions_in_fixture_validation() {
  const auto rejected = [](MatchState state) {
    try {
      (void)Match::from_state(std::move(state));
      return false;
    } catch (const std::invalid_argument&) {
      return true;
    }
  };

  auto buy_without_cash = base_state();
  buy_without_cash.players[0].action_submitted = true;
  buy_without_cash.players[0].order = TradeAction::Buy;
  buy_without_cash.players[0].credits = 9;
  CHECK(rejected(std::move(buy_without_cash)));

  auto sell_without_share = base_state();
  sell_without_share.players[0].action_submitted = true;
  sell_without_share.players[0].order = TradeAction::Sell;
  sell_without_share.players[0].shares = 0;
  sell_without_share.players[1].shares += 10; // preserve total supply
  CHECK(rejected(std::move(sell_without_share)));

  auto sell_exceeding_cap = base_state();
  sell_exceeding_cap.players[0].action_submitted = true;
  sell_exceeding_cap.players[0].order = TradeAction::Sell;
  sell_exceeding_cap.players[0].credits = kCreditBalanceCap;
  CHECK(rejected(std::move(sell_exceeding_cap)));

  auto valid_buy_pending_zero_reserve = base_state();
  valid_buy_pending_zero_reserve.reserve = 0;
  valid_buy_pending_zero_reserve.players[0].shares += 50; // preserve total supply
  valid_buy_pending_zero_reserve.players[0].action_submitted = true;
  valid_buy_pending_zero_reserve.players[0].order = TradeAction::Buy;
  valid_buy_pending_zero_reserve.players[0].credits = 100;
  // A BUY can be accepted as a request and rejected later by opening-reserve allocation.
  (void)Match::from_state(std::move(valid_buy_pending_zero_reserve));

  auto valid_sell_at_cap_boundary = base_state();
  valid_sell_at_cap_boundary.players[0].action_submitted = true;
  valid_sell_at_cap_boundary.players[0].order = TradeAction::Sell;
  valid_sell_at_cap_boundary.players[0].credits = kCreditBalanceCap - 10;
  (void)Match::from_state(std::move(valid_sell_at_cap_boundary));
}

void test_formula_bounds() {
  auto lower = market_fixture(1, 1, -2, {{0, TradeAction::Sell}});
  auto low = lower.resolve_market();
  CHECK(low && low->closing_health == 0 && low->closing_price == 1);
  CHECK(low && low->outcome == Outcome::InsiderWin);
  CHECK(lower.state().phase == Phase::Finished);

  auto upper = market_fixture(10, 100, 2, {{0, TradeAction::Buy}});
  auto high = upper.resolve_market();
  CHECK(high && high->closing_health == 10 && high->closing_price == 100);

  auto tie = market_fixture(5, 10, 0, {{0, TradeAction::Buy}, {1, TradeAction::Sell}});
  auto equal = tie.resolve_market();
  CHECK(equal && equal->closing_health == 5 && equal->closing_price == 10);

  auto vector = market_fixture(5, 10, 1, {{0, TradeAction::Buy}, {1, TradeAction::Buy}});
  auto exact = vector.resolve_market();
  CHECK(exact && exact->closing_health == 7 && exact->closing_price == 13);

  // Zero health is already terminal and cannot be an opening market state.
  for (std::int32_t health = kMinHealth + 1; health <= kMaxHealth; ++health) {
    for (std::int32_t price = kMinPrice; price <= kMaxPrice; ++price) {
      for (std::int32_t event = -2; event <= 2; ++event) {
        for (std::int32_t net : {-1, 0, 1}) {
          std::vector<std::pair<std::size_t, TradeAction>> orders;
          if (net < 0) orders.emplace_back(0U, TradeAction::Sell);
          if (net > 0) orders.emplace_back(0U, TradeAction::Buy);
          auto m = market_fixture(health, price, event, std::move(orders));
          const auto got = m.resolve_market();
          const auto expected_health = std::clamp(health + event + net, kMinHealth, kMaxHealth);
          const auto expected_price = std::clamp(price + 2 * event + net, kMinPrice, kMaxPrice);
          CHECK(got && got->closing_health == expected_health && got->closing_price == expected_price);
          std::uint32_t held = 0;
          for (const auto& player : m.state().players) held += player.shares;
          CHECK(held + m.state().reserve == m.state().total_supply);
        }
      }
    }
  }
}

void test_eliminated_players_cannot_act_or_be_targets() {
  Match match(roster());
  CHECK(match.start());
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  for (const PlayerId id : {10U, 20U, 30U, 40U, 50U}) {
    CHECK(match.submit_trade(id, TradeAction::Hold));
  }
  CHECK(match.resolve_market().has_value());
  CHECK(match.open_voting());
  CHECK(match.submit_vote(10, 50));
  CHECK(match.submit_vote(20, 50));
  CHECK(match.submit_vote(30, 50));
  const auto eliminated = match.resolve_votes();
  CHECK(eliminated && eliminated->eliminated == 50U);
  CHECK(match.state().players[4].shares == 10U); // holdings remain after elimination

  // The just-resolved vote may still point at its eliminated target, and the
  // eliminated player may have submitted a vote/action before elimination.
  (void)Match::from_state(match.state());
  CHECK(match.state().players[4].action_submitted);
  CHECK(match.state().players[4].order == TradeAction::Hold);

  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  CHECK(!match.state().players[4].action_submitted && !match.state().players[4].order);
  CHECK(match.submit_trade(50, TradeAction::Hold).error == ErrorCode::InactivePlayer);
  CHECK(match.resolve_market().has_value());
  CHECK(match.open_voting());
  CHECK(match.submit_vote(10, 50).error == ErrorCode::InvalidVoteTarget);
  CHECK(match.submit_vote(50, std::nullopt).error == ErrorCode::InactivePlayer);
}

void test_clue_scope_and_idempotency() {
  Match match(roster());
  start_to_trading(match, 1);
  CHECK(match.resolve_market().has_value());
  OperationResult result;
  CHECK(!match.use_clue(30, 10, result) && result.error == ErrorCode::NotInvestigator);
  CHECK(!match.use_clue(20, 20, result) && result.error == ErrorCode::InvalidClueTarget);
  const auto answer = match.use_clue(20, 10, result);
  CHECK(answer.has_value() && *answer && result);
  const auto retry = match.use_clue(20, 10, result);
  CHECK(retry.has_value() && *retry && result);
  CHECK(!match.use_clue(20, 30, result) && result.error == ErrorCode::ClueAlreadyUsed);
  CHECK(match.state().clue_used);
  CHECK(match.open_voting());
}

void test_multiple_investigators_have_independent_clues() {
  Match match(scaled_roster(24));
  CHECK(match.start());
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  CHECK(match.resolve_market().has_value());
  OperationResult first_result;
  const auto first = match.use_clue(4, 1, first_result);
  CHECK(first && *first && first_result);
  const auto retry = match.use_clue(4, 1, first_result);
  CHECK(retry && *retry && first_result);
  const auto state_before_retarget = match.canonical_state();
  const auto events_before_retarget = match.canonical_events();
  CHECK(!match.use_clue(4, 2, first_result) && first_result.error == ErrorCode::ClueAlreadyUsed);
  CHECK(match.canonical_state() == state_before_retarget);
  CHECK(match.canonical_events() == events_before_retarget);
  OperationResult second_result;
  const auto second = match.use_clue(5, 2, second_result);
  CHECK(second && *second && second_result);
  CHECK(match.state().players[3].clue_used && match.state().players[4].clue_used);
  CHECK(match.canonical_state().find("clue=4:1:1") != std::string::npos);
  CHECK(match.canonical_state().find("clue=5:2:1") != std::string::npos);
}

void test_multiple_investigators_independent_clues_reverse_order() {
  Match match(scaled_roster(24));
  CHECK(match.start());
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  CHECK(match.resolve_market().has_value());
  // Second investigator (ID 5) acts BEFORE first investigator (ID 4)
  OperationResult second_result;
  const auto second = match.use_clue(5, 2, second_result);
  CHECK(second && *second && second_result);
  const auto retry_second = match.use_clue(5, 2, second_result);
  CHECK(retry_second && *retry_second && second_result);
  CHECK(!match.use_clue(5, 3, second_result) && second_result.error == ErrorCode::ClueAlreadyUsed);

  // First investigator (ID 4) acts after second investigator has used clue
  OperationResult first_result;
  const auto first = match.use_clue(4, 1, first_result);
  CHECK(first && *first && first_result);
  const auto retry_first = match.use_clue(4, 1, first_result);
  CHECK(retry_first && *retry_first && first_result);
  CHECK(!match.use_clue(4, 3, first_result) && first_result.error == ErrorCode::ClueAlreadyUsed);

  // Non-investigator rejected
  OperationResult non_inv_result;
  CHECK(!match.use_clue(6, 1, non_inv_result) && non_inv_result.error == ErrorCode::NotInvestigator);

  CHECK(match.state().players[3].clue_used && match.state().players[4].clue_used);
  CHECK(match.state().clue_used);
  CHECK(match.state().clue_target && *match.state().clue_target == 1U);
  CHECK(match.state().clue_result && *match.state().clue_result == true);

  // State must validate and recreate cleanly via from_state
  CHECK(Match::from_state(match.state()).state().clue_used);
}

void test_multiple_investigators_clues_across_rounds() {
  Match match(scaled_roster(24));
  CHECK(match.start());
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  CHECK(match.resolve_market().has_value());

  // Round 1: only second investigator (ID 5) uses clue
  OperationResult r1_result;
  const auto clue_5 = match.use_clue(5, 2, r1_result);
  CHECK(clue_5 && *clue_5 && r1_result);
  CHECK(match.state().clue_used);
  CHECK(!match.state().clue_target.has_value());  // first investigator hasn't used clue yet
  CHECK(Match::from_state(match.state()).state().clue_used);

  // Advance through voting without elimination
  CHECK(match.open_voting());
  const auto vote_res = match.resolve_votes();
  CHECK(vote_res.has_value() && !vote_res->eliminated.has_value());
  CHECK(match.state().round == 2U);

  // Round 2: advance to Clue phase
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  CHECK(match.resolve_market().has_value());

  // Second investigator repeats request: same answer
  const auto repeat_5 = match.use_clue(5, 2, r1_result);
  CHECK(repeat_5 && *repeat_5 && r1_result);
  CHECK(!match.use_clue(5, 3, r1_result) && r1_result.error == ErrorCode::ClueAlreadyUsed);

  // First investigator (ID 4) now uses clue in Round 2
  OperationResult r2_result;
  const auto clue_4 = match.use_clue(4, 1, r2_result);
  CHECK(clue_4 && *clue_4 && r2_result);
  CHECK(match.state().clue_target && *match.state().clue_target == 1U);
  CHECK(Match::from_state(match.state()).state().clue_used);
}

void test_same_target_clue_retry_after_target_elimination() {
  Match match(roster());
  CHECK(match.start());
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  CHECK(match.resolve_market());
  OperationResult result;
  const auto clue = match.use_clue(20, 50, result);
  CHECK(clue && !*clue && result);
  CHECK(match.open_voting());
  CHECK(match.submit_vote(10, 50));
  CHECK(match.submit_vote(20, 50));
  CHECK(match.submit_vote(30, 50));
  CHECK(match.resolve_votes()->eliminated == 50U);
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
  CHECK(match.resolve_market());
  const auto retry = match.use_clue(20, 50, result);
  CHECK(retry && !*retry && result);
}

void test_multi_insider_victory_is_team_based() {
  Match match(scaled_roster(9));
  CHECK(match.start());
  round_to_voting(match, 0);
  for (PlayerId voter = 3; voter <= 7; ++voter) CHECK(match.submit_vote(voter, 1));
  const auto first = match.resolve_votes();
  CHECK(first && first->eliminated == 1U && first->outcome == Outcome::None);
  CHECK(match.state().phase == Phase::AwaitingEvent);

  round_to_voting(match, 0);
  for (PlayerId voter = 3; voter <= 7; ++voter) CHECK(match.submit_vote(voter, 2));
  const auto last = match.resolve_votes();
  CHECK(last && last->eliminated == 2U && last->outcome == Outcome::EmployeesWin);
  CHECK(match.state().phase == Phase::Finished);
}

void test_variable_voting_thresholds_and_team_survival() {
  Match minimum(scaled_roster(3));
  CHECK(minimum.start());
  round_to_voting(minimum, 0);
  CHECK(minimum.submit_vote(1, 3));
  CHECK(minimum.submit_vote(2, 3)); // 2 of 3 is a strict majority.
  const auto min_vote = minimum.resolve_votes();
  CHECK(min_vote && min_vote->eliminated == 3U && min_vote->eligible_voters == 3U);

  Match eight_tie(scaled_roster(8));
  CHECK(eight_tie.start());
  round_to_voting(eight_tie, 0);
  for (PlayerId voter = 1; voter <= 4; ++voter) CHECK(eight_tie.submit_vote(voter, 8));
  const auto tie = eight_tie.resolve_votes(); // 4 of 8 is not strictly more than half.
  CHECK(tie && !tie->eliminated && tie->eligible_voters == 8U);

  Match two_insiders_remain(scaled_roster(9));
  CHECK(two_insiders_remain.start());
  round_to_voting(two_insiders_remain, 0);
  for (PlayerId voter = 3; voter <= 7; ++voter) CHECK(two_insiders_remain.submit_vote(voter, 1));
  CHECK(two_insiders_remain.resolve_votes()->outcome == Outcome::None);
  round_to_voting(two_insiders_remain, 0);
  finish_round_without_elimination(two_insiders_remain);
  round_to_voting(two_insiders_remain, 0);
  finish_round_without_elimination(two_insiders_remain);
  CHECK(two_insiders_remain.state().outcome == Outcome::InsiderWin);

  auto health_state = Match(scaled_roster(9)).state();
  health_state.phase = Phase::Trading;
  health_state.round = 1;
  health_state.company.health = 1;
  health_state.event_impact = -2;
  Match collapse = Match::from_state(std::move(health_state));
  const auto market = collapse.resolve_market();
  CHECK(market && market->closing_health == 0 && market->outcome == Outcome::InsiderWin);
}

void test_eliminated_special_roles_cannot_act() {
  for (const PlayerId eliminated_id : {PlayerId{1}, PlayerId{3}}) {
    auto state = Match(scaled_roster(9)).state();
    state.phase = Phase::AwaitingEvent;
    state.round = 2;
    state.eligible_voters_at_open = 9;
    auto eliminated = std::find_if(state.players.begin(), state.players.end(),
        [eliminated_id](const PlayerState& player) { return player.id == eliminated_id; });
    eliminated->active = false;
    std::uint32_t votes = 0;
    for (auto& voter : state.players) {
      if (voter.active && votes < 5U) {
        voter.vote_submitted = true;
        voter.vote_target = eliminated_id;
        ++votes;
      }
    }
    Match match = Match::from_state(std::move(state));
    CHECK(match.publish_event(0));
    OperationResult result;
    CHECK(!match.insider_hint(eliminated_id, result) && result.error == ErrorCode::InactivePlayer);
    CHECK(match.open_trading());
    CHECK(match.submit_trade(eliminated_id, TradeAction::Hold).error == ErrorCode::InactivePlayer);
    CHECK(match.resolve_market());
    CHECK(!match.use_clue(eliminated_id, 2, result) && result.error == ErrorCode::InactivePlayer);
    CHECK(match.open_voting());
    CHECK(match.submit_vote(eliminated_id, std::nullopt).error == ErrorCode::InactivePlayer);
  }
}

void test_vote_edges_and_victories() {
  Match tied(roster());
  CHECK(tied.start());
  round_to_voting(tied, 0);
  CHECK(tied.submit_vote(10, 20));
  CHECK(tied.submit_vote(20, 30));
  CHECK(tied.submit_vote(30, std::nullopt));
  CHECK(tied.submit_vote(40, std::nullopt));
  // Player 50 is missing: it is an abstention and remains in E=5.
  const auto no_majority = tied.resolve_votes();
  CHECK(no_majority && !no_majority->eliminated && no_majority->eligible_voters == 5U);
  CHECK(tied.state().round == 2U && tied.state().phase == Phase::AwaitingEvent);

  Match insider_caught(roster());
  CHECK(insider_caught.start());
  round_to_voting(insider_caught, 0);
  CHECK(insider_caught.submit_vote(20, 10));
  CHECK(insider_caught.submit_vote(30, 10));
  CHECK(insider_caught.submit_vote(40, 10));
  const auto caught = insider_caught.resolve_votes();
  CHECK(caught && caught->eliminated == 10U && caught->outcome == Outcome::EmployeesWin);
  CHECK(insider_caught.state().phase == Phase::Finished);

  Match employee_out(roster());
  CHECK(employee_out.start());
  round_to_voting(employee_out, 0);
  CHECK(employee_out.submit_vote(10, 30));
  CHECK(employee_out.submit_vote(20, 30));
  CHECK(employee_out.submit_vote(40, 30));
  const auto eliminated_employee = employee_out.resolve_votes();
  CHECK(eliminated_employee && eliminated_employee->eliminated == 30U);
  CHECK(employee_out.state().players[2].shares == 10U);
  CHECK(employee_out.submit_vote(30, 10).error == ErrorCode::WrongPhase); // next round not opened

  Match final_survival(roster());
  CHECK(final_survival.start());
  round_to_voting(final_survival, 0);
  finish_round_without_elimination(final_survival);
  round_to_voting(final_survival, 0);
  finish_round_without_elimination(final_survival);
  round_to_voting(final_survival, 0);
  finish_round_without_elimination(final_survival);
  CHECK(final_survival.state().outcome == Outcome::InsiderWin);
}

void test_invalid_votes_and_eliminated_voter() {
  Match match(roster());
  CHECK(match.start());
  round_to_voting(match, 0);
  CHECK(match.submit_vote(10, 10).error == ErrorCode::InvalidVoteTarget);
  CHECK(match.submit_vote(10, 999).error == ErrorCode::InvalidVoteTarget);
  CHECK(match.submit_vote(10, 20));
  CHECK(match.submit_vote(10, std::nullopt).error == ErrorCode::VoteAlreadySubmitted);
}

void play_replay(Match& match) {
  CHECK(match.publish_event(-1));
  CHECK(match.open_trading());
  CHECK(match.submit_trade(50, TradeAction::Buy));
  CHECK(match.submit_trade(20, TradeAction::Hold));
  CHECK(match.submit_trade(10, TradeAction::Sell));
  CHECK(match.submit_trade(30, TradeAction::Buy));
  CHECK(match.submit_trade(40, TradeAction::Hold));
  CHECK(match.resolve_market().has_value());
  CHECK(match.open_voting());
  CHECK(match.submit_vote(10, std::nullopt));
  CHECK(match.submit_vote(20, 10));
  CHECK(match.submit_vote(30, 10));
  CHECK(match.submit_vote(40, std::nullopt));
  CHECK(match.submit_vote(50, std::nullopt));
  CHECK(match.resolve_votes().has_value());
}

void test_deterministic_replay() {
  Match first(roster());
  Match second(roster());
  CHECK(first.start() && second.start());
  play_replay(first);
  play_replay(second);
  CHECK(first.canonical_state() == second.canonical_state());
  CHECK(first.canonical_events() == second.canonical_events());

  Match arrival_a(roster());
  Match arrival_b(roster());
  start_to_trading(arrival_a, 0);
  start_to_trading(arrival_b, 0);
  CHECK(arrival_a.submit_trade(10, TradeAction::Buy));
  CHECK(arrival_a.submit_trade(20, TradeAction::Sell));
  CHECK(arrival_b.submit_trade(20, TradeAction::Sell));
  CHECK(arrival_b.submit_trade(10, TradeAction::Buy));
  CHECK(arrival_a.resolve_market().has_value());
  CHECK(arrival_b.resolve_market().has_value());
  CHECK(arrival_a.canonical_state() == arrival_b.canonical_state());
}

void test_stage4_quantity_trading() {
  // 1. Basic quantity BUY and SELL
  Match match(roster());
  CHECK(match.start());
  CHECK(match.publish_event(0));
  CHECK(match.open_trading());

  // Player 1 buys 3 shares at opening price 10. Cost = 30 credits.
  CHECK(match.submit_trade(10, TradeAction::Buy, 3));
  // Player 2 sells 2 shares at opening price 10. Proceeds = 20 credits.
  CHECK(match.submit_trade(20, TradeAction::Sell, 2));
  // Player 3 holds
  CHECK(match.submit_trade(30, TradeAction::Hold, 1));
  // Missing trades from players 40 and 50 default to Hold.

  const auto res = match.resolve_market();
  CHECK(res.has_value());
  CHECK(res->executed_buys == 3U);
  CHECK(res->executed_sells == 2U);
  CHECK(match.state().players[0].shares == 13U);
  CHECK(match.state().players[0].credits == 70U);
  CHECK(match.state().players[1].shares == 8U);
  CHECK(match.state().players[1].credits == 120U);
  CHECK(match.state().players[2].shares == 10U);
  CHECK(match.state().players[2].credits == 100U);
  CHECK(match.state().players[3].shares == 10U); // defaulted to hold
  CHECK(match.state().players[4].shares == 10U); // defaulted to hold

  // Conservation check
  std::uint32_t total_held = 0;
  for (const auto& p : match.state().players) total_held += p.shares;
  CHECK(total_held + match.state().reserve == match.state().total_supply);

  // 2. Quantity validation and rejection without state mutation
  // Zero quantity rejected
  Match match_zero(roster());
  CHECK(match_zero.start());
  CHECK(match_zero.publish_event(0));
  CHECK(match_zero.open_trading());
  auto err = match_zero.submit_trade(10, TradeAction::Buy, 0);
  CHECK(err.error == ErrorCode::InvalidQuantity);
  CHECK(match_zero.state().players[0].action_submitted); // slot consumed per game rules
  CHECK(!match_zero.state().players[0].order.has_value()); // but no order recorded

  // Overflowing / unaffordable buy quantity
  Match match_unaffordable(roster());
  CHECK(match_unaffordable.start());
  CHECK(match_unaffordable.publish_event(0));
  CHECK(match_unaffordable.open_trading());
  err = match_unaffordable.submit_trade(10, TradeAction::Buy, 20); // 20 * 10 = 200 > 100 credits
  CHECK(err.error == ErrorCode::InsufficientCredits);
  CHECK(!match_unaffordable.state().players[0].order.has_value());

  // Insufficient shares sell quantity
  Match match_noshares(roster());
  CHECK(match_noshares.start());
  CHECK(match_noshares.publish_event(0));
  CHECK(match_noshares.open_trading());
  err = match_noshares.submit_trade(10, TradeAction::Sell, 15); // player has 10 shares
  CHECK(err.error == ErrorCode::NoSharesToSell);
  CHECK(!match_noshares.state().players[0].order.has_value());

  // Sell that would exceed credit cap
  auto fixture_cap = base_state();
  fixture_cap.players[0].credits = kCreditBalanceCap - 10;
  fixture_cap.players[0].shares = 10;
  Match match_cap = Match::from_state(std::move(fixture_cap));
  err = match_cap.submit_trade(10, TradeAction::Sell, 2); // 2 * 10 = 20 > remaining room (10)
  CHECK(err.error == ErrorCode::CreditCapExceeded);

  // 3. Finite reserve shortage and all-or-nothing allocation (no partial fill)
  auto shortage_fixture = base_state();
  shortage_fixture.reserve = 3;
  shortage_fixture.players[0].shares += 47; // conserve total supply = 100
  Match match_shortage = Match::from_state(std::move(shortage_fixture));
  // In round 1, seat 0 (Player 10) goes first, seat 1 (Player 20) goes second
  CHECK(match_shortage.submit_trade(10, TradeAction::Buy, 2));
  CHECK(match_shortage.submit_trade(20, TradeAction::Buy, 2));

  const auto shortage_res = match_shortage.resolve_market();
  CHECK(shortage_res.has_value());
  CHECK(shortage_res->executed_buys == 2U);
  CHECK(shortage_res->receipts[0].disposition == TradeDisposition::Executed);
  CHECK(shortage_res->receipts[0].quantity == 2U);
  CHECK(shortage_res->receipts[1].disposition == TradeDisposition::RejectedInsufficientReserve);
  CHECK(shortage_res->receipts[1].quantity == 0U);
  CHECK(match_shortage.state().reserve == 1U);

  // Supply strictly conserved
  total_held = 0;
  for (const auto& p : match_shortage.state().players) total_held += p.shares;
  CHECK(total_held + match_shortage.state().reserve == match_shortage.state().total_supply);

  // 4. Default single-share compatibility
  Match match_single(roster());
  CHECK(match_single.start());
  CHECK(match_single.publish_event(0));
  CHECK(match_single.open_trading());
  CHECK(match_single.submit_trade(10, TradeAction::Buy));
  CHECK(match_single.state().players[0].order_quantity == 1U);
}

void test_stage4_research_action() {
  Match match(roster());
  CHECK(match.start());

  // 1. Wrong phase check: cannot research before publish_event (in AwaitingEvent)
  OperationResult res;
  auto out = match.conduct_research(10, res);
  CHECK(!res);
  CHECK(res.error == ErrorCode::WrongPhase);
  CHECK(!out.has_value());
  CHECK(match.state().players[0].credits == 100U);
  CHECK(!match.state().players[0].research_used);

  // 2. Publish positive event (+2)
  CHECK(match.publish_event(2));
  CHECK(match.state().phase == Phase::Discussion);

  // 3. Valid research call
  out = match.conduct_research(10, res);
  CHECK(res);
  CHECK(out.has_value());
  CHECK(out->direction == "Positive");
  CHECK(!out->already_used);
  CHECK(out->credits_remaining == 90U);
  CHECK(match.state().players[0].credits == 90U);
  CHECK(match.state().players[0].research_used);

  // 4. Idempotency: repeated call does not charge twice and reports already_used
  out = match.conduct_research(10, res);
  CHECK(res);
  CHECK(out.has_value());
  CHECK(out->direction == "Positive");
  CHECK(out->already_used);
  CHECK(out->credits_remaining == 90U);
  CHECK(match.state().players[0].credits == 90U);

  // 5. Another player researches
  out = match.conduct_research(20, res);
  CHECK(res);
  CHECK(out.has_value());
  CHECK(out->direction == "Positive");
  CHECK(!out->already_used);
  CHECK(out->credits_remaining == 90U);
  CHECK(match.state().players[1].credits == 90U);

  // 6. Cannot research once trading opens
  CHECK(match.open_trading());
  out = match.conduct_research(30, res);
  CHECK(!res);
  CHECK(res.error == ErrorCode::WrongPhase);
  CHECK(match.state().players[2].credits == 100U);
  CHECK(!match.state().players[2].research_used);

  // 7. Test negative and neutral coarse mapping
  Match match_neg(roster());
  CHECK(match_neg.start());
  CHECK(match_neg.publish_event(-1));
  out = match_neg.conduct_research(10, res);
  CHECK(res && out && out->direction == "Negative");

  Match match_neu(roster());
  CHECK(match_neu.start());
  CHECK(match_neu.publish_event(0));
  out = match_neu.conduct_research(10, res);
  CHECK(res && out && out->direction == "Neutral");

  // 8. Insufficient credits
  auto state_poor = base_state();
  state_poor.players[0].credits = 5;
  state_poor.phase = Phase::Discussion;
  state_poor.event_impact = 1;
  Match match_poor = Match::from_state(std::move(state_poor));
  out = match_poor.conduct_research(10, res);
  CHECK(!res);
  CHECK(res.error == ErrorCode::InsufficientCredits);
  CHECK(match_poor.state().players[0].credits == 5U);
  CHECK(!match_poor.state().players[0].research_used);
}

void test_stage4_advanced_market_model() {
  // 1. Replay determinism with candidate proposal coefficients
  const auto candidate = MarketModelCoefficients::candidate_proposal();
  Match m1(roster(), candidate);
  Match m2(roster(), candidate);

  CHECK(m1.start());
  CHECK(m2.start());
  CHECK(m1.publish_event(1));
  CHECK(m2.publish_event(1));
  CHECK(m1.open_trading());
  CHECK(m2.open_trading());

  CHECK(m1.submit_trade(10, TradeAction::Buy, 2));
  CHECK(m1.submit_trade(20, TradeAction::Sell, 1));
  CHECK(m2.submit_trade(10, TradeAction::Buy, 2));
  CHECK(m2.submit_trade(20, TradeAction::Sell, 1));

  const auto res1 = m1.resolve_market();
  const auto res2 = m2.resolve_market();
  CHECK(res1.has_value() && res2.has_value());
  CHECK(res1->closing_price == res2->closing_price);
  CHECK(res1->closing_health == res2->closing_health);
  CHECK(res1->sentiment == res2->sentiment);
  CHECK(res1->volatility == res2->volatility);
  CHECK(res1->liquidity == res2->liquidity);
  CHECK(m1.canonical_state() == m2.canonical_state());

  // 2. Neutral conditions with baseline and candidate coefficients
  const auto base_neutral_p = Match::calculate_price(10, 5, 0, 0, 0, 50, MarketModelCoefficients::approved_baseline());
  CHECK(base_neutral_p == 10);
  const auto cand_neutral_p = Match::calculate_price(10, 5, 0, 0, 0, 50, candidate);
  CHECK(cand_neutral_p == 10);

  // 3. Strict bounding under extreme inputs
  const auto extreme_high = Match::calculate_price(99, 10, 2, 20, 10, 100, candidate);
  CHECK(extreme_high == 100);
  const auto extreme_low = Match::calculate_price(2, 0, -2, -20, -10, 5, candidate);
  CHECK(extreme_low == 1);

  // Arithmetic overflow safety with large demand
  const auto safe_clamp = Match::calculate_price(50, 10, 2, 1000, 10, 50, candidate);
  CHECK(safe_clamp == 100);

  // 4. Indicator consistency
  CHECK(res1->sentiment >= -10 && res1->sentiment <= 10);
  CHECK(res1->volatility <= 20U);
  CHECK(res1->liquidity <= 100U);
}

void test_stage4_negative_floor_division() {
  // Test mathematical floor division vs C++ truncating division
  // For positive numbers: floor division matches C++ truncation
  CHECK(Match::floor_div(0, 2) == 0);
  CHECK(Match::floor_div(1, 2) == 0);
  CHECK(Match::floor_div(2, 2) == 1);
  CHECK(Match::floor_div(3, 2) == 1);
  CHECK(Match::floor_div(7, 2) == 3);
  CHECK(Match::floor_div(9, 2) == 4);

  // For negative odd numbers: mathematical floor rounds DOWN to more negative integer
  // (C++ truncating division rounds towards zero, which caused Stage 4 bugs)
  CHECK(Match::floor_div(-1, 2) == -1);  // C++: -1 / 2 == 0
  CHECK(Match::floor_div(-3, 2) == -2);  // C++: -3 / 2 == -1
  CHECK(Match::floor_div(-7, 2) == -4);  // C++: -7 / 2 == -3
  CHECK(Match::floor_div(-9, 2) == -5);  // C++: -9 / 2 == -4

  // For negative even numbers: division is exact
  CHECK(Match::floor_div(-2, 2) == -1);
  CHECK(Match::floor_div(-4, 2) == -2);
  CHECK(Match::floor_div(-6, 2) == -3);
  CHECK(Match::floor_div(-8, 2) == -4);

  // Odd/even boundaries around zero for net demand
  CHECK(Match::floor_div(-5, 2) == -3);
  CHECK(Match::floor_div(-4, 2) == -2);
  CHECK(Match::floor_div(-3, 2) == -2);
  CHECK(Match::floor_div(-2, 2) == -1);
  CHECK(Match::floor_div(-1, 2) == -1);
  CHECK(Match::floor_div(0, 2) == 0);
  CHECK(Match::floor_div(1, 2) == 0);
  CHECK(Match::floor_div(2, 2) == 1);
  CHECK(Match::floor_div(3, 2) == 1);
  CHECK(Match::floor_div(4, 2) == 2);
  CHECK(Match::floor_div(5, 2) == 2);

  // Negative divisor coverage
  CHECK(Match::floor_div(7, -2) == -4);
  CHECK(Match::floor_div(-7, -2) == 3);
  CHECK(Match::floor_div(-1, -2) == 0);
  CHECK(Match::floor_div(-3, -2) == 1);

  // Extreme arithmetic boundaries and INT32_MIN overflow safety (INT32_MIN / -1 must not UB)
  CHECK(Match::floor_div(std::numeric_limits<std::int32_t>::min(), -1) == std::numeric_limits<std::int32_t>::max());
  CHECK(Match::floor_div(std::numeric_limits<std::int32_t>::min(), 1) == std::numeric_limits<std::int32_t>::min());
  CHECK(Match::floor_div(std::numeric_limits<std::int32_t>::min(), 2) == -1073741824);
  CHECK(Match::floor_div(std::numeric_limits<std::int32_t>::max(), 1) == std::numeric_limits<std::int32_t>::max());
  CHECK(Match::floor_div(std::numeric_limits<std::int32_t>::max(), -1) == -std::numeric_limits<std::int32_t>::max());

  // Division by zero throws std::invalid_argument instead of silently returning 0
  bool threw_zero_pos = false;
  try { (void)Match::floor_div(10, 0); } catch (const std::invalid_argument&) { threw_zero_pos = true; }
  CHECK(threw_zero_pos);

  bool threw_zero_neg = false;
  try { (void)Match::floor_div(-10, 0); } catch (const std::invalid_argument&) { threw_zero_neg = true; }
  CHECK(threw_zero_neg);

  bool threw_zero_zero = false;
  try { (void)Match::floor_div(0, 0); } catch (const std::invalid_argument&) { threw_zero_zero = true; }
  CHECK(threw_zero_zero);

  // Verify candidate pricing explicitly across sentiment values: -9, -7, -3, -1, 0, 1, 3, 7, 9
  // Candidate coefficients: w_e=2, w_d=1, w_v=1, w_vol=1, w_h=1, w_s=1, w_l=2
  const auto candidate = MarketModelCoefficients::candidate_proposal();
  // Neutral opening: P=50, H=5, e=0, D=0, L=50% -> market_dir=0 -> volatility term=0
  CHECK(Match::calculate_price(50, 5, 0, 0, -9, 50, candidate) == 50 - 5); // 45
  CHECK(Match::calculate_price(50, 5, 0, 0, -7, 50, candidate) == 50 - 4); // 46
  CHECK(Match::calculate_price(50, 5, 0, 0, -3, 50, candidate) == 50 - 2); // 48
  CHECK(Match::calculate_price(50, 5, 0, 0, -1, 50, candidate) == 50 - 1); // 49
  CHECK(Match::calculate_price(50, 5, 0, 0,  0, 50, candidate) == 50 + 0); // 50
  CHECK(Match::calculate_price(50, 5, 0, 0,  1, 50, candidate) == 50 + 0); // 50
  CHECK(Match::calculate_price(50, 5, 0, 0,  3, 50, candidate) == 50 + 1); // 51
  CHECK(Match::calculate_price(50, 5, 0, 0,  7, 50, candidate) == 50 + 3); // 53
  CHECK(Match::calculate_price(50, 5, 0, 0,  9, 50, candidate) == 50 + 4); // 54
}

void test_stage4_recalculated_scenarios() {
  const auto candidate = MarketModelCoefficients::candidate_proposal();

  // Strong negative scenario (Scenario 2):
  // Opening P=10, H=5, e=-2, D=-1
  // Closing health H' = clamp(5 - 2 - 1, 0, 10) = 2
  // Sentiment S' = -1 -> floor(-1/2) = -1
  // Gross volume G = 1, e = -2 -> V = clamp(1 + 2*2, 0, 20) = 5
  // Volatility term: w_vol * dir_market * floor(5/4) = 1 * (-1) * 1 = -1
  // Components: w_e*e = -4, w_d*sign(D) = -1, w_v = 0, w_vol = -1, w_h*(H'-5) = -3, w_s*floor(S'/2) = -1, w_l = 0
  // Delta P = -4 - 1 + 0 - 1 - 3 - 1 = -10
  // Closing price P' = clamp(10 - 10, 1, 100) = 1 (clamped)
  const auto p_scen2_with_v = Match::calculate_price(10, 2, -2, -1, -1, 51, 5, candidate);
  CHECK(p_scen2_with_v == 1);
  const auto p_scen2_default_v = Match::calculate_price(10, 2, -2, -1, -1, 51, candidate);
  CHECK(p_scen2_default_v == 1);

  // Live match resolution of Scenario 2
  Match m(roster(), candidate);
  CHECK(m.start());
  CHECK(m.publish_event(-2));
  CHECK(m.open_trading());
  CHECK(m.submit_trade(10, TradeAction::Sell, 1));
  const auto res = m.resolve_market();
  CHECK(res.has_value());
  CHECK(res->closing_health == 2);
  CHECK(res->sentiment == -1);
  CHECK(res->volatility == 5U);
  CHECK(res->closing_price == 1);

  // Scenario 5 (Low Sentiment Bear Panic):
  // P=20, H=1, e=-1, D=-4, S'=-7, L=54%
  // Without volatility (V=0): Delta P = -12 -> P' = 8
  // With volatility (G=4, e=-1 -> V=6 -> floor(6/4)=1 -> vol term = -1): Delta P = -13 -> P' = 7
  const auto p_scen5_no_v = Match::calculate_price(20, 1, -1, -4, -7, 54, 0, candidate);
  CHECK(p_scen5_no_v == 8);
  const auto p_scen5_with_v = Match::calculate_price(20, 1, -1, -4, -7, 54, 6, candidate);
  CHECK(p_scen5_with_v == 7);

  // Scenario 11 (Distressed Company):
  // P=30, H=1, e=0, D=-1, S'=-1, L=51%
  // G=1, e=0 -> V=1 -> floor(1/4)=0 -> vol term = 0
  // w_e=0, w_d=-1, w_v=0, w_vol=0, w_h=-4, w_s=floor(-1/2)=-1 -> Delta P = -6 -> P' = 24
  const auto p_scen11 = Match::calculate_price(30, 1, 0, -1, -1, 51, 1, candidate);
  CHECK(p_scen11 == 24);
  CHECK(Match::calculate_price(30, 1, 0, -1, -1, 51, candidate) == 24);

  // Scenario 9 / 7B (Low Liquidity Squeeze):
  // Opening P=15, H=6, closing H'=8, e=1, D=2, S'=1, closing reserve=10, total supply=100 -> L=10% (< 20%)
  // G=2, e=1 -> V = clamp(2 + 2*1, 0, 20) = 4 -> floor(4/4)=1 -> vol term = +1
  // Scarcity surcharge (+w_l = +2) applies when buying into depleted reserve:
  // With V=4: Delta P = +2 (event) + 1 (demand) + 0 (volume) + 1 (vol) + 3 (health) + 0 (sentiment) + 2 (scarcity) = +9 -> P' = 24
  // Without volatility (V=0): Delta P = +2 + 1 + 0 + 0 + 3 + 0 + 2 = +8 -> P' = 23
  const auto p_scen9_with_v = Match::calculate_price(15, 8, 1, 2, 1, 10, 4, candidate);
  CHECK(p_scen9_with_v == 24);
  const auto p_scen9_no_v = Match::calculate_price(15, 8, 1, 2, 1, 10, 0, candidate);
  CHECK(p_scen9_no_v == 23);

  // Low vs High reserve liquidity with otherwise identical inputs:
  // Scenario 7A (High reserve L=68%): scarcity surcharge does NOT apply
  const auto p_scen7a_with_v = Match::calculate_price(15, 8, 1, 2, 1, 68, 4, candidate);
  CHECK(p_scen7a_with_v == 22);
  const auto p_scen7a_no_v = Match::calculate_price(15, 8, 1, 2, 1, 68, 0, candidate);
  CHECK(p_scen7a_no_v == 21);

  // Confirm scarcity surcharge difference: 7B is strictly +2 higher than 7A in both cases
  CHECK(p_scen9_with_v - p_scen7a_with_v == 2);
  CHECK(p_scen9_no_v - p_scen7a_no_v == 2);
}

void test_stage4_gross_vs_net_volume_and_volatility() {
  const auto candidate = MarketModelCoefficients::candidate_proposal();

  // Test 1: Zero trading and neutral event: G=0, D=0 -> Volatility = 0
  Match m1(roster(), candidate);
  CHECK(m1.start());
  CHECK(m1.publish_event(0));
  CHECK(m1.open_trading());
  const auto res1 = m1.resolve_market();
  CHECK(res1.has_value());
  CHECK(res1->executed_buys == 0U);
  CHECK(res1->executed_sells == 0U);
  CHECK(res1->volatility == 0U);
  CHECK(res1->closing_price == 10);

  // Test 2: Equal gross buys and sells (B=2, S=2, e=0): G=4, D=0
  // Gross volume G=4 must yield non-zero volatility (V = 4), not 0!
  // Neutral market direction (D=0, e=0 -> dir_market=0) ensures volatility term is 0.
  // Preserves neutral price behavior without upward or downward bias!
  Match m2(roster(), candidate);
  CHECK(m2.start());
  CHECK(m2.publish_event(0));
  CHECK(m2.open_trading());
  CHECK(m2.submit_trade(10, TradeAction::Buy, 2));
  CHECK(m2.submit_trade(20, TradeAction::Sell, 2));
  const auto res2 = m2.resolve_market();
  CHECK(res2.has_value());
  CHECK(res2->executed_buys == 2U);
  CHECK(res2->executed_sells == 2U);
  CHECK(res2->volatility == 4U);  // Repaired: correctly reflects 4 shares traded!
  CHECK(res2->closing_price == 10); // Price remains neutral (zero bias in balanced market)

  // Test 3: Same net demand (+3) but different gross volume
  // Case A: Opening P=10, B=3, S=0 -> G=3, D=+3, e=0 -> V=3 -> floor(3/4)=0 -> vol term = 0 -> Delta P = +3 -> P'=13
  Match mA(roster(), candidate);
  CHECK(mA.start());
  CHECK(mA.publish_event(0));
  CHECK(mA.open_trading());
  CHECK(mA.submit_trade(10, TradeAction::Buy, 3));
  const auto resA = mA.resolve_market();
  CHECK(resA.has_value());
  CHECK(resA->executed_buys == 3U);
  CHECK(resA->executed_sells == 0U);
  CHECK(resA->volatility == 3U);
  CHECK(resA->closing_price == 13);

  // Case B: Opening P=10, B=5, S=2 -> G=7, D=+3, e=0 -> V=7 -> floor(7/4)=1 -> vol term = +1 -> Delta P = +4 -> P'=14
  Match mB(roster(), candidate);
  CHECK(mB.start());
  CHECK(mB.publish_event(0));
  CHECK(mB.open_trading());
  CHECK(mB.submit_trade(10, TradeAction::Buy, 5));
  CHECK(mB.submit_trade(20, TradeAction::Sell, 2));
  const auto resB = mB.resolve_market();
  CHECK(resB.has_value());
  CHECK(resB->executed_buys == 5U);
  CHECK(resB->executed_sells == 2U);
  CHECK(resB->volatility == 7U); // Repaired: V=7 correctly reflects the 7 shares traded
  CHECK(resB->closing_price == 14);

  // Volatility explicitly changes closing price when gross volume changes with equal net demand
  CHECK(resB->closing_price > resA->closing_price);
  CHECK(resB->closing_price - resA->closing_price == 1);

  // With opening P=20 as in Scenario 5 table:
  CHECK(Match::calculate_price(20, 6, 0, 3, 1, 47, 3, candidate) == 23);
  CHECK(Match::calculate_price(20, 6, 0, 3, 1, 47, 7, candidate) == 24);
}

void test_stage4_all_nine_reproducible_scenarios() {
  const auto candidate = MarketModelCoefficients::candidate_proposal();

  // Scenario 1: No trading, neutral event (P=10, H=5, e=0, B=0, S=0) -> P'=10, H'=5, V=0
  CHECK(Match::calculate_price(10, 5, 0, 0, 0, 50, 0, candidate) == 10);

  // Scenario 2: Equal gross buys/sells (P=50, H=5, e=0, B=2, S=2, D=0, G=4) -> P'=50, H'=5, V=4
  CHECK(Match::calculate_price(50, 5, 0, 0, 0, 50, 4, candidate) == 50);

  // Scenario 3: Large positive net demand (P=50, H=6, e=0, D=+6, S'=+3, L=44%, V=6) -> P'=56
  // Delta P = 0 (event) + 1 (demand) + 2 (volume) + 1 (vol) + 1 (health) + 1 (sentiment) = +6 -> P'=56
  CHECK(Match::calculate_price(50, 6, 0, 6, 3, 44, 6, candidate) == 56);

  // Scenario 4: Large negative net demand (P=50, H=4, e=0, D=-6, S'=-3, L=56%, V=6) -> P'=43
  // Delta P = 0 - 1 - 2 - 1 - 1 - 2 = -7 -> P'=43
  CHECK(Match::calculate_price(50, 4, 0, -6, -3, 56, 6, candidate) == 43);

  // Scenario 5: Same net demand (+3), different gross volume (P=20, H=6, e=0, D=+3, S'=+1)
  // 5A: G=3 -> V=3 -> floor(3/4)=0 -> P'=23
  CHECK(Match::calculate_price(20, 6, 0, 3, 1, 47, 3, candidate) == 23);
  // 5B: G=7 -> V=7 -> floor(7/4)=1 -> P'=24
  CHECK(Match::calculate_price(20, 6, 0, 3, 1, 47, 7, candidate) == 24);

  // Scenario 6A: Positive event (P=10, H=8, e=+2, D=+1, S'=+1, L=49%, V=5) -> P'=19
  CHECK(Match::calculate_price(10, 8, 2, 1, 1, 49, 5, candidate) == 19);

  // Scenario 6B: Negative event (P=10, H=2, e=-2, D=-1, S'=-1, L=51%, V=5) -> P'=1 (clamped)
  CHECK(Match::calculate_price(10, 2, -2, -1, -1, 51, 5, candidate) == 1);

  // Scenario 7A: High liquidity reserve (P=15, H=8, e=+1, D=+2, S'=+1, L=68%, V=4) -> P'=22
  CHECK(Match::calculate_price(15, 8, 1, 2, 1, 68, 4, candidate) == 22);

  // Scenario 7B: Low liquidity squeeze (P=15, H=8, e=+1, D=+2, S'=+1, L=10%, V=4) -> P'=24
  CHECK(Match::calculate_price(15, 8, 1, 2, 1, 10, 4, candidate) == 24);

  // Scenario 8A: Healthy company (P=30, H=10, e=0, D=+1, S'=+1, L=49%, V=1) -> P'=36
  CHECK(Match::calculate_price(30, 10, 0, 1, 1, 49, 1, candidate) == 36);

  // Scenario 8B: Distressed company (P=30, H=1, e=0, D=-1, S'=-1, L=51%, V=1) -> P'=24
  CHECK(Match::calculate_price(30, 1, 0, -1, -1, 51, 1, candidate) == 24);

  // Scenario 9A: Upper bound clamping (P=95, H=10, e=+2, D=+5, S'=+2, L=45%, V=9) -> P'=100
  CHECK(Match::calculate_price(95, 10, 2, 5, 2, 45, 9, candidate) == 100);

  // Scenario 9B: Lower bound clamping (P=5, H=0, e=-2, D=-5, S'=-3, L=55%, V=9) -> P'=1
  CHECK(Match::calculate_price(5, 0, -2, -5, -3, 55, 9, candidate) == 1);
}

void test_settlement_order_provenance_and_defaults() {
  // 1. Verify default Match uses approved_baseline coefficients
  Match default_match(roster());
  CHECK(default_match.market_coefficients().event_weight == 2);
  CHECK(default_match.market_coefficients().demand_weight == 1);
  CHECK(default_match.market_coefficients().volume_weight == 0);
  CHECK(default_match.market_coefficients().volatility_weight == 0);
  CHECK(default_match.market_coefficients().health_weight == 0);
  CHECK(default_match.market_coefficients().sentiment_weight == 0);
  CHECK(default_match.market_coefficients().liquidity_penalty == 0);

  // 2. Verify calculate_price() default argument uses approved_baseline
  CHECK(Match::calculate_price(10, 5, 0, 0, 0, 50) == 10);
  // With baseline, health dev (H=10), sentiment (S=10), and volatility (V=20) do NOT affect price
  CHECK(Match::calculate_price(10, 10, 0, 0, 10, 50, 20) == 10);
  CHECK(Match::calculate_price(10, 10, 0, 0, 10, 50) == 10);

  // 3. Verify settlement order provenance: rotating seat priority ((round - 1) % N)
  // In round 1, seat priority is 0
  CHECK(default_match.start());
  CHECK(default_match.publish_event(0));
  CHECK(default_match.open_trading());
  CHECK(default_match.state().seat_priority == 0U);

  // Synthetic match with low reserve to test all-or-nothing allocation in rotating seat order
  auto st = base_state();
  st.reserve = 3;
  st.players[0].shares += 47; // held=97; total remains 100 including reserve
  Match shortage_match = Match::from_state(std::move(st));

  // Seat 0 requests 2 shares, Seat 1 requests 2 shares
  CHECK(shortage_match.submit_trade(10, TradeAction::Buy, 2));
  CHECK(shortage_match.submit_trade(20, TradeAction::Buy, 2));

  // Seat 0 is first in rotating priority: fills 2 shares (reserve drops 3 -> 1)
  // Seat 1 requests 2 shares, but remaining reserve is 1 < 2: rejected all-or-nothing!
  const auto res = shortage_match.resolve_market();
  CHECK(res.has_value());
  CHECK(res->executed_buys == 2U);
  CHECK(res->receipts[0].disposition == TradeDisposition::Executed);
  CHECK(res->receipts[0].quantity == 2U);
  CHECK(res->receipts[1].disposition == TradeDisposition::RejectedInsufficientReserve);
  CHECK(res->receipts[1].quantity == 0U);

  // Conservation of 20N supply: sum(shares) + reserve == 20N = 100
  Shares total_shares = shortage_match.state().reserve;
  for (const auto& p : shortage_match.state().players) {
    total_shares += p.shares;
  }
  CHECK(total_shares == shortage_match.state().total_supply);
}

void test_voting_regression_cases_a_through_f() {
  // Case A — one vote, four eligible voters
  // One player submits a vote for Player 2. The other three players do not submit votes.
  // Resolution produces no elimination.
  // Result reports: 1 submitted vote, 0 explicit abstentions, 3 missing votes, 4 eligible voters.
  // Must not claim 0 votes submitted.
  {
    Match match(scaled_roster(4));
    CHECK(match.start());
    round_to_voting(match, 0);
    CHECK(match.submit_vote(1, 2));
    const auto res = match.resolve_votes();
    CHECK(res.has_value());
    CHECK(!res->eliminated.has_value());
    CHECK(res->eligible_voters == 4U);
    CHECK(res->votes_cast == 1U);
    CHECK(res->votes_abstained == 0U);
    CHECK(res->votes_missing == 3U);
    CHECK(res->votes_received == 0U);
  }

  // Case B — successful strict-majority elimination
  // Four eligible voters. Three valid votes target the same active player.
  // Resolution eliminates that player. The eliminated candidate's received-vote count is 3.
  {
    Match match(scaled_roster(4));
    CHECK(match.start());
    round_to_voting(match, 0);
    CHECK(match.submit_vote(1, 2));
    CHECK(match.submit_vote(3, 2));
    CHECK(match.submit_vote(4, 2));
    CHECK(match.submit_vote(2, std::nullopt));
    const auto res = match.resolve_votes();
    CHECK(res.has_value());
    CHECK(res->eliminated.has_value() && *res->eliminated == 2U);
    CHECK(res->votes_received == 3U);
    CHECK(res->eligible_voters == 4U);
    CHECK(res->votes_cast == 4U);
    CHECK(res->votes_abstained == 1U);
    CHECK(res->votes_missing == 0U);
  }

  // Case C — insufficient majority
  // Four eligible voters. Two votes target one player and two target another.
  // Nobody is eliminated. Reporting correctly reflects four submitted votes without leaking individual ballots.
  {
    Match match(scaled_roster(4));
    CHECK(match.start());
    round_to_voting(match, 0);
    CHECK(match.submit_vote(1, 3));
    CHECK(match.submit_vote(2, 3));
    CHECK(match.submit_vote(3, 2));
    CHECK(match.submit_vote(4, 2));
    const auto res = match.resolve_votes();
    CHECK(res.has_value());
    CHECK(!res->eliminated.has_value());
    CHECK(res->eligible_voters == 4U);
    CHECK(res->votes_cast == 4U);
    CHECK(res->votes_abstained == 0U);
    CHECK(res->votes_missing == 0U);
  }

  // Case D — explicit abstentions
  // Three players submit explicit abstentions and one votes for a candidate.
  // Resolution records four submitted votes, three explicit abstentions and zero missing votes.
  // No candidate is eliminated.
  {
    Match match(scaled_roster(4));
    CHECK(match.start());
    round_to_voting(match, 0);
    CHECK(match.submit_vote(1, std::nullopt));
    CHECK(match.submit_vote(2, std::nullopt));
    CHECK(match.submit_vote(3, std::nullopt));
    CHECK(match.submit_vote(4, 2));
    const auto res = match.resolve_votes();
    CHECK(res.has_value());
    CHECK(!res->eliminated.has_value());
    CHECK(res->eligible_voters == 4U);
    CHECK(res->votes_cast == 4U);
    CHECK(res->votes_abstained == 3U);
    CHECK(res->votes_missing == 0U);
  }

  // Case E — deadline and duplicate behavior
  // A vote accepted before the authoritative deadline is included in resolution.
  // A vote arriving after deadline is rejected.
  // Duplicate submissions do not increase counts.
  {
    Match match(scaled_roster(4));
    CHECK(match.start());
    round_to_voting(match, 0);
    CHECK(match.submit_vote(1, 2));
    CHECK(match.submit_vote(1, 3).error == ErrorCode::VoteAlreadySubmitted);
    CHECK(match.submit_vote(1, std::nullopt).error == ErrorCode::VoteAlreadySubmitted);
    CHECK(match.submit_vote(2, 2).error == ErrorCode::InvalidVoteTarget);
    const auto res = match.resolve_votes();
    CHECK(res.has_value());
    CHECK(match.submit_vote(2, 3).error == ErrorCode::WrongPhase);
  }

  // Case F — end-of-match behavior
  // Final-round InsiderWin and immediate EmployeesWin precedence
  {
    Match match_emp_win(scaled_roster(4));
    CHECK(match_emp_win.start());
    round_to_voting(match_emp_win, 0);
    CHECK(match_emp_win.submit_vote(2, 1));
    CHECK(match_emp_win.submit_vote(3, 1));
    CHECK(match_emp_win.submit_vote(4, 1));
    const auto res1 = match_emp_win.resolve_votes();
    CHECK(res1.has_value());
    CHECK(res1->eliminated.has_value() && *res1->eliminated == 1U);
    CHECK(res1->outcome == Outcome::EmployeesWin);
    CHECK(match_emp_win.state().phase == Phase::Finished);

    Match match_insider_survives(scaled_roster(4));
    CHECK(match_insider_survives.start());
    for (int r = 1; r <= 3; ++r) {
      round_to_voting(match_insider_survives, 0);
      const auto res = match_insider_survives.resolve_votes();
      CHECK(res.has_value());
      if (r < 3) {
        CHECK(!res->eliminated.has_value());
        CHECK(res->outcome == Outcome::None);
      } else {
        CHECK(!res->eliminated.has_value());
        CHECK(res->outcome == Outcome::InsiderWin);
        CHECK(match_insider_survives.state().phase == Phase::Finished);
      }
    }
  }
}

}  // namespace

int main() {
  test_voting_regression_cases_a_through_f();
  test_state_fixture_invariants();
  test_initialization_and_supply();
  test_role_distribution_boundaries();
  test_variable_rosters_and_supply();
  test_variable_market_conservation_and_determinism();
  test_normal_five_player_worked_example();
  test_reserve_shortage_and_conservation();
  test_invalid_trades_and_credit_cap();
  test_trade_order_preconditions_in_fixture_validation();
  test_formula_bounds();
  test_clue_scope_and_idempotency();
  test_multiple_investigators_have_independent_clues();
  test_multiple_investigators_independent_clues_reverse_order();
  test_multiple_investigators_clues_across_rounds();
  test_same_target_clue_retry_after_target_elimination();
  test_multi_insider_victory_is_team_based();
  test_variable_voting_thresholds_and_team_survival();
  test_eliminated_special_roles_cannot_act();
  test_vote_edges_and_victories();
  test_invalid_votes_and_eliminated_voter();
  test_eliminated_players_cannot_act_or_be_targets();
  test_deterministic_replay();
  test_stage4_quantity_trading();
  test_stage4_research_action();
  test_stage4_advanced_market_model();
  test_stage4_negative_floor_division();
  test_stage4_recalculated_scenarios();
  test_stage4_gross_vs_net_volume_and_volatility();
  test_stage4_all_nine_reproducible_scenarios();
  test_settlement_order_provenance_and_defaults();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All DARKPOOL S1 engine tests passed\n";
  return EXIT_SUCCESS;
}

