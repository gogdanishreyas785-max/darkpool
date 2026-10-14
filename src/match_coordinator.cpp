#include "darkpool/match_coordinator.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <sys/random.h>
#endif

namespace darkpool::server {
namespace {

constexpr std::size_t kMaxMatches = 4;

std::optional<std::uint64_t> role_shuffle_seed() noexcept {
#if defined(__linux__)
  std::uint64_t seed = 0;
  auto* bytes = reinterpret_cast<unsigned char*>(&seed);
  std::size_t offset = 0;
  while (offset < sizeof(seed)) {
    const auto received = getrandom(bytes + offset, sizeof(seed) - offset, 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) return std::nullopt;
    offset += static_cast<std::size_t>(received);
  }
  return seed;
#else
  try {
    std::random_device entropy;
    return (static_cast<std::uint64_t>(entropy()) << 32U) ^ entropy();
  } catch (const std::exception&) {
    return std::nullopt;
  }
#endif
}

network::json error(std::string message) {
  return network::json{{"type", "error"}, {"error", std::move(message)}};
}

network::json operation_error(const OperationResult& result) {
  return network::json{{"type", "operation_result"}, {"ok", false},
                       {"code", Match::to_string(result.error)}, {"detail", result.detail}};
}

bool has_client_identity(const network::json& command) {
  return command.contains("player_id") || command.contains("voter_id") ||
      command.contains("investigator_id");
}

std::optional<std::uint64_t> read_match_id(const network::json& command) {
  if (!command.contains("match_id") ||
      !(command["match_id"].is_number_unsigned() || command["match_id"].is_number_integer())) {
    return std::nullopt;
  }
  try {
    if (command["match_id"].is_number_integer()) {
      const auto signed_id = command["match_id"].get<std::int64_t>();
      if (signed_id <= 0) return std::nullopt;
      return static_cast<std::uint64_t>(signed_id);
    }
    const auto unsigned_id = command["match_id"].get<std::uint64_t>();
    return unsigned_id == 0U ? std::nullopt : std::optional<std::uint64_t>(unsigned_id);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<PlayerId> read_player_target(const network::json& command,
                                           const char* field,
                                           bool allow_abstain) {
  if (!command.contains(field)) return std::nullopt;
  const auto& value = command[field];
  if (allow_abstain && value.is_null()) return PlayerId{0};
  if (!(value.is_number_unsigned() || value.is_number_integer())) return std::nullopt;
  try {
    if (value.is_number_integer()) {
      const auto signed_id = value.get<std::int64_t>();
      if (signed_id <= 0 || static_cast<std::uint64_t>(signed_id) >
          std::numeric_limits<PlayerId>::max()) return std::nullopt;
      return static_cast<PlayerId>(signed_id);
    }
    const auto unsigned_id = value.get<std::uint64_t>();
    if (unsigned_id == 0U || unsigned_id > std::numeric_limits<PlayerId>::max()) return std::nullopt;
    return static_cast<PlayerId>(unsigned_id);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::int32_t> read_impact(const network::json& command) {
  if (!command.contains("impact")) return std::nullopt;
  const auto& value = command["impact"];
  try {
    if (value.is_number_unsigned()) {
      const auto impact = value.get<std::uint64_t>();
      if (impact > 2U) return std::nullopt;
      return static_cast<std::int32_t>(impact);
    }
    if (!value.is_number_integer()) return std::nullopt;
    const auto impact = value.get<std::int64_t>();
    if (impact < -2 || impact > 2) return std::nullopt;
    return static_cast<std::int32_t>(impact);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

std::optional<std::size_t> read_player_count(const network::json& command) {
  if (!command.contains("player_count")) return kPlayerCount;
  const auto& value = command["player_count"];
  try {
    if (value.is_number_unsigned()) {
      const auto count = value.get<std::uint64_t>();
      if (count < kMinPlayerCount || count > kMaxPlayerCount) return std::nullopt;
      return static_cast<std::size_t>(count);
    }
    if (!value.is_number_integer()) return std::nullopt;
    const auto count = value.get<std::int64_t>();
    if (count < static_cast<std::int64_t>(kMinPlayerCount) ||
        count > static_cast<std::int64_t>(kMaxPlayerCount)) return std::nullopt;
    return static_cast<std::size_t>(count);
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

network::json public_status(std::uint64_t match_id, const MatchState& state,
                            std::size_t joined_players,
                            std::size_t player_count,
                            const std::optional<MarketResult>& last_market,
                            const std::optional<VoteResult>& last_vote) {
  network::json response{{"type", "match_status"}, {"match_id", match_id},
                         {"phase", Match::to_string(state.phase)}, {"round", state.round},
                         {"joined_players", joined_players},
                         {"player_count", player_count},
                         {"health", state.company.health}, {"price", state.company.price},
                         {"outcome", Match::to_string(state.outcome)}};
  if (last_market) {
    const auto& market = *last_market;
    response["last_market"] = network::json{{"executed_buys", market.executed_buys},
        {"executed_sells", market.executed_sells}, {"closing_health", market.closing_health},
        {"closing_price", market.closing_price}, {"closing_reserve", market.closing_reserve},
        {"outcome", Match::to_string(market.outcome)}};
  }
  if (last_vote) {
    const auto& vote = *last_vote;
    response["last_vote"] = network::json{{"eliminated_player", vote.eliminated
        ? network::json(*vote.eliminated) : network::json(nullptr)},
        {"votes_received", vote.votes_received}, {"eligible_voters", vote.eligible_voters},
        {"outcome", Match::to_string(vote.outcome)}};
  }
  return response;
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
  if (connection->second.role == SessionRole::Player &&
      connection->second.player_match == match_id && connection->second.player_id) return true;
  return connection->second.role == SessionRole::Moderator &&
      match->second.moderator == session;
}

std::optional<network::json> MatchCoordinator::handle(
    std::uint64_t session, const network::json& command) {
  if (!command.is_object() || !command.contains("type") || !command["type"].is_string()) {
    return std::nullopt;
  }
  const auto type = command["type"].get<std::string>();
  const bool coordinator_command = type == "create_match" || type == "join_match" ||
      type == "start_match" || type == "match_status" || type == "publish_event" ||
      type == "open_trading" || type == "resolve_market" || type == "trade" ||
      type == "request_clue" || type == "open_voting" || type == "vote" ||
      type == "resolve_votes" || type == "insider_hint";
  if (!coordinator_command) return std::nullopt;

  const auto connection = sessions_.find(session);
  if (connection == sessions_.end()) return error("connection is not registered");
  if (type == "create_match") {
    if (connection->second.role != SessionRole::Moderator) return error("moderator permission required");
    if (matches_.size() >= kMaxMatches) return error("match limit reached");
    if (command.contains("roles")) return error("client-supplied roles are not accepted");
    const auto player_count = read_player_count(command);
    if (!player_count) return error("player_count must be an integer from 3 through 64");
    const auto counts = role_counts(*player_count);
    if (!counts) return error("player_count must be an integer from 3 through 64");
    std::vector<PlayerSetup> roster;
    roster.reserve(*player_count);
    std::vector<Role> assigned_roles;
    assigned_roles.reserve(*player_count);
    assigned_roles.insert(assigned_roles.end(), counts->insiders, Role::Insider);
    assigned_roles.insert(assigned_roles.end(), counts->investigators, Role::Investigator);
    assigned_roles.insert(assigned_roles.end(), counts->employees, Role::Employee);
    const auto seed = role_shuffle_seed();
    if (!seed) return error("secure role assignment entropy is unavailable");
    std::mt19937_64 generator(*seed);
    std::shuffle(assigned_roles.begin(), assigned_roles.end(), generator);
    for (std::size_t i = 0; i < *player_count; ++i) {
      roster.push_back(PlayerSetup{static_cast<PlayerId>(i + 1U), assigned_roles[i]});
    }
    try {
      auto match = std::make_unique<Match>(roster);
      const auto match_id = next_match_id_++;
      MatchRecord record{std::move(match), session,
          std::vector<std::optional<std::uint64_t>>(*player_count), std::nullopt, std::nullopt};
      matches_.emplace(match_id, std::move(record));
      connection->second.moderator_matches.push_back(match_id);
      return network::json{{"type", "match_created"}, {"match_id", match_id},
          {"player_count", *player_count}, {"insiders", counts->insiders},
          {"investigators", counts->investigators}, {"employees", counts->employees}};
    } catch (const std::exception& exception) {
      return error(exception.what());
    }
  }

  const auto requested_match = read_match_id(command);
  if (!requested_match) return error("valid match_id is required");
  const auto match_id = *requested_match;
  auto match = matches_.find(match_id);
  if (match == matches_.end()) return error("unknown match");

  if (type == "join_match") {
    if (connection->second.role != SessionRole::Player) return error("player permission required");
    if (has_client_identity(command)) return error("client identity fields are not accepted");
    if (connection->second.player_match) return error("connection already belongs to a match");
    if (match->second.match->state().phase != Phase::Lobby) return error("match is not joinable");
    for (std::size_t seat = 0; seat < match->second.player_sessions.size(); ++seat) {
      if (!match->second.player_sessions[seat]) {
        const auto player_id = static_cast<PlayerId>(seat + 1U);
        match->second.player_sessions[seat] = session;
        connection->second.player_match = match_id;
        connection->second.player_id = player_id;
        const auto role = match->second.match->state().players[seat].role;
        return network::json{{"type", "joined_match"}, {"match_id", match_id},
                             {"player_id", player_id}, {"seat", seat + 1U},
                             {"private_role", Match::to_string(role)}};
      }
    }
    return error("match is full");
  }

  if (!is_member(session, match_id)) return error("connection is not a member of target match");
  const bool player_command = type == "trade" || type == "request_clue" ||
      type == "vote" || type == "insider_hint";
  const bool moderator_command = type == "start_match" || type == "publish_event" ||
      type == "open_trading" || type == "resolve_market" || type == "open_voting" ||
      type == "resolve_votes";
  if (player_command && connection->second.role != SessionRole::Player) {
    return error("player permission required");
  }
  if (moderator_command && (connection->second.role != SessionRole::Moderator ||
      match->second.moderator != session)) return error("creating moderator permission required");
  if (player_command && has_client_identity(command)) {
    return error("client identity fields are not accepted");
  }
  if (type == "match_status") {
    const auto joined = static_cast<std::size_t>(std::count_if(match->second.player_sessions.begin(),
        match->second.player_sessions.end(), [](const auto& item) { return item.has_value(); }));
    return public_status(match_id, match->second.match->state(), joined,
                         match->second.player_sessions.size(),
                         match->second.last_market, match->second.last_vote);
  }

  const auto player_id = connection->second.player_id;
  if (type == "start_match") {
    const auto joined = std::count_if(match->second.player_sessions.begin(),
        match->second.player_sessions.end(), [](const auto& item) { return item.has_value(); });
    if (static_cast<std::size_t>(joined) != match->second.player_sessions.size()) {
      return error("all configured players must join before start");
    }
    const auto result = match->second.match->start();
    if (!result) return operation_error(result);
    return network::json{{"type", "transition_result"}, {"ok", true},
                         {"phase", Match::to_string(match->second.match->state().phase)}};
  }
  if (type == "publish_event") {
    const auto impact = read_impact(command);
    if (!impact) return error("integer impact in [-2,2] is required");
    const auto result = match->second.match->publish_event(*impact);
    if (!result) return operation_error(result);
    return network::json{{"type", "transition_result"}, {"ok", true},
                         {"phase", Match::to_string(match->second.match->state().phase)},
                         {"round", match->second.match->state().round}};
  }
  if (type == "open_trading" || type == "open_voting") {
    const auto result = type == "open_trading" ? match->second.match->open_trading()
                                                  : match->second.match->open_voting();
    if (!result) return operation_error(result);
    return network::json{{"type", "transition_result"}, {"ok", true},
                         {"phase", Match::to_string(match->second.match->state().phase)},
                         {"round", match->second.match->state().round}};
  }
  if (type == "trade") {
    if (!player_id || !command.contains("action") || !command["action"].is_string()) return error("trade action string is required");
    const auto action_name = command["action"].get<std::string>();
    TradeAction action;
    if (action_name == "BUY") action = TradeAction::Buy;
    else if (action_name == "SELL") action = TradeAction::Sell;
    else if (action_name == "HOLD") action = TradeAction::Hold;
    else return error("action must be BUY, SELL, or HOLD");
    const auto result = match->second.match->submit_trade(*player_id, action);
    if (!result) return operation_error(result);
    return network::json{{"type", "trade_result"}, {"ok", true}, {"code", "Accepted"}};
  }
  if (type == "resolve_market") {
    auto result = match->second.match->resolve_market();
    if (!result) return operation_error(result.status);
    match->second.last_market = *result.value;
    const auto& market = *result.value;
    return network::json{{"type", "market_result"}, {"ok", true},
        {"executed_buys", market.executed_buys}, {"executed_sells", market.executed_sells},
        {"closing_health", market.closing_health}, {"closing_price", market.closing_price},
        {"closing_reserve", market.closing_reserve}, {"outcome", Match::to_string(market.outcome)}};
  }
  if (type == "request_clue") {
    if (!player_id) return error("player membership required");
    const auto target = read_player_target(command, "target_id", false);
    if (!target) return error("valid target_id is required");
    OperationResult result;
    const auto clue = match->second.match->use_clue(*player_id, *target, result);
    if (!result) return operation_error(result);
    if (!clue) return error("clue operation returned no result");
    return network::json{{"type", "clue_private"}, {"match_id", match_id}, {"is_insider", *clue}};
  }
  if (type == "vote") {
    if (!player_id || !command.contains("target_id")) return error("target_id is required; use null to abstain");
    std::optional<PlayerId> target;
    if (!command["target_id"].is_null()) {
      const auto parsed_target = read_player_target(command, "target_id", false);
      if (!parsed_target) return error("target_id must be a valid player ID or null");
      target = *parsed_target;
    }
    const auto result = match->second.match->submit_vote(*player_id, target);
    if (!result) return operation_error(result);
    return network::json{{"type", "vote_result"}, {"ok", true}, {"accepted", true}};
  }
  if (type == "resolve_votes") {
    auto result = match->second.match->resolve_votes();
    if (!result) return operation_error(result.status);
    match->second.last_vote = *result.value;
    const auto& vote = *result.value;
    return network::json{{"type", "vote_resolution"}, {"ok", true},
        {"eliminated_player", vote.eliminated ? network::json(*vote.eliminated) : network::json(nullptr)},
        {"votes_received", vote.votes_received}, {"eligible_voters", vote.eligible_voters},
        {"outcome", Match::to_string(vote.outcome)}};
  }
  if (type == "insider_hint") {
    if (!player_id) return error("player membership required");
    OperationResult result;
    const auto hint = match->second.match->insider_hint(*player_id, result);
    if (!result) return operation_error(result);
    if (!hint) return error("hint operation returned no result");
    return network::json{{"type", "hint_private"}, {"match_id", match_id}, {"event_impact", *hint}};
  }
  return error("unsupported command");
}

}  // namespace darkpool::server
