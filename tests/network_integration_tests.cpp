#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using darkpool::network::Frame;
using darkpool::network::json;

int failures = 0;
void check(bool condition, const char* expression, int line) {
  if (!condition) { ++failures; std::cerr << "FAIL line " << line << ": " << expression << '\n'; }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

bool send_all(int fd, const char* data, std::size_t size, bool fragment) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto request = fragment ? 1U : size - offset;
    const auto sent = send(fd, data + offset, request, MSG_NOSIGNAL);
    if (sent < 0) { if (errno == EINTR) continue; return false; }
    if (sent == 0) return false;
    offset += static_cast<std::size_t>(sent);
  }
  return true;
}

bool receive_exact(int fd, char* data, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const auto received = recv(fd, data + offset, size - offset, 0);
    if (received < 0) { if (errno == EINTR) continue; return false; }
    if (received == 0) return false;
    offset += static_cast<std::size_t>(received);
  }
  return true;
}

bool receive_echo(int fd, const json& payload) {
  char header[4];
  if (!receive_exact(fd, header, sizeof(header))) return false;
  const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
  const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
  const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
  const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
  if (length == 0U || length > darkpool::network::kMaxMessageSize) return false;
  std::string bytes(header, sizeof(header));
  bytes.resize(static_cast<std::size_t>(length) + sizeof(header));
  if (!receive_exact(fd, bytes.data() + sizeof(header), static_cast<std::size_t>(length))) return false;
  std::size_t consumed = 0;
  const auto echoed = Frame::parse(bytes, consumed);
  return echoed && consumed == bytes.size() && echoed->payload() == payload;
}

std::optional<json> request_json(int fd, const json& payload) {
  const auto outgoing = Frame(payload).serialize();
  if (!send_all(fd, outgoing.data(), outgoing.size(), false)) return std::nullopt;
  char header[4];
  if (!receive_exact(fd, header, sizeof(header))) return std::nullopt;
  const auto b0 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[0]));
  const auto b1 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[1]));
  const auto b2 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[2]));
  const auto b3 = static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
  const auto length = (b0 << 24U) | (b1 << 16U) | (b2 << 8U) | b3;
  if (length == 0U || length > darkpool::network::kMaxMessageSize) return std::nullopt;
  std::string bytes(header, sizeof(header));
  bytes.resize(static_cast<std::size_t>(length) + sizeof(header));
  if (!receive_exact(fd, bytes.data() + sizeof(header), static_cast<std::size_t>(length))) return std::nullopt;
  std::size_t consumed = 0;
  try {
    const auto response = Frame::parse(bytes, consumed);
    if (!response || consumed != bytes.size()) return std::nullopt;
    return response->payload();
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool exchange(int fd, const json& payload, bool fragment) {
  const auto outgoing = Frame(payload).serialize();
  return send_all(fd, outgoing.data(), outgoing.size(), fragment) && receive_echo(fd, payload);
}

bool closed_within_timeout(int fd, int timeout_ms) {
  timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char byte = 0;
  const auto received = recv(fd, &byte, 1U, 0);
  return received == 0 ||
         (received < 0 && (errno == ECONNRESET || errno == EPIPE));
}

int connect_to(std::uint16_t port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
  (void)close(fd);
  return -1;
}

int connect_with_retry(std::uint16_t port, int max_attempts = 80) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int fd = connect_to(port);
    if (fd >= 0) return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return -1;
}

