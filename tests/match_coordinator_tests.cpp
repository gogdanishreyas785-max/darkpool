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

inline int test_setenv(const char* name, const char* value) {
#if defined(_WIN32)
  return _putenv_s(name, value);
#else
  return ::setenv(name, value, 1);
#endif
}

inline int test_unsetenv(const char* name) {
#if defined(_WIN32)
  return _putenv_s(name, "");
#else
  return ::unsetenv(name);
#endif
}

using darkpool::network::json;
using darkpool::server::MatchCoordinator;
using darkpool::server::SessionRole;

json create(MatchCoordinator& coordinator, std::uint64_t moderator,
            std::optional<std::size_t> player_count = std::nullopt) {
  json command{{"type", "create_match"}, {"open_lobby", true}};
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

void test_stage2_notifications_and_market_snapshots() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 5; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }

  const auto created = create(coordinator, 100);
  const auto match_id = created["match_id"].get<std::uint64_t>();

  // 1. Initial snapshot on join
  const auto j1 = join(coordinator, 1, match_id);
  CHECK(j1["type"] == "joined_match");
  auto notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].recipients.size() == 1U && notifs[0].recipients[0] == 1U);
  CHECK(notifs[0].payload["type"] == "market_snapshot");
  CHECK(notifs[0].payload["match_id"] == match_id);
  CHECK(notifs[0].payload["state_version"] == 1U);
  CHECK(notifs[0].payload["phase"] == "Lobby");
  CHECK(notifs[0].payload["round"] == 0U);
  CHECK(notifs[0].payload["health"] == 5);
  CHECK(notifs[0].payload["price"] == 10);
  CHECK(notifs[0].payload["total_supply"] == 100U);
  CHECK(notifs[0].payload["reserve"] == 50U);
  CHECK(notifs[0].payload["shares_held_by_players"] == 50U);
  CHECK(notifs[0].payload["outcome"] == "None");

  // 2. Rejected transition does NOT emit notifications
  const auto early_start = coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}});
  CHECK(early_start && (*early_start)["type"] == "error");
  CHECK(coordinator.drain_notifications().empty());

  // Join remaining players
  for (std::uint64_t s = 2; s <= 5; ++s) {
    CHECK(join(coordinator, s, match_id)["type"] == "joined_match");
    auto join_notifs = coordinator.drain_notifications();
    CHECK(join_notifs.size() == 1U);
    CHECK(join_notifs[0].recipients.size() == 1U && join_notifs[0].recipients[0] == s);
    CHECK(join_notifs[0].payload["type"] == "market_snapshot");
    CHECK(join_notifs[0].payload["state_version"] == 1U);
  }

  // 3. Start match -> broadcasts match_started and market_snapshot to all 6 members (mod + 5 players)
  const auto started = coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}});
  CHECK(started && (*started)["type"] == "transition_result" && (*started)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  // Both notifications must target exactly the 6 members
  CHECK(notifs[0].recipients.size() == 6U);
  CHECK(notifs[0].payload["type"] == "server_event");
  CHECK(notifs[0].payload["event"] == "match_started");
  CHECK(notifs[0].payload["round"] == 1U);
  CHECK(notifs[0].payload["phase"] == "AwaitingEvent");
  CHECK(notifs[0].payload["state_version"] == 2U);

  CHECK(notifs[1].recipients.size() == 6U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["round"] == 1U);
  CHECK(notifs[1].payload["phase"] == "AwaitingEvent");
  CHECK(notifs[1].payload["state_version"] == 2U);

  // 4. Publish event -> broadcasts phase_started (Discussion) and market_snapshot
  const auto published = coordinator.handle(100, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}});
  CHECK(published && (*published)["type"] == "transition_result" && (*published)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["type"] == "server_event");
  CHECK(notifs[0].payload["event"] == "phase_started");
  CHECK(notifs[0].payload["phase"] == "Discussion");
  CHECK(notifs[0].payload["round"] == 1U);
  CHECK(notifs[0].payload["state_version"] == 3U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["phase"] == "Discussion");
  CHECK(notifs[1].payload["state_version"] == 3U);

  // 5. Open trading -> broadcasts phase_started (Trading) and market_snapshot
  const auto trading = coordinator.handle(100, json{{"type", "open_trading"}, {"match_id", match_id}});
  CHECK(trading && (*trading)["type"] == "transition_result" && (*trading)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["type"] == "server_event");
  CHECK(notifs[0].payload["event"] == "phase_started");
  CHECK(notifs[0].payload["phase"] == "Trading");
  CHECK(notifs[0].payload["state_version"] == 4U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["phase"] == "Trading");
  CHECK(notifs[1].payload["state_version"] == 4U);

  // 6. Trade submissions -> private, NO broadcast!
  const auto tr1 = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}});
  CHECK(tr1 && (*tr1)["type"] == "trade_result" && (*tr1)["ok"]);
  CHECK(coordinator.drain_notifications().empty());

  // 7. Resolve market -> broadcasts market_snapshot with last_market, phase_started (Clue), and phase_warning (30s)
  const auto mres = coordinator.handle(100, json{{"type", "resolve_market"}, {"match_id", match_id}});
  CHECK(mres && (*mres)["type"] == "market_result" && (*mres)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  CHECK(notifs[0].payload["type"] == "market_snapshot");
  CHECK(notifs[0].payload["phase"] == "Clue");
  CHECK(notifs[0].payload["state_version"] == 5U);
  CHECK(notifs[0].payload.contains("last_market"));
  CHECK(notifs[0].payload["last_market"]["executed_buys"] == 1U);
  CHECK(notifs[1].payload["type"] == "server_event");
  CHECK(notifs[1].payload["event"] == "phase_started");
  CHECK(notifs[1].payload["phase"] == "Clue");
  CHECK(notifs[1].payload["seconds_remaining"] == 30U);
  CHECK(notifs[1].payload["state_version"] == 5U);
  CHECK(notifs[2].payload["type"] == "server_event");
  CHECK(notifs[2].payload["event"] == "phase_warning");
  CHECK(notifs[2].payload["phase"] == "Clue");
  CHECK(notifs[2].payload["seconds_remaining"] == 30U);
  CHECK(notifs[2].payload["state_version"] == 5U);

  // 8. Open voting -> broadcasts phase_started (Voting) and market_snapshot
  const auto vopen = coordinator.handle(100, json{{"type", "open_voting"}, {"match_id", match_id}});
  CHECK(vopen && (*vopen)["type"] == "transition_result" && (*vopen)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["type"] == "server_event");
  CHECK(notifs[0].payload["event"] == "phase_started");
  CHECK(notifs[0].payload["phase"] == "Voting");
  CHECK(notifs[0].payload["state_version"] == 6U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["phase"] == "Voting");
  CHECK(notifs[1].payload["state_version"] == 6U);

  // 9. Resolve votes -> broadcasts vote_resolved, round_started (Round 2), and market_snapshot
  const auto vres = coordinator.handle(100, json{{"type", "resolve_votes"}, {"match_id", match_id}});
  CHECK(vres && (*vres)["type"] == "vote_resolution" && (*vres)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  CHECK(notifs[0].payload["type"] == "server_event");
  CHECK(notifs[0].payload["event"] == "vote_resolved");
  CHECK(notifs[0].payload["round"] == 1U);
  CHECK(notifs[0].payload["state_version"] == 7U);
  CHECK(notifs[1].payload["type"] == "server_event");
  CHECK(notifs[1].payload["event"] == "round_started");
  CHECK(notifs[1].payload["round"] == 2U);
  CHECK(notifs[1].payload["state_version"] == 7U);
  CHECK(notifs[2].payload["type"] == "market_snapshot");
  CHECK(notifs[2].payload["round"] == 2U);
  CHECK(notifs[2].payload["phase"] == "AwaitingEvent");
  CHECK(notifs[2].payload["state_version"] == 7U);

  // 10. Multi-match isolation test
  coordinator.register_session(200, SessionRole::Moderator);
  for (std::uint64_t session = 11; session <= 15; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }
  const auto created_b = create(coordinator, 200);
  const auto match_b = created_b["match_id"].get<std::uint64_t>();
  CHECK(match_b != match_id);
  for (std::uint64_t s = 11; s <= 15; ++s) {
    CHECK(join(coordinator, s, match_b)["type"] == "joined_match");
    auto b_join = coordinator.drain_notifications();
    CHECK(b_join.size() == 1U && b_join[0].recipients[0] == s);
    CHECK(b_join[0].payload["match_id"] == match_b);
  }
  const auto started_b = coordinator.handle(200, json{{"type", "start_match"}, {"match_id", match_b}});
  CHECK(started_b && (*started_b)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  // All recipients for match B must be 200 or 11..15, NONE from match A
  for (const auto& n : notifs) {
    CHECK(n.payload["match_id"] == match_b);
    for (const auto r : n.recipients) {
      CHECK(r == 200U || (r >= 11U && r <= 15U));
      CHECK(r != 100U && r > 5U);
    }
  }
}

void test_stage3_timed_gameplay_and_moderator_controls() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 5; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }

  const auto created = create(coordinator, 100);
  const auto match_id = created["match_id"].get<std::uint64_t>();
  for (std::uint64_t session = 1; session <= 5; ++session) {
    (void)coordinator.handle(session, json{{"type", "join_match"}, {"match_id", match_id}});
  }
  (void)coordinator.drain_notifications();

  auto t0 = std::chrono::steady_clock::now();
  coordinator.set_simulated_time(t0);

  // 1. Start match -> Round 1 AwaitingEvent
  const auto started = coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}});
  CHECK(started && (*started)["ok"]);
  (void)coordinator.drain_notifications();

  // 2. Publish event -> Discussion starts with 180s deadline
  const auto published = coordinator.handle(100, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}});
  CHECK(published && (*published)["ok"]);
  auto notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "phase_started");
  CHECK(notifs[0].payload["phase"] == "Discussion");
  CHECK(notifs[0].payload["seconds_remaining"] == 180U);

  // Tick before warning (e.g. at 60s elapsed = 120s remaining) -> no warnings, no advancement
  coordinator.tick(t0 + std::chrono::seconds(60));
  CHECK(coordinator.drain_notifications().empty());

  // Tick at 150s elapsed (30s remaining) -> warning 30s fires exactly once
  coordinator.tick(t0 + std::chrono::seconds(150));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 30U);
  CHECK(notifs[0].payload["phase"] == "Discussion");

  // Subsequent tick at 155s -> no duplicate 30s warning
  coordinator.tick(t0 + std::chrono::seconds(155));
  CHECK(coordinator.drain_notifications().empty());

  // Tick at 170s elapsed (10s remaining) -> warning 10s fires exactly once
  coordinator.tick(t0 + std::chrono::seconds(170));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 10U);
  CHECK(notifs[0].payload["phase"] == "Discussion");

  // Subsequent tick at 175s -> no duplicate 10s warning
  coordinator.tick(t0 + std::chrono::seconds(175));
  CHECK(coordinator.drain_notifications().empty());

  // Tick at 180s -> Discussion deadline expires -> auto transitions to Trading (120s)
  coordinator.tick(t0 + std::chrono::seconds(180));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "phase_started");
  CHECK(notifs[0].payload["phase"] == "Trading");
  CHECK(notifs[0].payload["seconds_remaining"] == 120U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["phase"] == "Trading");

  // 3. In Trading phase: test pause and resume
  auto t_trading = t0 + std::chrono::seconds(180);

  // Advance 70s in trading (50s remaining)
  coordinator.tick(t_trading + std::chrono::seconds(70));
  CHECK(coordinator.drain_notifications().empty());

  // Unauthorized pause attempt by player 1 -> rejected
  const auto bad_pause = coordinator.handle(1, json{{"type", "pause"}, {"match_id", match_id}});
  CHECK(bad_pause && (*bad_pause)["type"] == "error");

  // Moderator pauses match
  const auto pause_res = coordinator.handle(100, json{{"type", "pause"}, {"match_id", match_id}});
  CHECK(pause_res && (*pause_res)["type"] == "pause_result" && (*pause_res)["ok"]);
  CHECK((*pause_res)["seconds_remaining"] == 50U);

  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_paused");
  CHECK(notifs[0].payload["seconds_remaining"] == 50U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["is_paused"] == true);
  CHECK(notifs[1].payload["seconds_remaining"] == 50U);

  // Duplicate pause -> rejected
  const auto dup_pause = coordinator.handle(100, json{{"type", "pause"}, {"match_id", match_id}});
  CHECK(dup_pause && (*dup_pause)["type"] == "error");

  // Manual progression while paused -> rejected
  const auto bad_open = coordinator.handle(100, json{{"type", "resolve_market"}, {"match_id", match_id}});
  CHECK(bad_open && (*bad_open)["type"] == "error");

  // Advance clock by 100s while paused (well past original 120s deadline!)
  coordinator.tick(t_trading + std::chrono::seconds(170));
  // Nothing should fire while paused!
  CHECK(coordinator.drain_notifications().empty());

  // Status check while paused
  const auto paused_status = coordinator.handle(100, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(paused_status && (*paused_status)["is_paused"] == true);
  CHECK((*paused_status)["phase"] == "Trading");
  CHECK((*paused_status)["seconds_remaining"] == 50U);

  // During pause: Player 1 submits a BUY order (command in flight / during pause is accepted)
  const auto tr1 = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}});
  CHECK(tr1 && (*tr1)["type"] == "trade_result" && (*tr1)["ok"]);

  // Unauthorized resume by player -> rejected
  const auto bad_resume = coordinator.handle(1, json{{"type", "resume"}, {"match_id", match_id}});
  CHECK(bad_resume && (*bad_resume)["type"] == "error");

  // Moderator resumes match at t_resume = t_trading + 200s
  auto t_resume = t_trading + std::chrono::seconds(200);
  coordinator.set_simulated_time(t_resume);
  const auto resume_res = coordinator.handle(100, json{{"type", "resume"}, {"match_id", match_id}});
  CHECK(resume_res && (*resume_res)["type"] == "resume_result" && (*resume_res)["ok"]);
  CHECK((*resume_res)["seconds_remaining"] == 50U);

  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_resumed");
  CHECK(notifs[0].payload["seconds_remaining"] == 50U);
  CHECK(notifs[1].payload["is_paused"] == false);
  CHECK(notifs[1].payload["seconds_remaining"] == 50U);

  // Resume when not paused -> rejected
  const auto dup_resume = coordinator.handle(100, json{{"type", "resume"}, {"match_id", match_id}});
  CHECK(dup_resume && (*dup_resume)["type"] == "error");

  // Advance 20s (now 30s remaining) -> warning 30s fires
  coordinator.tick(t_resume + std::chrono::seconds(20));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 30U);

  // Advance 20s (now 10s remaining) -> warning 10s fires
  coordinator.tick(t_resume + std::chrono::seconds(40));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 10U);

  // Advance 10s (deadline expired!) -> auto resolves market to Clue (30s timed phase)
  coordinator.tick(t_resume + std::chrono::seconds(50));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  CHECK(notifs[0].payload["type"] == "market_snapshot");
  CHECK(notifs[0].payload.contains("last_market"));
  // Player 1 had 1 BUY, players 2-5 unsubmitted -> defaulted to hold! Executed buys = 1, executed sells = 0
  CHECK(notifs[0].payload["last_market"]["executed_buys"] == 1U);
  CHECK(notifs[0].payload["last_market"]["executed_sells"] == 0U);
  CHECK(notifs[1].payload["event"] == "phase_started");
  CHECK(notifs[1].payload["phase"] == "Clue");
  CHECK(notifs[1].payload["seconds_remaining"] == 30U);
  CHECK(notifs[2].payload["event"] == "phase_warning");
  CHECK(notifs[2].payload["phase"] == "Clue");
  CHECK(notifs[2].payload["seconds_remaining"] == 30U);

  // Late trade command after trading deadline -> rejected!
  const auto late_trade = coordinator.handle(2, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}});
  CHECK(late_trade && (*late_trade)["type"] == "operation_result" && !(*late_trade)["ok"]);

  // 4. Clue phase is active (30s): Investigator can submit clue privately
  std::uint64_t investigator_session = 0;
  for (std::uint64_t s = 1; s <= 5; ++s) {
    const uint32_t probe_target = (s == 1) ? 2U : 1U;
    const auto clue_try = coordinator.handle(s, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", probe_target}});
    if (clue_try && (*clue_try)["type"] == "clue_private") {
      investigator_session = s;
      break;
    }
  }
  CHECK(investigator_session != 0U);

  auto t_clue = t_resume + std::chrono::seconds(50);
  // Advance 20s in Clue (10s remaining) -> warning 10s fires
  coordinator.tick(t_clue + std::chrono::seconds(20));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 10U);
  CHECK(notifs[0].payload["phase"] == "Clue");

  // Advance 10s (total 30s in Clue) -> Clue deadline expires -> automatically starts transition interval (10s)
  coordinator.tick(t_clue + std::chrono::seconds(30));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "transition_interval");
  CHECK(notifs[0].payload["upcoming_phase"] == "Voting");
  CHECK(notifs[0].payload["seconds_remaining"] == 10U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");

  // During transition interval: clue request is rejected
  const auto late_clue = coordinator.handle(investigator_session, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", 2}});
  CHECK(late_clue && (*late_clue)["type"] == "error");

  // Advance 10s: transition interval expires -> automatically opens Voting (60s)
  coordinator.tick(t_clue + std::chrono::seconds(40));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "phase_started");
  CHECK(notifs[0].payload["phase"] == "Voting");
  CHECK(notifs[0].payload["seconds_remaining"] == 60U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");

  // 5. Voting phase (60s): test missing vote defaults to abstain
  auto t_voting = t_clue + std::chrono::seconds(40);
  // Player 1 votes to eliminate Player 2
  const auto v1 = coordinator.handle(1, json{{"type", "vote"}, {"match_id", match_id}, {"target_id", 2}});
  CHECK(v1 && (*v1)["type"] == "vote_result" && (*v1)["ok"]);

  // Players 2, 3, 4, 5 do NOT vote (missing vote -> abstain)
  // Advance 30s -> warning 30s fires
  coordinator.tick(t_voting + std::chrono::seconds(30));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 30U);

  // Advance 20s -> warning 10s fires
  coordinator.tick(t_voting + std::chrono::seconds(50));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 10U);

  // Advance 10s -> Voting deadline expires -> auto resolves votes!
  coordinator.tick(t_voting + std::chrono::seconds(60));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  CHECK(notifs[0].payload["event"] == "vote_resolved");
  CHECK(notifs[0].payload["votes_received"] == 0U);
  CHECK(notifs[0].payload["eligible_voters"] == 5U);
  // Player 2 had only 1 vote out of 5 eligible -> not strict majority (>2) -> eliminated is null!
  CHECK(notifs[0].payload["eliminated_player"].is_null());
  CHECK(notifs[1].payload["event"] == "round_started");
  CHECK(notifs[1].payload["round"] == 2U);
  CHECK(notifs[1].payload["phase"] == "AwaitingEvent");
  CHECK(notifs[2].payload["type"] == "market_snapshot");
  CHECK(notifs[2].payload["round"] == 2U);

  // 6. Abort testing: in Round 2 AwaitingEvent, publish event to enter Discussion
  auto t_r2 = t_voting + std::chrono::seconds(60);
  coordinator.set_simulated_time(t_r2);
  const auto pub2 = coordinator.handle(100, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}});
  CHECK(pub2 && (*pub2)["ok"]);
  (void)coordinator.drain_notifications();

  // Unauthorized abort attempt by player -> rejected
  const auto bad_abort = coordinator.handle(1, json{{"type", "abort"}, {"match_id", match_id}});
  CHECK(bad_abort && (*bad_abort)["type"] == "error");

  // Moderator aborts match
  const auto abort_res = coordinator.handle(100, json{{"type", "abort"}, {"match_id", match_id}});
  CHECK(abort_res && (*abort_res)["type"] == "abort_result" && (*abort_res)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_aborted");
  CHECK(notifs[0].payload["phase"] == "Finished");
  CHECK(notifs[0].payload["outcome"] == "Aborted");
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["phase"] == "Finished");
  CHECK(notifs[1].payload["outcome"] == "Aborted");

  // After abort: any subsequent command is rejected
  const auto post_trade = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}});
  CHECK(post_trade && (*post_trade)["type"] == "error");
  const auto post_vote = coordinator.handle(1, json{{"type", "vote"}, {"match_id", match_id}, {"target_id", 2}});
  CHECK(post_vote && (*post_vote)["type"] == "error");
  const auto post_pause = coordinator.handle(100, json{{"type", "pause"}, {"match_id", match_id}});
  CHECK(post_pause && (*post_pause)["type"] == "error");
  const auto post_resume = coordinator.handle(100, json{{"type", "resume"}, {"match_id", match_id}});
  CHECK(post_resume && (*post_resume)["type"] == "error");
  const auto post_abort = coordinator.handle(100, json{{"type", "abort"}, {"match_id", match_id}});
  CHECK(post_abort && (*post_abort)["type"] == "error");

  // Ticks do nothing on aborted match
  coordinator.tick(t_r2 + std::chrono::seconds(500));
  CHECK(coordinator.drain_notifications().empty());

  // 7. Multi-match isolation test for timers and pause
  MatchCoordinator coord2;
  coord2.register_session(101, SessionRole::Moderator);
  coord2.register_session(102, SessionRole::Moderator);
  for (std::uint64_t s = 1; s <= 10; ++s) coord2.register_session(s, SessionRole::Player);

  const auto m1_info = create(coord2, 101);
  CHECK(m1_info.contains("match_id"));
  const auto m1 = m1_info["match_id"].get<std::uint64_t>();
  for (std::uint64_t s = 1; s <= 5; ++s) {
    (void)coord2.handle(s, json{{"type", "join_match"}, {"match_id", m1}});
  }
  const auto m2_info = create(coord2, 102);
  CHECK(m2_info.contains("match_id"));
  const auto m2 = m2_info["match_id"].get<std::uint64_t>();
  for (std::uint64_t s = 6; s <= 10; ++s) {
    (void)coord2.handle(s, json{{"type", "join_match"}, {"match_id", m2}});
  }
  (void)coord2.drain_notifications();

  auto t_multi = std::chrono::steady_clock::now();
  coord2.set_simulated_time(t_multi);

  (void)coord2.handle(101, json{{"type", "start_match"}, {"match_id", m1}});
  (void)coord2.handle(102, json{{"type", "start_match"}, {"match_id", m2}});
  (void)coord2.handle(101, json{{"type", "publish_event"}, {"match_id", m1}, {"impact", 0}});
  (void)coord2.handle(102, json{{"type", "publish_event"}, {"match_id", m2}, {"impact", 0}});
  (void)coord2.drain_notifications();

  // Pause Match 1 at t_multi + 50s
  coord2.tick(t_multi + std::chrono::seconds(50));
  const auto p1 = coord2.handle(101, json{{"type", "pause"}, {"match_id", m1}});
  CHECK(p1 && (*p1)["ok"]);
  (void)coord2.drain_notifications();

  // Match 2 is NOT paused!
  const auto st2 = coord2.handle(102, json{{"type", "match_status"}, {"match_id", m2}});
  CHECK(st2 && (*st2)["is_paused"] == false);

  // Advance time past 180s for Match 2 -> Match 2 transitions to Trading, Match 1 stays paused in Discussion!
  coord2.tick(t_multi + std::chrono::seconds(180));
  const auto st1_check = coord2.handle(101, json{{"type", "match_status"}, {"match_id", m1}});
  CHECK(st1_check && (*st1_check)["phase"] == "Discussion" && (*st1_check)["is_paused"] == true);

  const auto st2_check = coord2.handle(102, json{{"type", "match_status"}, {"match_id", m2}});
  CHECK(st2_check && (*st2_check)["phase"] == "Trading" && (*st2_check)["is_paused"] == false);
}

