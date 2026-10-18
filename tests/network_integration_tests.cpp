#include "darkpool/network.hpp"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <fcntl.h>
#include <ifaddrs.h>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <utility>
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

std::optional<json> read_single_frame(int fd) {
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

std::optional<json> request_json(int fd, const json& payload) {
  const auto outgoing = Frame(payload).serialize();
  if (!send_all(fd, outgoing.data(), outgoing.size(), false)) return std::nullopt;
  while (true) {
    auto frame = read_single_frame(fd);
    if (!frame) return std::nullopt;
    if (frame->is_object()) {
      const auto t = frame->value("type", "");
      if (t == "server_event" || t == "market_snapshot") {
        continue;
      }
    }
    return frame;
  }
}

bool exchange(int fd, const json& payload, bool fragment) {
  const auto outgoing = Frame(payload).serialize();
  return send_all(fd, outgoing.data(), outgoing.size(), fragment) && receive_echo(fd, payload);
}

void set_receive_timeout(int fd, int timeout_ms) {
  timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

bool closed_within_timeout(int fd, int timeout_ms) {
  timeval timeout{static_cast<time_t>(timeout_ms / 1000),
                  static_cast<suseconds_t>((timeout_ms % 1000) * 1000)};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  char buffer[4096];
  while (true) {
    errno = 0;
    const auto received = recv(fd, buffer, sizeof(buffer), 0);
    if (received == 0) return true;  // Orderly peer shutdown (EOF/FIN)
    if (received < 0) {
      if (errno == EINTR) continue;
      // An ordinary receive timeout returns -1 with EAGAIN / EWOULDBLOCK / ETIMEDOUT.
      // A receive timeout is explicitly NOT proof of peer closure.
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
        return false;
      }
      return errno == ECONNRESET || errno == EPIPE;
    }
    // If bytes were pending in the receive buffer prior to peer closure,
    // drain them until EOF, connection reset, or receive timeout.
    continue;
  }
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

  const int additional_player = connect_to(port);
  CHECK(additional_player >= 0);
  if (additional_player >= 0) {
    CHECK(exchange(additional_player, json{{"type", "connect"}, {"role", "player"}}, false));
    peers.push_back(additional_player);
  }

  if (!peers.empty()) { (void)close(peers.back()); peers.pop_back(); }
  std::this_thread::sleep_for(std::chrono::milliseconds(30));
  std::vector<int> extra_moderators;
  for (int m = 1; m < 4; ++m) {
    const int mod_fd = connect_to(port);
    CHECK(mod_fd >= 0);
    if (mod_fd >= 0) {
      const auto reply = request_json(mod_fd, json{{"type", "connect"}, {"role", "moderator"}, {"token", "integration-test-mod-token"}});
      CHECK(reply && (*reply)["type"] == "connect" && (*reply)["role"] == "moderator");
      extra_moderators.push_back(mod_fd);
    }
  }
  duplicate_moderator = connect_to(port);
  CHECK(duplicate_moderator >= 0);
  if (duplicate_moderator >= 0) {
    const auto hello = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", "integration-test-mod-token"}}).serialize();
    CHECK(send_all(duplicate_moderator, hello.data(), hello.size(), false));
    CHECK(rejected_by_server(duplicate_moderator));
  }
  for (const auto fd : extra_moderators) (void)close(fd);

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

void test_complete_three_round_match(const char* server_path, std::size_t player_count) {
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

  for (std::size_t i = 0; i < player_count; ++i) {
    const int fd = connect_to(port);
    CHECK(fd >= 0);
    if (fd < 0) continue;
    connected.push_back(fd);
    players.push_back(fd);
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const auto hello = request_json(fd, json{{"type", "connect"}, {"role", "player"}});
    CHECK(hello && (*hello)["type"] == "connect");
  }
  if (players.size() != player_count) { cleanup(); return; }

  json create_match{{"type", "create_match"}};
  if (player_count != 5U) create_match["player_count"] = player_count;
  const auto created = request_json(moderator, create_match);
  CHECK(created && (*created)["type"] == "match_created");
  CHECK(created && (*created)["player_count"] == player_count);
  const auto expected_insiders = 1U + (player_count - 1U) / 8U;
  const auto expected_investigators = 1U + (player_count - 1U) / 16U;
  const auto expected_employees = player_count - expected_insiders - expected_investigators;
  const std::array<std::size_t, 3> expected_counts{expected_insiders, expected_investigators, expected_employees};
  CHECK(created && (*created)["insiders"] == expected_counts[0] &&
        (*created)["investigators"] == expected_counts[1] && (*created)["employees"] == expected_counts[2]);
  if (!created || !created->contains("match_id")) { cleanup(); return; }
  const auto match_id = (*created)["match_id"].get<std::uint64_t>();

  std::size_t insider_seat = player_count;
  std::size_t investigator_seat = player_count;
  std::uint64_t employee_target = 0;
  std::size_t insiders = 0, investigators = 0, employees = 0;
  for (std::size_t i = 0; i < players.size(); ++i) {
    if (i == players.size() - 1U) {
      const auto premature_start = command(moderator, match_id, "start_match");
      CHECK(premature_start && (*premature_start)["type"] == "error" &&
            (*premature_start)["error"] == "all configured players must join before start");
    }
    const auto joined = request_json(players[i], json{{"type", "join_match"}, {"match_id", match_id}});
    CHECK(joined && (*joined)["type"] == "joined_match");
    CHECK(joined && (*joined)["player_id"] == i + 1U);
    if (joined && (*joined)["private_role"] == "Insider") { insider_seat = i; ++insiders; }
    else if (joined && (*joined)["private_role"] == "Investigator") { investigator_seat = i; ++investigators; }
    else if (joined && (*joined)["private_role"] == "Employee") {
      if (employee_target == 0U) employee_target = (*joined)["player_id"].get<std::uint64_t>();
      ++employees;
    }
  }
  CHECK(insiders == expected_counts[0] && investigators == expected_counts[1] && employees == expected_counts[2]);
  CHECK(insider_seat < players.size() && investigator_seat < players.size() && employee_target != 0U);
  if (insider_seat >= players.size() || investigator_seat >= players.size() || employee_target == 0U) {
    cleanup();
    return;
  }

  const auto start = command(moderator, match_id, "start_match");
  CHECK(start && (*start)["type"] == "transition_result" && (*start)["ok"]);
  const auto status = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
  CHECK(status && (*status)["phase"] == "AwaitingEvent" && (*status)["player_count"] == player_count);

  for (std::uint32_t round = 1; round <= 3U; ++round) {
    const auto event = request_json(moderator, json{{"type", "publish_event"},
        {"match_id", match_id}, {"impact", 0}});
    CHECK(event && (*event)["type"] == "transition_result" && (*event)["phase"] == "Discussion");
    const auto hint = command(players[insider_seat], match_id, "insider_hint");
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
      const auto clue = request_json(players[investigator_seat], json{{"type", "request_clue"},
          {"match_id", match_id}, {"target_id", employee_target}});
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
      const auto created = request_json(valid_mod_fd, json{{"type", "create_match"}});
      CHECK(created && (*created)["type"] == "match_created");

      // 3G: Duplicate moderator beyond 4 slots with valid credentials rejected (slot limit 4)
      std::vector<int> extra_mod_fds;
      for (int m = 1; m < 4; ++m) {
        const int mod_fd = connect_to(port);
        CHECK(mod_fd >= 0);
        if (mod_fd >= 0) {
          const auto reply = request_json(mod_fd, json{{"type", "connect"}, {"role", "moderator"}, {"token", expected_token}});
          CHECK(reply && (*reply)["type"] == "connect" && (*reply)["role"] == "moderator");
          extra_mod_fds.push_back(mod_fd);
        }
      }
      const int dup_mod_fd = connect_to(port);
      CHECK(dup_mod_fd >= 0);
      if (dup_mod_fd >= 0) {
        const auto dup_payload = Frame(json{{"type", "connect"}, {"role", "moderator"}, {"token", expected_token}}).serialize();
        CHECK(send_all(dup_mod_fd, dup_payload.data(), dup_payload.size(), false));
        CHECK(rejected_by_server(dup_mod_fd));
      }
      for (const auto fd : extra_mod_fds) (void)close(fd);

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

  // 2. Connect 5 unassigned idle clients to exercise independent handshake deadlines.
  const auto connect_start = std::chrono::steady_clock::now();
  for (int i = 0; i < 5; ++i) {
    const int fd = connect_to(port);
    CHECK(fd >= 0);
    if (fd >= 0) guard.idle_peers.push_back(fd);
  }
  CHECK(guard.idle_peers.size() == 5U);
  if (guard.idle_peers.size() != 5U) return;

  // 3. Verify idle connections are not closed prematurely at t = 2s
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

  // Keep representative player and moderator sessions connected while malformed clients are isolated.
  std::vector<int> players;
  for (int i = 0; i < 4; ++i) {
    const int fd = connect_with_retry(port);
    CHECK(fd >= 0);
    if (fd < 0) { stop_server(); return; }
    open_fds.push_back(fd);
    set_receive_timeout(fd, 2000);
    players.push_back(fd);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
  }

  int moderator = connect_with_retry(port);
  CHECK(moderator >= 0);
  if (moderator < 0) { stop_server(); return; }
  open_fds.push_back(moderator);
  set_receive_timeout(moderator, 2000);
  const auto moderator_reply = request_json(
      moderator, json{{"type", "connect"}, {"role", "moderator"},
                      {"token", "integration-test-mod-token"}});
  CHECK(moderator_reply && (*moderator_reply)["type"] == "connect" &&
        (*moderator_reply)["role"] == "moderator" && !moderator_reply->contains("token"));

  // Explicitly verify that closed_within_timeout cannot accept an ordinary receive timeout
  // as proof of peer closure. On open, idle sessions, it must return false.
  CHECK(!closed_within_timeout(players.front(), 50));
  CHECK(!closed_within_timeout(moderator, 50));

  const std::string oversized_1mb{"\0\x10\0\x01", 4U};       // 1,048,577 > 4,096
  const std::string oversized_limit{"\0\0\x10\x01", 4U};     // 4,097 > 4,096 (1 byte over 4 KiB frame limit)
  const std::vector<std::pair<std::string, bool>> cases{
      {std::string{"\0\0\0\x01{", 5U}, false},  // malformed JSON
      {std::string{"\0\0\0\0", 4U}, false},     // zero-length frame
      {oversized_limit, false},                  // declared length 4,097 (immediately above 4 KiB limit)
      {oversized_1mb, false},                    // oversized header (1,048,577 > 4,096)
      {std::string{"\0\0", 2U}, true},          // truncated header + FIN
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

    // Verify all 4 existing players and the moderator survived and remain fully responsive.
    for (std::size_t p = 0; p < players.size(); ++p) {
      CHECK(exchange(players[p],
                     json{{"echo", "legitimate-player-" + std::to_string(p) + "-survives-" + std::to_string(i)}},
                     false));
    }
    const auto mod_survives = request_json(
        moderator, json{{"echo", "moderator-survives-" + std::to_string(i)}});
    CHECK(mod_survives && (*mod_survives)["echo"] == "moderator-survives-" + std::to_string(i));

    close_fd(offender);

    // Verify slot recovery with a replacement player session while existing peers remain connected.
    int replacement = connect_with_retry(port);
    CHECK(replacement >= 0);
    if (replacement >= 0) {
      open_fds.push_back(replacement);
      set_receive_timeout(replacement, 2000);
      CHECK(exchange(replacement, json{{"type", "connect"}, {"role", "player"}}, false));
      CHECK(exchange(replacement,
                     json{{"echo", "replacement-handshake-" + std::to_string(i)}},
                     false));

      // Concurrently verify communication across replacement, existing player, and moderator.
      CHECK(exchange(players.front(), json{{"echo", "concurrent-verify-" + std::to_string(i)}}, false));
      const auto mod_concurrent = request_json(
          moderator, json{{"echo", "moderator-concurrent-" + std::to_string(i)}});
      CHECK(mod_concurrent && (*mod_concurrent)["echo"] == "moderator-concurrent-" + std::to_string(i));

      CHECK(shutdown(replacement, SHUT_WR) == 0);
      CHECK(closed_within_timeout(replacement, 2000));
      close_fd(replacement);
    }
  }

  // Live TCP client triggering output-queue overflow (64 KiB limit):
  // An offending client completes a player handshake,
  // restricts its receive buffer, and floods requests without reading server replies.
  // The server buffers responses in OutputQueue until the 64 KiB limit is exceeded,
  // then cleanly closes the offending connection without disrupting the existing 4 players or moderator.
  {
    int offender = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(offender >= 0);
    if (offender >= 0) {
      open_fds.push_back(offender);
      int rcvbuf = 2048;
      (void)setsockopt(offender, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      CHECK(connect(offender, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

      // Offender connects as player
      CHECK(exchange(offender, json{{"type", "connect"}, {"role", "player"}}, false));

      // Prepare a valid ~3,800-byte frame payload (within 4 KiB frame limit)
      const auto overhead = json{{"echo", ""}}.dump().size();
      const std::string pad(3800U - overhead, 'q');
      const auto big_frame = Frame(json{{"echo", pad}}).serialize();
      CHECK(big_frame.size() <= darkpool::network::kMaxFramePayloadSize + 4U);

      // Minimum frames required for the server to accumulate > kMaxQueuedBytes (64 KiB)
      // in its pending output queue when responses are not drained by the client:
      const auto min_frames_for_overflow =
          (darkpool::network::kMaxQueuedBytes / big_frame.size()) + 1U;
      constexpr int target_frames = 50;
      CHECK(target_frames >= static_cast<int>(min_frames_for_overflow));

      // Flood the server with requests without reading responses.
      // Every send_all() result is recorded and checked:
      // - Successful sends advance the count toward the overflow threshold.
      // - Expected disconnection (EPIPE / ECONNRESET) indicates the server has closed
      //   the socket due to queue overflow.
      // - Any other error is an unexpected failure that immediately fails the test.
      int successful_sends = 0;
      for (int i = 0; i < target_frames; ++i) {
        errno = 0;
        if (send_all(offender, big_frame.data(), big_frame.size(), false)) {
          ++successful_sends;
        } else {
          if (errno == EPIPE || errno == ECONNRESET) {
            // Server closed connection while or after the queue limit was breached.
            break;
          }
          std::cerr << "Unexpected send_all failure on frame " << i
                    << ": errno=" << errno << " (" << std::strerror(errno) << ")\n";
          CHECK(false);
          break;
        }
      }

      // Prove that enough frames were transmitted to exceed the 64 KiB output queue limit
      // before any server disconnection occurred.
      CHECK(successful_sends >= static_cast<int>(min_frames_for_overflow));

      // Server output queue must overflow (64 KiB), causing server to evict the offender
      CHECK(closed_within_timeout(offender, 2000));

      int status = 0;
      CHECK(waitpid(child, &status, WNOHANG) == 0);

      // Existing 4 players and moderator remain intact and responsive
      for (std::size_t p = 0; p < players.size(); ++p) {
        CHECK(exchange(players[p],
                       json{{"echo", "legitimate-player-" + std::to_string(p) + "-survives-overflow"}},
                       false));
      }
      const auto mod_survives = request_json(
          moderator, json{{"echo", "moderator-survives-overflow"}});
      CHECK(mod_survives && (*mod_survives)["echo"] == "moderator-survives-overflow");

      close_fd(offender);

      // Verify slot recovery: a replacement player connects and completes a handshake.
      int replacement = connect_with_retry(port);
      CHECK(replacement >= 0);
      if (replacement >= 0) {
        open_fds.push_back(replacement);
        set_receive_timeout(replacement, 2000);
        CHECK(exchange(replacement, json{{"type", "connect"}, {"role", "player"}}, false));
        CHECK(exchange(replacement, json{{"echo", "replacement-handshake-after-overflow"}}, false));

        // Concurrently verify communication across replacement, existing player, and moderator
        CHECK(exchange(players.front(), json{{"echo", "concurrent-verify-after-overflow"}}, false));
        const auto mod_concurrent = request_json(
            moderator, json{{"echo", "moderator-concurrent-after-overflow"}});
        CHECK(mod_concurrent && (*mod_concurrent)["echo"] == "moderator-concurrent-after-overflow");

        CHECK(shutdown(replacement, SHUT_WR) == 0);
        CHECK(closed_within_timeout(replacement, 2000));
        close_fd(replacement);
      }
    }
  }

  stop_server();
}

int connect_unix(const std::string& path) {
  const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (path.size() >= sizeof(address.sun_path)) {
    (void)close(fd);
    return -1;
  }
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) return fd;
  (void)close(fd);
  return -1;
}

int connect_unix_with_retry(const std::string& path, int max_attempts = 80) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int fd = connect_unix(path);
    if (fd >= 0) return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return -1;
}

pid_t start_unix_server(const char* server_path, const std::string& socket_path, const char* token_env) {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    if (token_env) {
      setenv("DARKPOOL_MODERATOR_TOKEN", token_env, 1);
    } else {
      unsetenv("DARKPOOL_MODERATOR_TOKEN");
    }
    execl(server_path, server_path, "--socket", socket_path.c_str(), "--no-tcp", static_cast<char*>(nullptr));
    _exit(127);
  }
  return child;
}

void test_unix_socket_lifecycle_and_gameplay(const char* server_path) {
  const auto pid_str = std::to_string(static_cast<long>(getpid()));

  // 1. Refusal to unlink non-socket files
  {
    const std::string nonsock_path = "/tmp/darkpool_test_nonsock_" + pid_str + ".txt";
    (void)::unlink(nonsock_path.c_str());
    const int fd = open(nonsock_path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    CHECK(fd >= 0);
    if (fd >= 0) {
      const char msg[] = "sensitive regular file content";
      CHECK(write(fd, msg, sizeof(msg) - 1) == static_cast<ssize_t>(sizeof(msg) - 1));
      (void)close(fd);

      const auto child = start_unix_server(server_path, nonsock_path, nullptr);
      CHECK(child >= 0);
      if (child >= 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
      }

      struct stat st {};
      CHECK(::lstat(nonsock_path.c_str(), &st) == 0);
      CHECK(S_ISREG(st.st_mode));
      (void)::unlink(nonsock_path.c_str());
    }
  }

  // 2. Stale socket file recovery and cleanup on shutdown
  {
    const std::string stale_path = "/tmp/darkpool_test_stale_" + pid_str + ".sock";
    (void)::unlink(stale_path.c_str());

    const int dummy_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(dummy_sock >= 0);
    if (dummy_sock >= 0) {
      sockaddr_un d_addr{};
      d_addr.sun_family = AF_UNIX;
      std::strncpy(d_addr.sun_path, stale_path.c_str(), sizeof(d_addr.sun_path) - 1);
      CHECK(bind(dummy_sock, reinterpret_cast<sockaddr*>(&d_addr), sizeof(d_addr)) == 0);
      (void)close(dummy_sock);

      struct stat st {};
      CHECK(::lstat(stale_path.c_str(), &st) == 0);
      CHECK(S_ISSOCK(st.st_mode));

      const auto child = start_unix_server(server_path, stale_path, "mod-token");
      CHECK(child >= 0);
      if (child >= 0) {
        const int client = connect_unix_with_retry(stale_path);
        CHECK(client >= 0);
        if (client >= 0) {
          CHECK(exchange(client, json{{"type", "connect"}, {"role", "player"}}, false));
          CHECK(exchange(client, json{{"echo", "stale-socket-recovery-success"}}, false));
          (void)close(client);
        }

        CHECK(kill(child, SIGTERM) == 0);
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);

        CHECK(::lstat(stale_path.c_str(), &st) != 0 && errno == ENOENT);
      }
    }
  }

  // 3. Non-ECONNREFUSED probe failure refusal (EACCES: does not unlink)
  {
    const std::string perm_path = "/tmp/darkpool_test_perm_" + pid_str + ".sock";
    (void)::unlink(perm_path.c_str());

    const int dummy_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    CHECK(dummy_sock >= 0);
    if (dummy_sock >= 0) {
      sockaddr_un d_addr{};
      d_addr.sun_family = AF_UNIX;
      std::strncpy(d_addr.sun_path, perm_path.c_str(), sizeof(d_addr.sun_path) - 1);
      CHECK(bind(dummy_sock, reinterpret_cast<sockaddr*>(&d_addr), sizeof(d_addr)) == 0);
      (void)close(dummy_sock);

      // Set permissions to 0000 so connect() yields EACCES
      CHECK(chmod(perm_path.c_str(), 0000) == 0);

      const auto child = start_unix_server(server_path, perm_path, "mod-token");
      CHECK(child >= 0);
      if (child >= 0) {
        int status = 0;
        CHECK(waitpid(child, &status, 0) == child);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
      }

      // Verify the socket file was preserved and NOT unlinked
      struct stat st {};
      CHECK(::lstat(perm_path.c_str(), &st) == 0);
      CHECK(S_ISSOCK(st.st_mode));

      (void)chmod(perm_path.c_str(), 0700);
      (void)::unlink(perm_path.c_str());
    }
  }

  // 4. Active server conflict refusal
  {
    const std::string active_path = "/tmp/darkpool_test_active_" + pid_str + ".sock";
    (void)::unlink(active_path.c_str());

    const auto child1 = start_unix_server(server_path, active_path, "token1");
    CHECK(child1 >= 0);
    if (child1 >= 0) {
      const int client = connect_unix_with_retry(active_path);
      CHECK(client >= 0);

      const auto child2 = start_unix_server(server_path, active_path, "token2");
      CHECK(child2 >= 0);
      if (child2 >= 0) {
        int status = 0;
        CHECK(waitpid(child2, &status, 0) == child2);
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) != 0);
      }

      if (client >= 0) {
        CHECK(exchange(client, json{{"type", "connect"}, {"role", "player"}}, false));
        (void)close(client);
      }

      CHECK(kill(child1, SIGTERM) == 0);
      int status = 0;
      CHECK(waitpid(child1, &status, 0) == child1);

      struct stat st {};
      CHECK(::lstat(active_path.c_str(), &st) != 0 && errno == ENOENT);
    }
  }

  // 4. Normal gameplay communication over UNIX domain socket
  {
    const std::string game_path = "/tmp/darkpool_test_game_" + pid_str + ".sock";
    (void)::unlink(game_path.c_str());

    const auto child = start_unix_server(server_path, game_path, "mod-token-game");
    CHECK(child >= 0);
    if (child >= 0) {
      const int mod_fd = connect_unix_with_retry(game_path);
      CHECK(mod_fd >= 0);
      if (mod_fd >= 0) {
        struct stat st {};
        CHECK(::lstat(game_path.c_str(), &st) == 0);
        CHECK(S_ISSOCK(st.st_mode));
        CHECK((st.st_mode & 0777U) == 0700U);

        const auto mod_resp = request_json(
            mod_fd,
            json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token-game"}});
        CHECK(mod_resp && (*mod_resp)["type"] == "connect" && (*mod_resp)["role"] == "moderator");

        std::vector<int> players;
        for (int i = 0; i < 5; ++i) {
          const int pfd = connect_unix(game_path);
          CHECK(pfd >= 0);
          if (pfd >= 0) {
            players.push_back(pfd);
            CHECK(exchange(pfd, json{{"type", "connect"}, {"role", "player"}}, false));
          }
        }
        CHECK(players.size() == 5U);

        const auto create_resp = request_json(
            mod_fd, json{{"type", "create_match"}, {"player_count", 5U}});
        CHECK(create_resp && (*create_resp)["type"] == "match_created" && (*create_resp)["match_id"] == 1U);

        int insider_fd = -1;
        int investigator_fd = -1;
        uint32_t employee_target = 0;
        for (std::size_t i = 0; i < players.size(); ++i) {
          const auto join_resp = request_json(
              players[i], json{{"type", "join_match"}, {"match_id", 1U}});
          CHECK(join_resp && (*join_resp)["type"] == "joined_match" && (*join_resp)["player_id"] == i + 1U);
          if (join_resp && join_resp->contains("private_role")) {
            const auto role = (*join_resp)["private_role"].get<std::string>();
            if (role == "Insider") insider_fd = players[i];
            if (role == "Investigator") investigator_fd = players[i];
            if (role == "Employee" && employee_target == 0U) {
              employee_target = static_cast<uint32_t>(i + 1U);
            }
          }
        }
        CHECK(insider_fd >= 0);
        CHECK(investigator_fd >= 0);
        CHECK(employee_target > 0U);

        const auto start_resp = request_json(
            mod_fd, json{{"type", "start_match"}, {"match_id", 1U}});
        CHECK(start_resp && (*start_resp)["type"] == "transition_result" && (*start_resp)["phase"] == "AwaitingEvent");

        const auto event_resp = request_json(
            mod_fd, json{{"type", "publish_event"}, {"match_id", 1U}, {"impact", 1}});
        CHECK(event_resp && (*event_resp)["type"] == "transition_result" && (*event_resp)["phase"] == "Discussion");

        if (insider_fd >= 0) {
          const auto hint_resp = request_json(
              insider_fd, json{{"type", "insider_hint"}, {"match_id", 1U}});
          CHECK(hint_resp && (*hint_resp)["type"] == "hint_private" && (*hint_resp)["event_impact"] == 1);
        }

        const auto trade_phase = request_json(
            mod_fd, json{{"type", "open_trading"}, {"match_id", 1U}});
        CHECK(trade_phase && (*trade_phase)["type"] == "transition_result" && (*trade_phase)["phase"] == "Trading");

        for (const auto pfd : players) {
          const auto tr = request_json(
              pfd, json{{"type", "trade"}, {"match_id", 1U}, {"action", "BUY"}});
          CHECK(tr && (*tr)["type"] == "trade_result" && (*tr)["ok"] == true);
        }

        const auto market_resp = request_json(
            mod_fd, json{{"type", "resolve_market"}, {"match_id", 1U}});
        CHECK(market_resp && (*market_resp)["type"] == "market_result");

        if (investigator_fd >= 0 && employee_target > 0U) {
          const auto clue_resp = request_json(
              investigator_fd, json{{"type", "request_clue"}, {"match_id", 1U}, {"target_id", employee_target}});
          CHECK(clue_resp && (*clue_resp)["type"] == "clue_private");
        }

        const auto vote_phase = request_json(
            mod_fd, json{{"type", "open_voting"}, {"match_id", 1U}});
        CHECK(vote_phase && (*vote_phase)["type"] == "transition_result" && (*vote_phase)["phase"] == "Voting");

        for (const auto pfd : players) {
          const auto vr = request_json(
              pfd, json{{"type", "vote"}, {"match_id", 1U}, {"target_id", nullptr}});
          CHECK(vr && (*vr)["type"] == "vote_result" && (*vr)["ok"] == true);
        }

        const auto resolve_resp = request_json(
            mod_fd, json{{"type", "resolve_votes"}, {"match_id", 1U}});
        CHECK(resolve_resp && (*resolve_resp)["type"] == "vote_resolution");

        const auto status_resp = request_json(
            mod_fd, json{{"type", "match_status"}, {"match_id", 1U}});
        CHECK(status_resp && (*status_resp)["type"] == "match_status");

        for (const auto pfd : players) {
          (void)close(pfd);
        }
        (void)close(mod_fd);
      }

      CHECK(kill(child, SIGTERM) == 0);
      int status = 0;
      CHECK(waitpid(child, &status, 0) == child);

      struct stat st {};
      CHECK(::lstat(game_path.c_str(), &st) != 0 && errno == ENOENT);
    }
  }

  // 5. Malformed frame isolation over UNIX domain socket
  {
    const std::string malformed_path = "/tmp/darkpool_test_malformed_" + pid_str + ".sock";
    (void)::unlink(malformed_path.c_str());

    const auto child = start_unix_server(server_path, malformed_path, "mod-token");
    CHECK(child >= 0);
    if (child >= 0) {
      const int client_a = connect_unix_with_retry(malformed_path);
      const int client_b = connect_unix(malformed_path);
      const int offender = connect_unix(malformed_path);
      CHECK(client_a >= 0 && client_b >= 0 && offender >= 0);

      if (client_a >= 0 && client_b >= 0 && offender >= 0) {
        CHECK(exchange(client_a, json{{"type", "connect"}, {"role", "player"}}, false));
        CHECK(exchange(client_b, json{{"type", "connect"}, {"role", "player"}}, false));
        CHECK(exchange(offender, json{{"type", "connect"}, {"role", "player"}}, false));

        const std::uint32_t oversized = 5000U;
        char header[4];
        header[0] = static_cast<char>((oversized >> 24U) & 0xFFU);
        header[1] = static_cast<char>((oversized >> 16U) & 0xFFU);
        header[2] = static_cast<char>((oversized >> 8U) & 0xFFU);
        header[3] = static_cast<char>(oversized & 0xFFU);
        CHECK(send_all(offender, header, sizeof(header), false));

        CHECK(closed_within_timeout(offender, 2000));
        (void)close(offender);

        CHECK(exchange(client_a, json{{"echo", "client-a-survives-unix-malformed"}}, false));
        CHECK(exchange(client_b, json{{"echo", "client-b-survives-unix-malformed"}}, false));

        (void)close(client_a);
        (void)close(client_b);
      }

      CHECK(kill(child, SIGTERM) == 0);
      int status = 0;
      CHECK(waitpid(child, &status, 0) == child);

      struct stat st {};
      CHECK(::lstat(malformed_path.c_str(), &st) != 0 && errno == ENOENT);
    }
  }

  // 6. Asynchronous push notifications and moderator controls over UNIX domain socket
  {
    const std::string notif_path = "/tmp/darkpool_test_notif_" + pid_str + ".sock";
    (void)::unlink(notif_path.c_str());

    const auto child = start_unix_server(server_path, notif_path, "mod-token-unix-notif");
    CHECK(child >= 0);
    if (child >= 0) {
      const int mod_fd = connect_unix_with_retry(notif_path);
      CHECK(mod_fd >= 0);
      if (mod_fd >= 0) {
        set_receive_timeout(mod_fd, 3000);
        const auto mod_resp = request_json(
            mod_fd,
            json{{"type", "connect"}, {"role", "moderator"}, {"token", "mod-token-unix-notif"}});
        CHECK(mod_resp && (*mod_resp)["type"] == "connect");

        const auto create_resp = request_json(
            mod_fd, json{{"type", "create_match"}, {"player_count", 5U}});
        CHECK(create_resp && (*create_resp)["type"] == "match_created");
        const auto match_id = (*create_resp)["match_id"].get<uint64_t>();

        const int player_fd = connect_unix_with_retry(notif_path);
        CHECK(player_fd >= 0);
        if (player_fd >= 0) {
          set_receive_timeout(player_fd, 3000);
          const auto p_conn = request_json(
              player_fd, json{{"type", "connect"}, {"role", "player"}});
          CHECK(p_conn && (*p_conn)["type"] == "connect");

          // Player joins match over unix socket
          const auto join_bytes = Frame(json{{"type", "join_match"}, {"match_id", match_id}}).serialize();
          CHECK(send_all(player_fd, join_bytes.data(), join_bytes.size(), false));

          // Player receives joined_match response first
          const auto join_resp = read_single_frame(player_fd);
          CHECK(join_resp && (*join_resp)["type"] == "joined_match");

          // Player receives initial market_snapshot pushed over unix socket
          const auto init_snap = read_single_frame(player_fd);
          CHECK(init_snap && (*init_snap)["type"] == "market_snapshot");
          CHECK(init_snap && (*init_snap)["phase"] == "Lobby");

          // Connect 4 more players so match can start
          std::vector<int> extra_players;
          for (int i = 2; i <= 5; ++i) {
            const int efd = connect_unix_with_retry(notif_path);
            CHECK(efd >= 0);
            extra_players.push_back(efd);
            (void)request_json(efd, json{{"type", "connect"}, {"role", "player"}});
            (void)request_json(efd, json{{"type", "join_match"}, {"match_id", match_id}});
          }

          // Moderator starts match
          const auto start_bytes = Frame(json{{"type", "start_match"}, {"match_id", match_id}}).serialize();
          CHECK(send_all(mod_fd, start_bytes.data(), start_bytes.size(), false));
          const auto mod_start = read_single_frame(mod_fd);
          CHECK(mod_start && (*mod_start)["type"] == "transition_result");
          (void)read_single_frame(mod_fd); // mod server_event
          (void)read_single_frame(mod_fd); // mod market_snapshot

          // Player 1 receives pushed match_started event and market_snapshot while idle over unix socket!
          const auto p_start_ev = read_single_frame(player_fd);
          CHECK(p_start_ev && (*p_start_ev)["type"] == "server_event" && (*p_start_ev)["event"] == "match_started");
          const auto p_start_snap = read_single_frame(player_fd);
          CHECK(p_start_snap && (*p_start_snap)["type"] == "market_snapshot" && (*p_start_snap)["phase"] == "AwaitingEvent");

          // Moderator publishes event
          const auto pub_bytes = Frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}}).serialize();
          CHECK(send_all(mod_fd, pub_bytes.data(), pub_bytes.size(), false));
          const auto mod_pub = read_single_frame(mod_fd);
          CHECK(mod_pub && (*mod_pub)["type"] == "transition_result");
          (void)read_single_frame(mod_fd); // mod server_event
          (void)read_single_frame(mod_fd); // mod market_snapshot

          // Player 1 receives phase_started (Discussion) and snapshot pushed over unix socket
          const auto p_disc_ev = read_single_frame(player_fd);
          CHECK(p_disc_ev && (*p_disc_ev)["type"] == "server_event" && (*p_disc_ev)["phase"] == "Discussion");
          const auto p_disc_snap = read_single_frame(player_fd);
          CHECK(p_disc_snap && (*p_disc_snap)["type"] == "market_snapshot" && (*p_disc_snap)["phase"] == "Discussion");

          // Moderator pauses match over unix socket
          const auto pause_bytes = Frame(json{{"type", "pause"}, {"match_id", match_id}}).serialize();
          CHECK(send_all(mod_fd, pause_bytes.data(), pause_bytes.size(), false));
          const auto mod_pause = read_single_frame(mod_fd);
          CHECK(mod_pause && (*mod_pause)["type"] == "pause_result" && (*mod_pause)["ok"]);
          (void)read_single_frame(mod_fd); // mod server_event
          (void)read_single_frame(mod_fd); // mod market_snapshot

          // Player 1 receives match_paused and snapshot with is_paused == true over unix socket
          const auto p_pause_ev = read_single_frame(player_fd);
          CHECK(p_pause_ev && (*p_pause_ev)["type"] == "server_event" && (*p_pause_ev)["event"] == "match_paused");
          const auto p_pause_snap = read_single_frame(player_fd);
          CHECK(p_pause_snap && (*p_pause_snap)["type"] == "market_snapshot" && (*p_pause_snap)["is_paused"] == true);

          // Player 1 sends command during pause (match_status) -> receives correlated response
          const auto p_st_req = request_json(player_fd, json{{"type", "match_status"}, {"match_id", match_id}});
          CHECK(p_st_req && (*p_st_req)["type"] == "match_status" && (*p_st_req)["is_paused"] == true);

          // Moderator resumes match over unix socket
          const auto resume_bytes = Frame(json{{"type", "resume"}, {"match_id", match_id}}).serialize();
          CHECK(send_all(mod_fd, resume_bytes.data(), resume_bytes.size(), false));
          const auto mod_resume = read_single_frame(mod_fd);
          CHECK(mod_resume && (*mod_resume)["type"] == "resume_result" && (*mod_resume)["ok"]);
          (void)read_single_frame(mod_fd); // mod server_event
          (void)read_single_frame(mod_fd); // mod market_snapshot

          // Player 1 receives match_resumed and snapshot with is_paused == false over unix socket
          const auto p_resume_ev = read_single_frame(player_fd);
          CHECK(p_resume_ev && (*p_resume_ev)["type"] == "server_event" && (*p_resume_ev)["event"] == "match_resumed");
          const auto p_resume_snap = read_single_frame(player_fd);
          CHECK(p_resume_snap && (*p_resume_snap)["type"] == "market_snapshot" && (*p_resume_snap)["is_paused"] == false);

          // Moderator aborts match over unix socket
          const auto abort_bytes = Frame(json{{"type", "abort"}, {"match_id", match_id}}).serialize();
          CHECK(send_all(mod_fd, abort_bytes.data(), abort_bytes.size(), false));
          const auto mod_abort = read_single_frame(mod_fd);
          CHECK(mod_abort && (*mod_abort)["type"] == "abort_result" && (*mod_abort)["ok"]);
          (void)read_single_frame(mod_fd); // mod server_event
          (void)read_single_frame(mod_fd); // mod market_snapshot

          // Player 1 receives match_aborted and terminal snapshot with outcome Aborted
          const auto p_abort_ev = read_single_frame(player_fd);
          CHECK(p_abort_ev && (*p_abort_ev)["type"] == "server_event" && (*p_abort_ev)["event"] == "match_aborted");
          const auto p_abort_snap = read_single_frame(player_fd);
          CHECK(p_abort_snap && (*p_abort_snap)["type"] == "market_snapshot" && (*p_abort_snap)["outcome"] == "Aborted");

          // Post-abort command over unix socket is rejected
          const auto post_cmd = request_json(player_fd, json{{"type", "trade"}, {"match_id", match_id}, {"action", "HOLD"}});
          CHECK(post_cmd && (*post_cmd)["type"] == "error");

          for (const auto fd : extra_players) (void)close(fd);
          (void)close(player_fd);
        }
        (void)close(mod_fd);
      }

      CHECK(kill(child, SIGTERM) == 0);
      int status = 0;
      CHECK(waitpid(child, &status, 0) == child);
      struct stat st {};
      CHECK(::lstat(notif_path.c_str(), &st) != 0 && errno == ENOENT);
    }
  }
}