bool rejected_by_server(int fd) {
  timeval timeout{1, 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char byte = 0;
  const auto received = recv(fd, &byte, 1U, 0);
  (void)close(fd);
  return received == 0 || (received < 0 && (errno == ECONNRESET || errno == EPIPE));
}

std::uint16_t reserve_port() {
  const int probe = socket(AF_INET, SOCK_STREAM, 0);
  if (probe < 0) return 0;
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  const bool bound = bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  socklen_t address_length = sizeof(address);
  const bool named = bound && getsockname(probe, reinterpret_cast<sockaddr*>(&address), &address_length) == 0;
  (void)close(probe);
  if (!named) return 0U;
  return ntohs(address.sin_port);
}

pid_t start_server(const char* server_path, std::uint16_t port, const char* token_env) {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (token_env) {
      setenv("DARKPOOL_MODERATOR_TOKEN", token_env, 1);
    } else {
      unsetenv("DARKPOOL_MODERATOR_TOKEN");
    }
    const auto port_text = std::to_string(port);
    execl(server_path, server_path, port_text.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  return child;
}

void test_server_capacity_and_echo(const char* server_path) {
  const auto port = reserve_port();
  CHECK(port != 0U);
  if (port == 0U) return;

  const auto child = start_server(server_path, port, "integration-test-mod-token");
  CHECK(child >= 0);
  if (child < 0) return;

  std::vector<int> peers;
  int moderator = -1;
  int duplicate_moderator = -1;
  const int initial_peer = connect_with_retry(port);
  CHECK(initial_peer >= 0);
  if (initial_peer < 0) { (void)kill(child, SIGTERM); (void)waitpid(child, nullptr, 0); return; }
  peers.push_back(initial_peer);

  CHECK(exchange(peers[0], json{{"type", "connect"}, {"role", "player"}}, true));
  CHECK(exchange(peers[0], json{{"echo", "fragmented hello"}}, true));

  const int half_closed = peers[0];
  const json last_message{{"half_close", "echo before eof"}};
  const auto final_frame = Frame(last_message).serialize();
  CHECK(send_all(half_closed, final_frame.data(), final_frame.size(), false));
  CHECK(shutdown(half_closed, SHUT_WR) == 0);
  CHECK(receive_echo(half_closed, last_message));
  char trailing = 0;
  CHECK(recv(half_closed, &trailing, 1U, 0) == 0);
  (void)close(half_closed);
  peers.erase(peers.begin());

  for (int i = 0; i < 5; ++i) {
    const int fd = connect_to(port);
    CHECK(fd >= 0);
    if (fd < 0) continue;
    peers.push_back(fd);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
  }
  moderator = connect_to(port);
  CHECK(moderator >= 0);
  if (moderator >= 0) {
    const auto reply = request_json(moderator, json{{"type", "connect"}, {"role", "moderator"}, {"token", "integration-test-mod-token"}});
    CHECK(reply && (*reply)["type"] == "connect" && (*reply)["role"] == "moderator" && !reply->contains("token"));
  }

  const int excess = connect_to(port);
  CHECK(excess >= 0);
  if (excess >= 0) CHECK(rejected_by_server(excess));

  if (!peers.empty()) { (void)close(peers.back()); peers.pop_back(); }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  duplicate_moderator = connect_to(port);
  CHECK(duplicate_moderator >= 0);
  if (duplicate_moderator >= 0) {
    const auto hello = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "integration-test-mod-token"}}).serialize();
    CHECK(send_all(duplicate_moderator, hello.data(), hello.size(), false));
    CHECK(rejected_by_server(duplicate_moderator));
  }

  const int replacement = connect_to(port);
  CHECK(replacement >= 0);
  if (replacement >= 0) {
    CHECK(exchange(replacement, json{{"type", "connect"}, {"role", "player"}}, false));
    peers.push_back(replacement);
  }
  for (const auto fd : peers) (void)close(fd);
  if (moderator >= 0) (void)close(moderator);
  (void)kill(child, SIGTERM);
  (void)waitpid(child, nullptr, 0);
}

std::optional<json> command(int fd, std::uint64_t match_id, const char* type) {
  return request_json(fd, json{{"type", type}, {"match_id", match_id}});
}

