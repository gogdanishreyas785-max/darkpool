#include "darkpool/cli.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace {

using darkpool::cli::ClientRole;
using darkpool::cli::ClientState;
using darkpool::network::json;

int failures = 0;
void check(bool condition, const char *expression, int line) {
  if (!condition) {
    ++failures;
    std::cerr << "FAIL line " << line << ": " << expression << '\n';
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

void test_moderator_commands() {
  ClientState mod(ClientRole::Moderator);
  CHECK(mod.prompt() == "[Moderator] > ");

  // Default create
  auto r = mod.process_input("create");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "create_match");
  CHECK(!r.payload.contains("player_count")); // omitted count preserves the five-player default
  CHECK(!r.payload.contains("roles")); // server owns the role assignment
  r = mod.process_input("create 9");
  CHECK(r.send_frame && r.payload["player_count"] == 9U);
  r = mod.process_input("create 65");
  CHECK(!r.send_frame && r.local_message.find("3 through 64") != std::string::npos);

  // Format match created
  const auto created_resp = json{{"type", "match_created"}, {"match_id", 42},
      {"player_count", 9}, {"insiders", 2}, {"investigators", 1}, {"employees", 6}};
  const auto formatted_created = mod.format_response(created_resp);
  CHECK(formatted_created.find("Match #42") != std::string::npos);
  CHECK(formatted_created.find("Waiting for 9 players") != std::string::npos);
  CHECK(mod.match_id() == 42U);
  CHECK(mod.prompt() == "[Moderator Match #42] > ");

  // Start
  r = mod.process_input("start");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "start_match");
  CHECK(r.payload["match_id"] == 42U);

  // Event with arg
  r = mod.process_input("event 1");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "publish_event");
  CHECK(r.payload["impact"] == 1);
  CHECK(r.payload["match_id"] == 42U);

  // Event negative
  r = mod.process_input("event -2");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "publish_event");
  CHECK(r.payload["impact"] == -2);

  // Event prompt flow
  r = mod.process_input("event");
  CHECK(!r.send_frame);
  CHECK(r.prompt_for_input.has_value());
  auto pr = mod.process_prompt_response("0");
  CHECK(pr.send_frame);
  CHECK(pr.payload["type"] == "publish_event");
  CHECK(pr.payload["impact"] == 0);

  // Event prompt invalid
  r = mod.process_input("event");
  pr = mod.process_prompt_response("5");
  CHECK(!pr.send_frame);
  CHECK(pr.local_message.find("impact must be -2 to 2") != std::string::npos);

  // Phase commands
  r = mod.process_input("trading");
  CHECK(r.send_frame && r.payload["type"] == "open_trading");
  r = mod.process_input("market");
  CHECK(r.send_frame && r.payload["type"] == "resolve_market");
  r = mod.process_input("voting");
  CHECK(r.send_frame && r.payload["type"] == "open_voting");
  r = mod.process_input("resolve");
  CHECK(r.send_frame && r.payload["type"] == "resolve_votes");
  r = mod.process_input("status");
  CHECK(r.send_frame && r.payload["type"] == "match_status");

  // Custom match selection
  r = mod.process_input("match 7");
  CHECK(mod.match_id() == 7U);
  CHECK(mod.prompt() == "[Moderator Match #7] > ");
}

