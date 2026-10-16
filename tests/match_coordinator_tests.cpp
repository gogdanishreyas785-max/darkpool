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

void test_multiple_investigator_clues_reverse_order() {
  MatchCoordinator coordinator;
  coordinator.register_session(900, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 24; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }
  const auto created = create(coordinator, 900, 24);
  const auto match_id = created["match_id"].get<std::uint64_t>();
  std::vector<std::uint64_t> investigator_sessions;
  std::vector<darkpool::PlayerId> insider_ids;
  for (std::uint64_t session = 1; session <= 24; ++session) {
    const auto joined = join(coordinator, session, match_id);
    const auto role = joined["private_role"].get<std::string>();
    if (role == "Insider") insider_ids.push_back(joined["player_id"].get<darkpool::PlayerId>());
    else if (role == "Investigator") investigator_sessions.push_back(session);
  }
  CHECK(insider_ids.size() == 3U && investigator_sessions.size() == 2U);
  CHECK((*coordinator.handle(900, json{{"type", "start_match"}, {"match_id", match_id}}))["ok"]);
  CHECK((*coordinator.handle(900, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}}))["ok"]);
  CHECK((*coordinator.handle(900, json{{"type", "open_trading"}, {"match_id", match_id}}))["ok"]);
  for (std::uint64_t session = 1; session <= 24; ++session) {
    (void)coordinator.handle(session, json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}});
  }
  CHECK((*coordinator.handle(900, json{{"type", "resolve_market"}, {"match_id", match_id}}))["ok"]);

  // Reverse order: second investigator acts BEFORE first investigator
  const auto clue_b = coordinator.handle(investigator_sessions[1], json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", insider_ids[1]}});
  const auto clue_a = coordinator.handle(investigator_sessions[0], json{{"type", "request_clue"},
      {"match_id", match_id}, {"target_id", insider_ids[0]}});
  CHECK(clue_b && (*clue_b)["type"] == "clue_private" && (*clue_b)["is_insider"]);
  CHECK(clue_a && (*clue_a)["type"] == "clue_private" && (*clue_a)["is_insider"]);
}

