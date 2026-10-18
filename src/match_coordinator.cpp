#include "darkpool/match_coordinator.hpp"
#include "darkpool/token_store.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <random>
#include <string>
#include <utility>

#if defined(__linux__)
#include <cerrno>
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

std::string generate_secure_join_token() noexcept {
  unsigned char bytes[16] = {0};
#if defined(__linux__)
  std::size_t offset = 0;
  while (offset < sizeof(bytes)) {
    const auto received = getrandom(bytes + offset, sizeof(bytes) - offset, 0);
    if (received < 0 && errno == EINTR) continue;
    if (received <= 0) break;
    offset += static_cast<std::size_t>(received);
  }
  if (offset < sizeof(bytes)) {
    try {
      std::random_device rd;
      for (std::size_t i = offset; i < sizeof(bytes); ++i) {
        bytes[i] = static_cast<unsigned char>(rd() & 0xFF);
      }
    } catch (...) {}
  }
#else
  try {
    std::random_device rd;
    for (std::size_t i = 0; i < sizeof(bytes); ++i) {
      bytes[i] = static_cast<unsigned char>(rd() & 0xFF);
    }
  } catch (...) {}
#endif
  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string token;
  token.reserve(sizeof(bytes) * 2);
  for (unsigned char b : bytes) {
    token.push_back(kHexDigits[(b >> 4) & 0x0F]);
    token.push_back(kHexDigits[b & 0x0F]);
  }
  return token;
}

bool secure_compare(std::string_view a, std::string_view b) noexcept {
  const std::size_t max_size = std::max(a.size(), b.size());
  std::size_t diff = a.size() ^ b.size();
  for (std::size_t i = 0; i < max_size; ++i) {
    const auto ac = i < a.size() ? static_cast<unsigned char>(a[i]) : 0U;
    const auto bc = i < b.size() ? static_cast<unsigned char>(b[i]) : 0U;
    diff |= static_cast<std::size_t>(ac ^ bc);
  }
  return diff == 0U;
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
      command.contains("investigator_id") || command.contains("seat") ||
      command.contains("role") || command.contains("private_role");
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

network::json make_market_snapshot(std::uint64_t match_id, std::uint64_t state_version,
                                  const MatchState& state,
                                  const std::optional<MarketResult>& last_market,
                                  const MatchTimer& timer,
                                  std::chrono::steady_clock::time_point now,
                                  bool is_aborted) {
  const auto shares_held_by_players = state.total_supply >= state.reserve
      ? (state.total_supply - state.reserve) : 0U;
  std::uint64_t sec_rem = 0;
  if (timer.active && !is_aborted) {
    if (timer.paused) {
      sec_rem = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(timer.paused_remaining).count());
    } else if (timer.deadline > now) {
      sec_rem = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(timer.deadline - now).count());
    }
  }
  network::json snapshot{
    {"type", "market_snapshot"},
    {"match_id", match_id},
    {"state_version", state_version},
    {"round", state.round},
    {"phase", is_aborted ? "Finished" : Match::to_string(state.phase)},
    {"health", state.company.health},
    {"price", state.company.price},
    {"total_supply", state.total_supply},
    {"reserve", state.reserve},
    {"shares_held_by_players", shares_held_by_players},
    {"sentiment", state.sentiment},
    {"volatility", state.volatility},
    {"liquidity", state.liquidity},
    {"outcome", is_aborted ? "Aborted" : Match::to_string(state.outcome)},
    {"is_paused", timer.paused},
    {"seconds_remaining", sec_rem}
  };
  if (last_market) {
    const auto& market = *last_market;
    snapshot["last_market"] = network::json{
      {"executed_buys", market.executed_buys},
      {"executed_sells", market.executed_sells},
      {"closing_health", market.closing_health},
      {"closing_price", market.closing_price},
      {"closing_reserve", market.closing_reserve},
      {"sentiment", market.sentiment},
      {"volatility", market.volatility},
      {"liquidity", market.liquidity},
      {"outcome", Match::to_string(market.outcome)}
    };
  }
  return snapshot;
}