void test_player_commands() {
  ClientState player(ClientRole::Player);
  CHECK(player.prompt() == "[Player] > ");

  // Join
  auto r = player.process_input("join 1");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "join_match");
  CHECK(r.payload["match_id"] == 1U);

  // Format joined match as Insider
  auto joined_resp = json{{"type", "joined_match"},
                          {"match_id", 1},
                          {"player_id", 1},
                          {"seat", 1},
                          {"private_role", "Insider"}};
  auto formatted = player.format_response(joined_resp);
  CHECK(formatted.find("INSIDER") != std::string::npos);
  CHECK(player.player_id() == 1U);
  CHECK(player.private_role() == "Insider");
  CHECK(player.prompt() == "[Player 1 (Insider)] > ");

  // Hint
  r = player.process_input("hint");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "insider_hint");
  CHECK(r.payload["match_id"] == 1U);

  // Format private hint
  auto hint_resp =
      json{{"type", "hint_private"}, {"match_id", 1}, {"event_impact", 2}};
  formatted = player.format_response(hint_resp);
  CHECK(formatted.find("Event Impact: 2") != std::string::npos);

  // Trade command with arg
  r = player.process_input("trade buy");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "trade");
  CHECK(r.payload["action"] == "BUY");

  // Trade shortcuts
  r = player.process_input("sell");
  CHECK(r.send_frame && r.payload["action"] == "SELL");
  r = player.process_input("hold");
  CHECK(r.send_frame && r.payload["action"] == "HOLD");

  // Trade prompt flow
  r = player.process_input("trade");
  CHECK(!r.send_frame);
  CHECK(r.prompt_for_input.has_value());
  auto pr = player.process_prompt_response("buy");
  CHECK(pr.send_frame && pr.payload["action"] == "BUY");

  // Clue command with arg
  r = player.process_input("clue 3");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "request_clue");
  CHECK(r.payload["target_id"] == 3U);

  // Clue prompt flow
  r = player.process_input("clue");
  CHECK(!r.send_frame);
  CHECK(r.prompt_for_input.has_value());
  pr = player.process_prompt_response("4");
  CHECK(pr.send_frame);
  CHECK(pr.payload["type"] == "request_clue");
  CHECK(pr.payload["target_id"] == 4U);

  // Format clue response
  auto clue_resp =
      json{{"type", "clue_private"}, {"match_id", 1}, {"is_insider", true}};
  formatted = player.format_response(clue_resp);
  CHECK(formatted.find("THE INSIDER") != std::string::npos);

  auto clue_false =
      json{{"type", "clue_private"}, {"match_id", 1}, {"is_insider", false}};
  formatted = player.format_response(clue_false);
  CHECK(formatted.find("NOT the Insider") != std::string::npos);

  // Vote command
  r = player.process_input("vote 2");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "vote");
  CHECK(r.payload["target_id"] == 2U);

  // Vote abstain
  r = player.process_input("vote abstain");
  CHECK(r.send_frame);
  CHECK(r.payload["target_id"].is_null());

  r = player.process_input("vote 0");
  CHECK(r.send_frame);
  CHECK(r.payload["target_id"].is_null());

  // Vote prompt flow
  r = player.process_input("vote");
  CHECK(!r.send_frame && r.prompt_for_input.has_value());
  pr = player.process_prompt_response("abstain");
  CHECK(pr.send_frame && pr.payload["target_id"].is_null());

  // Raw JSON bypass
  r = player.process_input("{\"custom\": 123}");
  CHECK(r.send_frame);
  CHECK(r.payload["custom"] == 123);

  // Status
  r = player.process_input("status");
  CHECK(r.send_frame && r.payload["type"] == "match_status");
}