void test_canonical_clue_phase_and_investigator_window() {
  MatchCoordinator coordinator;
  // 1. Verify configured default Clue duration is exactly 30 seconds
  CHECK(coordinator.phase_durations().clue == std::chrono::seconds(30));
  CHECK(coordinator.phase_durations().transition == std::chrono::seconds(10));
  CHECK(coordinator.phase_durations().voting == std::chrono::seconds(60));

  coordinator.register_session(100, SessionRole::Moderator);
  for (std::uint64_t session = 1; session <= 5; ++session) {
    coordinator.register_session(session, SessionRole::Player);
  }

  const auto created = create(coordinator, 100);
  const auto match_id = created["match_id"].get<std::uint64_t>();
  std::vector<std::pair<std::uint64_t, std::string>> player_roles;
  for (std::uint64_t session = 1; session <= 5; ++session) {
    const auto j = coordinator.handle(session, json{{"type", "join_match"}, {"match_id", match_id}});
    CHECK(j && (*j)["type"] == "joined_match");
    player_roles.emplace_back(session, (*j)["private_role"].get<std::string>());
  }
  (void)coordinator.drain_notifications();

  auto t0 = std::chrono::steady_clock::now();
  coordinator.set_simulated_time(t0);

  // Start -> AwaitingEvent
  (void)coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}});
  // Publish -> Discussion (180s)
  (void)coordinator.handle(100, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}});
  (void)coordinator.drain_notifications();

  // Advance 180s -> Discussion deadline expires -> Trading opens (120s)
  coordinator.tick(t0 + std::chrono::seconds(180));
  (void)coordinator.drain_notifications();

  auto t_trading = t0 + std::chrono::seconds(180);
  // Players submit trades
  (void)coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}});
  (void)coordinator.handle(2, json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}});

  // 2. Advance 120s -> Trading deadline expires -> Resolves market to independent Clue phase (30s)
  coordinator.tick(t_trading + std::chrono::seconds(120));
  auto notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  // 17. Notification order: 1) Market resolution and public snapshot, 2) Clue phase start, 3) Clue 30s warning
  CHECK(notifs[0].payload["type"] == "market_snapshot");
  CHECK(notifs[0].payload["phase"] == "Clue");
  CHECK(notifs[0].payload.contains("last_market"));
  CHECK(notifs[0].payload["last_market"]["executed_buys"] == 1U);
  CHECK(notifs[0].payload["seconds_remaining"] == 30U);

  // 2. Clue phase start
  CHECK(notifs[1].payload["type"] == "server_event");
  CHECK(notifs[1].payload["event"] == "phase_started");
  CHECK(notifs[1].payload["phase"] == "Clue");
  CHECK(notifs[1].payload["seconds_remaining"] == 30U);

  // 3. Clue 30-second warning appears once at phase entry
  CHECK(notifs[2].payload["type"] == "server_event");
  CHECK(notifs[2].payload["event"] == "phase_warning");
  CHECK(notifs[2].payload["phase"] == "Clue");
  CHECK(notifs[2].payload["seconds_remaining"] == 30U);

  // Subsequent tick at 5s elapsed (25s remaining) -> no duplicate 30s warning
  auto t_clue = t_trading + std::chrono::seconds(120);
  coordinator.tick(t_clue + std::chrono::seconds(5));
  CHECK(coordinator.drain_notifications().empty());

  // Late trade command during Clue is rejected
  const auto late_tr = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}});
  CHECK(late_tr && (*late_tr)["type"] == "operation_result" && !(*late_tr)["ok"]);

  // Identify roles
  std::uint64_t inv_s = 0;
  std::uint64_t non_inv_s = 0;
  for (const auto& [s, role] : player_roles) {
    if (role == "Investigator") inv_s = s;
    else if (non_inv_s == 0) non_inv_s = s;
  }
  CHECK(inv_s != 0U);
  CHECK(non_inv_s != 0U);

  // 6. Valid Investigator can request a clue during Clue, and the result remains private
  const auto bad_clue = coordinator.handle(non_inv_s, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", 2}});
  CHECK(bad_clue && (*bad_clue)["type"] == "operation_result" && !(*bad_clue)["ok"]);

  // Target = self -> rejected
  const auto self_clue = coordinator.handle(inv_s, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", inv_s}});
  CHECK(self_clue && (*self_clue)["type"] == "operation_result" && !(*self_clue)["ok"]);

  // Valid clue target (player other than investigator)
  const uint32_t target_pid = (inv_s == 1) ? 2U : 1U;
  const auto clue_res1 = coordinator.handle(inv_s, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", target_pid}});
  CHECK(clue_res1 && (*clue_res1)["type"] == "clue_private");
  CHECK(clue_res1->contains("is_insider"));
  const bool first_answer = (*clue_res1)["is_insider"].get<bool>();

  // Same-target retry by same investigator -> replayed with same answer
  const auto clue_retry = coordinator.handle(inv_s, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", target_pid}});
  CHECK(clue_retry && (*clue_retry)["type"] == "clue_private");
  CHECK((*clue_retry)["is_insider"] == first_answer);

  // Different target after clue already used -> rejected
  const uint32_t other_target = (target_pid == 2U) ? 3U : 2U;
  const auto diff_target = coordinator.handle(inv_s, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", other_target}});
  CHECK(diff_target && (*diff_target)["type"] == "operation_result" && !(*diff_target)["ok"]);

  // Verify public status and notifications have zero clue leaks
  CHECK(coordinator.drain_notifications().empty());
  const auto pub_st = coordinator.handle(non_inv_s, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(pub_st && !pub_st->contains("clue_result") && !pub_st->contains("clue_target"));

  // 13. Pause/resume during Clue preserves exact remaining duration
  // Pause at 10s elapsed in Clue (20s remaining)
  coordinator.tick(t_clue + std::chrono::seconds(10));
  const auto p_clue = coordinator.handle(100, json{{"type", "pause"}, {"match_id", match_id}});
  CHECK(p_clue && (*p_clue)["ok"] && (*p_clue)["seconds_remaining"] == 20U);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_paused");
  CHECK(notifs[0].payload["seconds_remaining"] == 20U);
  CHECK(notifs[1].payload["is_paused"] == true);

  // Advance 50s while paused -> no deadline expiration
  coordinator.tick(t_clue + std::chrono::seconds(60));
  CHECK(coordinator.drain_notifications().empty());

  // Resume at t_clue + 80s
  auto t_clue_res = t_clue + std::chrono::seconds(80);
  coordinator.set_simulated_time(t_clue_res);
  const auto r_clue = coordinator.handle(100, json{{"type", "resume"}, {"match_id", match_id}});
  CHECK(r_clue && (*r_clue)["ok"] && (*r_clue)["seconds_remaining"] == 20U);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_resumed");
  CHECK(notifs[0].payload["seconds_remaining"] == 20U);
  CHECK(notifs[1].payload["is_paused"] == false);

  // 4. Clue 10-second warning appears once at correct deadline (after 10s more = 10s remaining)
  coordinator.tick(t_clue_res + std::chrono::seconds(10));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["phase"] == "Clue");
  CHECK(notifs[0].payload["seconds_remaining"] == 10U);

  // 5. Clue remains active immediately before deadline (at 19s elapsed after resume = 1s remaining)
  coordinator.tick(t_clue_res + std::chrono::seconds(19));
  CHECK(coordinator.drain_notifications().empty());
  const auto st_imm = coordinator.handle(100, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(st_imm && (*st_imm)["phase"] == "Clue" && (*st_imm)["seconds_remaining"] == 1U);

  // 8. At Clue expiry (20s after resume = 30s Clue total), coordinator enters distinct 10-second transition interval and does NOT open Voting immediately
  coordinator.tick(t_clue_res + std::chrono::seconds(20));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  // 5. Transition interval announcement
  CHECK(notifs[0].payload["event"] == "transition_interval");
  CHECK(notifs[0].payload["upcoming_phase"] == "Voting");
  CHECK(notifs[0].payload["seconds_remaining"] == 10U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");

  // 7. Clue requests are rejected once the transition interval begins
  const auto late_clue_in_trans = coordinator.handle(inv_s, json{{"type", "request_clue"}, {"match_id", match_id}, {"target_id", 2}});
  CHECK(late_clue_in_trans && (*late_clue_in_trans)["type"] == "error");

  // 9. Voting does not start before the transition deadline (at 5s into transition)
  auto t_trans = t_clue_res + std::chrono::seconds(20);
  coordinator.tick(t_trans + std::chrono::seconds(5));
  CHECK(coordinator.drain_notifications().empty());
  const auto st_trans = coordinator.handle(100, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(st_trans && (*st_trans)["seconds_remaining"] == 5U);

  // Voting command during transition interval is rejected
  const auto premature_vote = coordinator.handle(1, json{{"type", "vote"}, {"match_id", match_id}, {"target_id", 2}});
  CHECK(premature_vote && (*premature_vote)["type"] == "operation_result" && !(*premature_vote)["ok"]);

  // 14. Pause/resume during transition interval preserves exact remaining duration
  const auto p_trans = coordinator.handle(100, json{{"type", "pause"}, {"match_id", match_id}});
  CHECK(p_trans && (*p_trans)["ok"] && (*p_trans)["seconds_remaining"] == 5U);
  (void)coordinator.drain_notifications();

  auto t_trans_res = t_trans + std::chrono::seconds(30);
  coordinator.set_simulated_time(t_trans_res);
  const auto r_trans = coordinator.handle(100, json{{"type", "resume"}, {"match_id", match_id}});
  CHECK(r_trans && (*r_trans)["ok"] && (*r_trans)["seconds_remaining"] == 5U);
  (void)coordinator.drain_notifications();

  // Duplicate open_voting with with_transition during transition interval -> rejected
  const auto dup_open_trans = coordinator.handle(100, json{{"type", "open_voting"}, {"match_id", match_id}, {"with_transition", true}});
  CHECK(dup_open_trans && (*dup_open_trans)["type"] == "error");

  // 10. At transition expiry (5s after resume), Voting starts exactly once with a 60-second deadline
  coordinator.tick(t_trans_res + std::chrono::seconds(5));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  // 6. Voting phase start and updated public snapshot
  CHECK(notifs[0].payload["event"] == "phase_started");
  CHECK(notifs[0].payload["phase"] == "Voting");
  CHECK(notifs[0].payload["seconds_remaining"] == 60U);
  CHECK(notifs[1].payload["type"] == "market_snapshot");
  CHECK(notifs[1].payload["phase"] == "Voting");

  // 12. Duplicate ticks do not duplicate transitions or events
  coordinator.tick(t_trans_res + std::chrono::seconds(5));
  CHECK(coordinator.drain_notifications().empty());

  // 11. Missing votes default to abstention; voting resolution occurs at 60s
  auto t_voting = t_trans_res + std::chrono::seconds(5);
  // Only player 1 votes, players 2-5 unsubmitted (abstain)
  (void)coordinator.handle(1, json{{"type", "vote"}, {"match_id", match_id}, {"target_id", 2}});
  (void)coordinator.drain_notifications();

  coordinator.tick(t_voting + std::chrono::seconds(60));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  CHECK(notifs[0].payload["event"] == "vote_resolved");
  CHECK(notifs[1].payload["event"] == "round_started");
  CHECK(notifs[2].payload["type"] == "market_snapshot");

  // 15. Abort during Clue / transition prevents all subsequent automatic transitions
  MatchCoordinator coord_abort;
  auto t_ab0 = std::chrono::steady_clock::now();
  coord_abort.set_simulated_time(t_ab0);
  coord_abort.register_session(10, SessionRole::Moderator);
  for (std::uint64_t s = 1; s <= 5; ++s) coord_abort.register_session(s, SessionRole::Player);
  const auto m_ab = create(coord_abort, 10)["match_id"].get<std::uint64_t>();
  for (std::uint64_t s = 1; s <= 5; ++s) (void)coord_abort.handle(s, json{{"type", "join_match"}, {"match_id", m_ab}});
  (void)coord_abort.handle(10, json{{"type", "start_match"}, {"match_id", m_ab}});
  (void)coord_abort.handle(10, json{{"type", "publish_event"}, {"match_id", m_ab}, {"impact", 0}});
  coord_abort.tick(t_ab0 + std::chrono::seconds(180)); // Discussion -> Trading
  coord_abort.tick(t_ab0 + std::chrono::seconds(300)); // Trading -> Clue
  (void)coord_abort.drain_notifications();

  // Abort during Clue
  const auto ab_res = coord_abort.handle(10, json{{"type", "abort"}, {"match_id", m_ab}});
  CHECK(ab_res && (*ab_res)["ok"]);
  (void)coord_abort.drain_notifications();

  // Advance 100s past Clue and transition deadlines
  coord_abort.tick(t_ab0 + std::chrono::seconds(450));
  // Nothing should fire, no transitions to Voting, no vote resolution
  CHECK(coord_abort.drain_notifications().empty());
  const auto ab_st = coord_abort.handle(10, json{{"type", "match_status"}, {"match_id", m_ab}});
  CHECK(ab_st && (*ab_st)["phase"] == "Finished" && (*ab_st)["outcome"] == "Aborted");

  // 16. Delayed processing: 10s warning ordered before transition entry when both observable in one tick
  MatchCoordinator coord_delay;
  auto t_del0 = std::chrono::steady_clock::now();
  coord_delay.set_simulated_time(t_del0);
  coord_delay.register_session(20, SessionRole::Moderator);
  for (std::uint64_t s = 1; s <= 5; ++s) coord_delay.register_session(s, SessionRole::Player);
  const auto m_del = create(coord_delay, 20)["match_id"].get<std::uint64_t>();
  for (std::uint64_t s = 1; s <= 5; ++s) (void)coord_delay.handle(s, json{{"type", "join_match"}, {"match_id", m_del}});
  (void)coord_delay.handle(20, json{{"type", "start_match"}, {"match_id", m_del}});
  (void)coord_delay.handle(20, json{{"type", "publish_event"}, {"match_id", m_del}, {"impact", 0}});
  coord_delay.tick(t_del0 + std::chrono::seconds(180)); // Discussion -> Trading
  coord_delay.tick(t_del0 + std::chrono::seconds(300)); // Trading -> Clue
  (void)coord_delay.drain_notifications();

  // Single delayed tick jumps past Clue deadline directly (at 35s into Clue, without ticking at 20s)
  coord_delay.tick(t_del0 + std::chrono::seconds(335));
  auto del_notifs = coord_delay.drain_notifications();
  CHECK(del_notifs.size() == 3U);
  if (del_notifs.size() == 3U) {
    // Must order 10s warning BEFORE transition entry
    CHECK(del_notifs[0].payload["event"] == "phase_warning");
    CHECK(del_notifs[0].payload["phase"] == "Clue");
    CHECK(del_notifs[0].payload["seconds_remaining"] == 10U);
    CHECK(del_notifs[1].payload["event"] == "transition_interval");
    CHECK(del_notifs[1].payload["upcoming_phase"] == "Voting");
    CHECK(del_notifs[1].payload["seconds_remaining"] == 10U);
    CHECK(del_notifs[2].payload["type"] == "market_snapshot");
  }
}

void test_stage3_state_version_and_notification_ordering() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  for (std::uint64_t s = 1; s <= 5; ++s) coordinator.register_session(s, SessionRole::Player);

  // Match created -> version 1
  const auto created = create(coordinator, 100);
  const auto match_id = created["match_id"].get<std::uint64_t>();
  for (std::uint64_t s = 1; s <= 5; ++s) {
    (void)coordinator.handle(s, json{{"type", "join_match"}, {"match_id", match_id}});
  }
  auto notifs = coordinator.drain_notifications();
  // All initial join snapshots must have state_version == 1
  for (const auto& n : notifs) {
    CHECK(n.payload["state_version"] == 1U);
  }

  auto t0 = std::chrono::steady_clock::now();
  coordinator.set_simulated_time(t0);

  // 1. start_match: version 1 -> 2
  const auto st_res = coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}});
  CHECK(st_res && (*st_res)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_started" && notifs[0].payload["state_version"] == 2U);
  CHECK(notifs[1].payload["type"] == "market_snapshot" && notifs[1].payload["state_version"] == 2U);

  // 2. publish_event: version 2 -> 3
  const auto pub_res = coordinator.handle(100, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}});
  CHECK(pub_res && (*pub_res)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "phase_started" && notifs[0].payload["state_version"] == 3U);
  CHECK(notifs[1].payload["type"] == "market_snapshot" && notifs[1].payload["state_version"] == 3U);

  // 3. Discussion timer ticks (warnings): do NOT increment version
  coordinator.tick(t0 + std::chrono::seconds(150));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 1U);
  CHECK(notifs[0].payload["event"] == "phase_warning");
  CHECK(notifs[0].payload["seconds_remaining"] == 30U);
  CHECK(notifs[0].payload["state_version"] == 3U); // Unchanged!

  // 4. Discussion deadline expires: Discussion -> Trading (version 3 -> 4)
  coordinator.tick(t0 + std::chrono::seconds(180));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "phase_started" && notifs[0].payload["phase"] == "Trading");
  CHECK(notifs[0].payload["state_version"] == 4U);
  CHECK(notifs[1].payload["type"] == "market_snapshot" && notifs[1].payload["state_version"] == 4U);

  auto t_tr = t0 + std::chrono::seconds(180);
  // 5. Pause: version 4 -> 5
  coordinator.tick(t_tr + std::chrono::seconds(10));
  const auto p_res = coordinator.handle(100, json{{"type", "pause"}, {"match_id", match_id}});
  CHECK(p_res && (*p_res)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_paused" && notifs[0].payload["state_version"] == 5U);
  CHECK(notifs[1].payload["type"] == "market_snapshot" && notifs[1].payload["state_version"] == 5U);

  // 6. Resume: version 5 -> 6
  auto t_res = t_tr + std::chrono::seconds(50);
  coordinator.set_simulated_time(t_res);
  const auto r_res = coordinator.handle(100, json{{"type", "resume"}, {"match_id", match_id}});
  CHECK(r_res && (*r_res)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_resumed" && notifs[0].payload["state_version"] == 6U);
  CHECK(notifs[1].payload["type"] == "market_snapshot" && notifs[1].payload["state_version"] == 6U);

  // 7. Trading deadline expires: Trading -> Clue (version 6 -> 7)
  coordinator.tick(t_res + std::chrono::seconds(110));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  CHECK(notifs[0].payload["type"] == "market_snapshot" && notifs[0].payload["state_version"] == 7U);
  CHECK(notifs[1].payload["event"] == "phase_started" && notifs[1].payload["phase"] == "Clue");
  CHECK(notifs[1].payload["state_version"] == 7U);
  CHECK(notifs[2].payload["event"] == "phase_warning" && notifs[2].payload["seconds_remaining"] == 30U);
  CHECK(notifs[2].payload["state_version"] == 7U);

  // 8. open_voting: Clue -> Voting (version 7 -> 8)
  auto t_v = t_res + std::chrono::seconds(110);
  coordinator.set_simulated_time(t_v);
  const auto ov = coordinator.handle(100, json{{"type", "open_voting"}, {"match_id", match_id}});
  CHECK(ov && (*ov)["phase"] == "Voting");
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "phase_started" && notifs[0].payload["state_version"] == 8U);
  CHECK(notifs[1].payload["type"] == "market_snapshot" && notifs[1].payload["state_version"] == 8U);

  // 9. Voting deadline expires: Voting resolves (version 8 -> 9)
  coordinator.tick(t_v + std::chrono::seconds(60));
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 3U);
  CHECK(notifs[0].payload["event"] == "vote_resolved" && notifs[0].payload["state_version"] == 9U);
  CHECK(notifs[1].payload["event"] == "round_started" && notifs[1].payload["state_version"] == 9U);
  CHECK(notifs[2].payload["type"] == "market_snapshot" && notifs[2].payload["state_version"] == 9U);

  // 10. Abort in Round 2: version 9 -> 10
  const auto ab_res = coordinator.handle(100, json{{"type", "abort"}, {"match_id", match_id}});
  CHECK(ab_res && (*ab_res)["ok"]);
  notifs = coordinator.drain_notifications();
  CHECK(notifs.size() == 2U);
  CHECK(notifs[0].payload["event"] == "match_aborted" && notifs[0].payload["state_version"] == 10U);
  CHECK(notifs[1].payload["type"] == "market_snapshot" && notifs[1].payload["state_version"] == 10U);

  // After abort: no mutation, rejected commands don't increment version
  const auto post_cmd = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}});
  CHECK(post_cmd && (*post_cmd)["type"] == "error");
  const auto final_st = coordinator.handle(100, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(final_st && (*final_st)["state_version"] == 10U);
}