network::json public_status(std::uint64_t match_id, std::uint64_t state_version,
                            const MatchState& state,
                            std::size_t joined_players,
                            std::size_t player_count,
                            const std::optional<MarketResult>& last_market,
                            const std::optional<VoteResult>& last_vote,
                            const MatchTimer& timer,
                            std::chrono::steady_clock::time_point now,
                            bool is_aborted) {
  std::uint64_t sec_rem = 0;
  if (timer.active && !is_aborted) {
    if (timer.paused) {
      sec_rem = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(timer.paused_remaining).count());
    } else if (timer.deadline > now) {
      sec_rem = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(timer.deadline - now).count());
    }
  }
  network::json response{{"type", "match_status"}, {"match_id", match_id},
                         {"state_version", state_version},
                         {"phase", is_aborted ? "Finished" : Match::to_string(state.phase)}, {"round", state.round},
                         {"joined_players", joined_players},
                         {"player_count", player_count},
                         {"health", state.company.health}, {"price", state.company.price},
                         {"sentiment", state.sentiment}, {"volatility", state.volatility},
                         {"liquidity", state.liquidity},
                         {"outcome", is_aborted ? "Aborted" : Match::to_string(state.outcome)},
                         {"is_paused", timer.paused},
                         {"seconds_remaining", sec_rem}};
  if (last_market) {
    const auto& market = *last_market;
    response["last_market"] = network::json{{"executed_buys", market.executed_buys},
        {"executed_sells", market.executed_sells}, {"closing_health", market.closing_health},
        {"closing_price", market.closing_price}, {"closing_reserve", market.closing_reserve},
        {"sentiment", market.sentiment}, {"volatility", market.volatility},
        {"liquidity", market.liquidity},
        {"outcome", Match::to_string(market.outcome)}};
  }
  if (last_vote) {
    const auto& vote = *last_vote;
    response["last_vote"] = network::json{{"eliminated_player", vote.eliminated
        ? network::json(*vote.eliminated) : network::json(nullptr)},
        {"votes_received", vote.votes_received},
        {"votes_cast", vote.votes_cast},
        {"votes_submitted", vote.votes_cast},
        {"votes_abstained", vote.votes_abstained},
        {"votes_missing", vote.votes_missing},
        {"eligible_voters", vote.eligible_voters},
        {"outcome", Match::to_string(vote.outcome)}};
  }
  return response;
}

}  // namespace

MatchCoordinator::~MatchCoordinator() {
  for (const auto& [id, _] : matches_) {
    security::TokenStore::remove_match_token(id);
  }
}

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
    security::TokenStore::remove_match_token(match_id);
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

std::vector<std::uint64_t> MatchCoordinator::get_match_members(const MatchRecord& record) {
  std::vector<std::uint64_t> members;
  if (record.moderator != 0U) {
    members.push_back(record.moderator);
  }
  for (const auto& opt_session : record.player_sessions) {
    if (opt_session) {
      members.push_back(*opt_session);
    }
  }
  return members;
}

std::vector<OutgoingNotification> MatchCoordinator::drain_notifications() {
  std::vector<OutgoingNotification> result;
  result.swap(pending_notifications_);
  return result;
}

void MatchCoordinator::set_simulated_time(std::optional<std::chrono::steady_clock::time_point> now) {
  simulated_time_ = now;
}

void MatchCoordinator::set_phase_durations(PhaseDurations durations) {
  phase_durations_ = durations;
}

std::chrono::steady_clock::time_point MatchCoordinator::now_() const {
  if (simulated_time_) return *simulated_time_;
  return std::chrono::steady_clock::now();
}

OperationResult MatchCoordinator::apply_open_trading(std::uint64_t match_id, MatchRecord& record,
                                                    std::chrono::steady_clock::time_point now) {
  const auto result = record.match->open_trading();
  if (!result) return result;
  ++record.state_version;
  record.timer.active = true;
  record.timer.paused = false;
  record.timer.phase = Phase::Trading;
  record.timer.total_duration = phase_durations_.trading;
  record.timer.deadline = now + phase_durations_.trading;
  record.timer.paused_remaining = phase_durations_.trading;
  record.timer.warning_30_fired = false;
  record.timer.warning_10_fired = false;

  const auto members = get_match_members(record);
  const auto& st = record.match->state();
  pending_notifications_.push_back(OutgoingNotification{
      members,
      network::json{{"type", "server_event"}, {"event", "phase_started"},
                    {"match_id", match_id}, {"round", st.round},
                    {"phase", Match::to_string(st.phase)},
                    {"seconds_remaining", static_cast<std::uint64_t>(phase_durations_.trading.count())},
                    {"state_version", record.state_version}}});
  pending_notifications_.push_back(OutgoingNotification{
      members,
      make_market_snapshot(match_id, record.state_version, st, record.last_market,
                           record.timer, now, record.is_aborted)});
  return result;
}