void test_response_formatting() {
  ClientState client(ClientRole::Player);

  // Market result
  json market{{"type", "market_result"}, {"ok", true},
              {"executed_buys", 3},      {"executed_sells", 1},
              {"closing_health", 7},     {"closing_price", 12},
              {"closing_reserve", 18},   {"outcome", "None"}};
  auto formatted = client.format_response(market);
  CHECK(formatted.find("Executed Buys  : 3") != std::string::npos);
  CHECK(formatted.find("Closing Health : 7 / 10") != std::string::npos);

  // Vote resolution with elimination
  json vote_elim{{"type", "vote_resolution"}, {"ok", true},
  {"eliminated_player", 2},    {"votes_received", 3},
  {"eligible_voters", 5},      {"outcome", "EmployeesWin"}};
  formatted = client.format_response(vote_elim);
  CHECK(formatted.find("ELIMINATION    : Player 2 WAS ELIMINATED!") !=
        std::string::npos);
  CHECK(formatted.find("EMPLOYEES & INVESTIGATOR WIN") != std::string::npos);

  // Vote resolution without elimination
  json vote_tie{{"type", "vote_resolution"},    {"ok", true},
                {"eliminated_player", nullptr}, {"votes_received", 1},
                {"eligible_voters", 5},         {"outcome", "None"}};
  formatted = client.format_response(vote_tie);
  CHECK(formatted.find("ELIMINATION    : None") != std::string::npos);

  // Full match status
  json status{{"type", "match_status"},
              {"match_id", 1},
              {"phase", "Trading"},
              {"round", 2},
              {"joined_players", 5},
              {"health", 6},
              {"price", 11},
              {"outcome", "None"},
              {"last_market", market},
              {"last_vote", vote_tie}};
  formatted = client.format_response(status);
  CHECK(formatted.find("MATCH #1 STATUS") != std::string::npos);
  CHECK(formatted.find("Phase          : Trading") != std::string::npos);
  CHECK(formatted.find("Round          : 2 / 3") != std::string::npos);
  CHECK(formatted.find("Company Health : 6 / 10") != std::string::npos);
  CHECK(formatted.find("Last Market Resolution:") != std::string::npos);
  CHECK(formatted.find("Last Vote Resolution:") != std::string::npos);

  // Operation error
  json op_err{{"type", "operation_result"},
              {"ok", false},
              {"code", "WrongPhase"},
              {"detail", "trading is closed"}};
  formatted = client.format_response(op_err);
  CHECK(formatted.find("WrongPhase: trading is closed") != std::string::npos);

  // Connect response formatting
  json connect_resp{{"type", "connect"}, {"role", "moderator"}};
  formatted = client.format_response(connect_resp);
  CHECK(formatted.find("[CONNECTED] Session established as moderator") !=
        std::string::npos);
}

