#include "darkpool/match_coordinator.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void check(bool condition, const char* expression, int line) {
  if (!condition) { ++failures; std::cerr << "FAIL line " << line << ": " << expression << '\n'; }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

using darkpool::network::json;
using darkpool::server::MatchCoordinator;
using darkpool::server::SessionRole;

json create(MatchCoordinator& coordinator, std::uint64_t moderator) {
  return *coordinator.handle(moderator, json{{"type", "create_match"},
      {"roles", {"Insider", "Investigator", "Employee", "Employee", "Employee"}}});
}

json join(MatchCoordinator& coordinator, std::uint64_t player, std::uint64_t match_id) {
  return *coordinator.handle(player, json{{"type", "join_match"}, {"match_id", match_id}});
}

void test_match_isolation_and_authorization() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 10; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }

  const auto created_a = create(coordinator, 100);
  const auto created_b = create(coordinator, 100);
  CHECK(created_a["type"] == "match_created");
  CHECK(created_b["type"] == "match_created");
  const auto match_a = created_a["match_id"].get<std::uint64_t>();
  const auto match_b = created_b["match_id"].get<std::uint64_t>();
  CHECK(match_a != match_b);

  for (std::uint64_t seat = 0; seat < 5; ++seat) {
    const auto a = join(coordinator, seat + 1, match_a);
    const auto b = join(coordinator, seat + 6, match_b);
    CHECK(a["type"] == "joined_match");
    CHECK(b["type"] == "joined_match");
    CHECK(a["player_id"] == b["player_id"]);
  }
  const auto started_a = coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_a}});
  CHECK(started_a && (*started_a)["type"] == "match_started");

  const auto own_status = coordinator.handle(1, json{{"type", "match_status"}, {"match_id", match_a}});
  const auto other_status = coordinator.handle(6, json{{"type", "match_status"}, {"match_id", match_b}});
  CHECK(own_status && (*own_status)["phase"] == "AwaitingEvent");
  CHECK(other_status && (*other_status)["phase"] == "Lobby");

  // A caller cannot gain access to another match by claiming its player ID.
  const auto unauthorized = coordinator.handle(1, json{{"type", "match_status"},
      {"match_id", match_b}, {"player_id", 1}});
  CHECK(unauthorized && (*unauthorized)["type"] == "error");
  CHECK((*unauthorized)["error"] == "connection is not a member of target match");
  const auto cross_match_gameplay = coordinator.handle(1, json{{"type", "insider_hint"},
      {"match_id", match_b}});
  CHECK(cross_match_gameplay && (*cross_match_gameplay)["type"] == "error");
  const auto unchanged = coordinator.handle(6, json{{"type", "match_status"}, {"match_id", match_b}});
  CHECK(unchanged && (*unchanged)["phase"] == "Lobby");

  const auto outsider_start = coordinator.handle(1, json{{"type", "start_match"}, {"match_id", match_b}});
  CHECK(outsider_start && (*outsider_start)["type"] == "error");
}

void test_session_identity_and_moderator_transitions() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  coordinator.register_session(101, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 5; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }
  const auto created = create(coordinator, 100);
  const auto match_id = created["match_id"].get<std::uint64_t>();
  for (std::uint64_t session = 1; session <= 5; ++session) {
    CHECK(join(coordinator, session, match_id)["type"] == "joined_match");
  }
  CHECK((*coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}}))["ok"]);

  const auto player_transition = coordinator.handle(1, json{{"type", "publish_event"},
      {"match_id", match_id}, {"impact", 1}});
  const auto other_moderator_transition = coordinator.handle(101, json{{"type", "publish_event"},
      {"match_id", match_id}, {"impact", 1}});
  CHECK(player_transition && (*player_transition)["type"] == "error");
  CHECK(other_moderator_transition && (*other_moderator_transition)["type"] == "error");
  const auto unchanged = coordinator.handle(1, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(unchanged && (*unchanged)["phase"] == "AwaitingEvent");

  const auto published = coordinator.handle(100, json{{"type", "publish_event"},
      {"match_id", match_id}, {"impact", 1}});
  CHECK(published && (*published)["type"] == "transition_result");
  const auto opened = coordinator.handle(100, json{{"type", "open_trading"}, {"match_id", match_id}});
  CHECK(opened && (*opened)["phase"] == "Trading");

  // Client identity fields are rejected; a subsequent request acts as the session's own seat.
  const auto spoof = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id},
      {"player_id", 2}, {"action", "HOLD"}});
  CHECK(spoof && (*spoof)["type"] == "error");
  const auto own_action = coordinator.handle(1, json{{"type", "trade"},
      {"match_id", match_id}, {"action", "HOLD"}});
  CHECK(own_action && (*own_action)["type"] == "trade_result" && (*own_action)["ok"]);
}

