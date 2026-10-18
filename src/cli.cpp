#include "darkpool/cli.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <sstream>

namespace darkpool::cli {
namespace {

std::string trim(std::string_view sv) {
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
  while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
  return std::string(sv);
}

std::string to_upper(std::string_view sv) {
  std::string s(sv);
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::toupper(c));
  });
  return s;
}

std::vector<std::string> split_words(std::string_view line) {
  std::vector<std::string> words;
  std::string current;
  for (const char c : line) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!current.empty()) {
        words.push_back(std::move(current));
        current.clear();
      }
    } else {
      current.push_back(c);
    }
  }
  if (!current.empty()) words.push_back(std::move(current));
  return words;
}

std::optional<std::uint64_t> parse_uint64(std::string_view sv) {
  const std::string text(sv);
  std::uint64_t value = 0;
  const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
  if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

std::optional<std::int32_t> parse_int32(std::string_view sv) {
  const std::string text(sv);
  std::int32_t value = 0;
  const auto res = std::from_chars(text.data(), text.data() + text.size(), value);
  if (res.ec != std::errc{} || res.ptr != text.data() + text.size()) return std::nullopt;
  return value;
}

}  // namespace

ClientState::ClientState(ClientRole role) : role_(role) {}

std::string ClientState::prompt() const {
  if (role_ == ClientRole::Moderator) {
    if (match_id_) {
      return "[Moderator Match #" + std::to_string(*match_id_) + "] > ";
    }
    return "[Moderator] > ";
  }
  if (player_id_) {
    if (private_role_) {
      return "[Player " + std::to_string(*player_id_) + " (" + *private_role_ + ")] > ";
    }
    return "[Player " + std::to_string(*player_id_) + "] > ";
  }
  return "[Player] > ";
}

std::string ClientState::help_text(ClientRole role) {
  std::ostringstream oss;
  if (role == ClientRole::Moderator) {
    oss << "============================================================\n"
        << "DARKPOOL MODERATOR COMMANDS:\n"
        << "  create [player_count]     Create a 3-64 player match (default: 5; roles assigned by server)\n"
        << "  match <id>                Select active match ID (default: 1)\n"
        << "  start                     Start match once all configured players have joined\n"
        << "  event <impact>            Publish business event impact (-2 to 2)\n"
        << "  trading                   Open trading phase for player orders\n"
        << "  market                    Resolve market orders and display price/health changes\n"
        << "  voting                    Open voting phase for player elimination votes\n"
        << "  resolve                   Resolve votes, announce eliminations, and check victory\n"
        << "  status                    Display public match status and latest board state\n"
        << "  pause                     Pause the active match timer\n"
        << "  resume                    Resume the active match timer\n"
        << "  abort                     Abort and terminate the active match\n"
        << "  help                      Show this help screen\n"
        << "  quit / exit               Exit CLI\n"
        << "  {...}                     Pass raw JSON directly to the server\n"
        << "============================================================";
  } else {
    oss << "============================================================\n"
        << "DARKPOOL PLAYER COMMANDS:\n"
        << "  join [match_id]           Join match (default: 1) and receive private role\n"
        << "  status                    View public company health, stock price, and round phase\n"
        << "  hint                      [Insider only] Secretly inspect current round event impact\n"
        << "  research / scout          Cost 10 credits, once/match. Private event direction signal\n"
        << "  trade [BUY|SELL|HOLD] [q] Submit order during Trading phase (or: buy [q] / sell [q] / hold)\n"
        << "  clue [target_id]          [Investigator only] Secretly investigate suspect (1-64)\n"
        << "  vote [target_id|abstain]  Cast vote to eliminate suspect (1-64) or abstain (0)\n"
        << "  help                      Show this help screen\n"
        << "  quit / exit               Exit CLI\n"
        << "  {...}                     Pass raw JSON directly to the server\n"
        << "============================================================";
  }
  return oss.str();
}