void test_stage2_cli_events_and_snapshots() {
  ClientState client(ClientRole::Player);
  CHECK(client.latest_state_version() == 0U);

  // 1. Initial market snapshot
  json snapshot_v1{
    {"type", "market_snapshot"},
    {"match_id", 1},
    {"state_version", 1},
    {"round", 0},
    {"phase", "Lobby"},
    {"health", 5},
    {"price", 10},
    {"total_supply", 60},
    {"reserve", 10},
    {"shares_held_by_players", 50},
    {"outcome", "None"}
  };
  std::string formatted = client.format_response(snapshot_v1);
  CHECK(!formatted.empty());
  CHECK(client.latest_state_version() == 1U);
  CHECK(formatted.find("[MARKET SNAPSHOT] Match #1 (Version 1)") != std::string::npos);
  CHECK(formatted.find("Company Health : 5 / 10") != std::string::npos);
  CHECK(formatted.find("Total Supply   : 60") != std::string::npos);
  CHECK(formatted.find("Player Shares  : 50") != std::string::npos);

  // 2. Server events
  json ev_match_started{
    {"type", "server_event"},
    {"event", "match_started"},
    {"match_id", 1},
    {"round", 1},
    {"phase", "AwaitingEvent"},
    {"state_version", 2}
  };
  formatted = client.format_response(ev_match_started);
  CHECK(client.latest_state_version() == 2U);
  CHECK(formatted.find("[MATCH STARTED] Match has begun. Round 1 (AwaitingEvent).") != std::string::npos);

  json ev_disc{
    {"type", "server_event"},
    {"event", "phase_started"},
    {"match_id", 1},
    {"round", 1},
    {"phase", "Discussion"},
    {"state_version", 3}
  };
  formatted = client.format_response(ev_disc);
  CHECK(client.latest_state_version() == 3U);
  CHECK(formatted.find("[PHASE START] Discussion — Round 1.") != std::string::npos);

  json ev_trade{
    {"type", "server_event"},
    {"event", "phase_started"},
    {"match_id", 1},
    {"round", 1},
    {"phase", "Trading"},
    {"state_version", 4}
  };
  formatted = client.format_response(ev_trade);
  CHECK(client.latest_state_version() == 4U);
  CHECK(formatted.find("[PHASE START] Trading — Round 1.") != std::string::npos);

  json ev_vote{
    {"type", "server_event"},
    {"event", "phase_started"},
    {"match_id", 1},
    {"round", 1},
    {"phase", "Voting"},
    {"state_version", 6}
  };
  formatted = client.format_response(ev_vote);
  CHECK(client.latest_state_version() == 6U);
  CHECK(formatted.find("[VOTING STARTED] Voting is now open — Round 1.") != std::string::npos);

  json ev_vote_res{
    {"type", "server_event"},
    {"event", "vote_resolved"},
    {"match_id", 1},
    {"round", 1},
    {"phase", "Voting"},
    {"eliminated_player", 3},
    {"votes_received", 3},
    {"eligible_voters", 5},
    {"outcome", "None"},
    {"state_version", 7}
  };
  formatted = client.format_response(ev_vote_res);
  CHECK(client.latest_state_version() == 7U);
  CHECK(formatted.find("[VOTE RESOLVED]") != std::string::npos);
  CHECK(formatted.find("Player 3 was eliminated") != std::string::npos);

  json ev_round2{
    {"type", "server_event"},
    {"event", "round_started"},
    {"match_id", 1},
    {"round", 2},
    {"phase", "AwaitingEvent"},
    {"state_version", 7}
  };
  formatted = client.format_response(ev_round2);
  CHECK(formatted.find("[ROUND START] Round 2 has begun.") != std::string::npos);

  json ev_finished{
    {"type", "server_event"},
    {"event", "match_finished"},
    {"match_id", 1},
    {"round", 3},
    {"phase", "Finished"},
    {"outcome", "InsiderWin"},
    {"state_version", 8}
  };
  formatted = client.format_response(ev_finished);
  CHECK(client.latest_state_version() == 8U);
  CHECK(formatted.find("[MATCH FINISHED] Final outcome: InsiderWin") != std::string::npos);

  // 3. Stale snapshot rejection (version 5 < current version 8)
  json stale_snapshot{
    {"type", "market_snapshot"},
    {"match_id", 1},
    {"state_version", 5},
    {"round", 1},
    {"phase", "Clue"},
    {"health", 5},
    {"price", 10},
    {"total_supply", 60},
    {"reserve", 10},
    {"shares_held_by_players", 50},
    {"outcome", "None"}
  };
  formatted = client.format_response(stale_snapshot);
  CHECK(formatted.empty()); // Stale updates are dropped
  CHECK(client.latest_state_version() == 8U);

  // 4. Newer snapshot accepted (version 9 > 8)
  json newer_snapshot{
    {"type", "market_snapshot"},
    {"match_id", 1},
    {"state_version", 9},
    {"round", 3},
    {"phase", "Finished"},
    {"health", 0},
    {"price", 1},
    {"total_supply", 60},
    {"reserve", 15},
    {"shares_held_by_players", 45},
    {"outcome", "InsiderWin"},
    {"last_market", {
      {"executed_buys", 0},
      {"executed_sells", 2},
      {"closing_health", 0},
      {"closing_price", 1},
      {"closing_reserve", 15},
      {"outcome", "InsiderWin"}
    }}
  };
  formatted = client.format_response(newer_snapshot);
  CHECK(!formatted.empty());
  CHECK(client.latest_state_version() == 9U);
  CHECK(formatted.find("Last Market Resolution:") != std::string::npos);

  // 5. Malformed payloads
  json malformed_empty = json::object();
  CHECK(client.format_response(malformed_empty).empty());
  json non_object = json::array();
  CHECK(client.format_response(non_object).find("[SERVER]") != std::string::npos);
}