void test_match_lifecycle_cleanup_and_isolation() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  coordinator.register_session(200, SessionRole::Moderator);

  for (std::uint64_t session = 1; session <= 15; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }

  // Moderator 100 creates two matches: Match A (5 players) and Match B (3 players)
  const auto match_a_info = create(coordinator, 100, 5);
  const auto match_b_info = create(coordinator, 100, 3);
  CHECK(match_a_info["type"] == "match_created");
  CHECK(match_b_info["type"] == "match_created");
  const auto match_a = match_a_info["match_id"].get<std::uint64_t>();
  const auto match_b = match_b_info["match_id"].get<std::uint64_t>();

  // Moderator 200 creates Match C (3 players)
  const auto match_c_info = create(coordinator, 200, 3);
  CHECK(match_c_info["type"] == "match_created");
  const auto match_c = match_c_info["match_id"].get<std::uint64_t>();

  // Players 1..4 join Match A (leaving 1 seat open)
  for (std::uint64_t s = 1; s <= 4; ++s) {
    CHECK(join(coordinator, s, match_a)["type"] == "joined_match");
  }
  // Players 6..8 join Match B
  for (std::uint64_t s = 6; s <= 8; ++s) {
    CHECK(join(coordinator, s, match_b)["type"] == "joined_match");
  }
  // Player 11 joins Match C (leaving seats open)
  CHECK(join(coordinator, 11, match_c)["type"] == "joined_match");

  // Player 2 disconnects from Match A in Lobby
  coordinator.unregister_session(2);
  // Repeated unregister is safe
  coordinator.unregister_session(2);
  coordinator.unregister_session(999);  // nonexistent session

  // Unregistered player cannot act
  const auto unreg_act = coordinator.handle(2, json{{"type", "match_status"}, {"match_id", match_a}});
  CHECK(unreg_act && (*unreg_act)["type"] == "error");

  // Player 5 joins Match A in the seat previously held or empty seat
  const auto p5_join = join(coordinator, 5, match_a);
  CHECK(p5_join["type"] == "joined_match");

  // Register a new player session 12 and let them fill the remaining seat in Match A
  coordinator.register_session(12, SessionRole::Player);
  const auto p12_join = join(coordinator, 12, match_a);
  CHECK(p12_join["type"] == "joined_match");

  // All 5 seats in Match A filled; Match A starts
  CHECK((*coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_a}}))["ok"]);

  // Stale match ID handling
  const auto stale = coordinator.handle(1, json{{"type", "match_status"}, {"match_id", 99999}});
  CHECK(stale && (*stale)["type"] == "error" && (*stale)["error"] == "unknown match");

  // Ordinary player disconnect does not affect unrelated Match B or Match C
  const auto b_status_before = coordinator.handle(6, json{{"type", "match_status"}, {"match_id", match_b}});
  const auto c_status_before = coordinator.handle(11, json{{"type", "match_status"}, {"match_id", match_c}});
  CHECK(b_status_before && (*b_status_before)["phase"] == "Lobby");
  CHECK(c_status_before && (*c_status_before)["phase"] == "Lobby");

  // Moderator 100 disconnects after creating multiple matches (Match A and Match B)
  coordinator.unregister_session(100);

  // Match A and Match B are cleaned up
  const auto act_a = coordinator.handle(1, json{{"type", "match_status"}, {"match_id", match_a}});
  const auto act_b = coordinator.handle(6, json{{"type", "match_status"}, {"match_id", match_b}});
  CHECK(act_a && (*act_a)["type"] == "error" && (*act_a)["error"] == "unknown match");
  CHECK(act_b && (*act_b)["type"] == "error" && (*act_b)["error"] == "unknown match");

  // Detached player 1 can now join valid Match C (created by Moderator 200) without stale identity
  const auto p1_join_c = join(coordinator, 1, match_c);
  CHECK(p1_join_c["type"] == "joined_match");
  CHECK(p1_join_c["match_id"] == match_c);

  // Detached player 6 can also join Match C
  const auto p6_join_c = join(coordinator, 6, match_c);
  CHECK(p6_join_c["type"] == "joined_match");
  CHECK(p6_join_c["match_id"] == match_c);

  // Match C now has 3 players (11, 1, 6) and can start successfully under Moderator 200
  CHECK((*coordinator.handle(200, json{{"type", "start_match"}, {"match_id", match_c}}))["ok"]);
  const auto c_status_after = coordinator.handle(1, json{{"type", "match_status"}, {"match_id", match_c}});
  CHECK(c_status_after && (*c_status_after)["phase"] == "AwaitingEvent");
}

