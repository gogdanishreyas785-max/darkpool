#pragma once

#include "darkpool/game_engine.hpp"
#include "darkpool/network.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace darkpool::server {

enum class SessionRole { Player, Moderator };

class MatchCoordinator {
 public:
  void register_session(std::uint64_t session, SessionRole role);
  void unregister_session(std::uint64_t session) noexcept;

  // Returns nullopt for messages outside the coordinator command set (echo path).
  [[nodiscard]] std::optional<network::json> handle(
      std::uint64_t session, const network::json& command);

 private:
  struct Session {
    SessionRole role{SessionRole::Player};
    std::optional<std::uint64_t> player_match;
    std::optional<PlayerId> player_id;
    std::vector<std::uint64_t> moderator_matches;
  };
  struct MatchRecord {
    std::unique_ptr<Match> match;
    std::uint64_t moderator{};
    std::vector<std::optional<std::uint64_t>> player_sessions;
  };

  [[nodiscard]] bool is_member(std::uint64_t session, std::uint64_t match_id) const;
  std::map<std::uint64_t, Session> sessions_;
  std::map<std::uint64_t, MatchRecord> matches_;
  std::uint64_t next_match_id_{1};
};

}  // namespace darkpool::server
