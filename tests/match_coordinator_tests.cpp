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

json create(MatchCoordinator& coordinator, std::uint64_t moderator,
            std::optional<std::size_t> player_count = std::nullopt) {
  json command{{"type", "create_match"}};
  if (player_count) command["player_count"] = *player_count;
  return *coordinator.handle(moderator, command);
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
  CHECK(started_a && (*started_a)["type"] == "transition_result" && (*started_a)["ok"]);

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
  std::uint64_t insider_session = 0, investigator_session = 0, employee_session = 0;
  darkpool::PlayerId insider_id = 0, vote_target_id = 0;
  for (std::uint64_t session = 1; session <= 5; ++session) {
    const auto joined = join(coordinator, session, match_id);
    CHECK(joined["type"] == "joined_match");
    const auto role = joined["private_role"].get<std::string>();
    const auto player_id = joined["player_id"].get<darkpool::PlayerId>();
    if (role == "Insider") { insider_session = session; insider_id = player_id; }
    if (role == "Investigator") investigator_session = session;
    if (role == "Employee" && employee_session == 0U) employee_session = session;
    if (session == 4U) vote_target_id = player_id;
  }
  CHECK(insider_session != 0U && investigator_session != 0U && employee_session != 0U);

  CHECK((*coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}}))["ok"]);
  const auto published = coordinator.handle(100, json{{"type", "publish_event"},
      {"match_id", match_id}, {"impact", 2}});
  CHECK(published && (*published)["ok"]);
  const auto employee_hint = coordinator.handle(employee_session, json{{"type", "insider_hint"}, {"match_id", match_id}});
  const auto insider_hint = coordinator.handle(insider_session, json{{"type", "insider_hint"}, {"match_id", match_id}});
  CHECK(employee_hint && (*employee_hint)["type"] == "operation_result");
  CHECK(insider_hint && (*insider_hint)["type"] == "hint_private" && (*insider_hint)["event_impact"] == 2);

  const auto public_before = coordinator.handle(employee_session, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_before && !public_before->contains("event_impact") && !public_before->contains("private_role") &&
        !public_before->contains("clue_target") && !public_before->contains("clue_result"));

  const auto open_trading = coordinator.handle(100, json{{"type", "open_trading"}, {"match_id", match_id}});
  CHECK(open_trading && (*open_trading)["phase"] == "Trading");
  const auto malformed_trade = coordinator.handle(employee_session, json{{"type", "trade"},
      {"match_id", match_id}, {"action", 7}});
  CHECK(malformed_trade && (*malformed_trade)["type"] == "error");
  const auto valid_trade = coordinator.handle(employee_session, json{{"type", "trade"},
      {"match_id", match_id}, {"action", "HOLD"}});
  CHECK(valid_trade && (*valid_trade)["type"] == "trade_result" && (*valid_trade)["ok"]);
  const auto market = coordinator.handle(100, json{{"type", "resolve_market"}, {"match_id", match_id}});
  CHECK(market && (*market)["type"] == "market_result");
  CHECK(!market->contains("receipts") && !market->contains("orders") && !market->contains("player_id"));
  const auto public_after_market = coordinator.handle(3, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_after_market && (*public_after_market).contains("last_market"));
  CHECK(!(*public_after_market)["last_market"].contains("receipts"));

  const auto spoofed_clue = coordinator.handle(investigator_session, json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", insider_id}, {"investigator_id", 1}});
  CHECK(spoofed_clue && (*spoofed_clue)["type"] == "error");
  const auto clue = coordinator.handle(investigator_session, json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", insider_id}});
  CHECK(clue && (*clue)["type"] == "clue_private" && (*clue)["is_insider"]);
  const auto public_after_clue = coordinator.handle(employee_session, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_after_clue && !(*public_after_clue).contains("clue_target") &&
        !(*public_after_clue).contains("clue_result"));

  const auto open_voting = coordinator.handle(100, json{{"type", "open_voting"}, {"match_id", match_id}});
  CHECK(open_voting && (*open_voting)["phase"] == "Voting");
  const auto malformed_vote = coordinator.handle(employee_session, json{{"type", "vote"},
      {"match_id", match_id}, {"target_id", "one"}});
  CHECK(malformed_vote && (*malformed_vote)["type"] == "error");
  for (std::uint64_t session = 1; session <= 3; ++session) {
    if (session == 3) {
      const auto spoofed_vote = coordinator.handle(session, json{{"type", "vote"},
          {"match_id", match_id}, {"target_id", vote_target_id}, {"voter_id", 1}});
      CHECK(spoofed_vote && (*spoofed_vote)["type"] == "error");
    }
    const auto submitted = coordinator.handle(session, json{{"type", "vote"},
        {"match_id", match_id}, {"target_id", vote_target_id}});
    CHECK(submitted && (*submitted)["type"] == "vote_result");
  }
  const auto resolved = coordinator.handle(100, json{{"type", "resolve_votes"}, {"match_id", match_id}});
  CHECK(resolved && (*resolved)["type"] == "vote_resolution");
  CHECK(!resolved->contains("vote_targets") && !resolved->contains("votes"));
  const auto public_after_vote = coordinator.handle(employee_session, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_after_vote && (*public_after_vote).contains("last_vote"));
  CHECK(!(*public_after_vote)["last_vote"].contains("vote_targets"));
}

void test_mixed_capacity_matches_and_server_assigned_roles() {
  MatchCoordinator coordinator;
  coordinator.register_session(500, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 12; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }
  const auto small = create(coordinator, 500, 3);
  const auto large = create(coordinator, 500, 8);
  CHECK(small["type"] == "match_created" && small["player_count"] == 3U);
  CHECK(small["insiders"] == 1U && small["investigators"] == 1U && small["employees"] == 1U);
  CHECK(large["type"] == "match_created" && large["player_count"] == 8U);
  CHECK(large["insiders"] == 1U && large["investigators"] == 1U && large["employees"] == 6U);
  const auto small_id = small["match_id"].get<std::uint64_t>();
  const auto large_id = large["match_id"].get<std::uint64_t>();
  for (std::uint64_t seat = 1; seat <= 3; ++seat) CHECK(join(coordinator, seat, small_id)["type"] == "joined_match");
  for (std::uint64_t seat = 4; seat <= 11; ++seat) CHECK(join(coordinator, seat, large_id)["type"] == "joined_match");
  CHECK(join(coordinator, 12, small_id)["type"] == "error");
  CHECK((*coordinator.handle(500, json{{"type", "start_match"}, {"match_id", small_id}}))["ok"]);
  CHECK((*coordinator.handle(500, json{{"type", "start_match"}, {"match_id", large_id}}))["ok"]);
  const auto small_status = coordinator.handle(1, json{{"type", "match_status"}, {"match_id", small_id}});
  const auto large_status = coordinator.handle(4, json{{"type", "match_status"}, {"match_id", large_id}});
  CHECK(small_status && (*small_status)["player_count"] == 3U && (*small_status)["joined_players"] == 3U);
  CHECK(large_status && (*large_status)["player_count"] == 8U && (*large_status)["joined_players"] == 8U);
  const auto cross_match = coordinator.handle(1, json{{"type", "match_status"}, {"match_id", large_id}});
  CHECK(cross_match && (*cross_match)["type"] == "error");

  const auto legacy_roles = coordinator.handle(500, json{{"type", "create_match"},
      {"roles", {"Insider", "Investigator", "Employee", "Employee", "Employee"}}});
  CHECK(legacy_roles && (*legacy_roles)["type"] == "error");
  const auto malformed_count = coordinator.handle(500, json{{"type", "create_match"}, {"player_count", 2}});
  CHECK(malformed_count && (*malformed_count)["type"] == "error");
  const auto above_maximum = coordinator.handle(500, json{{"type", "create_match"}, {"player_count", 65}});
  CHECK(above_maximum && (*above_maximum)["type"] == "error");
  const auto maximum = create(coordinator, 500, 64);
  CHECK(maximum["type"] == "match_created" && maximum["player_count"] == 64U);
  const auto fourth = create(coordinator, 500, 3);
  CHECK(fourth["type"] == "match_created");
  const auto fifth = create(coordinator, 500, 3);
  CHECK(fifth["type"] == "error");
}

void test_multiple_investigator_clues_remain_private() {
  MatchCoordinator coordinator;
  coordinator.register_session(800, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 24; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }
  const auto created = create(coordinator, 800, 24);
  CHECK(created["type"] == "match_created" && created["insiders"] == 3U &&
        created["investigators"] == 2U && created["employees"] == 19U);
  const auto match_id = created["match_id"].get<std::uint64_t>();
  std::vector<std::uint64_t> investigator_sessions;
  std::vector<darkpool::PlayerId> insider_ids;
  std::uint64_t employee_session = 0;
  for (std::uint64_t session = 1; session <= 24; ++session) {
    const auto joined = join(coordinator, session, match_id);
    CHECK(joined["type"] == "joined_match");
    const auto role = joined["private_role"].get<std::string>();
    if (role == "Insider") insider_ids.push_back(joined["player_id"].get<darkpool::PlayerId>());
    else if (role == "Investigator") investigator_sessions.push_back(session);
    else if (employee_session == 0U) employee_session = session;
  }
  CHECK(insider_ids.size() == 3U && investigator_sessions.size() == 2U);
  CHECK((*coordinator.handle(800, json{{"type", "start_match"}, {"match_id", match_id}}))["ok"]);
  CHECK((*coordinator.handle(800, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}}))["ok"]);
  CHECK((*coordinator.handle(800, json{{"type", "open_trading"}, {"match_id", match_id}}))["ok"]);
  for (std::uint64_t session = 1; session <= 24; ++session) {
    const auto trade = coordinator.handle(session, json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}});
    CHECK(trade && (*trade)["type"] == "trade_result");
  }
  CHECK((*coordinator.handle(800, json{{"type", "resolve_market"}, {"match_id", match_id}}))["ok"]);
  const auto clue_a = coordinator.handle(investigator_sessions[0], json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", insider_ids[0]}});
  const auto clue_b = coordinator.handle(investigator_sessions[1], json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", insider_ids[1]}});
  CHECK(clue_a && (*clue_a)["type"] == "clue_private" && (*clue_a)["is_insider"]);
  CHECK(clue_b && (*clue_b)["type"] == "clue_private" && (*clue_b)["is_insider"]);
  const auto public_status = coordinator.handle(employee_session, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(public_status && !public_status->contains("clue_result") && !public_status->contains("clue_target"));
}

}  // namespace

int main() {
  test_match_isolation_and_authorization();
  test_session_identity_and_moderator_transitions();
  test_private_responses_and_malformed_requests();
  test_mixed_capacity_matches_and_server_assigned_roles();
  test_multiple_investigator_clues_remain_private();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Match coordinator isolation tests passed.\n";
  return EXIT_SUCCESS;
}