CommandResult ClientState::process_input(std::string_view raw_line) {
  const auto trimmed = trim(raw_line);
  if (trimmed.empty()) {
    return CommandResult{};
  }

  // Raw JSON bypass for protocol pass-through / testing
  if (trimmed.front() == '{') {
    try {
      auto parsed = network::json::parse(trimmed);
      return CommandResult{.send_frame = true, .payload = std::move(parsed)};
    } catch (const std::exception& e) {
      return CommandResult{.local_message = std::string("[ERROR] Invalid JSON: ") + e.what()};
    }
  }

  const auto words = split_words(trimmed);
  if (words.empty()) return CommandResult{};

  const auto cmd = to_upper(words[0]);
  if (cmd == "QUIT" || cmd == "EXIT") {
    return CommandResult{.should_exit = true, .local_message = "Disconnecting..."};
  }
  if (cmd == "HELP") {
    return CommandResult{.local_message = help_text(role_)};
  }

  std::vector<std::string> args;
  for (std::size_t i = 1; i < words.size(); ++i) {
    args.push_back(words[i]);
  }

  if (role_ == ClientRole::Moderator) {
    return handle_moderator_command(cmd, args);
  }
  return handle_player_command(cmd, args);
}

CommandResult ClientState::handle_moderator_command(std::string_view command,
                                                    const std::vector<std::string>& args) {
  const auto active_match = match_id_.value_or(1U);

  if (command == "CREATE") {
    if (args.size() > 1U) return CommandResult{.local_message = "[ERROR] Use 'create [player_count]' (3-64)."};
    if (!args.empty()) {
      const auto count = parse_uint64(args[0]);
      if (!count || *count < 3U || *count > 64U)
        return CommandResult{.local_message = "[ERROR] Player count must be an integer from 3 through 64."};
      return CommandResult{.send_frame = true,
                           .payload = {{"type", "create_match"}, {"player_count", *count}}};
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "create_match"}}};
  }

  if (command == "MATCH") {
    if (args.empty()) {
      return CommandResult{.local_message = "[INFO] Current match ID: " + std::to_string(active_match)};
    }
    const auto parsed = parse_uint64(args[0]);
    if (!parsed || *parsed == 0U) {
      return CommandResult{.local_message = "[ERROR] Invalid match ID."};
    }
    match_id_ = *parsed;
    return CommandResult{.local_message = "[INFO] Active match set to #" + std::to_string(*parsed)};
  }

  if (command == "START") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "start_match"}, {"match_id", target_match}}};
  }

  if (command == "EVENT") {
    std::uint64_t target_match = active_match;
    if (args.empty()) {
      pending_prompt_ = PendingPrompt::EventImpact;
      return CommandResult{.prompt_for_input = "Enter business event impact (-2 to 2): "};
    }
    const auto impact = parse_int32(args[0]);
    if (!impact || *impact < -2 || *impact > 2) {
      return CommandResult{.local_message = "[ERROR] Impact must be an integer between -2 and 2."};
    }
    if (args.size() > 1U) {
      if (const auto p = parse_uint64(args[1]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "publish_event"},
                                     {"match_id", target_match},
                                     {"impact", *impact}}};
  }

  if (command == "TRADING" || command == "OPEN_TRADING" || command == "OPEN-TRADING") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "open_trading"}, {"match_id", target_match}}};
  }

  if (command == "MARKET" || command == "RESOLVE_MARKET" || command == "RESOLVE-MARKET") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "resolve_market"}, {"match_id", target_match}}};
  }

  if (command == "VOTING" || command == "OPEN_VOTING" || command == "OPEN-VOTING") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "open_voting"}, {"match_id", target_match}}};
  }

  if (command == "RESOLVE" || command == "RESOLVE_VOTES" || command == "RESOLVE-VOTES") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "resolve_votes"}, {"match_id", target_match}}};
  }

  if (command == "STATUS") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "match_status"}, {"match_id", target_match}}};
  }

  if (command == "PAUSE") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "pause"}, {"match_id", target_match}}};
  }

  if (command == "RESUME") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "resume"}, {"match_id", target_match}}};
  }

  if (command == "ABORT") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "abort"}, {"match_id", target_match}}};
  }

  return CommandResult{.local_message = "[ERROR] Unknown moderator command: '" + std::string(command) + "'. Type 'help' for commands."};
}

