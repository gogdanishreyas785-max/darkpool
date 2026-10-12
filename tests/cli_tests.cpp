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
  auto hint_resp = json{{"type", "hint_private"}, {"match_id", 1}, {"event_impact", 2}};
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
  auto clue_resp = json{{"type", "clue_private"}, {"match_id", 1}, {"is_insider", true}};
  formatted = player.format_response(clue_resp);
  CHECK(formatted.find("THE INSIDER") != std::string::npos);

  auto clue_false = json{{"type", "clue_private"}, {"match_id", 1}, {"is_insider", false}};
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
  json market{
    {"type", "market_result"},
    {"ok", true},
    {"executed_buys", 3},
    {"executed_sells", 1},
    {"closing_health", 7},
    {"closing_price", 12},
    {"closing_reserve", 18},
    {"outcome", "None"}
  };
  auto formatted = client.format_response(market);
  CHECK(formatted.find("Executed Buys  : 3") != std::string::npos);
  CHECK(formatted.find("Closing Health : 7 / 10") != std::string::npos);

  // Vote resolution with elimination
  json vote_elim{
    {"type", "vote_resolution"},
    {"ok", true},
    {"eliminated_player", 2},
    {"votes_received", 3},
    {"eligible_voters", 5},
    {"outcome", "EmployeesWin"}
  };
  formatted = client.format_response(vote_elim);
  CHECK(formatted.find("ELIMINATION    : Player 2 WAS ELIMINATED!") != std::string::npos);
  CHECK(formatted.find("EMPLOYEES & INVESTIGATOR WIN") != std::string::npos);

  // Vote resolution without elimination
  json vote_tie{
    {"type", "vote_resolution"},
    {"ok", true},
    {"eliminated_player", nullptr},
    {"votes_received", 1},
    {"eligible_voters", 5},
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