void test_stage4_coordinator_quantity_and_research() {
  MatchCoordinator coordinator;
  coordinator.register_session(100, SessionRole::Moderator);
  for (std::uint64_t s = 1; s <= 5; ++s) coordinator.register_session(s, SessionRole::Player);

  const auto created = coordinator.handle(100, json{{"type", "create_match"}, {"player_count", 5}, {"open_lobby", true}});
  CHECK(created && (*created)["type"] == "match_created");
  const auto match_id = (*created)["match_id"].get<std::uint64_t>();

  for (std::uint64_t s = 1; s <= 5; ++s) {
    const auto joined = coordinator.handle(s, json{{"type", "join_match"}, {"match_id", match_id}});
    CHECK(joined && (*joined)["type"] == "joined_match");
  }
  (void)coordinator.drain_notifications();

  CHECK(coordinator.handle(100, json{{"type", "start_match"}, {"match_id", match_id}}));
  (void)coordinator.drain_notifications();

  // 1. Pre-event research attempt -> rejected (not in Discussion phase)
  const auto pre_event_res = coordinator.handle(1, json{{"type", "research"}, {"match_id", match_id}});
  CHECK(pre_event_res && (*pre_event_res)["type"] == "operation_result" && !(*pre_event_res)["ok"]);
  CHECK((*pre_event_res)["code"] == "WrongPhase");

  // Publish event (+1) -> enters Discussion
  CHECK(coordinator.handle(100, json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 1}}));
  (void)coordinator.drain_notifications();

  // 2. Research action during Discussion
  // Moderator cannot research
  const auto mod_res = coordinator.handle(100, json{{"type", "research"}, {"match_id", match_id}});
  CHECK(mod_res && (*mod_res)["type"] == "error");

  // Valid research by Player 1
  const auto p1_res = coordinator.handle(1, json{{"type", "research"}, {"match_id", match_id}});
  CHECK(p1_res && (*p1_res)["type"] == "research_result" && (*p1_res)["ok"]);
  CHECK((*p1_res)["direction"] == "Positive"); // coarse signal
  CHECK((*p1_res)["already_used"] == false);
  CHECK((*p1_res)["credits_remaining"] == 90);

  // PRIVACY CHECK: research action produces NO notifications for other players
  const auto drained = coordinator.drain_notifications();
  CHECK(drained.empty());

  // Idempotent retry: reports already_used, does not charge twice
  const auto p1_retry = coordinator.handle(1, json{{"type", "scout"}, {"match_id", match_id}});
  CHECK(p1_retry && (*p1_retry)["type"] == "research_result" && (*p1_retry)["ok"]);
  CHECK((*p1_retry)["direction"] == "Positive");
  CHECK((*p1_retry)["already_used"] == true);
  CHECK((*p1_retry)["credits_remaining"] == 90);
  CHECK(coordinator.drain_notifications().empty());

  // Open trading
  CHECK(coordinator.handle(100, json{{"type", "open_trading"}, {"match_id", match_id}}));
  (void)coordinator.drain_notifications();

  // Research during trading -> rejected
  const auto p2_late_res = coordinator.handle(2, json{{"type", "research"}, {"match_id", match_id}});
  CHECK(p2_late_res && (*p2_late_res)["type"] == "operation_result" && !(*p2_late_res)["ok"]);
  CHECK((*p2_late_res)["code"] == "WrongPhase");

  // 3. Quantity trading validation
  // Invalid zero quantity
  const auto zero_qty = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}, {"quantity", 0}});
  CHECK(zero_qty && (*zero_qty)["type"] == "error");

  // Invalid negative quantity
  const auto neg_qty = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}, {"quantity", -3}});
  CHECK(neg_qty && (*neg_qty)["type"] == "error");

  // Invalid string quantity
  const auto str_qty = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}, {"quantity", "five"}});
  CHECK(str_qty && (*str_qty)["type"] == "error");

  // Valid quantity trade
  const auto valid_buy = coordinator.handle(1, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}, {"quantity", 3}});
  CHECK(valid_buy && (*valid_buy)["type"] == "trade_result" && (*valid_buy)["ok"]);
  CHECK((*valid_buy)["quantity"] == 3U);

  // Single share backward compatibility (omitted quantity defaults to 1)
  const auto single_buy = coordinator.handle(2, json{{"type", "trade"}, {"match_id", match_id}, {"action", "BUY"}});
  CHECK(single_buy && (*single_buy)["type"] == "trade_result" && (*single_buy)["ok"]);
  CHECK((*single_buy)["quantity"] == 1U);

  // Player 3 sells with quantity
  const auto valid_sell = coordinator.handle(3, json{{"type", "trade"}, {"match_id", match_id}, {"action", "SELL"}, {"quantity", 2}});
  CHECK(valid_sell && (*valid_sell)["type"] == "trade_result" && (*valid_sell)["ok"]);
  CHECK((*valid_sell)["quantity"] == 2U);

  // 4. Market resolution and public indicators verification
  const auto market_res = coordinator.handle(100, json{{"type", "resolve_market"}, {"match_id", match_id}});
  CHECK(market_res && (*market_res)["type"] == "market_result" && (*market_res)["ok"]);
  CHECK((*market_res)["executed_buys"] == 4U);
  CHECK((*market_res)["executed_sells"] == 2U);

  // Snapshot contains public sentiment, volatility, liquidity
  auto notifs = coordinator.drain_notifications();
  CHECK(!notifs.empty());
  CHECK(notifs[0].payload["type"] == "market_snapshot");
  CHECK(notifs[0].payload.contains("sentiment"));
  CHECK(notifs[0].payload.contains("volatility"));
  CHECK(notifs[0].payload.contains("liquidity"));
  CHECK(notifs[0].payload.contains("last_market"));
  CHECK(notifs[0].payload["last_market"].contains("sentiment"));
  CHECK(notifs[0].payload["last_market"].contains("volatility"));
  CHECK(notifs[0].payload["last_market"].contains("liquidity"));

  // Match status also contains public indicators
  const auto status = coordinator.handle(100, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(status && status->contains("sentiment") && status->contains("volatility") && status->contains("liquidity"));
}

