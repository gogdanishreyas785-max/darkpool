#include "darkpool/match_coordinator.hpp"

#include <algorithm>
#include <stdexcept>

namespace darkpool::server {
namespace {

constexpr std::size_t kMaxMatches = 4;

network::json error(std::string message) {
  return network::json{{"type", "error"}, {"error", std::move(message)}};
}

std::optional<Role> parse_role(const std::string& value) {
  if (value == "Insider") return Role::Insider;
  if (value == "Investigator") return Role::Investigator;
  if (value == "Employee") return Role::Employee;
  return std::nullopt;
}

}  // namespace

void MatchCoordinator::register_session(std::uint64_t session, SessionRole role) {
  sessions_.insert_or_assign(session, Session{role, std::nullopt, std::nullopt, {}});
}

void MatchCoordinator::unregister_session(std::uint64_t session) noexcept {
  const auto found = sessions_.find(session);
  if (found == sessions_.end()) return;
  if (found->second.player_match) {
    const auto match = matches_.find(*found->second.player_match);
    if (match != matches_.end() && found->second.player_id &&
        *found->second.player_id > 0U && *found->second.player_id <= match->second.player_sessions.size()) {
      match->second.player_sessions[static_cast<std::size_t>(*found->second.player_id - 1U)].reset();
    }
  }
  for (const auto match_id : found->second.moderator_matches) {
    const auto match = matches_.find(match_id);
    if (match == matches_.end()) continue;
    for (const auto& player_session : match->second.player_sessions) {
      if (!player_session) continue;
      const auto player = sessions_.find(*player_session);
      if (player != sessions_.end()) {
        player->second.player_match.reset();
        player->second.player_id.reset();
      }
    }
    matches_.erase(match);
  }
  sessions_.erase(found);
}

bool MatchCoordinator::is_member(std::uint64_t session, std::uint64_t match_id) const {
  const auto connection = sessions_.find(session);
  const auto match = matches_.find(match_id);
  if (connection == sessions_.end() || match == matches_.end()) return false;
  if (connection->second.player_match == match_id) return true;
  return connection->second.role == SessionRole::Moderator &&
      match->second.moderator == session;
}

std::optional<network::json> MatchCoordinator::handle(
    std::uint64_t session, const network::json& command) {
  if (!command.is_object() || !command.contains("type") || !command["type"].is_string()) {
    return std::nullopt;
  }
  const auto type = command["type"].get<std::string>();
  if (type != "create_match" && type != "join_match" &&
      type != "start_match" && type != "match_status") return std::nullopt;

  const auto connection = sessions_.find(session);
  if (connection == sessions_.end()) return error("connection is not registered");
  if (type == "create_match") {
    if (connection->second.role != SessionRole::Moderator) return error("moderator permission required");
    if (matches_.size() >= kMaxMatches) return error("match limit reached");
    if (!command.contains("roles") || !command["roles"].is_array() ||
        command["roles"].size() != kPlayerCount) return error("exactly five roles are required");
    std::vector<PlayerSetup> roster;
    roster.reserve(kPlayerCount);
    for (std::size_t i = 0; i < kPlayerCount; ++i) {
      if (!command["roles"][i].is_string()) return error("role names must be strings");
      const auto role = parse_role(command["roles"][i].get<std::string>());
      if (!role) return error("unknown role");
      roster.push_back(PlayerSetup{static_cast<PlayerId>(i + 1U), *role});
    }
    try {
      auto match = std::make_unique<Match>(roster);
      const auto match_id = next_match_id_++;
      MatchRecord record{std::move(match), session, std::vector<std::optional<std::uint64_t>>(kPlayerCount)};
      matches_.emplace(match_id, std::move(record));
      connection->second.moderator_matches.push_back(match_id);
      return network::json{{"type", "match_created"}, {"match_id", match_id}};
    } catch (const std::exception& exception) {
      return error(exception.what());
    }
  }

  if (!command.contains("match_id") ||
      !(command["match_id"].is_number_unsigned() || command["match_id"].is_number_integer())) {
    return error("match_id is required");
  }
  std::uint64_t match_id = 0;
  try {
    const auto raw_id = command["match_id"].get<std::int64_t>();
    if (raw_id <= 0) return error("invalid match_id");
    match_id = static_cast<std::uint64_t>(raw_id);
  } catch (const std::exception&) {
    return error("invalid match_id");
  }
  auto match = matches_.find(match_id);
  if (match == matches_.end()) return error("unknown match");

  if (type == "join_match") {
    if (connection->second.role != SessionRole::Player) return error("player permission required");
    if (connection->second.player_match) return error("connection already belongs to a match");
    if (match->second.match->state().phase != Phase::Lobby) return error("match is not joinable");
    for (std::size_t seat = 0; seat < match->second.player_sessions.size(); ++seat) {
      if (!match->second.player_sessions[seat]) {
        const auto player_id = static_cast<PlayerId>(seat + 1U);
        match->second.player_sessions[seat] = session;
        connection->second.player_match = match_id;
        connection->second.player_id = player_id;
        return network::json{{"type", "joined_match"}, {"match_id", match_id},
                             {"player_id", player_id}, {"seat", seat + 1U}};
      }
    }
    return error("match is full");
  }

  if (!is_member(session, match_id)) return error("connection is not a member of target match");
  if (type == "match_status") {
    const auto& state = match->second.match->state();
    return network::json{{"type", "match_status"}, {"match_id", match_id},
                         {"phase", Match::to_string(state.phase)}, {"round", state.round},
                         {"joined_players", std::count_if(match->second.player_sessions.begin(),
                             match->second.player_sessions.end(), [](const auto& item) { return item.has_value(); })}};
  }
  if (connection->second.role != SessionRole::Moderator || match->second.moderator != session) {
    return error("creating moderator permission required");
  }
  const auto joined = std::count_if(match->second.player_sessions.begin(),
      match->second.player_sessions.end(), [](const auto& item) { return item.has_value(); });
  if (static_cast<std::size_t>(joined) != kPlayerCount) return error("five players must join before start");
  const auto started = match->second.match->start();
  if (!started) return error(started.detail);
  return network::json{{"type", "match_started"}, {"match_id", match_id},
                       {"phase", Match::to_string(match->second.match->state().phase)}};
}

}  // namespace darkpool::server
