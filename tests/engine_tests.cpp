#include "darkpool/game_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
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

MatchState base_state() {
  MatchState state;
  state.players = {{10, Role::Insider}, {20, Role::Investigator}, {30, Role::Employee},
                   {40, Role::Employee}, {50, Role::Employee}};
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

void test_initialization_and_supply() {
  Match match(roster());
  CHECK(match.state().players.size() == 5U);
  CHECK(match.state().reserve == 50U);
  CHECK(match.state().total_supply == 100U);
  CHECK(match.state().company.health == 5);
  CHECK(match.state().company.price == 10);
  CHECK(match.state().phase == Phase::Lobby);
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
  CHECK(!supply_plan(4));
  CHECK(!supply_plan(65));

  bool rejected = false;
  try { Match invalid({{1, Role::Insider}, {2, Role::Investigator}, {3, Role::Employee}, {4, Role::Employee}, {5, Role::Insider}}); }
  catch (const std::invalid_argument&) { rejected = true; }
  CHECK(rejected);
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

void test_formula_bounds() {
  auto lower = market_fixture(0, 1, -2, {{0, TradeAction::Sell}});
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

  for (std::int32_t health = kMinHealth; health <= kMaxHealth; ++health) {
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
  round_to_voting(match, 0);
  CHECK(match.submit_vote(10, 50));
  CHECK(match.submit_vote(20, 50));
  CHECK(match.submit_vote(30, 50));
  const auto eliminated = match.resolve_votes();
  CHECK(eliminated && eliminated->eliminated == 50U);
  CHECK(match.state().players[4].shares == 10U); // holdings remain after elimination

  CHECK(match.publish_event(0));
  CHECK(match.open_trading());
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

}  // namespace

int main() {
  test_initialization_and_supply();
  test_normal_five_player_worked_example();
  test_reserve_shortage_and_conservation();
  test_invalid_trades_and_credit_cap();
  test_formula_bounds();
  test_clue_scope_and_idempotency();
  test_vote_edges_and_victories();
  test_invalid_votes_and_eliminated_voter();
  test_eliminated_players_cannot_act_or_be_targets();
  test_deterministic_replay();
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return EXIT_FAILURE;
  }
  std::cout << "All DARKPOOL S1 engine tests passed\n";
  return EXIT_SUCCESS;
}