void test_complete_three_round_match(const char* server_path) {
  const auto port = reserve_port();
  CHECK(port != 0U);
  if (port == 0U) return;
  const auto child = start_server(server_path, port, "gameplay-test-token");
  CHECK(child >= 0);
  if (child < 0) return;

  int moderator = -1;
  std::vector<int> players;
  std::vector<int> connected;
  const auto cleanup = [&]() {
    for (const auto fd : connected) (void)close(fd);
    (void)kill(child, SIGTERM);
    (void)waitpid(child, nullptr, 0);
  };
  moderator = connect_with_retry(port);
  CHECK(moderator >= 0);
  if (moderator < 0) { cleanup(); return; }
  connected.push_back(moderator);
  timeval timeout{2, 0};
  (void)setsockopt(moderator, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  const auto moderator_hello = request_json(moderator, json{{"type", "connect"}, {"role", "moderator"}, {"token", "gameplay-test-token"}});
  CHECK(moderator_hello && (*moderator_hello)["type"] == "connect");
  CHECK(moderator_hello && (*moderator_hello)["role"] == "moderator");
  CHECK(moderator_hello && !moderator_hello->contains("token"));

  for (std::size_t i = 0; i < 5U; ++i) {
    const int fd = connect_to(port);
    CHECK(fd >= 0);
    if (fd < 0) continue;
    connected.push_back(fd);
    players.push_back(fd);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const auto hello = request_json(fd, json{{"type", "connect"}, {"role", "player"}});
    CHECK(hello && (*hello)["type"] == "connect");
  }
  if (players.size() != 5U) { cleanup(); return; }

  const json roles{"Insider", "Investigator", "Employee", "Employee", "Employee"};
  const auto created = request_json(moderator, json{{"type", "create_match"}, {"roles", roles}});
  CHECK(created && (*created)["type"] == "match_created");
  if (!created || !created->contains("match_id")) { cleanup(); return; }
  const auto match_id = (*created)["match_id"].get<std::uint64_t>();

  const std::vector<std::string> expected_roles{"Insider", "Investigator", "Employee", "Employee", "Employee"};
  for (std::size_t i = 0; i < players.size(); ++i) {
    const auto joined = request_json(players[i], json{{"type", "join_match"}, {"match_id", match_id}});
    CHECK(joined && (*joined)["type"] == "joined_match");
    CHECK(joined && (*joined)["player_id"] == i + 1U);
    CHECK(joined && (*joined)["private_role"] == expected_roles[i]);
  }

  const auto start = command(moderator, match_id, "start_match");
  CHECK(start && (*start)["type"] == "transition_result" && (*start)["ok"]);
  const auto status = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(status && (*status)["phase"] == "AwaitingEvent");

  for (std::uint32_t round = 1; round <= 3U; ++round) {
    const auto event = request_json(moderator, json{{"type", "publish_event"},
        {"match_id", match_id}, {"impact", 0}});
    CHECK(event && (*event)["type"] == "transition_result" && (*event)["phase"] == "Discussion");
    const auto hint = command(players[0], match_id, "insider_hint");
    CHECK(hint && (*hint)["type"] == "hint_private" && (*hint)["event_impact"] == 0);
    const auto public_during_discussion = request_json(players[2],
        json{{"type", "match_status"}, {"match_id", match_id}});
    CHECK(public_during_discussion && !public_during_discussion->contains("event_impact"));

    const auto trading = command(moderator, match_id, "open_trading");
    CHECK(trading && (*trading)["phase"] == "Trading");
    for (const auto player : players) {
      const auto trade = request_json(player, json{{"type", "trade"},
          {"match_id", match_id}, {"action", "HOLD"}});
      CHECK(trade && (*trade)["type"] == "trade_result" && (*trade)["ok"]);
    }
    const auto market = command(moderator, match_id, "resolve_market");
    CHECK(market && (*market)["type"] == "market_result" && (*market)["ok"]);
    CHECK(market && (*market)["closing_health"] == 5 && (*market)["outcome"] == "None");
    const auto clue_phase = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
    CHECK(clue_phase && (*clue_phase)["phase"] == "Clue");
    if (round == 1U) {
      const auto clue = request_json(players[1], json{{"type", "request_clue"},
          {"match_id", match_id}, {"target_id", 3}});
      CHECK(clue && (*clue)["type"] == "clue_private" && (*clue)["is_insider"] == false);
    }
    const auto public_after_clue = request_json(players[2],
        json{{"type", "match_status"}, {"match_id", match_id}});
    CHECK(public_after_clue && !public_after_clue->contains("clue_result") &&
          !public_after_clue->contains("clue_target"));

    const auto voting = command(moderator, match_id, "open_voting");
    CHECK(voting && (*voting)["phase"] == "Voting");
    for (const auto player : players) {
      const auto vote = request_json(player, json{{"type", "vote"},
          {"match_id", match_id}, {"target_id", nullptr}});
      CHECK(vote && (*vote)["type"] == "vote_result" && (*vote)["accepted"]);
    }
    const auto resolved = command(moderator, match_id, "resolve_votes");
    CHECK(resolved && (*resolved)["type"] == "vote_resolution" && (*resolved)["ok"]);
    CHECK(resolved && (*resolved)["eliminated_player"].is_null());
    if (round < 3U) {
      CHECK(resolved && (*resolved)["outcome"] == "None");
      const auto next = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
      CHECK(next && (*next)["phase"] == "AwaitingEvent" && (*next)["round"] == round + 1U);
    } else {
      CHECK(resolved && (*resolved)["outcome"] == "InsiderWin");
      const auto finished = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
      CHECK(finished && (*finished)["phase"] == "Finished");
      CHECK(finished && (*finished)["outcome"] == "InsiderWin" && (*finished)["health"] > 0);
    }
  }
  cleanup();
}

void test_moderator_authentication(const char* server_path) {
  // -------------------------------------------------------------------------
  // Scenario 1: Server configured WITHOUT DARKPOOL_MODERATOR_TOKEN (fail closed)
  // -------------------------------------------------------------------------
  {
    const auto port = reserve_port();
    CHECK(port != 0U);
    const auto server_pid = start_server(server_path, port, nullptr);
    CHECK(server_pid > 0);
    const auto stop_server = [&]() {
      (void)kill(server_pid, SIGTERM);
      (void)waitpid(server_pid, nullptr, 0);
    };

    // 1A. Player connection succeeds even when server moderator token is unset
    const int player_fd = connect_with_retry(port);
    CHECK(player_fd >= 0);
    if (player_fd >= 0) {
      timeval timeout{2, 0};
      (void)setsockopt(player_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      const auto player_res = request_json(player_fd, json{{"type", "connect"}, {"role", "player"}});
      CHECK(player_res && (*player_res)["type"] == "connect" && (*player_res)["role"] == "player");
      (void)close(player_fd);
    }

    // 1B. Moderator connection with a token fails closed because server has no secret configured
    const int mod_fd1 = connect_to(port);
    CHECK(mod_fd1 >= 0);
    if (mod_fd1 >= 0) {
      const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "arbitrary-secret"}}).serialize();
      CHECK(send_all(mod_fd1, payload.data(), payload.size(), false));
      CHECK(rejected_by_server(mod_fd1));
    }

    // 1C. Moderator connection without a token fails closed
    const int mod_fd2 = connect_to(port);
    CHECK(mod_fd2 >= 0);
    if (mod_fd2 >= 0) {
      const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}}).serialize();
      CHECK(send_all(mod_fd2, payload.data(), payload.size(), false));
      CHECK(rejected_by_server(mod_fd2));
    }

    stop_server();
  }

  // -------------------------------------------------------------------------
  // Scenario 2: Server configured WITH EMPTY DARKPOOL_MODERATOR_TOKEN (fail closed)
  // -------------------------------------------------------------------------
  {
    const auto port = reserve_port();
    CHECK(port != 0U);
    const auto server_pid = start_server(server_path, port, "");
    CHECK(server_pid > 0);
    const auto stop_server = [&]() {
      (void)kill(server_pid, SIGTERM);
      (void)waitpid(server_pid, nullptr, 0);
    };

    // 2A. Player connection succeeds
    const int player_fd = connect_with_retry(port);
    CHECK(player_fd >= 0);
    if (player_fd >= 0) {
      timeval timeout{2, 0};
      (void)setsockopt(player_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      const auto player_res = request_json(player_fd, json{{"type", "connect"}, {"role", "player"}});
      CHECK(player_res && (*player_res)["type"] == "connect" && (*player_res)["role"] == "player");
      (void)close(player_fd);
    }

    // 2B. Moderator connection with empty token fails closed
    const int mod_fd1 = connect_to(port);
    CHECK(mod_fd1 >= 0);
    if (mod_fd1 >= 0) {
      const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", ""}}).serialize();
      CHECK(send_all(mod_fd1, payload.data(), payload.size(), false));
      CHECK(rejected_by_server(mod_fd1));
    }

    // 2C. Moderator connection with non-empty token fails closed
    const int mod_fd2 = connect_to(port);
    CHECK(mod_fd2 >= 0);
    if (mod_fd2 >= 0) {
      const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "some-token"}}).serialize();
      CHECK(send_all(mod_fd2, payload.data(), payload.size(), false));
      CHECK(rejected_by_server(mod_fd2));
    }

    stop_server();
  }

  // -------------------------------------------------------------------------
  // Scenario 3: Server configured WITH valid DARKPOOL_MODERATOR_TOKEN
  // -------------------------------------------------------------------------
  {
    const char* expected_token = "correct-moderator-secret-789";
    const auto port = reserve_port();
    CHECK(port != 0U);
    const auto server_pid = start_server(server_path, port, expected_token);
    CHECK(server_pid > 0);
    const auto stop_server = [&]() {
      (void)kill(server_pid, SIGTERM);
      (void)waitpid(server_pid, nullptr, 0);
    };

    const int probe_fd = connect_with_retry(port);
    CHECK(probe_fd >= 0);
    if (probe_fd >= 0) (void)close(probe_fd);

    // 3A: Missing credentials (no "token" field)
    {
      const int fd = connect_to(port);
      CHECK(fd >= 0);
      if (fd >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}}).serialize();
        CHECK(send_all(fd, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd));
      }
    }

    // 3B: Empty token ("")
    {
      const int fd = connect_to(port);
      CHECK(fd >= 0);
      if (fd >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", ""}}).serialize();
        CHECK(send_all(fd, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd));
      }
    }

    // 3C: Non-string token types
    {
      // integer token
      const int fd1 = connect_to(port);
      CHECK(fd1 >= 0);
      if (fd1 >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", 12345}}).serialize();
        CHECK(send_all(fd1, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd1));
      }

      // boolean token
      const int fd2 = connect_to(port);
      CHECK(fd2 >= 0);
      if (fd2 >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", true}}).serialize();
        CHECK(send_all(fd2, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd2));
      }

      // array token
      const int fd3 = connect_to(port);
      CHECK(fd3 >= 0);
      if (fd3 >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", json::array({"correct-moderator-secret-789"})}}).serialize();
        CHECK(send_all(fd3, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd3));
      }

      // object token
      const int fd4 = connect_to(port);
      CHECK(fd4 >= 0);
      if (fd4 >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", json{{"secret", "correct-moderator-secret-789"}}}}).serialize();
        CHECK(send_all(fd4, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd4));
      }
    }

    // 3D: Incorrect credentials
    {
      // Completely wrong secret
      const int fd1 = connect_to(port);
      CHECK(fd1 >= 0);
      if (fd1 >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "wrong-password-999"}}).serialize();
        CHECK(send_all(fd1, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd1));
      }

      // Same length, 1 character difference
      const int fd2 = connect_to(port);
      CHECK(fd2 >= 0);
      if (fd2 >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "correct-moderator-secret-788"}}).serialize();
        CHECK(send_all(fd2, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd2));
      }

      // Prefix only
      const int fd3 = connect_to(port);
      CHECK(fd3 >= 0);
      if (fd3 >= 0) {
        const auto payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "correct-moderator-secret"}}).serialize();
        CHECK(send_all(fd3, payload.data(), payload.size(), false));
        CHECK(rejected_by_server(fd3));
      }
    }

    // 3E: Existing player connection path succeeds without token
    {
      const int player_fd = connect_to(port);
      CHECK(player_fd >= 0);
      if (player_fd >= 0) {
        timeval timeout{2, 0};
        (void)setsockopt(player_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const auto res = request_json(player_fd, json{{"type", "connect"}, {"role", "player"}});
        CHECK(res && (*res)["type"] == "connect" && (*res)["role"] == "player");

        // Verify player cannot elevate privileges by sending subsequent moderator connect
        const auto elevate = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", expected_token}}).serialize();
        CHECK(send_all(player_fd, elevate.data(), elevate.size(), false));
        CHECK(rejected_by_server(player_fd));
      }
    }

    // 3F: Valid credentials succeed, token is NOT reflected, moderator commands succeed
    int valid_mod_fd = connect_to(port);
    CHECK(valid_mod_fd >= 0);
    if (valid_mod_fd >= 0) {
      timeval timeout{2, 0};
      (void)setsockopt(valid_mod_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
      const auto res = request_json(valid_mod_fd, json{{"type", "connect"}, {"role", "moderator"}, {"token", expected_token}});
      CHECK(res && (*res)["type"] == "connect");
      CHECK(res && (*res)["role"] == "moderator");
      CHECK(res && !res->contains("token"));  // Token must never be reflected

      // Verify moderator capabilities
      const json roles{"Insider", "Investigator", "Employee", "Employee", "Employee"};
      const auto created = request_json(valid_mod_fd, json{{"type", "create_match"}, {"roles", roles}});
      CHECK(created && (*created)["type"] == "match_created");

      // 3G: Duplicate moderator with valid credentials rejected (slot limit 1)
      const int dup_mod_fd = connect_to(port);
      CHECK(dup_mod_fd >= 0);
      if (dup_mod_fd >= 0) {
        const auto dup_payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", expected_token}}).serialize();
        CHECK(send_all(dup_mod_fd, dup_payload.data(), dup_payload.size(), false));
        CHECK(rejected_by_server(dup_mod_fd));
      }

      // Disconnect original moderator
      (void)close(valid_mod_fd);
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // 3H: Slot conservation: after moderator disconnects, a new moderator can authenticate
    {
      const int new_mod_fd = connect_to(port);
      CHECK(new_mod_fd >= 0);
      if (new_mod_fd >= 0) {
        timeval timeout{2, 0};
        (void)setsockopt(new_mod_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        const auto res = request_json(new_mod_fd, json{{"type", "connect"}, {"role", "moderator"}, {"token", expected_token}});
        CHECK(res && (*res)["type"] == "connect");
        CHECK(res && (*res)["role"] == "moderator");
        (void)close(new_mod_fd);
      }
    }

    stop_server();
  }
}

struct TimeoutTestGuard {
  pid_t child{-1};
  int player{-1};
  std::vector<int> idle_peers;
  int excess{-1};
  int replacement{-1};

  ~TimeoutTestGuard() {
    if (player >= 0) {
      (void)close(player);
      player = -1;
    }
    for (int& fd : idle_peers) {
      if (fd >= 0) {
        (void)close(fd);
        fd = -1;
      }
    }
    if (excess >= 0) {
      (void)close(excess);
      excess = -1;
    }
    if (replacement >= 0) {
      (void)close(replacement);
      replacement = -1;
    }
    if (child > 0) {
      (void)kill(child, SIGTERM);
      (void)waitpid(child, nullptr, 0);
      child = -1;
    }
  }
};

void test_idle_unassigned_timeout(const char* server_path) {
  const auto port = reserve_port();
  CHECK(port != 0U);
  if (port == 0U) return;

  TimeoutTestGuard guard;
  guard.child = start_server(server_path, port, "integration-test-mod-token");
  CHECK(guard.child >= 0);
  if (guard.child < 0) return;

  // 1. Connect and authenticate one player
  guard.player = connect_with_retry(port);
  CHECK(guard.player >= 0);
  if (guard.player < 0) return;

  timeval timeout{2, 0};
  (void)setsockopt(guard.player, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  CHECK(exchange(guard.player, json{{"type", "connect"}, {"role", "player"}}, false));

  // 2. Connect 5 unassigned idle clients (saturating 6 total capacity: 1 player + 5 idle)
  const auto connect_start = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i) {
    const int fd = connect_to(port);
    CHECK(fd >= 0);
    if (fd >= 0) guard.idle_peers.push_back(fd);
  }
  CHECK(guard.idle_peers.size() == 5U);
  if (guard.idle_peers.size() != 5U) return;

  // 3. Verify server capacity is exhausted: attempt replacement/7th connection while
  // all 6 existing connections are still open. Verify it is rejected immediately.
  guard.excess = connect_to(port);
  CHECK(guard.excess >= 0);
  if (guard.excess >= 0) {
    const int excess_fd = guard.excess;
    guard.excess = -1;  // rejected_by_server closes the socket
    CHECK(rejected_by_server(excess_fd));
  }

  // 4. Verify idle connections are not closed prematurely at t = 2s
  std::this_thread::sleep_for(std::chrono::seconds(2));
  for (const int fd : guard.idle_peers) {
    char byte = 0;
    const auto res = recv(fd, &byte, 1U, MSG_DONTWAIT);
    CHECK(res < 0 && (errno == EAGAIN || errno == EWOULDBLOCK));
  }

  // 5. Wait for the 5 idle connections to be closed by the 5-second timeout,
  // and assert that closure occurs within a bounded interval [4500ms, 8500ms].
  bool all_expired = false;
  std::chrono::steady_clock::time_point first_closed_at{};
  std::chrono::steady_clock::time_point last_closed_at{};
  std::vector<bool> fd_closed(guard.idle_peers.size(), false);
  std::size_t closed_count = 0;

  while (std::chrono::steady_clock::now() - connect_start < std::chrono::seconds(10)) {
    for (std::size_t i = 0; i < guard.idle_peers.size(); ++i) {
      if (fd_closed[i]) continue;
      char byte = 0;
      const auto res = recv(guard.idle_peers[i], &byte, 1U, MSG_DONTWAIT);
      const bool closed = (res == 0) || (res < 0 && (errno == ECONNRESET || errno == EPIPE));
      if (closed) {
        fd_closed[i] = true;
        ++closed_count;
        const auto now = std::chrono::steady_clock::now();
        if (first_closed_at == std::chrono::steady_clock::time_point{}) {
          first_closed_at = now;
        }
        last_closed_at = now;
      }
    }
    if (closed_count == guard.idle_peers.size()) {
      all_expired = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  CHECK(all_expired);
  if (all_expired) {
    const auto first_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(first_closed_at - connect_start).count();
    const auto last_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(last_closed_at - connect_start).count();
    CHECK(first_elapsed >= 4500);
    CHECK(last_elapsed <= 8500);
  }

  // Close the expired idle sockets
  for (int& fd : guard.idle_peers) {
    if (fd >= 0) {
      (void)close(fd);
      fd = -1;
    }
  }

  // 6. Verify authenticated player connection was NOT closed by the timeout
  // Keep player open while testing replacement connection!
  CHECK(exchange(guard.player, json{{"echo", "player_still_authenticated"}}, false));

  // 7. Verify capacity is restored: a replacement connection can connect and complete handshake
  // while the existing authenticated player connection remains active.
  guard.replacement = connect_to(port);
  CHECK(guard.replacement >= 0);
  if (guard.replacement >= 0) {
    (void)setsockopt(guard.replacement, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    CHECK(exchange(guard.replacement, json{{"type", "connect"}, {"role", "player"}}, false));
    CHECK(exchange(guard.replacement, json{{"echo", "replacement_active"}}, false));
  }

  // Verify both the initial player and replacement client can communicate simultaneously
  CHECK(exchange(guard.player, json{{"echo", "player_concurrent_active"}}, false));

  if (guard.replacement >= 0) {
    (void)close(guard.replacement);
    guard.replacement = -1;
  }
  if (guard.player >= 0) {
    (void)close(guard.player);
    guard.player = -1;
  }
}

void test_malformed_tcp_isolation(const char* server_path) {
  const auto port = reserve_port();
  CHECK(port != 0U);
  if (port == 0U) return;

  const auto child = start_server(server_path, port, "integration-test-mod-token");
  CHECK(child > 0);
  if (child <= 0) return;

  std::vector<int> open_fds;
  const auto close_fd = [&](int& fd) {
    if (fd >= 0) {
      (void)close(fd);
      for (auto& tracked : open_fds) {
        if (tracked == fd) tracked = -1;
      }
      fd = -1;
    }
  };
  const auto stop_server = [&]() {
    for (auto& fd : open_fds) {
      if (fd >= 0) {
        (void)close(fd);
        fd = -1;
      }
    }
    (void)kill(child, SIGTERM);
    (void)waitpid(child, nullptr, 0);
  };

  // Five established sessions plus one offender fill the server's six
  // connection slots, so each replacement handshake verifies slot recovery.
  std::vector<int> players;
  for (int i = 0; i < 4; ++i) {
    const int fd = connect_with_retry(port);
    CHECK(fd >= 0);
    if (fd < 0) { stop_server(); return; }
    open_fds.push_back(fd);
    players.push_back(fd);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
  }

  int moderator = connect_with_retry(port);
  CHECK(moderator >= 0);
  if (moderator < 0) { stop_server(); return; }
  open_fds.push_back(moderator);
  CHECK(exchange(moderator,
                 json{{"type", "connect"}, {"role", "moderator"},
                      {"token", "integration-test-mod-token"}}, false));

  const std::string oversized_header{"\0\x10\0\x01", 4U};
  const std::vector<std::pair<std::string, bool>> cases{
      {std::string{"\0\0\0\x01{", 5U}, false},  // malformed JSON
      {std::string{"\0\0\0\0", 4U}, false},     // zero-length frame
      {oversized_header, false},                     // header only; no body
      {std::string{"\0\0", 2U}, true},             // truncated header + FIN
      {std::string{"\0\0\0\x05{}", 6U}, true},  // truncated payload + FIN
  };

  for (std::size_t i = 0; i < cases.size(); ++i) {
    int offender = connect_to(port);
    CHECK(offender >= 0);
    if (offender < 0) continue;
    open_fds.push_back(offender);

    CHECK(send_all(offender, cases[i].first.data(), cases[i].first.size(), false));
    if (cases[i].second) CHECK(shutdown(offender, SHUT_WR) == 0);
    CHECK(closed_within_timeout(offender, 2000));

    int status = 0;
    CHECK(waitpid(child, &status, WNOHANG) == 0);

    CHECK(exchange(players.front(),
                   json{{"echo", "legitimate-client-survives-" + std::to_string(i)}},
                   false));

    close_fd(offender);
    int replacement = connect_with_retry(port);
    CHECK(replacement >= 0);
    if (replacement >= 0) {
      open_fds.push_back(replacement);
      CHECK(exchange(replacement, json{{"type", "connect"}, {"role", "player"}}, false));
      CHECK(exchange(replacement,
                     json{{"echo", "replacement-handshake-" + std::to_string(i)}},
                     false));
      CHECK(shutdown(replacement, SHUT_WR) == 0);
      CHECK(closed_within_timeout(replacement, 2000));
      close_fd(replacement);
    }
  }

  stop_server();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "expected server executable path [capacity|timeout|auth|gameplay|malformed|all]\n";
    return EXIT_FAILURE;
  }
  if (argc == 3 && std::string(argv[2]) == "gameplay") {
    test_complete_three_round_match(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "auth") {
    test_moderator_authentication(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "malformed") {
    test_malformed_tcp_isolation(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "timeout") {
    test_idle_unassigned_timeout(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "all") {
    test_server_capacity_and_echo(argv[1]);
    test_idle_unassigned_timeout(argv[1]);
    test_moderator_authentication(argv[1]);
    test_complete_three_round_match(argv[1]);
  } else {
    test_server_capacity_and_echo(argv[1]);
  }
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Integration tests passed.\n";
  return EXIT_SUCCESS;
}