void test_boundary_player_counts_and_three_player_match() {
  MatchCoordinator coordinator;
  coordinator.register_session(900, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 10; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }

  // 1. Explicitly verify N=2 boundary rejection (lower illegal boundary)
  const auto req_two = coordinator.handle(900, json{{"type", "create_match"}, {"player_count", 2}});
  CHECK(req_two && (*req_two)["type"] == "error");
  CHECK((*req_two)["error"] == "player_count must be an integer from 3 through 64");

  // 2. Reject other invalid counts: 0, 1, 65, -1, string, float
  for (const auto& bad_val : json::array({0, 1, 65, -1, -5, "3", 3.14})) {
    const auto bad_req = coordinator.handle(900, json{{"type", "create_match"}, {"player_count", bad_val}});
    CHECK(bad_req && (*bad_req)["type"] == "error");
    CHECK((*bad_req)["error"] == "player_count must be an integer from 3 through 64");
  }

  // 3. Reject client-supplied roles
  const auto bad_roles = coordinator.handle(900, json{{"type", "create_match"},
      {"roles", {"Insider", "Investigator", "Employee"}}});
  CHECK(bad_roles && (*bad_roles)["type"] == "error");
  CHECK((*bad_roles)["error"] == "client-supplied roles are not accepted");

  // 4. Verify boundary role counts: N=3, N=5, N=32, N=64
  const auto m3 = create(coordinator, 900, 3);
  CHECK(m3["type"] == "match_created" && m3["player_count"] == 3U);
  CHECK(m3["insiders"] == 1U && m3["investigators"] == 1U && m3["employees"] == 1U);
  const auto m3_id = m3["match_id"].get<std::uint64_t>();

  // Moderator cannot join as a player
  const auto mod_join = coordinator.handle(900, json{{"type", "join_match"}, {"match_id", m3_id}});
  CHECK(mod_join && (*mod_join)["type"] == "error");
  CHECK((*mod_join)["error"] == "player permission required");

  // Premature start with 0 players
  const auto start_0 = coordinator.handle(900, json{{"type", "start_match"}, {"match_id", m3_id}});
  CHECK(start_0 && (*start_0)["type"] == "error" && (*start_0)["error"] == "all configured players must join before start");

  // Player 1 joins seat 1
  const auto j1 = join(coordinator, 1, m3_id);
  CHECK(j1["type"] == "joined_match" && j1["seat"] == 1U && j1["player_id"] == 1U);

  // Premature start with 1 player
  const auto start_1 = coordinator.handle(900, json{{"type", "start_match"}, {"match_id", m3_id}});
  CHECK(start_1 && (*start_1)["type"] == "error" && (*start_1)["error"] == "all configured players must join before start");

  // Player 2 joins seat 2
  const auto j2 = join(coordinator, 2, m3_id);
  CHECK(j2["type"] == "joined_match" && j2["seat"] == 2U && j2["player_id"] == 2U);

  // Premature start with 2 players (out of 3 required)
  const auto start_2 = coordinator.handle(900, json{{"type", "start_match"}, {"match_id", m3_id}});
  CHECK(start_2 && (*start_2)["type"] == "error" && (*start_2)["error"] == "all configured players must join before start");

  // Player 3 joins seat 3
  const auto j3 = join(coordinator, 3, m3_id);
  CHECK(j3["type"] == "joined_match" && j3["seat"] == 3U && j3["player_id"] == 3U);

  // 4th player attempts to join full 3-player match
  const auto j4 = join(coordinator, 4, m3_id);
  CHECK(j4["type"] == "error" && j4["error"] == "match is full");

  // Assert exact role counts across the 3 seats
  std::size_t insiders = 0, investigators = 0, employees = 0;
  for (const auto& j : {j1, j2, j3}) {
    const auto r = j["private_role"].get<std::string>();
    if (r == "Insider") ++insiders;
    else if (r == "Investigator") ++investigators;
    else if (r == "Employee") ++employees;
  }
  CHECK(insiders == 1U && investigators == 1U && employees == 1U);

  // Start succeeds once all 3 players joined
  const auto start_ok = coordinator.handle(900, json{{"type", "start_match"}, {"match_id", m3_id}});
  CHECK(start_ok && (*start_ok)["type"] == "transition_result" && (*start_ok)["ok"]);
  CHECK((*start_ok)["phase"] == "AwaitingEvent");

  // Verify other boundary role distributions: N=5, N=32, N=64
  coordinator.unregister_session(900); // clears m3
  coordinator.register_session(901, SessionRole::Moderator);
  const auto m5 = create(coordinator, 901, 5);
  CHECK(m5["type"] == "match_created" && m5["insiders"] == 1U && m5["investigators"] == 1U && m5["employees"] == 3U);

  const auto m32 = create(coordinator, 901, 32);
  CHECK(m32["type"] == "match_created" && m32["insiders"] == 4U && m32["investigators"] == 2U && m32["employees"] == 26U);

  const auto m64 = create(coordinator, 901, 64);
  CHECK(m64["type"] == "match_created" && m64["insiders"] == 8U && m64["investigators"] == 4U && m64["employees"] == 52U);
}

}  // namespace

int main() {
  test_match_isolation_and_authorization();
  test_session_identity_and_moderator_transitions();
  test_private_responses_and_malformed_requests();
  test_mixed_capacity_matches_and_server_assigned_roles();
  test_multiple_investigator_clues_remain_private();
  test_multiple_investigator_clues_reverse_order();
  test_match_lifecycle_cleanup_and_isolation();
  test_boundary_player_counts_and_three_player_match();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Match coordinator isolation tests passed.\n";
  return EXIT_SUCCESS;
}
