#include "darkpool/cli.hpp"

#include <cassert>
#include <iostream>
#include <string>

namespace {

using darkpool::cli::ClientRole;
using darkpool::cli::ClientState;
using darkpool::network::json;

int failures = 0;
void check(bool condition, const char* expression, int line) {
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
  CHECK(r.payload["roles"].size() == 5U);
  CHECK(r.payload["roles"][0] == "Insider");
  CHECK(r.payload["roles"][1] == "Investigator");
  CHECK(r.payload["roles"][2] == "Employee");

  // Format match created
  const auto created_resp = json{{"type", "match_created"}, {"match_id", 42}};
  const auto formatted_created = mod.format_response(created_resp);
  CHECK(formatted_created.find("Match #42") != std::string::npos);
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

  // Event out of bounds
  r = mod.process_input("event 5");
  CHECK(!r.send_frame);
  CHECK(r.local_message.find("between -2 and 2") != std::string::npos);

  // Trading
  r = mod.process_input("trading");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "open_trading");

  // Market
  r = mod.process_input("market");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "resolve_market");

  // Voting
  r = mod.process_input("voting");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "open_voting");

  // Resolve
  r = mod.process_input("resolve");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "resolve_votes");

  // Status
  r = mod.process_input("status");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "match_status");

  // Prompt mode for event
  r = mod.process_input("event");
  CHECK(r.prompt_for_input.has_value());
  auto p = mod.process_prompt_response("-1");
  CHECK(p.send_frame);
  CHECK(p.payload["impact"] == -1);

  // Custom roster
  r = mod.process_input("create Insider Investigator Employee Employee Employee");
  CHECK(r.send_frame);
  CHECK(r.payload["roles"].size() == 5U);

  // Invalid roster size
  r = mod.process_input("create Insider Employee");
  CHECK(!r.send_frame);
  CHECK(r.local_message.find("exactly 5 roles") != std::string::npos);
}

void test_player_commands() {
  ClientState player(ClientRole::Player);
  CHECK(player.prompt() == "[Player] > ");

  // Join default
  auto r = player.process_input("join");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "join_match");
  CHECK(r.payload["match_id"] == 1U);

  // Join specific
  r = player.process_input("join 3");
  CHECK(r.send_frame);
  CHECK(r.payload["match_id"] == 3U);

  // Format joined
  const auto joined_resp = json{
    {"type", "joined_match"},
    {"match_id", 3},
    {"player_id", 2},
    {"seat", 2},
    {"private_role", "Investigator"}
  };
  auto formatted = player.format_response(joined_resp);
  CHECK(formatted.find("Player ID   : 2") != std::string::npos);
  CHECK(formatted.find("Private Role: Investigator") != std::string::npos);
  CHECK(formatted.find("CONFIDENTIAL INVESTIGATOR INTEL") != std::string::npos);
  CHECK(player.player_id() == 2U);
  CHECK(player.private_role() == "Investigator");
  CHECK(player.prompt() == "[Player 2 (Investigator)] > ");

  // Hint
  r = player.process_input("hint");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "insider_hint");
  const auto hint_resp = json{{"type", "hint_private"}, {"event_impact", 2}};
  formatted = player.format_response(hint_resp);
  CHECK(formatted.find("CONFIDENTIAL INSIDER INTEL") != std::string::npos);
  CHECK(formatted.find("Upcoming Event Impact: +2") != std::string::npos);

  // Trade direct shortcuts
  r = player.process_input("buy");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "trade");
  CHECK(r.payload["action"] == "BUY");

  r = player.process_input("sell");
  CHECK(r.send_frame);
  CHECK(r.payload["action"] == "SELL");

  r = player.process_input("hold");
  CHECK(r.send_frame);
  CHECK(r.payload["action"] == "HOLD");

  // Trade command with args
  r = player.process_input("trade buy");
  CHECK(r.send_frame);
  CHECK(r.payload["action"] == "BUY");

  // Trade prompt mode
  r = player.process_input("trade");
  CHECK(r.prompt_for_input.has_value());
  auto p = player.process_prompt_response("sell");
  CHECK(p.send_frame);
  CHECK(p.payload["action"] == "SELL");

  // Invalid trade
  r = player.process_input("trade short");
  CHECK(!r.send_frame);
  CHECK(r.local_message.find("Invalid trade action") != std::string::npos);

  // Clue command
  r = player.process_input("clue 1");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "request_clue");
  CHECK(r.payload["target_id"] == 1U);

  // Clue prompt mode
  r = player.process_input("clue");
  CHECK(r.prompt_for_input.has_value());
  p = player.process_prompt_response("4");
  CHECK(p.send_frame);
  CHECK(p.payload["target_id"] == 4U);

  // Format clue response
  const auto clue_resp = json{{"type", "clue_private"}, {"target_id", 1}, {"is_insider", true}};
  formatted = player.format_response(clue_resp);
  CHECK(formatted.find("CONFIDENTIAL INVESTIGATION DOSSIER") != std::string::npos);
  CHECK(formatted.find("Target Player 1 IS THE INSIDER") != std::string::npos);

  const auto clue_false = json{{"type", "clue_private"}, {"target_id", 3}, {"is_insider", false}};
  formatted = player.format_response(clue_false);
  CHECK(formatted.find("Target Player 3 is NOT the Insider") != std::string::npos);

  // Vote command
  r = player.process_input("vote 1");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "vote");
  CHECK(r.payload["target_id"] == 1U);

  r = player.process_input("vote abstain");
  CHECK(r.send_frame);
  CHECK(r.payload["target_id"].is_null());

  r = player.process_input("vote 0");
  CHECK(r.send_frame);
  CHECK(r.payload["target_id"].is_null());

  // Vote prompt mode
  r = player.process_input("vote");
  CHECK(r.prompt_for_input.has_value());
  p = player.process_prompt_response("5");
  CHECK(p.send_frame);
  CHECK(p.payload["target_id"] == 5U);

  // Status
  r = player.process_input("status");
  CHECK(r.send_frame);
  CHECK(r.payload["type"] == "match_status");

  // Help
  r = player.process_input("help");
  CHECK(!r.send_frame);
  CHECK(r.local_message.find("DARKPOOL PLAYER COMMANDS") != std::string::npos);

  // Moderator blocked from player
  r = player.process_input("start");
  CHECK(!r.send_frame);
  CHECK(r.local_message.find("not recognized") != std::string::npos);
}