void test_stage3_cli_commands_and_events() {
  ClientState mod(ClientRole::Moderator);
  mod.set_match_id(1);

  // 1. Moderator commands: pause, resume, abort
  auto r = mod.process_input("pause");
  CHECK(r.send_frame && r.payload["type"] == "pause" && r.payload["match_id"] == 1U);
  r = mod.process_input("pause 3");
  CHECK(r.send_frame && r.payload["type"] == "pause" && r.payload["match_id"] == 3U);

  r = mod.process_input("resume");
  CHECK(r.send_frame && r.payload["type"] == "resume" && r.payload["match_id"] == 1U);
  r = mod.process_input("resume 2");
  CHECK(r.send_frame && r.payload["type"] == "resume" && r.payload["match_id"] == 2U);

  r = mod.process_input("abort");
  CHECK(r.send_frame && r.payload["type"] == "abort" && r.payload["match_id"] == 1U);
  r = mod.process_input("abort 5");
  CHECK(r.send_frame && r.payload["type"] == "abort" && r.payload["match_id"] == 5U);

  // 2. Format responses: pause_result, resume_result, abort_result
  const auto pause_resp = json{{"type", "pause_result"}, {"ok", true}, {"match_id", 1}, {"seconds_remaining", 45}};
  auto fmt = mod.format_response(pause_resp);
  CHECK(fmt.find("[MATCH PAUSED]") != std::string::npos && fmt.find("45 seconds remaining") != std::string::npos);

  const auto resume_resp = json{{"type", "resume_result"}, {"ok", true}, {"match_id", 1}, {"seconds_remaining", 45}};
  fmt = mod.format_response(resume_resp);
  CHECK(fmt.find("[MATCH RESUMED]") != std::string::npos && fmt.find("45 seconds remaining") != std::string::npos);

  const auto abort_resp = json{{"type", "abort_result"}, {"ok", true}, {"match_id", 1}};
  fmt = mod.format_response(abort_resp);
  CHECK(fmt.find("[MATCH ABORTED]") != std::string::npos && fmt.find("terminated") != std::string::npos);

  // 3. Format server_event: phase_warning, transition_interval, match_paused, match_resumed, match_aborted
  ClientState player(ClientRole::Player);
  player.set_match_id(1);

  const auto warn_event = json{{"type", "server_event"}, {"event", "phase_warning"},
                               {"match_id", 1}, {"round", 1}, {"phase", "Discussion"},
                               {"seconds_remaining", 30}, {"state_version", 10}};
  fmt = player.format_response(warn_event);
  CHECK(fmt.find("[COUNTDOWN WARNING]") != std::string::npos && fmt.find("30 seconds remaining") != std::string::npos);

  const auto trans_event = json{{"type", "server_event"}, {"event", "transition_interval"},
                                {"match_id", 1}, {"round", 1}, {"phase", "Clue"},
                                {"upcoming_phase", "Voting"}, {"seconds_remaining", 10}, {"state_version", 11}};
  fmt = player.format_response(trans_event);
  CHECK(fmt.find("[TRANSITION INTERVAL]") != std::string::npos && fmt.find("Voting") != std::string::npos);

  const auto pause_event = json{{"type", "server_event"}, {"event", "match_paused"},
                                {"match_id", 1}, {"round", 1}, {"phase", "Trading"},
                                {"seconds_remaining", 50}, {"state_version", 12}};
  fmt = player.format_response(pause_event);
  CHECK(fmt.find("[MATCH PAUSED]") != std::string::npos && fmt.find("50 seconds remaining") != std::string::npos);

  const auto resume_event = json{{"type", "server_event"}, {"event", "match_resumed"},
                                 {"match_id", 1}, {"round", 1}, {"phase", "Trading"},
                                 {"seconds_remaining", 50}, {"state_version", 13}};
  fmt = player.format_response(resume_event);
  CHECK(fmt.find("[MATCH RESUMED]") != std::string::npos && fmt.find("50 seconds remaining") != std::string::npos);

  const auto abort_event = json{{"type", "server_event"}, {"event", "match_aborted"},
                                {"match_id", 1}, {"round", 1}, {"phase", "Finished"},
                                {"outcome", "Aborted"}, {"state_version", 14}};
  fmt = player.format_response(abort_event);
  CHECK(fmt.find("[MATCH ABORTED]") != std::string::npos);

  // 4. Format market_snapshot and match_status with timer state
  const auto snap_paused = json{
    {"type", "market_snapshot"}, {"match_id", 1}, {"state_version", 15},
    {"round", 1}, {"phase", "Trading"}, {"health", 8}, {"price", 10},
    {"total_supply", 50}, {"reserve", 20}, {"shares_held_by_players", 30},
    {"outcome", "None"}, {"is_paused", true}, {"seconds_remaining", 50}
  };
  fmt = player.format_response(snap_paused);
  CHECK(fmt.find("PAUSED (50s remaining)") != std::string::npos);

  const auto status_paused = json{
    {"type", "match_status"}, {"match_id", 1}, {"state_version", 15},
    {"round", 1}, {"phase", "Trading"}, {"joined_players", 5}, {"player_count", 5},
    {"health", 8}, {"price", 10}, {"outcome", "None"},
    {"is_paused", true}, {"seconds_remaining", 50}
  };
  fmt = player.format_response(status_paused);
  CHECK(fmt.find("PAUSED (50s remaining)") != std::string::npos);
}