int run_server_exit_code(const char* server_path, const std::vector<std::string>& args) {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    const int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
      close(dev_null);
    }
    std::vector<const char*> argv;
    argv.push_back(server_path);
    for (const auto& a : args) {
      argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    execv(server_path, const_cast<char* const*>(argv.data()));
    _exit(127);
  }
  int status = 0;
  waitpid(child, &status, 0);
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return -1;
}

pid_t spawn_server_proc(const char* server_path, const std::vector<std::string>& args) {
  const auto child = fork();
  if (child < 0) return -1;
  if (child == 0) {
    const int dev_null = open("/dev/null", O_WRONLY);
    if (dev_null >= 0) {
      dup2(dev_null, STDOUT_FILENO);
      dup2(dev_null, STDERR_FILENO);
      close(dev_null);
    }
    std::vector<const char*> argv;
    argv.push_back(server_path);
    for (const auto& a : args) {
      argv.push_back(a.c_str());
    }
    argv.push_back(nullptr);
    execv(server_path, const_cast<char* const*>(argv.data()));
    _exit(127);
  }
  return child;
}

int connect_to_addr(const std::string& host, std::uint16_t port, int max_attempts = 80) {
  for (int attempt = 0; attempt < max_attempts; ++attempt) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_port = htons(port);
      if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) > 0) {
        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
          return fd;
        }
      }
      close(fd);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return -1;
}