void test_response_formatting() {
  ClientState client(ClientRole::Player);

  // Market response
  json market{
    {"type", "market_result"},
    {"ok", true},
    {"executed_buys", 2},
    {"executed_sells", 1},
    {"closing_health", 8},
    {"closing_price", 12},
    {"closing_reserve", 19},
    {"round", 1},
    {"outcome", "None"}
  };
  auto formatted = client.format_response(market);
  CHECK(formatted.find("MARKET RESOLUTION") != std::string::npos);
  CHECK(formatted.find("Company Health : 8 / 10") != std::string::npos);
  CHECK(formatted.find("Executed Buys  : 2") != std::string::npos);

  // Vote response with elimination
  json vote_elim{
    {"type", "vote_resolution"},
    {"ok", true},
    {"eliminated_player", 1},
    {"votes_received", 3},
    {"eligible_voters", 5},
    {"round", 2},
    {"outcome", "EmployeesWin"}
  };
  formatted = client.format_response(vote_elim);
  CHECK(formatted.find("VOTING RESOLUTION") != std::string::npos);
  CHECK(formatted.find("ELIMINATION    : Player 1 ELIMINATED") != std::string::npos);
  CHECK(formatted.find("EMPLOYEES & INVESTIGATOR WIN") != std::string::npos);

  // Vote tie response
  json vote_tie{
    {"type", "vote_resolution"},
    {"ok", true},
    {"eliminated_player", nullptr},
    {"votes_received", 2},
    {"eligible_voters", 5},
    {"round", 1},
    {"outcome", "None"}
  };
  formatted = client.format_response(vote_tie);
  CHECK(formatted.find("ELIMINATION    : None") != std::string::npos);

  // Full match status
  json status{
    {"type", "match_status"},
    {"match_id", 1},
    {"phase", "Trading"},
    {"round", 2},
    {"joined_players", 5},
    {"health", 6},
    {"price", 11},
    {"outcome", "None"},
    {"last_market", market},
    {"last_vote", vote_tie}
  };
  formatted = client.format_response(status);
  CHECK(formatted.find("MATCH #1 STATUS") != std::string::npos);
  CHECK(formatted.find("Phase          : Trading") != std::string::npos);
  CHECK(formatted.find("Round          : 2 / 3") != std::string::npos);
  CHECK(formatted.find("Company Health : 6 / 10") != std::string::npos);
  CHECK(formatted.find("Last Market Resolution:") != std::string::npos);
  CHECK(formatted.find("Last Vote Resolution:") != std::string::npos);

  // Operation error
  json op_err{
    {"type", "operation_result"},
    {"ok", false},
    {"code", "WrongPhase"},
    {"detail", "trading is closed"}
  };
  formatted = client.format_response(op_err);
  CHECK(formatted.find("WrongPhase: trading is closed") != std::string::npos);

  // Connect response formatting
  json connect_resp{{"type", "connect"}, {"role", "moderator"}};
  formatted = client.format_response(connect_resp);
  CHECK(formatted.find("[CONNECTED] Session established as moderator") != std::string::npos);
}

}  // namespace

int main() {
  test_moderator_commands();
  test_player_commands();
  test_response_formatting();

  if (failures != 0) {
    std::cerr << failures << " CLI test(s) failed!\n";
    return 1;
  }
  std::cout << "All CLI unit tests passed successfully!\n";
  return 0;
}