void test_stage4_cli_quantity_and_research() {
  ClientState player(ClientRole::Player);
  (void)player.process_input("join 1");

  // 1. Quantity trading commands
  auto r = player.process_input("buy 3");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "trade");
  CHECK(r.payload["action"] == "BUY");
  CHECK(r.payload["quantity"] == 3U);

  r = player.process_input("sell 2");
  CHECK(r.send_frame && r.payload["action"] == "SELL" && r.payload["quantity"] == 2U);

  r = player.process_input("trade buy 5");
  CHECK(r.send_frame && r.payload["action"] == "BUY" && r.payload["quantity"] == 5U);

  r = player.process_input("trade sell 4");
  CHECK(r.send_frame && r.payload["action"] == "SELL" && r.payload["quantity"] == 4U);

  // Single share defaults
  r = player.process_input("buy");
  CHECK(r.send_frame && r.payload["quantity"] == 1U);
  r = player.process_input("sell");
  CHECK(r.send_frame && r.payload["quantity"] == 1U);

  // Invalid quantities
  r = player.process_input("buy 0");
  CHECK(!r.send_frame && r.local_message.find("positive integer") != std::string::npos);
  r = player.process_input("sell -5");
  CHECK(!r.send_frame && r.local_message.find("positive integer") != std::string::npos);
  r = player.process_input("buy abc");
  CHECK(!r.send_frame && r.local_message.find("positive integer") != std::string::npos);

  // 2. Research and scout commands
  r = player.process_input("research");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "research");
  CHECK(r.payload["match_id"] == 1U);

  r = player.process_input("scout");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "research");

  // 3. Response formatting for research
  const auto research_new = json{
    {"type", "research_result"}, {"ok", true}, {"match_id", 1},
    {"direction", "Positive"}, {"already_used", false}, {"credits_remaining", 90}
  };
  auto fmt = player.format_response(research_new);
  CHECK(fmt.find("[SCOUTING / RESEARCH REPORT]") != std::string::npos);
  CHECK(fmt.find("Signal Direction : Positive") != std::string::npos);
  CHECK(fmt.find("10 Credits debited") != std::string::npos);

  const auto research_used = json{
    {"type", "research_result"}, {"ok", true}, {"match_id", 1},
    {"direction", "Positive"}, {"already_used", true}, {"credits_remaining", 90}
  };
  fmt = player.format_response(research_used);
  CHECK(fmt.find("Already used in this match") != std::string::npos);

  // 4. Response formatting for indicators in snapshot and status
  const auto snapshot_stage4 = json{
    {"type", "market_snapshot"}, {"match_id", 1}, {"state_version", 20},
    {"round", 2}, {"phase", "Discussion"}, {"health", 7}, {"price", 14},
    {"sentiment", 3}, {"volatility", 5}, {"liquidity", 45},
    {"total_supply", 100}, {"reserve", 45}, {"shares_held_by_players", 55},
    {"outcome", "None"}
  };
  fmt = player.format_response(snapshot_stage4);
  CHECK(fmt.find("Sentiment      : 3") != std::string::npos);
  CHECK(fmt.find("Volatility     : 5") != std::string::npos);
  CHECK(fmt.find("Liquidity      : 45%") != std::string::npos);
}

} // namespace

int main() {
  test_moderator_commands();
  test_player_commands();
  test_response_formatting();
  test_stage2_cli_events_and_snapshots();
  test_stage3_cli_commands_and_events();
  test_stage4_cli_quantity_and_research();

  if (failures != 0) {
    std::cerr << failures << " CLI test(s) failed!\n";
    return 1;
  }
  std::cout << "All CLI unit tests passed successfully!\n";
  return 0;
}