CommandResult ClientState::handle_player_command(std::string_view command,
                                                 const std::vector<std::string>& args) {
  const auto active_match = match_id_.value_or(1U);

  if (command == "JOIN") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      const auto parsed = parse_uint64(args[0]);
      if (!parsed || *parsed == 0U) {
        return CommandResult{.local_message = "[ERROR] Invalid match ID to join."};
      }
      target_match = *parsed;
    }
    match_id_ = target_match;
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "join_match"}, {"match_id", target_match}}};
  }

  if (command == "STATUS") {
    std::uint64_t target_match = active_match;
    if (!args.empty()) {
      if (const auto p = parse_uint64(args[0]); p && *p > 0U) target_match = *p;
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "match_status"}, {"match_id", target_match}}};
  }

  if (command == "HINT" || command == "INSIDER_HINT" || command == "INTEL") {
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "insider_hint"}, {"match_id", active_match}}};
  }

  if (command == "BUY" || command == "SELL" || command == "HOLD") {
    Shares quantity = 1;
    if (!args.empty() && command != "HOLD") {
      const auto parsed = parse_uint64(args[0]);
      if (!parsed || *parsed == 0U || *parsed > std::numeric_limits<Shares>::max()) {
        return CommandResult{.local_message = "[ERROR] Quantity must be a positive integer."};
      }
      quantity = static_cast<Shares>(*parsed);
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "trade"},
                                     {"match_id", active_match},
                                     {"action", std::string(command)},
                                     {"quantity", quantity}}};
  }

  if (command == "TRADE") {
    if (args.empty()) {
      pending_prompt_ = PendingPrompt::TradeAction;
      return CommandResult{.prompt_for_input = "Choose trade action (BUY, SELL, HOLD): "};
    }
    const auto action_upper = to_upper(args[0]);
    if (action_upper != "BUY" && action_upper != "SELL" && action_upper != "HOLD") {
      return CommandResult{.local_message = "[ERROR] Action must be BUY, SELL, or HOLD."};
    }
    Shares quantity = 1;
    if (args.size() > 1U && action_upper != "HOLD") {
      const auto parsed = parse_uint64(args[1]);
      if (!parsed || *parsed == 0U || *parsed > std::numeric_limits<Shares>::max()) {
        return CommandResult{.local_message = "[ERROR] Quantity must be a positive integer."};
      }
      quantity = static_cast<Shares>(*parsed);
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "trade"},
                                     {"match_id", active_match},
                                     {"action", action_upper},
                                     {"quantity", quantity}}};
  }

  if (command == "RESEARCH" || command == "SCOUT") {
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "research"}, {"match_id", active_match}}};
  }

  if (command == "CLUE" || command == "INVESTIGATE") {
    if (args.empty()) {
      pending_prompt_ = PendingPrompt::ClueTarget;
      return CommandResult{.prompt_for_input = "Enter suspect player ID to investigate (1-64): "};
    }
    const auto target = parse_uint64(args[0]);
    if (!target || *target < 1U || *target > 64U) {
      return CommandResult{.local_message = "[ERROR] Clue target must be a valid player ID (1-64)."};
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "request_clue"},
                                     {"match_id", active_match},
                                     {"target_id", *target}}};
  }

  if (command == "VOTE") {
    if (args.empty()) {
      pending_prompt_ = PendingPrompt::VoteTarget;
      return CommandResult{.prompt_for_input = "Enter player ID to eliminate (1-64, or 0/abstain to abstain): "};
    }
    const auto upper_arg = to_upper(args[0]);
    if (upper_arg == "0" || upper_arg == "ABSTAIN" || upper_arg == "NONE" || upper_arg == "NULL") {
      return CommandResult{.send_frame = true,
                           .payload = {{"type", "vote"},
                                       {"match_id", active_match},
                                       {"target_id", nullptr}}};
    }
    const auto target = parse_uint64(args[0]);
    if (!target || *target < 1U || *target > 64U) {
      return CommandResult{.local_message = "[ERROR] Vote target must be 1-64 or 0/abstain."};
    }
    return CommandResult{.send_frame = true,
                         .payload = {{"type", "vote"},
                                     {"match_id", active_match},
                                     {"target_id", *target}}};
  }

  return CommandResult{.local_message = "[ERROR] Unknown player command: '" + std::string(command) + "'. Type 'help' for commands."};
}