void test_player_admission_security_and_identity() {
  MatchCoordinator coordinator;
  // Register moderator session 100
  coordinator.register_session(100, SessionRole::Moderator);
  // Register player sessions 1..5
  for (std::uint64_t s = 1; s <= 5; ++s) {
    coordinator.register_session(s, SessionRole::Player);
  }

  // 1. Moderator creates match with admission token
  const auto created = coordinator.handle(100, json{
    {"type", "create_match"},
    {"player_count", 5},
    {"join_token", "protected-token-abc"}
  });
  CHECK(created && (*created)["type"] == "match_created");
  const auto match_id = (*created)["match_id"].get<std::uint64_t>();
  CHECK((*created)["join_token"] == "protected-token-abc");

  // 2. Player joins without join token -> rejected
  const auto no_token = coordinator.handle(1, json{
    {"type", "join_match"},
    {"match_id", match_id}
  });
  CHECK(no_token && (*no_token)["type"] == "error");
  CHECK((*no_token)["error"] == "invalid or missing match join token");

  // 3. Player joins with wrong join token -> rejected
  const auto bad_token = coordinator.handle(1, json{
    {"type", "join_match"},
    {"match_id", match_id},
    {"join_token", "invalid-token-xyz"}
  });
  CHECK(bad_token && (*bad_token)["type"] == "error");
  CHECK((*bad_token)["error"] == "invalid or missing match join token");

  // 4. Player attempts to spoof seat or role -> rejected
  const auto spoof_seat = coordinator.handle(1, json{
    {"type", "join_match"},
    {"match_id", match_id},
    {"join_token", "protected-token-abc"},
    {"seat", 1}
  });
  CHECK(spoof_seat && (*spoof_seat)["type"] == "error");
  CHECK((*spoof_seat)["error"] == "client identity fields are not accepted");

  const auto spoof_role = coordinator.handle(1, json{
    {"type", "join_match"},
    {"match_id", match_id},
    {"join_token", "protected-token-abc"},
    {"role", "Insider"}
  });
  CHECK(spoof_role && (*spoof_role)["type"] == "error");
  CHECK((*spoof_role)["error"] == "client identity fields are not accepted");

  // 5. Authorized player joins with valid token -> accepted
  const auto valid_join = coordinator.handle(1, json{
    {"type", "join_match"},
    {"match_id", match_id},
    {"join_token", "protected-token-abc"}
  });
  CHECK(valid_join && (*valid_join)["type"] == "joined_match");
  CHECK((*valid_join)["player_id"] == 1U);
  CHECK((*valid_join)["seat"] == 1U);
  CHECK(valid_join->contains("private_role"));

  // 6. Impersonation / seat bypass test
  // Cannot join a second time from same session
  const auto rejoin = coordinator.handle(1, json{
    {"type", "join_match"},
    {"match_id", match_id},
    {"join_token", "protected-token-abc"}
  });
  CHECK(rejoin && (*rejoin)["type"] == "error");
  CHECK((*rejoin)["error"] == "connection already belongs to a match");

  // 7. Moderator / player separation
  // Moderator cannot join as player
  const auto mod_join = coordinator.handle(100, json{
    {"type", "join_match"},
    {"match_id", match_id},
    {"join_token", "protected-token-abc"}
  });
  CHECK(mod_join && (*mod_join)["type"] == "error");
  CHECK((*mod_join)["error"] == "player permission required");

  // Player cannot invoke moderator commands
  const auto player_start = coordinator.handle(1, json{
    {"type", "start_match"},
    {"match_id", match_id}
  });
  CHECK(player_start && (*player_start)["type"] == "error");
  CHECK((*player_start)["error"] == "creating moderator permission required");

  // 8. Join remaining players
  for (std::uint64_t s = 2; s <= 5; ++s) {
    const auto j = coordinator.handle(s, json{
      {"type", "join_match"},
      {"match_id", match_id},
      {"join_token", "protected-token-abc"}
    });
    CHECK(j && (*j)["type"] == "joined_match");
  }

  // 9. Privacy checks: public status and snapshots do NOT leak join_token or private roles
  const auto status = coordinator.handle(100, json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(status && (*status)["type"] == "match_status");
  CHECK(!status->contains("join_token"));
  CHECK(!status->contains("private_role"));
  CHECK(!status->contains("token"));

  const auto notifs = coordinator.drain_notifications();
  for (const auto& n : notifs) {
    CHECK(!n.payload.contains("join_token"));
    CHECK(!n.payload.contains("token"));
  }

  // 10. Production Default: Auto-generated token when none specified
  MatchCoordinator coord_autogen;
  coord_autogen.register_session(200, SessionRole::Moderator);
  coord_autogen.register_session(201, SessionRole::Player);
  const auto autogen_created = coord_autogen.handle(200, json{
    {"type", "create_match"},
    {"player_count", 5}
  });
  CHECK(autogen_created && (*autogen_created)["type"] == "match_created");
  const auto m_auto_id = (*autogen_created)["match_id"].get<std::uint64_t>();
  CHECK((*autogen_created)["open_lobby"] == false);
  CHECK(autogen_created->contains("join_token"));
  const auto autogen_tok = (*autogen_created)["join_token"].get<std::string>();
  CHECK(autogen_tok.size() == 32U); // 128-bit hex string

  // Player without token rejected on default protected match
  const auto auto_fail = coord_autogen.handle(201, json{
    {"type", "join_match"},
    {"match_id", m_auto_id}
  });
  CHECK(auto_fail && (*auto_fail)["type"] == "error");
  CHECK((*auto_fail)["error"] == "invalid or missing match join token");

  // Player with autogen token joins successfully
  const auto auto_ok = coord_autogen.handle(201, json{
    {"type", "join_match"},
    {"match_id", m_auto_id},
    {"join_token", autogen_tok}
  });
  CHECK(auto_ok && (*auto_ok)["type"] == "joined_match");

  // 11. Explicit Open Lobby mode allows unauthenticated join
  MatchCoordinator coord_open;
  coord_open.register_session(300, SessionRole::Moderator);
  coord_open.register_session(301, SessionRole::Player);
  const auto open_created = coord_open.handle(300, json{
    {"type", "create_match"},
    {"player_count", 5},
    {"open_lobby", true}
  });
  CHECK(open_created && (*open_created)["type"] == "match_created");
  const auto m_open_id = (*open_created)["match_id"].get<std::uint64_t>();
  CHECK((*open_created)["open_lobby"] == true);
  CHECK(!open_created->contains("join_token"));

  // Player joins without token in explicit open lobby
  const auto open_join = coord_open.handle(301, json{
    {"type", "join_match"},
    {"match_id", m_open_id}
  });
  CHECK(open_join && (*open_join)["type"] == "joined_match");
}

void test_list_matches_and_enrollment_token_authorization() {
  MatchCoordinator coord;
  coord.register_session(10, SessionRole::Moderator);
  coord.register_session(20, SessionRole::Player);
  coord.register_session(21, SessionRole::Player);

  // 1. Create protected match with token
  const auto created = coord.handle(10, json{
    {"type", "create_match"},
    {"player_count", 4},
    {"join_token", "prot-secret-88"}
  });
  CHECK(created && (*created)["type"] == "match_created");
  const auto mid = (*created)["match_id"].get<std::uint64_t>();

  // 2. list_matches
  const auto list_res = coord.handle(20, json{{"type", "list_matches"}});
  CHECK(list_res && (*list_res)["type"] == "matches_list");
  CHECK((*list_res)["matches"].size() == 1U);
  CHECK((*list_res)["matches"][0]["match_id"] == mid);
  CHECK((*list_res)["matches"][0]["player_count"] == 4U);
  CHECK((*list_res)["matches"][0]["open_lobby"] == false);

  // 3. get_match_token when server has no enrollment token configured
  test_unsetenv("DARKPOOL_ENROLLMENT_TOKEN");
  test_unsetenv("DARKPOOL_PLAYER_SECRET");
  const auto unauth_token_req = coord.handle(20, json{
    {"type", "get_match_token"},
    {"match_id", mid},
    {"enrollment_token", "some-token"}
  });
  CHECK(unauth_token_req && (*unauth_token_req)["type"] == "error");

  // 4. Set server enrollment secret
  test_setenv("DARKPOOL_ENROLLMENT_TOKEN", "valid-enroll-token");

  // Wrong enrollment token -> rejected
  const auto bad_req = coord.handle(20, json{
    {"type", "get_match_token"},
    {"match_id", mid},
    {"enrollment_token", "wrong-enroll-token"}
  });
  CHECK(bad_req && (*bad_req)["type"] == "error");

  // Correct enrollment token -> succeeds
  const auto good_req = coord.handle(20, json{
    {"type", "get_match_token"},
    {"match_id", mid},
    {"enrollment_token", "valid-enroll-token"}
  });
  CHECK(good_req && (*good_req)["type"] == "match_token_result");
  CHECK((*good_req)["ok"] == true);
  CHECK((*good_req)["join_token"] == "prot-secret-88");

  // Player joins using retrieved token
  const auto join_res = coord.handle(20, json{
    {"type", "join_match"},
    {"match_id", mid},
    {"join_token", (*good_req)["join_token"]}
  });
  CHECK(join_res && (*join_res)["type"] == "joined_match");

  test_unsetenv("DARKPOOL_ENROLLMENT_TOKEN");
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
  test_stage2_notifications_and_market_snapshots();
  test_stage3_timed_gameplay_and_moderator_controls();
  test_canonical_clue_phase_and_investigator_window();
  test_stage3_state_version_and_notification_ordering();
  test_stage4_coordinator_quantity_and_research();
  test_player_admission_security_and_identity();
  test_list_matches_and_enrollment_token_authorization();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Match coordinator isolation tests passed.\n";
  return EXIT_SUCCESS;
}
