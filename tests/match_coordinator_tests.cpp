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
  const auto unchanged = coordinator.handle(6, json{{"type", "match_status"}, {"match_id", match_b}});
  CHECK(unchanged && (*unchanged)["phase"] == "Lobby");

  const auto outsider_start = coordinator.handle(1, json{{"type", "start_match"}, {"match_id", match_b}});
  CHECK(outsider_start && (*outsider_start)["type"] == "error");
}

}  // namespace

int main() {
  test_match_isolation_and_authorization();
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Match coordinator isolation tests passed.\n";
  return EXIT_SUCCESS;
}