void test_lan_binding(const char* server_path) {
  std::cout << "Testing LAN bind configuration, default loopback, and argument validation...\n";

  // 1. Invalid bind address rejections (exit code 2)
  CHECK(run_server_exit_code(server_path, {"--bind-address"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address="}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", ""}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "0.0.0.0"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address=0.0.0.0"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "8.8.8.8"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "224.0.0.1"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "255.255.255.255"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "::1"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "fe80::1"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "invalid_host"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "192.168.1.999"}) == 2);
  CHECK(run_server_exit_code(server_path, {"--bind-address", "192.168.1"}) == 2);
  CHECK(run_server_exit_code(server_path, {"-b", "8.8.8.8"}) == 2);
  CHECK(run_server_exit_code(server_path, {"-b=8.8.8.8"}) == 2);

  // 2. Unassigned RFC1918 address fails startup with exit code 1
  CHECK(run_server_exit_code(server_path, {"--bind-address", "192.168.254.254", "--no-unix"}) == 1);

  // 3. Default loopback binding (no --bind-address specified)
  {
    const auto p = reserve_port();
    CHECK(p != 0U);
    const auto child = spawn_server_proc(server_path, {"--port", std::to_string(p), "--no-unix"});
    CHECK(child > 0);
    const int fd = connect_to_addr("127.0.0.1", p);
    CHECK(fd >= 0);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
    (void)close(fd);
    CHECK(kill(child, SIGTERM) == 0);
    waitpid(child, nullptr, 0);
  }

  // 4. Explicit loopback binding with --bind-address
  {
    const auto p = reserve_port();
    CHECK(p != 0U);
    const auto child = spawn_server_proc(server_path, {"--bind-address", "127.0.0.1", "--port", std::to_string(p), "--no-unix"});
    CHECK(child > 0);
    const int fd = connect_to_addr("127.0.0.1", p);
    CHECK(fd >= 0);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
    (void)close(fd);
    CHECK(kill(child, SIGTERM) == 0);
    waitpid(child, nullptr, 0);
  }

  // 5. Option order and flag formats (--bind-address=, -b, and reversed order)
  {
    const auto p = reserve_port();
    CHECK(p != 0U);
    const auto child = spawn_server_proc(server_path, {"--port", std::to_string(p), "--bind-address=127.0.0.1", "--no-unix"});
    CHECK(child > 0);
    const int fd = connect_to_addr("127.0.0.1", p);
    CHECK(fd >= 0);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
    (void)close(fd);
    CHECK(kill(child, SIGTERM) == 0);
    waitpid(child, nullptr, 0);
  }

  // 6. Test binding to an assigned non-loopback private IPv4 interface if one exists
  std::string found_lan_ip;
  struct ifaddrs *ifaddr = nullptr;
  if (getifaddrs(&ifaddr) == 0) {
    for (struct ifaddrs *ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == nullptr) continue;
      if (ifa->ifa_addr->sa_family == AF_INET) {
        char host[INET_ADDRSTRLEN];
        auto *sa = reinterpret_cast<struct sockaddr_in *>(ifa->ifa_addr);
        if (inet_ntop(AF_INET, &(sa->sin_addr), host, sizeof(host)) != nullptr) {
          const std::string ip(host);
          const uint32_t host_ip = ntohl(sa->sin_addr.s_addr);
          const bool is_loopback = ((host_ip >> 24U) == 127U);
          const bool is_10_net = ((host_ip >> 24U) == 10U);
          const bool is_172_net = (host_ip >= 0xAC100000U && host_ip <= 0xAC1FFFFFU);
          const bool is_192_net = ((host_ip >> 16U) == 0xC0A8U);
          if (!is_loopback && (is_10_net || is_172_net || is_192_net)) {
            found_lan_ip = ip;
            break;
          }
        }
      }
    }
    freeifaddrs(ifaddr);
  }

  if (!found_lan_ip.empty()) {
    std::cout << "  Found active private RFC1918 interface: " << found_lan_ip << ", testing live binding...\n";
    const auto p = reserve_port();
    CHECK(p != 0U);
    const auto child = spawn_server_proc(server_path, {"--bind-address", found_lan_ip, "--port", std::to_string(p), "--no-unix"});
    CHECK(child > 0);
    const int fd = connect_to_addr(found_lan_ip, p);
    CHECK(fd >= 0);
    CHECK(exchange(fd, json{{"type", "connect"}, {"role", "player"}}, false));
    (void)close(fd);
    CHECK(kill(child, SIGTERM) == 0);
    waitpid(child, nullptr, 0);
    std::cout << "  Live LAN binding and protocol handshake verified on " << found_lan_ip << ":" << p << "\n";
  } else {
    std::cout << "  [SKIPPED] No non-loopback private IPv4 interface found in environment for live LAN bind test\n";
  }
}

void test_min_player_and_boundary_rejection(const char* server_path) {
  std::cout << "Running test_min_player_and_boundary_rejection...\n";
  const auto port = reserve_port();
  CHECK(port != 0U);
  if (port == 0U) return;
  const auto child = start_server(server_path, port, "min-player-test-token");
  CHECK(child >= 0);
  if (child < 0) return;

  std::vector<int> connected;
  const auto cleanup = [&]() {
    for (const auto fd : connected) (void)close(fd);
    (void)kill(child, SIGTERM);
    (void)waitpid(child, nullptr, 0);
  };

  const int moderator = connect_with_retry(port);
  CHECK(moderator >= 0);
  if (moderator < 0) { cleanup(); return; }
  connected.push_back(moderator);
  timeval timeout{2, 0};
  (void)setsockopt(moderator, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  // Authenticate moderator
  const auto mod_hello = request_json(moderator, json{{"type", "connect"}, {"role", "moderator"}, {"token", "min-player-test-token"}});
  CHECK(mod_hello && (*mod_hello)["type"] == "connect" && (*mod_hello)["role"] == "moderator");

  // 1. Explicitly verify N=2 boundary rejection over TCP protocol (lower illegal boundary)
  const auto req_two = request_json(moderator, json{{"type", "create_match"}, {"player_count", 2}});
  CHECK(req_two && (*req_two)["type"] == "error");
  CHECK(req_two && (*req_two)["error"] == "player_count must be an integer from 3 through 64");

  // 2. Reject other invalid counts: 0, 1, 65, -1, string, float
  for (const auto& bad_val : json::array({0, 1, 65, -1, -10, "3", 3.5})) {
    const auto bad_req = request_json(moderator, json{{"type", "create_match"}, {"player_count", bad_val}});
    CHECK(bad_req && (*bad_req)["type"] == "error");
    CHECK(bad_req && (*bad_req)["error"] == "player_count must be an integer from 3 through 64");
  }

  // 3. Reject client-supplied roles
  const auto bad_roles = request_json(moderator, json{{"type", "create_match"},
      {"roles", {"Insider", "Investigator", "Employee"}}});
  CHECK(bad_roles && (*bad_roles)["type"] == "error");
  CHECK(bad_roles && (*bad_roles)["error"] == "client-supplied roles are not accepted");

  // 4. Verify boundary role counts on match creation: N=3, N=5, N=32, N=64
  const auto m32 = request_json(moderator, json{{"type", "create_match"}, {"player_count", 32}});
  CHECK(m32 && (*m32)["type"] == "match_created" && (*m32)["player_count"] == 32U);
  CHECK(m32 && (*m32)["insiders"] == 4U && (*m32)["investigators"] == 2U && (*m32)["employees"] == 26U);

  const auto m64 = request_json(moderator, json{{"type", "create_match"}, {"player_count", 64}});
  CHECK(m64 && (*m64)["type"] == "match_created" && (*m64)["player_count"] == 64U);
  CHECK(m64 && (*m64)["insiders"] == 8U && (*m64)["investigators"] == 4U && (*m64)["employees"] == 52U);

  const auto m5 = request_json(moderator, json{{"type", "create_match"}, {"player_count", 5}});
  CHECK(m5 && (*m5)["type"] == "match_created" && (*m5)["player_count"] == 5U);
  CHECK(m5 && (*m5)["insiders"] == 1U && (*m5)["investigators"] == 1U && (*m5)["employees"] == 3U);

  // 5. Create 3-player match and verify complete end-to-end match lifecycle
  const auto m3 = request_json(moderator, json{{"type", "create_match"}, {"player_count", 3}});
  CHECK(m3 && (*m3)["type"] == "match_created" && (*m3)["player_count"] == 3U);
  CHECK(m3 && (*m3)["insiders"] == 1U && (*m3)["investigators"] == 1U && (*m3)["employees"] == 1U);
  if (!m3 || !m3->contains("match_id")) { cleanup(); return; }
  const auto match_id = (*m3)["match_id"].get<std::uint64_t>();

  // Moderator cannot join as a player
  const auto mod_join = request_json(moderator, json{{"type", "join_match"}, {"match_id", match_id}});
  CHECK(mod_join && (*mod_join)["type"] == "error");
  CHECK(mod_join && (*mod_join)["error"] == "player permission required");

  // Connect 3 player TCP clients
  std::vector<int> players;
  for (std::size_t i = 0; i < 3U; ++i) {
    const int pfd = connect_to(port);
    CHECK(pfd >= 0);
    if (pfd < 0) { cleanup(); return; }
    connected.push_back(pfd);
    players.push_back(pfd);
    (void)setsockopt(pfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    const auto phello = request_json(pfd, json{{"type", "connect"}, {"role", "player"}});
    CHECK(phello && (*phello)["type"] == "connect" && (*phello)["role"] == "player");
  }

  // Premature start with 0 players joined
  const auto start_0 = command(moderator, match_id, "start_match");
  CHECK(start_0 && (*start_0)["type"] == "error" && (*start_0)["error"] == "all configured players must join before start");

  // Player 1 joins seat 1
  const auto j1 = request_json(players[0], json{{"type", "join_match"}, {"match_id", match_id}});
  CHECK(j1 && (*j1)["type"] == "joined_match" && (*j1)["seat"] == 1U && (*j1)["player_id"] == 1U);

  // Premature start with 1 player joined
  const auto start_1 = command(moderator, match_id, "start_match");
  CHECK(start_1 && (*start_1)["type"] == "error" && (*start_1)["error"] == "all configured players must join before start");

  // Player 2 joins seat 2
  const auto j2 = request_json(players[1], json{{"type", "join_match"}, {"match_id", match_id}});
  CHECK(j2 && (*j2)["type"] == "joined_match" && (*j2)["seat"] == 2U && (*j2)["player_id"] == 2U);

  // Premature start with 2 players joined (out of 3 required)
  const auto start_2 = command(moderator, match_id, "start_match");
  CHECK(start_2 && (*start_2)["type"] == "error" && (*start_2)["error"] == "all configured players must join before start");

  // Player 3 joins seat 3
  const auto j3 = request_json(players[2], json{{"type", "join_match"}, {"match_id", match_id}});
  CHECK(j3 && (*j3)["type"] == "joined_match" && (*j3)["seat"] == 3U && (*j3)["player_id"] == 3U);

  // Connect a 4th player and verify 4th join is rejected ("match is full")
  const int p4 = connect_to(port);
  CHECK(p4 >= 0);
  if (p4 >= 0) {
    connected.push_back(p4);
    (void)setsockopt(p4, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)request_json(p4, json{{"type", "connect"}, {"role", "player"}});
    const auto j4 = request_json(p4, json{{"type", "join_match"}, {"match_id", match_id}});
    CHECK(j4 && (*j4)["type"] == "error" && (*j4)["error"] == "match is full");
  }

  // Verify role distribution across the 3 seats
  std::size_t insider_idx = 3;
  std::size_t investigator_idx = 3;
  std::size_t employee_idx = 3;
  std::size_t insiders = 0, investigators = 0, employees = 0;
  const std::array<json, 3> joins{j1.value(), j2.value(), j3.value()};
  for (std::size_t i = 0; i < 3U; ++i) {
    const auto r = joins[i]["private_role"].get<std::string>();
    if (r == "Insider") { ++insiders; insider_idx = i; }
    else if (r == "Investigator") { ++investigators; investigator_idx = i; }
    else if (r == "Employee") { ++employees; employee_idx = i; }
  }
  CHECK(insiders == 1U && investigators == 1U && employees == 1U);
  CHECK(insider_idx < 3U && investigator_idx < 3U && employee_idx < 3U);

  // Moderator starts match after all 3 players joined
  const auto start_ok = command(moderator, match_id, "start_match");
  CHECK(start_ok && (*start_ok)["type"] == "transition_result" && (*start_ok)["ok"]);
  CHECK(start_ok && (*start_ok)["phase"] == "AwaitingEvent");

  // Execute all 3 rounds of the 3-player match
  for (std::uint32_t round = 1; round <= 3U; ++round) {
    // 1. Publish event
    const auto event = request_json(moderator, json{{"type", "publish_event"},
        {"match_id", match_id}, {"impact", 0}});
    CHECK(event && (*event)["type"] == "transition_result" && (*event)["phase"] == "Discussion");

    // 2. Insider private hint
    const auto hint = command(players[insider_idx], match_id, "insider_hint");
    CHECK(hint && (*hint)["type"] == "hint_private" && (*hint)["event_impact"] == 0);

    // Verify public status does not leak event impact
    const auto pub_disc = request_json(players[employee_idx],
        json{{"type", "match_status"}, {"match_id", match_id}});
    CHECK(pub_disc && !pub_disc->contains("event_impact"));

    // 3. Open trading and submit trades
    const auto trading = command(moderator, match_id, "open_trading");
    CHECK(trading && (*trading)["phase"] == "Trading");
    for (const auto pfd : players) {
      const auto trade = request_json(pfd, json{{"type", "trade"},
          {"match_id", match_id}, {"action", "HOLD"}});
      CHECK(trade && (*trade)["type"] == "trade_result" && (*trade)["ok"]);
    }

    // 4. Resolve market
    const auto market = command(moderator, match_id, "resolve_market");
    CHECK(market && (*market)["type"] == "market_result" && (*market)["ok"]);
    CHECK(market && (*market)["closing_health"] == 5 && (*market)["outcome"] == "None");

    // 5. Clue phase
    const auto clue_st = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
    CHECK(clue_st && (*clue_st)["phase"] == "Clue");

    if (round == 1U) {
      const auto employee_pid = joins[employee_idx]["player_id"].get<std::uint64_t>();
      const auto clue = request_json(players[investigator_idx], json{{"type", "request_clue"},
          {"match_id", match_id}, {"target_id", employee_pid}});
      CHECK(clue && (*clue)["type"] == "clue_private" && (*clue)["is_insider"] == false);
    }

    // Public status must not leak clue
    const auto pub_clue = request_json(players[employee_idx],
        json{{"type", "match_status"}, {"match_id", match_id}});
    CHECK(pub_clue && !pub_clue->contains("clue_result") && !pub_clue->contains("clue_target"));

    // 6. Open voting and submit votes
    const auto voting = command(moderator, match_id, "open_voting");
    CHECK(voting && (*voting)["phase"] == "Voting");
    for (const auto pfd : players) {
      const auto vote = request_json(pfd, json{{"type", "vote"},
          {"match_id", match_id}, {"target_id", nullptr}});
      CHECK(vote && (*vote)["type"] == "vote_result" && (*vote)["accepted"]);
    }

    // 7. Resolve votes
    const auto resolved = command(moderator, match_id, "resolve_votes");
    CHECK(resolved && (*resolved)["type"] == "vote_resolution" && (*resolved)["ok"]);
    CHECK(resolved && (*resolved)["eliminated_player"].is_null());

    if (round < 3U) {
      CHECK(resolved && (*resolved)["outcome"] == "None");
      const auto next_st = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
      CHECK(next_st && (*next_st)["phase"] == "AwaitingEvent" && (*next_st)["round"] == round + 1U);
    } else {
      CHECK(resolved && (*resolved)["outcome"] == "InsiderWin");
      const auto fin_st = request_json(players[0], json{{"type", "match_status"}, {"match_id", match_id}});
      CHECK(fin_st && (*fin_st)["phase"] == "Finished" && (*fin_st)["outcome"] == "InsiderWin");
    }
  }

  cleanup();
}

void test_notifications_and_market_snapshots(const char* server_path) {
  std::cout << "Running test_notifications_and_market_snapshots...\n";
  const auto port = reserve_port();
  CHECK(port != 0U);
  if (port == 0U) return;
  const auto child = start_server(server_path, port, "notif-test-token");
  CHECK(child >= 0);
  if (child < 0) return;

  std::vector<int> connected;
  const auto cleanup = [&]() {
    for (const auto fd : connected) (void)close(fd);
    (void)kill(child, SIGTERM);
    (void)waitpid(child, nullptr, 0);
  };

  const int moderator = connect_with_retry(port);
  CHECK(moderator >= 0);
  if (moderator < 0) { cleanup(); return; }
  connected.push_back(moderator);
  set_receive_timeout(moderator, 3000);

  const auto mod_hello = request_json(moderator, json{{"type", "connect"}, {"role", "moderator"}, {"token", "notif-test-token"}});
  CHECK(mod_hello && (*mod_hello)["type"] == "connect");

  const auto create_resp = request_json(moderator, json{{"type", "create_match"}, {"player_count", 5}});
  CHECK(create_resp && (*create_resp)["type"] == "match_created");
  const auto match_id = (*create_resp)["match_id"].get<std::uint64_t>();

  std::vector<int> players;
  for (std::size_t i = 0; i < 5; ++i) {
    const int pfd = connect_with_retry(port);
    CHECK(pfd >= 0);
    if (pfd < 0) { cleanup(); return; }
    connected.push_back(pfd);
    players.push_back(pfd);
    set_receive_timeout(pfd, 3000);

    const auto p_hello = request_json(pfd, json{{"type", "connect"}, {"role", "player"}});
    CHECK(p_hello && (*p_hello)["type"] == "connect");

    // Send join_match
    const auto join_outgoing = Frame(json{{"type", "join_match"}, {"match_id", match_id}}).serialize();
    CHECK(send_all(pfd, join_outgoing.data(), join_outgoing.size(), false));

    // Player must receive joined_match response first
    const auto join_resp = read_single_frame(pfd);
    CHECK(join_resp && (*join_resp)["type"] == "joined_match");
    CHECK(join_resp && (*join_resp)["match_id"] == match_id);
    CHECK(join_resp && (*join_resp)["player_id"] == i + 1U);

    // Player must immediately receive initial market snapshot pushed without polling status!
    const auto init_snapshot = read_single_frame(pfd);
    CHECK(init_snapshot && (*init_snapshot)["type"] == "market_snapshot");
    CHECK(init_snapshot && (*init_snapshot)["match_id"] == match_id);
    CHECK(init_snapshot && (*init_snapshot)["state_version"] == 1U);
    CHECK(init_snapshot && (*init_snapshot)["phase"] == "Lobby");
    CHECK(init_snapshot && (*init_snapshot)["round"] == 0U);
    CHECK(init_snapshot && (*init_snapshot)["health"] == 5);
    CHECK(init_snapshot && (*init_snapshot)["price"] == 10);
    CHECK(init_snapshot && (*init_snapshot)["total_supply"] == 100U);
    CHECK(init_snapshot && (*init_snapshot)["reserve"] == 50U);
    CHECK(init_snapshot && (*init_snapshot)["shares_held_by_players"] == 50U);
    CHECK(init_snapshot && (*init_snapshot)["outcome"] == "None");
  }

  // Moderator starts the match
  const auto start_outgoing = Frame(json{{"type", "start_match"}, {"match_id", match_id}}).serialize();
  CHECK(send_all(moderator, start_outgoing.data(), start_outgoing.size(), false));

  // Moderator gets transition_result first
  const auto start_resp = read_single_frame(moderator);
  CHECK(start_resp && (*start_resp)["type"] == "transition_result" && (*start_resp)["ok"]);

  // Moderator gets pushed match_started event and market snapshot
  const auto mod_ev = read_single_frame(moderator);
  CHECK(mod_ev && (*mod_ev)["type"] == "server_event" && (*mod_ev)["event"] == "match_started");
  CHECK(mod_ev && (*mod_ev)["state_version"] == 2U);
  const auto mod_snap = read_single_frame(moderator);
  CHECK(mod_snap && (*mod_snap)["type"] == "market_snapshot" && (*mod_snap)["state_version"] == 2U);

  // ALL 5 players receive match_started event and market_snapshot pushed while idle!
  for (std::size_t i = 0; i < 5; ++i) {
    const auto p_ev = read_single_frame(players[i]);
    CHECK(p_ev && (*p_ev)["type"] == "server_event");
    CHECK(p_ev && (*p_ev)["event"] == "match_started");
    CHECK(p_ev && (*p_ev)["match_id"] == match_id);
    CHECK(p_ev && (*p_ev)["round"] == 1U);
    CHECK(p_ev && (*p_ev)["phase"] == "AwaitingEvent");
    CHECK(p_ev && (*p_ev)["state_version"] == 2U);

    const auto p_snap = read_single_frame(players[i]);
    CHECK(p_snap && (*p_snap)["type"] == "market_snapshot");
    CHECK(p_snap && (*p_snap)["match_id"] == match_id);
    CHECK(p_snap && (*p_snap)["round"] == 1U);
    CHECK(p_snap && (*p_snap)["phase"] == "AwaitingEvent");
    CHECK(p_snap && (*p_snap)["state_version"] == 2U);
  }

  // Moderator publishes event (impact 0)
  const auto pub_outgoing = Frame(json{{"type", "publish_event"}, {"match_id", match_id}, {"impact", 0}}).serialize();
  CHECK(send_all(moderator, pub_outgoing.data(), pub_outgoing.size(), false));
  const auto pub_resp = read_single_frame(moderator);
  CHECK(pub_resp && (*pub_resp)["type"] == "transition_result" && (*pub_resp)["ok"]);
  (void)read_single_frame(moderator); // mod server_event
  (void)read_single_frame(moderator); // mod market_snapshot

  // All 5 players receive phase_started (Discussion) and market_snapshot
  for (std::size_t i = 0; i < 5; ++i) {
    const auto p_ev = read_single_frame(players[i]);
    CHECK(p_ev && (*p_ev)["type"] == "server_event");
    CHECK(p_ev && (*p_ev)["event"] == "phase_started");
    CHECK(p_ev && (*p_ev)["phase"] == "Discussion");
    CHECK(p_ev && (*p_ev)["state_version"] == 3U);

    const auto p_snap = read_single_frame(players[i]);
    CHECK(p_snap && (*p_snap)["type"] == "market_snapshot");
    CHECK(p_snap && (*p_snap)["phase"] == "Discussion");
    CHECK(p_snap && (*p_snap)["state_version"] == 3U);
  }

  // Moderator opens trading
  const auto tr_outgoing = Frame(json{{"type", "open_trading"}, {"match_id", match_id}}).serialize();
  CHECK(send_all(moderator, tr_outgoing.data(), tr_outgoing.size(), false));
  const auto tr_resp = read_single_frame(moderator);
  CHECK(tr_resp && (*tr_resp)["type"] == "transition_result" && (*tr_resp)["ok"]);
  (void)read_single_frame(moderator); // mod server_event
  (void)read_single_frame(moderator); // mod market_snapshot

  // All 5 players receive phase_started (Trading) and market_snapshot
  for (std::size_t i = 0; i < 5; ++i) {
    const auto p_ev = read_single_frame(players[i]);
    CHECK(p_ev && (*p_ev)["type"] == "server_event");
    CHECK(p_ev && (*p_ev)["event"] == "phase_started");
    CHECK(p_ev && (*p_ev)["phase"] == "Trading");
    CHECK(p_ev && (*p_ev)["state_version"] == 4U);

    const auto p_snap = read_single_frame(players[i]);
    CHECK(p_snap && (*p_snap)["type"] == "market_snapshot");
    CHECK(p_snap && (*p_snap)["phase"] == "Trading");
    CHECK(p_snap && (*p_snap)["state_version"] == 4U);
  }

  // Stage 3: Test pause, resume, abort push notification delivery over TCP
  // 1. Moderator pauses match
  const auto pause_outgoing = Frame(json{{"type", "pause"}, {"match_id", match_id}}).serialize();
  CHECK(send_all(moderator, pause_outgoing.data(), pause_outgoing.size(), false));
  const auto pause_resp = read_single_frame(moderator);
  CHECK(pause_resp && (*pause_resp)["type"] == "pause_result" && (*pause_resp)["ok"]);
  (void)read_single_frame(moderator); // mod server_event (match_paused)
  (void)read_single_frame(moderator); // mod market_snapshot

  // All 5 players receive match_paused and market_snapshot with is_paused == true
  for (std::size_t i = 0; i < 5; ++i) {
    const auto p_ev = read_single_frame(players[i]);
    CHECK(p_ev && (*p_ev)["type"] == "server_event");
    CHECK(p_ev && (*p_ev)["event"] == "match_paused");
    CHECK(p_ev && (*p_ev)["state_version"] == 5U);

    const auto p_snap = read_single_frame(players[i]);
    CHECK(p_snap && (*p_snap)["type"] == "market_snapshot");
    CHECK(p_snap && (*p_snap)["is_paused"] == true);
    CHECK(p_snap && (*p_snap)["state_version"] == 5U);
  }

  // 2. Moderator resumes match
  const auto resume_outgoing = Frame(json{{"type", "resume"}, {"match_id", match_id}}).serialize();
  CHECK(send_all(moderator, resume_outgoing.data(), resume_outgoing.size(), false));
  const auto resume_resp = read_single_frame(moderator);
  CHECK(resume_resp && (*resume_resp)["type"] == "resume_result" && (*resume_resp)["ok"]);
  (void)read_single_frame(moderator); // mod server_event (match_resumed)
  (void)read_single_frame(moderator); // mod market_snapshot

  // All 5 players receive match_resumed and market_snapshot with is_paused == false
  for (std::size_t i = 0; i < 5; ++i) {
    const auto p_ev = read_single_frame(players[i]);
    CHECK(p_ev && (*p_ev)["type"] == "server_event");
    CHECK(p_ev && (*p_ev)["event"] == "match_resumed");
    CHECK(p_ev && (*p_ev)["state_version"] == 6U);

    const auto p_snap = read_single_frame(players[i]);
    CHECK(p_snap && (*p_snap)["type"] == "market_snapshot");
    CHECK(p_snap && (*p_snap)["is_paused"] == false);
    CHECK(p_snap && (*p_snap)["state_version"] == 6U);
  }

  // 3. Moderator aborts match
  const auto abort_outgoing = Frame(json{{"type", "abort"}, {"match_id", match_id}}).serialize();
  CHECK(send_all(moderator, abort_outgoing.data(), abort_outgoing.size(), false));
  const auto abort_resp = read_single_frame(moderator);
  CHECK(abort_resp && (*abort_resp)["type"] == "abort_result" && (*abort_resp)["ok"]);
  (void)read_single_frame(moderator); // mod server_event (match_aborted)
  (void)read_single_frame(moderator); // mod market_snapshot

  // All 5 players receive match_aborted and market_snapshot with outcome == "Aborted"
  for (std::size_t i = 0; i < 5; ++i) {
    const auto p_ev = read_single_frame(players[i]);
    CHECK(p_ev && (*p_ev)["type"] == "server_event");
    CHECK(p_ev && (*p_ev)["event"] == "match_aborted");
    CHECK(p_ev && (*p_ev)["phase"] == "Finished");
    CHECK(p_ev && (*p_ev)["outcome"] == "Aborted");
    CHECK(p_ev && (*p_ev)["state_version"] == 7U);

    const auto p_snap = read_single_frame(players[i]);
    CHECK(p_snap && (*p_snap)["type"] == "market_snapshot");
    CHECK(p_snap && (*p_snap)["phase"] == "Finished");
    CHECK(p_snap && (*p_snap)["outcome"] == "Aborted");
    CHECK(p_snap && (*p_snap)["state_version"] == 7U);
  }

  cleanup();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2 && argc != 3) {
    std::cerr << "expected server executable path [capacity|timeout|auth|gameplay|variable-gameplay|min-player|malformed|unix|lan-binding|notifications|all]\n";
    return EXIT_FAILURE;
  }
  if (argc == 3 && std::string(argv[2]) == "gameplay") {
    test_complete_three_round_match(argv[1], 5U);
  } else if (argc == 3 && std::string(argv[2]) == "variable-gameplay") {
    test_complete_three_round_match(argv[1], 3U);
    test_complete_three_round_match(argv[1], 8U);
  } else if (argc == 3 && std::string(argv[2]) == "min-player") {
    test_min_player_and_boundary_rejection(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "auth") {
    test_moderator_authentication(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "malformed") {
    test_malformed_tcp_isolation(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "timeout") {
    test_idle_unassigned_timeout(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "unix") {
    test_unix_socket_lifecycle_and_gameplay(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "lan-binding") {
    test_lan_binding(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "notifications") {
    test_notifications_and_market_snapshots(argv[1]);
  } else if (argc == 3 && std::string(argv[2]) == "all") {
    test_server_capacity_and_echo(argv[1]);
    test_idle_unassigned_timeout(argv[1]);
    test_moderator_authentication(argv[1]);
    test_min_player_and_boundary_rejection(argv[1]);
    test_complete_three_round_match(argv[1], 3U);
    test_complete_three_round_match(argv[1], 5U);
    test_complete_three_round_match(argv[1], 8U);
    test_unix_socket_lifecycle_and_gameplay(argv[1]);
    test_lan_binding(argv[1]);
    test_notifications_and_market_snapshots(argv[1]);
  } else {
    test_server_capacity_and_echo(argv[1]);
  }
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Integration tests passed.\n";
  return EXIT_SUCCESS;
}