void test_private_responses_and_malformed_requests() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 5; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }
  const auto created = create(coordinator, 100);
  const auto match_id = created["match_id"].get<std::uint64_t>();
  const auto insider_join = join(coordinator, 1, match_id);
  const auto investigator_join = join(coordinator, 2, match_id);
  CHECK(insider_join["private_role"] == "Insider");
  CHECK(investigator_join["private_role"] == "Investigator");
  for (std::uint64_t session = 3; session <= 5; ++session) CHECK(join(coordinator, session, match_id)["type"] == "joined_match");

  CHECK((*coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}}))["ok"]);
  const auto published = coordinator.handle(100, json{{"type", "publish_event"},
      {"match_id", match_id}, {"impact", 2}});
  CHECK(published && (*published)["ok"]);
  const auto employee_hint = coordinator.handle(3, json{{"type", "insider_hint"}, {"match_id", match_id}});
  const auto insider_hint = coordinator.handle(1, json{{"type", "insider_hint"}, {"match_id", match_id}});
  CHECK(employee_hint && (*employee_hint)["type"] == "operation_result");
  CHECK(insider_hint && (*insider_hint)["type"] == "hint_private" && (*insider_hint)["event_impact"] == 2);

  const auto public_before = coordinator.handle(3, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_before && !public_before->contains("event_impact") && !public_before->contains("private_role") &&
        !public_before->contains("clue_target") && !public_before->contains("clue_result"));

  const auto open_trading = coordinator.handle(100, json{{"type", "open_trading"}, {"match_id", match_id}});
  CHECK(open_trading && (*open_trading)["phase"] == "Trading");
  const auto malformed_trade = coordinator.handle(3, json{{"type", "trade"},
      {"match_id", match_id}, {"action", 7}});
  CHECK(malformed_trade && (*malformed_trade)["type"] == "error");
  const auto valid_trade = coordinator.handle(3, json{{"type", "trade"},
      {"match_id", match_id}, {"action", "HOLD"}});
  CHECK(valid_trade && (*valid_trade)["type"] == "trade_result" && (*valid_trade)["ok"]);
  const auto market = coordinator.handle(100, json{{"type", "resolve_market"}, {"match_id", match_id}});
  CHECK(market && (*market)["type"] == "market_result");
  CHECK(!market->contains("receipts") && !market->contains("orders") && !market->contains("player_id"));
  const auto public_after_market = coordinator.handle(3, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_after_market && (*public_after_market).contains("last_market"));
  CHECK(!(*public_after_market)["last_market"].contains("receipts"));

  const auto spoofed_clue = coordinator.handle(2, json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", 1}, {"investigator_id", 1}});
  CHECK(spoofed_clue && (*spoofed_clue)["type"] == "error");
  const auto clue = coordinator.handle(2, json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", 1}});
  CHECK(clue && (*clue)["type"] == "clue_private" && (*clue)["is_insider"]);
  const auto public_after_clue = coordinator.handle(3, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_after_clue && !(*public_after_clue).contains("clue_target") &&
        !(*public_after_clue).contains("clue_result"));

  const auto open_voting = coordinator.handle(100, json{{"type", "open_voting"}, {"match_id", match_id}});
  CHECK(open_voting && (*open_voting)["phase"] == "Voting");
  const auto malformed_vote = coordinator.handle(3, json{{"type", "vote"},
      {"match_id", match_id}, {"target_id", "one"}});
  CHECK(malformed_vote && (*malformed_vote)["type"] == "error");
  for (std::uint64_t session = 1; session <= 3; ++session) {
    if (session == 3) {
      const auto spoofed_vote = coordinator.handle(session, json{{"type", "vote"},
          {"match_id", match_id}, {"target_id", 4}, {"voter_id", 1}});
      CHECK(spoofed_vote && (*spoofed_vote)["type"] == "error");
    }
    const auto submitted = coordinator.handle(session, json{{"type", "vote"},
        {"match_id", match_id}, {"target_id", 4}});
    CHECK(submitted && (*submitted)["type"] == "vote_result");
  }
  const auto resolved = coordinator.handle(100, json{{"type", "resolve_votes"}, {"match_id", match_id}});
  CHECK(resolved && (*resolved)["type"] == "vote_resolution");
  CHECK(!resolved->contains("vote_targets") && !resolved->contains("votes"));
  const auto public_after_vote = coordinator.handle(3, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_after_vote && (*public_after_vote).contains("last_vote"));
  CHECK(!(*public_after_vote)["last_vote"].contains("vote_targets"));
}

}  // namespace

int main() {
  test_match_isolation_and_authorization();
  test_session_identity_and_moderator_transitions();
  test_private_responses_and_malformed_requests();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Match coordinator isolation tests passed.\n";
  return EXIT_SUCCESS;
}