MarketResolution MatchCoordinator::apply_resolve_market(std::uint64_t match_id, MatchRecord& record,
                                                        std::chrono::steady_clock::time_point now) {
  auto result = record.match->resolve_market();
  if (!result) return result;
  record.last_market = *result.value;
  ++record.state_version;
  const auto members = get_match_members(record);
  const auto& st = record.match->state();

  if (st.phase == Phase::Finished) {
    security::TokenStore::remove_match_token(match_id);
    record.timer.active = false;
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "match_finished"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", "Finished"},
                      {"outcome", Match::to_string(st.outcome)},
                      {"state_version", record.state_version}}});
    pending_notifications_.push_back(OutgoingNotification{
        members,
        make_market_snapshot(match_id, record.state_version, st, record.last_market,
                             record.timer, now, record.is_aborted)});
  } else {
    // Engine is now in canonical Phase::Clue (approved duration: 30 seconds)
    record.timer.active = true;
    record.timer.paused = false;
    record.timer.phase = Phase::Clue;
    record.timer.in_transition = false;
    record.timer.total_duration = phase_durations_.clue;
    record.timer.deadline = now + phase_durations_.clue;
    record.timer.paused_remaining = phase_durations_.clue;
    record.timer.warning_30_fired = true;
    record.timer.warning_10_fired = false;

    // 1. Market resolution and public market snapshot
    pending_notifications_.push_back(OutgoingNotification{
        members,
        make_market_snapshot(match_id, record.state_version, st, record.last_market,
                             record.timer, now, record.is_aborted)});

    // 2. Clue phase start
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "phase_started"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", Match::to_string(st.phase)},
                      {"seconds_remaining", static_cast<std::uint64_t>(phase_durations_.clue.count())},
                      {"state_version", record.state_version}}});

    // 3. Clue 30-second warning (emitted once at Clue entry because 30s Clue begins with 30s remaining)
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "phase_warning"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", Match::to_string(st.phase)},
                      {"seconds_remaining", 30},
                      {"state_version", record.state_version}}});
  }
  return result;
}

void MatchCoordinator::start_transition_interval(std::uint64_t match_id, MatchRecord& record, Phase upcoming,
                                                 std::chrono::steady_clock::time_point now) {
  record.timer.active = true;
  record.timer.paused = false;
  record.timer.in_transition = true;
  record.timer.upcoming_phase = upcoming;
  record.timer.total_duration = phase_durations_.transition;
  record.timer.deadline = now + phase_durations_.transition;
  record.timer.paused_remaining = phase_durations_.transition;
  record.timer.warning_30_fired = false;
  record.timer.warning_10_fired = false;

  const auto members = get_match_members(record);
  const auto& st = record.match->state();
  pending_notifications_.push_back(OutgoingNotification{
      members,
      network::json{{"type", "server_event"}, {"event", "transition_interval"},
                    {"match_id", match_id}, {"round", st.round},
                    {"upcoming_phase", Match::to_string(upcoming)},
                    {"seconds_remaining", static_cast<std::uint64_t>(phase_durations_.transition.count())},
                    {"state_version", record.state_version}}});
  pending_notifications_.push_back(OutgoingNotification{
      members,
      make_market_snapshot(match_id, record.state_version, st, record.last_market,
                           record.timer, now, record.is_aborted)});
}

OperationResult MatchCoordinator::apply_open_voting(std::uint64_t match_id, MatchRecord& record,
                                                    std::chrono::steady_clock::time_point now) {
  const auto result = record.match->open_voting();
  if (!result) return result;
  ++record.state_version;
  record.timer.active = true;
  record.timer.paused = false;
  record.timer.in_transition = false;
  record.timer.phase = Phase::Voting;
  record.timer.total_duration = phase_durations_.voting;
  record.timer.deadline = now + phase_durations_.voting;
  record.timer.paused_remaining = phase_durations_.voting;
  record.timer.warning_30_fired = false;
  record.timer.warning_10_fired = false;

  const auto members = get_match_members(record);
  const auto& st = record.match->state();
  pending_notifications_.push_back(OutgoingNotification{
      members,
      network::json{{"type", "server_event"}, {"event", "phase_started"},
                    {"match_id", match_id}, {"round", st.round},
                    {"phase", Match::to_string(st.phase)},
                    {"seconds_remaining", static_cast<std::uint64_t>(phase_durations_.voting.count())},
                    {"state_version", record.state_version}}});
  pending_notifications_.push_back(OutgoingNotification{
      members,
      make_market_snapshot(match_id, record.state_version, st, record.last_market,
                           record.timer, now, record.is_aborted)});
  return result;
}