CommandResult ClientState::process_prompt_response(std::string_view response) {
  const auto trimmed = trim(response);
  const auto active_match = match_id_.value_or(1U);
  const auto current_prompt = pending_prompt_;
  pending_prompt_ = PendingPrompt::None;

  switch (current_prompt) {
    case PendingPrompt::TradeAction: {
      const auto action = to_upper(trimmed);
      if (action != "BUY" && action != "SELL" && action != "HOLD") {
        return CommandResult{.local_message = "[ERROR] Trade cancelled: action must be BUY, SELL, or HOLD."};
      }
      return CommandResult{.send_frame = true,
                           .payload = {{"type", "trade"},
                                       {"match_id", active_match},
                                       {"action", action}}};
    }
    case PendingPrompt::ClueTarget: {
      const auto target = parse_uint64(trimmed);
      if (!target || *target < 1U || *target > 64U) {
        return CommandResult{.local_message = "[ERROR] Clue cancelled: target must be 1-64."};
      }
      return CommandResult{.send_frame = true,
                           .payload = {{"type", "request_clue"},
                                       {"match_id", active_match},
                                       {"target_id", *target}}};
    }
    case PendingPrompt::VoteTarget: {
      const auto upper_resp = to_upper(trimmed);
      if (upper_resp == "0" || upper_resp == "ABSTAIN" || upper_resp == "NONE" || upper_resp == "NULL") {
        return CommandResult{.send_frame = true,
                             .payload = {{"type", "vote"},
                                         {"match_id", active_match},
                                         {"target_id", nullptr}}};
      }
      const auto target = parse_uint64(trimmed);
      if (!target || *target < 1U || *target > 64U) {
        return CommandResult{.local_message = "[ERROR] Vote cancelled: target must be 1-64 or abstain."};
      }
      return CommandResult{.send_frame = true,
                           .payload = {{"type", "vote"},
                                       {"match_id", active_match},
                                       {"target_id", *target}}};
    }
    case PendingPrompt::EventImpact: {
      const auto impact = parse_int32(trimmed);
      if (!impact || *impact < -2 || *impact > 2) {
        return CommandResult{.local_message = "[ERROR] Event cancelled: impact must be -2 to 2."};
      }
      return CommandResult{.send_frame = true,
                           .payload = {{"type", "publish_event"},
                                       {"match_id", active_match},
                                       {"impact", *impact}}};
    }
    case PendingPrompt::JoinMatch: {
      std::uint64_t target = active_match;
      if (!trimmed.empty()) {
        if (const auto p = parse_uint64(trimmed); p && *p > 0U) target = *p;
      }
      match_id_ = target;
      return CommandResult{.send_frame = true,
                           .payload = {{"type", "join_match"}, {"match_id", target}}};
    }
    case PendingPrompt::None:
      break;
  }
  return CommandResult{};
}

std::string ClientState::format_response(const network::json& response) {
  if (!response.is_object()) {
    return "[SERVER] " + response.dump();
  }
  if (response.empty()) {
    return "";
  }

  if (response.contains("state_version") &&
      (response["state_version"].is_number_unsigned() || response["state_version"].is_number_integer())) {
    const auto version = response["state_version"].get<std::uint64_t>();
    if (version < latest_state_version_) {
      // Ignore stale update
      return "";
    }
    if (version > latest_state_version_) {
      latest_state_version_ = version;
    }
  }

  if (response.contains("match_id") && !match_id_ &&
      (response["match_id"].is_number_unsigned() || response["match_id"].is_number_integer())) {
    match_id_ = response["match_id"].get<std::uint64_t>();
  }

  const auto type = response.value("type", std::string{});
  std::ostringstream oss;

  if (type == "server_event") {
    const auto event = response.value("event", std::string{});
    const auto round = response.value("round", 0U);
    const auto phase = response.value("phase", std::string{});
    if (event == "match_started") {
      oss << "[MATCH STARTED] Match has begun.";
      if (round > 0U) oss << " Round " << round << " (" << phase << ").";
      return oss.str();
    }
    if (event == "phase_started") {
      if (phase == "Voting") {
        oss << "[VOTING STARTED] Voting is now open";
        if (round > 0U) oss << " — Round " << round << ".";
        else oss << ".";
      } else {
        oss << "[PHASE START] " << phase;
        if (round > 0U) oss << " — Round " << round << ".";
        else oss << ".";
      }
      return oss.str();
    }
    if (event == "round_started") {
      oss << "[ROUND START] Round " << round << " has begun.";
      return oss.str();
    }
    if (event == "phase_warning") {
      oss << "[COUNTDOWN WARNING] " << phase << " phase has "
          << response.value("seconds_remaining", 0U) << " seconds remaining!";
      return oss.str();
    }
    if (event == "transition_interval") {
      oss << "[TRANSITION INTERVAL] Upcoming phase: "
          << response.value("upcoming_phase", "Voting")
          << " (" << response.value("seconds_remaining", 10U) << "s remaining).";
      return oss.str();
    }
    if (event == "match_paused") {
      oss << "[MATCH PAUSED] Timer frozen at "
          << response.value("seconds_remaining", 0U) << " seconds remaining.";
      return oss.str();
    }
    if (event == "match_resumed") {
      oss << "[MATCH RESUMED] Timer resumed with "
          << response.value("seconds_remaining", 0U) << " seconds remaining.";
      return oss.str();
    }
    if (event == "match_aborted") {
      oss << "============================================================\n"
          << "[MATCH ABORTED] The match has been aborted by the moderator.\n"
          << "============================================================";
      return oss.str();
    }
    if (event == "vote_resolved") {
      oss << "============================================================\n"
          << "[VOTE RESOLVED]\n";
      if (response.contains("eliminated_player") && !response["eliminated_player"].is_null()) {
        oss << "  Elimination    : Player " << response["eliminated_player"].get<PlayerId>() << " was eliminated.\n";
      } else {
        oss << "  Elimination    : None (tie or no majority reached)\n";
      }
      oss << "  Votes Received : " << response.value("votes_received", 0U) << " / "
          << response.value("eligible_voters", 0U) << " eligible voters\n"
          << "  Round Outcome  : " << response.value("outcome", "None") << "\n"
          << "============================================================";
      return oss.str();
    }
    if (event == "match_finished") {
      const auto outcome = response.value("outcome", "None");
      oss << "============================================================\n"
          << "[MATCH FINISHED] Final outcome: " << outcome << "\n";
      if (outcome == "InsiderWin") {
        oss << "  The INSIDER team has achieved victory!\n";
      } else if (outcome == "EmployeesWin") {
        oss << "  The EMPLOYEES & INVESTIGATOR team has achieved victory!\n";
      }
      oss << "============================================================";
      return oss.str();
    }
    oss << "[SERVER EVENT] " << event;
    if (!phase.empty()) oss << " — " << phase;
    if (round > 0U) oss << " (Round " << round << ")";
    return oss.str();
  }

  if (type == "market_snapshot") {
    const auto match_id = response.value("match_id", 0ULL);
    const auto version = response.value("state_version", 0ULL);
    oss << "============================================================\n"
        << "[MARKET SNAPSHOT] Match #" << match_id << " (Version " << version << ")\n"
        << "------------------------------------------------------------\n"
        << "  Phase          : " << response.value("phase", "Unknown") << "\n"
        << "  Round          : " << response.value("round", 0U) << " / 3\n"
        << "  Company Health : " << response.value("health", 0) << " / 10\n"
        << "  Stock Price    : " << response.value("price", 0) << "\n";
    if (response.contains("sentiment")) {
      oss << "  Sentiment      : " << response.value("sentiment", 0) << "\n";
    }
    if (response.contains("volatility")) {
      oss << "  Volatility     : " << response.value("volatility", 0) << "\n";
    }
    if (response.contains("liquidity")) {
      oss << "  Liquidity      : " << response.value("liquidity", 0) << "%\n";
    }
    oss << "  Total Supply   : " << response.value("total_supply", 0U) << "\n"
        << "  Reserve Shares : " << response.value("reserve", 0U) << "\n"
        << "  Player Shares  : " << response.value("shares_held_by_players", 0U) << "\n"
        << "  Status Outcome : " << response.value("outcome", "None") << "\n";
    if (response.value("is_paused", false)) {
      oss << "  Timer Status   : PAUSED (" << response.value("seconds_remaining", 0U) << "s remaining)\n";
    } else if (response.contains("seconds_remaining") && response["seconds_remaining"].get<std::uint64_t>() > 0U) {
      oss << "  Time Remaining : " << response["seconds_remaining"].get<std::uint64_t>() << "s\n";
    }
    if (response.contains("last_market") && response["last_market"].is_object()) {
      const auto& lm = response["last_market"];
      oss << "------------------------------------------------------------\n"
          << "  Last Market Resolution:\n"
          << "    Buys: " << lm.value("executed_buys", 0U)
          << " | Sells: " << lm.value("executed_sells", 0U)
          << " | Health: " << lm.value("closing_health", 0)
          << " | Price: " << lm.value("closing_price", 0)
          << " | Reserve: " << lm.value("closing_reserve", 0U);
      if (lm.contains("sentiment")) {
        oss << " | Sent: " << lm.value("sentiment", 0);
      }
      if (lm.contains("volatility")) {
        oss << " | Vol: " << lm.value("volatility", 0);
      }
      if (lm.contains("liquidity")) {
        oss << " | Liq: " << lm.value("liquidity", 0) << "%";
      }
      oss << "\n";
    }
    oss << "============================================================";
    return oss.str();
  }

  if (type == "match_created") {
    if (response.contains("match_id")) {
      const auto id = response["match_id"].get<std::uint64_t>();
      match_id_ = id;
      oss << "============================================================\n"
          << "[MATCH CREATED] Match #" << id << " created successfully.\n"
          << "  Roster: " << response.value("insiders", 1U) << " Insiders, "
          << response.value("investigators", 1U) << " Investigators, "
          << response.value("employees", 3U) << " Employees\n"
          << "  Waiting for " << response.value("player_count", 5U)
          << " players to connect and run: join " << id << "\n"
          << "============================================================";
      return oss.str();
    }
  }

  if (type == "joined_match") {
    if (response.contains("match_id")) match_id_ = response["match_id"].get<std::uint64_t>();
    if (response.contains("player_id")) player_id_ = response["player_id"].get<PlayerId>();
    if (response.contains("private_role")) private_role_ = response["private_role"].get<std::string>();

    oss << "============================================================\n"
        << "[MATCH JOINED] Match #" << (match_id_ ? *match_id_ : 0U) << "\n"
        << "  Player ID   : " << (player_id_ ? *player_id_ : 0U) << "\n"
        << "  Seat        : " << response.value("seat", 0U) << "\n"
        << "  Private Role: " << (private_role_ ? *private_role_ : "Unknown") << "\n"
        << "------------------------------------------------------------\n";
    if (private_role_ && *private_role_ == "Insider") {
      oss << "CONFIDENTIAL INSIDER INTEL:\n"
          << "  You are the INSIDER.\n"
          << "  Use 'hint' during Discussion to secretly preview the event impact.\n"
          << "  Victory: Drive company health to 0 or survive through Round 3.\n";
    } else if (private_role_ && *private_role_ == "Investigator") {
      oss << "CONFIDENTIAL INVESTIGATOR INTEL:\n"
          << "  You are the INVESTIGATOR.\n"
          << "  Use 'clue <player_id>' during Clue phase to investigate suspects.\n"
          << "  Victory: Help employees uncover and vote out the Insider team.\n";
    } else {
      oss << "CONFIDENTIAL EMPLOYEE INTEL:\n"
          << "  You are an EMPLOYEE.\n"
          << "  Submit orders ('buy'/'sell'/'hold') during Trading, and vote out suspects.\n"
          << "  Victory: Maintain company health and eliminate the Insider team.\n";
    }
    oss << "============================================================";
    return oss.str();
  }

  if (type == "hint_private") {
    oss << "============================================================\n"
        << "[CONFIDENTIAL INSIDER HINT]\n"
        << "  Business Event Impact: " << response.value("event_impact", 0) << "\n"
        << "  (Negative hurts company health/stock; positive benefits company)\n"
        << "============================================================";
    return oss.str();
  }

  if (type == "clue_private") {
    const bool is_insider = response.value("is_insider", false);
    oss << "============================================================\n"
        << "[CONFIDENTIAL INVESTIGATION RESULT]\n"
        << "  Target Player is: " << (is_insider ? "*** THE INSIDER ***" : "NOT the Insider (Employee / Investigator)") << "\n"
        << "============================================================";
    return oss.str();
  }

  if (type == "research_result") {
    oss << "============================================================\n"
        << "[SCOUTING / RESEARCH REPORT]\n"
        << "  Match ID         : " << response.value("match_id", 0U) << "\n"
        << "  Signal Direction : " << response.value("direction", "Unknown") << "\n";
    if (response.value("already_used", false)) {
      oss << "  Status           : Already used in this match (no fee charged)\n";
    } else {
      oss << "  Cost             : 10 Credits debited\n";
    }
    oss << "  Credits Balance  : " << response.value("credits_remaining", 0) << "\n"
        << "============================================================";
    return oss.str();
  }

  if (type == "transition_result") {
    const auto phase = response.value("phase", "Unknown");
    const auto round = response.value("round", 0U);
    oss << "[PHASE TRANSITION] Phase is now: " << phase;
    if (round > 0U) oss << " (Round " << round << ")";
    return oss.str();
  }

  if (type == "trade_result") {
    if (response.value("ok", false)) {
      if (response.contains("quantity") && response["quantity"].get<std::uint64_t>() > 1U) {
        return "[TRADE SUCCESS] Your trade order (" + std::to_string(response["quantity"].get<std::uint64_t>()) + " shares) has been accepted.";
      }
      return "[TRADE SUCCESS] Your trade order has been accepted.";
    }
    return "[TRADE FAILED] Trade order was rejected.";
  }

  if (type == "vote_result") {
    if (response.value("ok", false)) {
      return "[VOTE RECORDED] Your vote has been recorded.";
    }
    return "[VOTE FAILED] Vote was rejected.";
  }

  if (type == "market_result") {
    const auto outcome = response.value("outcome", "None");
    oss << "============================================================\n"
        << "[MARKET RESOLUTION]\n"
        << "  Executed Buys  : " << response.value("executed_buys", 0U) << "\n"
        << "  Executed Sells : " << response.value("executed_sells", 0U) << "\n"
        << "  Closing Health : " << response.value("closing_health", 0) << " / 10\n"
        << "  Closing Price  : " << response.value("closing_price", 0) << "\n"
        << "  Reserve Shares : " << response.value("closing_reserve", 0U) << "\n"
        << "  Phase Outcome  : " << outcome << "\n";
    if (outcome == "InsiderWin") {
      oss << "************************************************************\n"
          << "[GAME OVER] The company has fallen! The INSIDER WINS!\n"
          << "************************************************************\n";
    } else if (outcome == "EmployeesWin") {
      oss << "************************************************************\n"
          << "[GAME OVER] The EMPLOYEES & INVESTIGATOR WIN!\n"
          << "************************************************************\n";
    }
    oss << "============================================================";
    return oss.str();
  }

  if (type == "vote_resolution") {
    const auto outcome = response.value("outcome", "None");
    oss << "============================================================\n"
        << "[VOTE RESOLUTION]\n";
    if (response.contains("eliminated_player") && !response["eliminated_player"].is_null()) {
      const auto elim = response["eliminated_player"].get<PlayerId>();
      oss << "  ELIMINATION    : Player " << elim << " WAS ELIMINATED!\n";
    } else {
      oss << "  ELIMINATION    : None (tie or no majority reached)\n";
    }
    oss << "  Votes Received : " << response.value("votes_received", 0U) << " / "
        << response.value("eligible_voters", 0U) << " eligible voters\n"
        << "  Game Outcome   : " << outcome << "\n";
    if (outcome == "EmployeesWin") {
      oss << "************************************************************\n"
          << "[GAME OVER] The Insider was eliminated! EMPLOYEES & INVESTIGATOR WIN!\n"
          << "************************************************************\n";
    } else if (outcome == "InsiderWin") {
      oss << "************************************************************\n"
          << "[GAME OVER] The Insider survived! The INSIDER WINS!\n"
          << "************************************************************\n";
    }
    oss << "============================================================";
    return oss.str();
  }

  if (type == "pause_result") {
    if (response.value("ok", false)) {
      oss << "[MATCH PAUSED] Match #" << response.value("match_id", 0U)
          << " timer paused with " << response.value("seconds_remaining", 0U) << " seconds remaining.";
    } else {
      oss << "[PAUSE FAILED] Could not pause match.";
    }
    return oss.str();
  }

  if (type == "resume_result") {
    if (response.value("ok", false)) {
      oss << "[MATCH RESUMED] Match #" << response.value("match_id", 0U)
          << " timer resumed with " << response.value("seconds_remaining", 0U) << " seconds remaining.";
    } else {
      oss << "[RESUME FAILED] Could not resume match.";
    }
    return oss.str();
  }

  if (type == "abort_result") {
    if (response.value("ok", false)) {
      oss << "[MATCH ABORTED] Match #" << response.value("match_id", 0U)
          << " has been aborted and terminated.";
    } else {
      oss << "[ABORT FAILED] Could not abort match.";
    }
    return oss.str();
  }

  if (type == "match_status") {
    const auto outcome = response.value("outcome", "None");
    oss << "============================================================\n"
        << "MATCH #" << response.value("match_id", 0U) << " STATUS\n"
        << "------------------------------------------------------------\n"
        << "  Phase          : " << response.value("phase", "Unknown") << "\n"
        << "  Round          : " << response.value("round", 0U) << " / 3\n"
        << "  Active Players : " << response.value("joined_players", 0U) << " / "
        << response.value("player_count", 5U) << "\n"
        << "  Company Health : " << response.value("health", 0) << " / 10\n"
        << "  Stock Price    : " << response.value("price", 0) << "\n";
    if (response.contains("sentiment")) {
      oss << "  Sentiment      : " << response.value("sentiment", 0) << "\n";
    }
    if (response.contains("volatility")) {
      oss << "  Volatility     : " << response.value("volatility", 0) << "\n";
    }
    if (response.contains("liquidity")) {
      oss << "  Liquidity      : " << response.value("liquidity", 0) << "%\n";
    }
    oss << "  Outcome        : " << outcome << "\n";
    if (response.value("is_paused", false)) {
      oss << "  Timer Status   : PAUSED (" << response.value("seconds_remaining", 0U) << "s remaining)\n";
    } else if (response.contains("seconds_remaining") && response["seconds_remaining"].get<std::uint64_t>() > 0U) {
      oss << "  Time Remaining : " << response["seconds_remaining"].get<std::uint64_t>() << "s\n";
    }

    if (response.contains("last_market") && response["last_market"].is_object()) {
      const auto& lm = response["last_market"];
      oss << "------------------------------------------------------------\n"
          << "  Last Market Resolution:\n"
          << "    Buys: " << lm.value("executed_buys", 0U)
          << " | Sells: " << lm.value("executed_sells", 0U)
          << " | Health: " << lm.value("closing_health", 0)
          << " | Price: " << lm.value("closing_price", 0)
          << " | Reserve: " << lm.value("closing_reserve", 0U) << "\n";
    }

    if (response.contains("last_vote") && response["last_vote"].is_object()) {
      const auto& lv = response["last_vote"];
      oss << "------------------------------------------------------------\n"
          << "  Last Vote Resolution:\n";
      if (lv.contains("eliminated_player") && !lv["eliminated_player"].is_null()) {
        oss << "    Eliminated: Player " << lv["eliminated_player"].get<PlayerId>();
      } else {
        oss << "    Eliminated: None";
      }
      oss << " (Votes: " << lv.value("votes_received", 0U) << "/"
          << lv.value("eligible_voters", 0U) << ")\n";
    }

    if (outcome != "None") {
      oss << "************************************************************\n"
          << "[GAME OVER] Final Winner: " << outcome << "\n"
          << "************************************************************\n";
    }
    oss << "============================================================";
    return oss.str();
  }

  if (type == "operation_result") {
    oss << "[ACTION REJECTED] " << response.value("code", "Error");
    if (response.contains("detail") && !response["detail"].get<std::string>().empty()) {
      oss << ": " << response["detail"].get<std::string>();
    }
    return oss.str();
  }

  if (type == "error") {
    return "[ERROR] " + response.value("error", "Unknown error");
  }

  if (type == "connect") {
    return "[CONNECTED] Session established as " + response.value("role", "peer");
  }

  return "[SERVER] " + response.dump();
}

}  // namespace darkpool::cli