VoteResolution MatchCoordinator::apply_resolve_votes(std::uint64_t match_id, MatchRecord& record,
                                                     std::chrono::steady_clock::time_point now) {
  const auto vote_round = record.match->state().round;
  auto result = record.match->resolve_votes();
  if (!result) return result;
  record.last_vote = *result.value;
  const auto& vote = *result.value;
  ++record.state_version;
  record.timer.active = false;
  const auto members = get_match_members(record);
  const auto& st = record.match->state();
  pending_notifications_.push_back(OutgoingNotification{
      members,
      network::json{{"type", "server_event"}, {"event", "vote_resolved"},
                    {"match_id", match_id}, {"round", vote_round},
                    {"phase", "Voting"},
                    {"eliminated_player", vote.eliminated ? network::json(*vote.eliminated) : network::json(nullptr)},
                    {"votes_received", vote.votes_received},
                    {"votes_cast", vote.votes_cast},
                    {"votes_submitted", vote.votes_cast},
                    {"votes_abstained", vote.votes_abstained},
                    {"votes_missing", vote.votes_missing},
                    {"eligible_voters", vote.eligible_voters},
                    {"outcome", Match::to_string(vote.outcome)},
                    {"state_version", record.state_version}}});
  if (st.phase == Phase::Finished) {
    security::TokenStore::remove_match_token(match_id);
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "match_finished"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", "Finished"},
                      {"outcome", Match::to_string(st.outcome)},
                      {"state_version", record.state_version}}});
  } else {
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "round_started"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", Match::to_string(st.phase)},
                      {"state_version", record.state_version}}});
  }
  pending_notifications_.push_back(OutgoingNotification{
      members,
      make_market_snapshot(match_id, record.state_version, st, record.last_market,
                           record.timer, now, record.is_aborted)});
  return result;
}

void MatchCoordinator::tick(std::optional<std::chrono::steady_clock::time_point> now) {
  if (now) {
    simulated_time_ = *now;
  }
  const auto current = now_();

  for (auto& [match_id, record] : matches_) {
    if (record.is_aborted || record.timer.paused || !record.timer.active) {
      continue;
    }
    const auto& st = record.match->state();
    if (st.phase == Phase::Finished) {
      record.timer.active = false;
      continue;
    }

    if (current < record.timer.deadline) {
      if (!record.timer.in_transition) {
        const auto remaining_sec = std::chrono::duration_cast<std::chrono::seconds>(
            record.timer.deadline - current).count();
        const auto members = get_match_members(record);

        if (remaining_sec <= 30 && !record.timer.warning_30_fired &&
            record.timer.total_duration > std::chrono::seconds(30)) {
          record.timer.warning_30_fired = true;
          pending_notifications_.push_back(OutgoingNotification{
              members,
              network::json{{"type", "server_event"},
                            {"event", "phase_warning"},
                            {"match_id", match_id},
                            {"round", st.round},
                            {"phase", Match::to_string(st.phase)},
                            {"seconds_remaining", 30},
                            {"state_version", record.state_version}}});
        }

        if (remaining_sec <= 10 && !record.timer.warning_10_fired &&
            record.timer.total_duration >= std::chrono::seconds(10)) {
          record.timer.warning_10_fired = true;
          pending_notifications_.push_back(OutgoingNotification{
              members,
              network::json{{"type", "server_event"},
                            {"event", "phase_warning"},
                            {"match_id", match_id},
                            {"round", st.round},
                            {"phase", Match::to_string(st.phase)},
                            {"seconds_remaining", 10},
                            {"state_version", record.state_version}}});
        }
      }
    } else {
      // Deadline reached or exceeded!
      // Ensure 10-second warning is ordered before transition entry when both become observable at the same tick
      if (st.phase == Phase::Clue && !record.timer.in_transition) {
        if (!record.timer.warning_10_fired) {
          record.timer.warning_10_fired = true;
          const auto members = get_match_members(record);
          pending_notifications_.push_back(OutgoingNotification{
              members,
              network::json{{"type", "server_event"},
                            {"event", "phase_warning"},
                            {"match_id", match_id},
                            {"round", st.round},
                            {"phase", Match::to_string(st.phase)},
                            {"seconds_remaining", 10},
                            {"state_version", record.state_version}}});
        }
      }

      record.timer.active = false;
      if (record.timer.in_transition) {
        record.timer.in_transition = false;
        if (record.timer.upcoming_phase == Phase::Voting) {
          apply_open_voting(match_id, record, current);
        } else if (record.timer.upcoming_phase == Phase::Trading) {
          apply_open_trading(match_id, record, current);
        }
      } else {
        if (st.phase == Phase::Discussion) {
          apply_open_trading(match_id, record, current);
        } else if (st.phase == Phase::Trading) {
          apply_resolve_market(match_id, record, current);
        } else if (st.phase == Phase::Clue) {
          // Automatic 10-second transition interval to Voting on Clue expiry
          start_transition_interval(match_id, record, Phase::Voting, current);
        } else if (st.phase == Phase::Voting) {
          apply_resolve_votes(match_id, record, current);
        }
      }
    }
  }
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
      type == "resolve_votes" || type == "insider_hint" ||
      type == "research" || type == "scout" ||
      type == "pause" || type == "resume" || type == "abort" ||
      type == "list_matches" || type == "get_match_token";
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
      auto match = std::make_unique<Match>(roster, default_market_coefficients_);
      const auto match_id = next_match_id_++;

      bool is_open_lobby = default_open_lobby_;
      if (command.contains("open_lobby") && command["open_lobby"].is_boolean()) {
        is_open_lobby = command["open_lobby"].get<bool>();
      } else if (const char* env_open = std::getenv("DARKPOOL_OPEN_LOBBY")) {
        if (std::strcmp(env_open, "1") == 0 || std::strcmp(env_open, "true") == 0 || std::strcmp(env_open, "TRUE") == 0) {
          is_open_lobby = true;
        }
      }

      std::optional<std::string> join_token;
      if (!is_open_lobby) {
        if (command.contains("join_token") && command["join_token"].is_string()) {
          const auto tok = command["join_token"].get<std::string>();
          if (!tok.empty()) join_token = tok;
        } else if (const char* env_token = std::getenv("DARKPOOL_MATCH_JOIN_TOKEN")) {
          if (std::strlen(env_token) > 0) join_token = std::string(env_token);
        } else if (const char* env_p_token = std::getenv("DARKPOOL_PLAYER_TOKEN")) {
          if (std::strlen(env_p_token) > 0) join_token = std::string(env_p_token);
        }
        if (!join_token) {
          // Production default: auto-generate unpredictable 128-bit hex token
          join_token = generate_secure_join_token();
        }
      }

      MatchRecord record{std::move(match), session,
          std::vector<std::optional<std::uint64_t>>(*player_count), std::nullopt, std::nullopt, 1U, false, {}, join_token};
      matches_.emplace(match_id, std::move(record));
      connection->second.moderator_matches.push_back(match_id);
      network::json resp{{"type", "match_created"}, {"match_id", match_id},
          {"player_count", *player_count}, {"insiders", counts->insiders},
          {"investigators", counts->investigators}, {"employees", counts->employees},
          {"open_lobby", is_open_lobby}};
      if (join_token) {
        resp["join_token"] = *join_token;
        if (const char* prov = std::getenv("DARKPOOL_PROVISION_TOKENS");
            prov && (std::strcmp(prov, "1") == 0 || std::strcmp(prov, "true") == 0)) {
          security::TokenStore::save_match_token(match_id, *join_token);
        }
      }
      return resp;
    } catch (const std::exception& exception) {
      return error(exception.what());
    }
  }

  if (type == "list_matches") {
    network::json matches_arr = network::json::array();
    for (const auto& [id, record] : matches_) {
      std::size_t joined = 0;
      for (const auto& s : record.player_sessions) {
        if (s) ++joined;
      }
      matches_arr.push_back({
        {"match_id", id},
        {"phase", Match::to_string(record.match->state().phase)},
        {"player_count", record.player_sessions.size()},
        {"joined_players", joined},
        {"open_lobby", !record.join_token.has_value()},
        {"is_aborted", record.is_aborted}
      });
    }
    return network::json{{"type", "matches_list"}, {"matches", std::move(matches_arr)}};
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
    if (match->second.is_aborted) return error("match has been aborted");
    if (match->second.match->state().phase != Phase::Lobby) return error("match is not joinable");

    if (match->second.join_token.has_value()) {
      std::string provided_token;
      if (command.contains("join_token") && command["join_token"].is_string()) {
        provided_token = command["join_token"].get<std::string>();
      } else if (command.contains("token") && command["token"].is_string()) {
        provided_token = command["token"].get<std::string>();
      } else if (const char* env_token = std::getenv("DARKPOOL_PLAYER_TOKEN")) {
        provided_token = std::string(env_token);
      } else if (const char* env_j_token = std::getenv("DARKPOOL_JOIN_TOKEN")) {
        provided_token = std::string(env_j_token);
      }

      if (provided_token.empty() || provided_token != *match->second.join_token) {
        return error("invalid or missing match join token");
      }
    }

    for (std::size_t seat = 0; seat < match->second.player_sessions.size(); ++seat) {
      if (!match->second.player_sessions[seat]) {
        const auto player_id = static_cast<PlayerId>(seat + 1U);
        match->second.player_sessions[seat] = session;
        connection->second.player_match = match_id;
        connection->second.player_id = player_id;
        const auto role = match->second.match->state().players[seat].role;
        pending_notifications_.push_back(OutgoingNotification{
            {session},
            make_market_snapshot(match_id, match->second.state_version,
                                 match->second.match->state(), match->second.last_market,
                                 match->second.timer, now_(), match->second.is_aborted)});
        return network::json{{"type", "joined_match"}, {"match_id", match_id},
                             {"player_id", player_id}, {"seat", seat + 1U},
                             {"private_role", Match::to_string(role)}};
      }
    }
    return error("match is full");
  }

  if (type == "get_match_token") {
    if (connection->second.role != SessionRole::Player) {
      return error("player permission required");
    }
    if (match->second.is_aborted || match->second.match->state().phase != Phase::Lobby) {
      return error("match is not joinable");
    }
    if (!match->second.join_token.has_value()) {
      return network::json{{"type", "match_token_result"}, {"ok", true},
                           {"match_id", match_id}, {"open_lobby", true}};
    }
    const char* server_enroll_token = std::getenv("DARKPOOL_ENROLLMENT_TOKEN");
    if (!server_enroll_token || server_enroll_token[0] == '\0') {
      server_enroll_token = std::getenv("DARKPOOL_PLAYER_SECRET");
    }
    if (!server_enroll_token || server_enroll_token[0] == '\0') {
      return error("enrollment authorization not configured on server");
    }
    if (!command.contains("enrollment_token") || !command["enrollment_token"].is_string()) {
      return error("enrollment_token is required");
    }
    const auto client_enroll = command["enrollment_token"].get<std::string>();
    if (!secure_compare(client_enroll, server_enroll_token)) {
      return error("invalid enrollment authorization");
    }
    return network::json{{"type", "match_token_result"}, {"ok", true},
                         {"match_id", match_id}, {"join_token", *match->second.join_token}};
  }

  if (!is_member(session, match_id)) return error("connection is not a member of target match");

  if (match->second.is_aborted) {
    if (type == "match_status") {
      const auto joined = static_cast<std::size_t>(std::count_if(match->second.player_sessions.begin(),
          match->second.player_sessions.end(), [](const auto& item) { return item.has_value(); }));
      return public_status(match_id, match->second.state_version, match->second.match->state(), joined,
                           match->second.player_sessions.size(),
                           match->second.last_market, match->second.last_vote,
                           match->second.timer, now_(), match->second.is_aborted);
    }
    return error("match has been aborted");
  }

  const bool player_command = type == "trade" || type == "request_clue" ||
      type == "vote" || type == "insider_hint" || type == "research" || type == "scout";
  const bool moderator_command = type == "start_match" || type == "publish_event" ||
      type == "open_trading" || type == "resolve_market" || type == "open_voting" ||
      type == "resolve_votes" || type == "pause" || type == "resume" || type == "abort";
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
    return public_status(match_id, match->second.state_version, match->second.match->state(), joined,
                         match->second.player_sessions.size(),
                         match->second.last_market, match->second.last_vote,
                         match->second.timer, now_(), match->second.is_aborted);
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
    ++match->second.state_version;
    const auto members = get_match_members(match->second);
    const auto& st = match->second.match->state();
    const auto now = now_();
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "match_started"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", Match::to_string(st.phase)},
                      {"state_version", match->second.state_version}}});
    pending_notifications_.push_back(OutgoingNotification{
        members,
        make_market_snapshot(match_id, match->second.state_version, st, match->second.last_market,
                             match->second.timer, now, match->second.is_aborted)});
    return network::json{{"type", "transition_result"}, {"ok", true},
                         {"phase", Match::to_string(match->second.match->state().phase)}};
  }
  if (type == "publish_event") {
    if (match->second.timer.paused) return error("match is paused");
    const auto impact = read_impact(command);
    if (!impact) return error("integer impact in [-2,2] is required");
    const auto result = match->second.match->publish_event(*impact);
    if (!result) return operation_error(result);
    ++match->second.state_version;
    const auto now = now_();
    match->second.timer.active = true;
    match->second.timer.paused = false;
    match->second.timer.phase = Phase::Discussion;
    match->second.timer.total_duration = phase_durations_.discussion;
    match->second.timer.deadline = now + phase_durations_.discussion;
    match->second.timer.paused_remaining = phase_durations_.discussion;
    match->second.timer.warning_30_fired = false;
    match->second.timer.warning_10_fired = false;

    const auto members = get_match_members(match->second);
    const auto& st = match->second.match->state();
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "phase_started"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", Match::to_string(st.phase)},
                      {"seconds_remaining", static_cast<std::uint64_t>(phase_durations_.discussion.count())},
                      {"state_version", match->second.state_version}}});
    pending_notifications_.push_back(OutgoingNotification{
        members,
        make_market_snapshot(match_id, match->second.state_version, st, match->second.last_market,
                             match->second.timer, now, match->second.is_aborted)});
    return network::json{{"type", "transition_result"}, {"ok", true},
                         {"phase", Match::to_string(match->second.match->state().phase)},
                         {"round", match->second.match->state().round}};
  }
  if (type == "open_trading") {
    if (match->second.timer.paused) return error("match is paused");
    const auto result = apply_open_trading(match_id, match->second, now_());
    if (!result) return operation_error(result);
    return network::json{{"type", "transition_result"}, {"ok", true},
                         {"phase", Match::to_string(match->second.match->state().phase)},
                         {"round", match->second.match->state().round}};
  }
  if (type == "open_voting") {
    if (match->second.timer.paused) return error("match is paused");
    if (command.value("with_transition", false)) {
      if (match->second.timer.in_transition) return error("transition interval already active");
      start_transition_interval(match_id, match->second, Phase::Voting, now_());
      return network::json{{"type", "transition_result"}, {"ok", true},
                           {"phase", Match::to_string(match->second.match->state().phase)},
                           {"upcoming_phase", "Voting"},
                           {"in_transition", true},
                           {"round", match->second.match->state().round}};
    }
    const auto result = apply_open_voting(match_id, match->second, now_());
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

    Shares quantity = 1;
    if (command.contains("quantity")) {
      if (!command["quantity"].is_number_integer() && !command["quantity"].is_number_unsigned()) {
        return error("quantity must be a positive integer");
      }
      const auto raw_qty = command["quantity"].get<std::int64_t>();
      if (raw_qty <= 0 || static_cast<std::uint64_t>(raw_qty) > std::numeric_limits<Shares>::max()) {
        return error("quantity must be a positive integer");
      }
      quantity = static_cast<Shares>(raw_qty);
    }

    const auto result = match->second.match->submit_trade(*player_id, action, quantity);
    if (!result) return operation_error(result);
    return network::json{{"type", "trade_result"}, {"ok", true}, {"code", "Accepted"}, {"quantity", quantity}};
  }
  if (type == "resolve_market") {
    if (match->second.timer.paused) return error("match is paused");
    auto result = apply_resolve_market(match_id, match->second, now_());
    if (!result) return operation_error(result.status);
    const auto& market = *result.value;
    return network::json{{"type", "market_result"}, {"ok", true},
        {"executed_buys", market.executed_buys}, {"executed_sells", market.executed_sells},
        {"closing_health", market.closing_health}, {"closing_price", market.closing_price},
        {"closing_reserve", market.closing_reserve}, {"outcome", Match::to_string(market.outcome)}};
  }
  if (type == "request_clue") {
    if (!player_id) return error("player membership required");
    if (match->second.timer.in_transition || match->second.match->state().phase != Phase::Clue) {
      return error("clue phase has ended");
    }
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
    if (match->second.timer.paused) return error("match is paused");
    auto result = apply_resolve_votes(match_id, match->second, now_());
    if (!result) return operation_error(result.status);
    const auto& vote = *result.value;
    return network::json{{"type", "vote_resolution"}, {"ok", true},
        {"eliminated_player", vote.eliminated ? network::json(*vote.eliminated) : network::json(nullptr)},
        {"votes_received", vote.votes_received},
        {"votes_cast", vote.votes_cast},
        {"votes_submitted", vote.votes_cast},
        {"votes_abstained", vote.votes_abstained},
        {"votes_missing", vote.votes_missing},
        {"eligible_voters", vote.eligible_voters},
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
  if (type == "research" || type == "scout") {
    if (!player_id) return error("player membership required");
    OperationResult op_result;
    const auto outcome = match->second.match->conduct_research(*player_id, op_result);
    if (!op_result) return operation_error(op_result);
    if (!outcome) return error("research operation returned no result");
    return network::json{{"type", "research_result"},
                         {"ok", true},
                         {"match_id", match_id},
                         {"direction", outcome->direction},
                         {"already_used", outcome->already_used},
                         {"credits_remaining", outcome->credits_remaining}};
  }
  if (type == "pause") {
    if (match->second.is_aborted || match->second.match->state().phase == Phase::Finished) {
      return error("match is already finished");
    }
    if (match->second.match->state().phase == Phase::Lobby) {
      return error("match has not started");
    }
    if (match->second.timer.paused) return error("match is already paused");

    match->second.timer.paused = true;
    const auto now = now_();
    if (match->second.timer.active && match->second.timer.deadline > now) {
      match->second.timer.paused_remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(match->second.timer.deadline - now);
    } else {
      match->second.timer.paused_remaining = std::chrono::milliseconds{0};
    }
    ++match->second.state_version;
    const auto members = get_match_members(match->second);
    const auto& st = match->second.match->state();
    const auto sec_rem = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(match->second.timer.paused_remaining).count());
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "match_paused"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", Match::to_string(st.phase)},
                      {"seconds_remaining", sec_rem},
                      {"state_version", match->second.state_version}}});
    pending_notifications_.push_back(OutgoingNotification{
        members,
        make_market_snapshot(match_id, match->second.state_version, st, match->second.last_market,
                             match->second.timer, now, match->second.is_aborted)});
    return network::json{{"type", "pause_result"}, {"ok", true},
                         {"match_id", match_id}, {"seconds_remaining", sec_rem}};
  }
  if (type == "resume") {
    if (match->second.is_aborted || match->second.match->state().phase == Phase::Finished) {
      return error("match is already finished");
    }
    if (!match->second.timer.paused) return error("match is not paused");

    match->second.timer.paused = false;
    const auto now = now_();
    if (match->second.timer.active) {
      match->second.timer.deadline = now + match->second.timer.paused_remaining;
    }
    ++match->second.state_version;
    const auto members = get_match_members(match->second);
    const auto& st = match->second.match->state();
    const auto sec_rem = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(match->second.timer.paused_remaining).count());
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "match_resumed"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", Match::to_string(st.phase)},
                      {"seconds_remaining", sec_rem},
                      {"state_version", match->second.state_version}}});
    pending_notifications_.push_back(OutgoingNotification{
        members,
        make_market_snapshot(match_id, match->second.state_version, st, match->second.last_market,
                             match->second.timer, now, match->second.is_aborted)});
    return network::json{{"type", "resume_result"}, {"ok", true},
                         {"match_id", match_id}, {"seconds_remaining", sec_rem}};
  }
  if (type == "abort") {
    if (match->second.is_aborted || match->second.match->state().phase == Phase::Finished) {
      return error("match is already finished");
    }
    match->second.is_aborted = true;
    security::TokenStore::remove_match_token(match_id);
    match->second.timer.active = false;
    match->second.timer.paused = false;
    match->second.timer.in_transition = false;
    ++match->second.state_version;
    const auto members = get_match_members(match->second);
    const auto& st = match->second.match->state();
    const auto now = now_();
    pending_notifications_.push_back(OutgoingNotification{
        members,
        network::json{{"type", "server_event"}, {"event", "match_aborted"},
                      {"match_id", match_id}, {"round", st.round},
                      {"phase", "Finished"}, {"outcome", "Aborted"},
                      {"state_version", match->second.state_version}}});
    pending_notifications_.push_back(OutgoingNotification{
        members,
        make_market_snapshot(match_id, match->second.state_version, st, match->second.last_market,
                             match->second.timer, now, match->second.is_aborted)});
    return network::json{{"type", "abort_result"}, {"ok", true}, {"match_id", match_id}};
  }
  return error("unsupported command");
}

}  // namespace darkpool::server
