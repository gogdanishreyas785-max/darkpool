#pragma once

#include "darkpool/game_engine.hpp"
#include "darkpool/network.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace darkpool::cli {

enum class ClientRole { Player, Moderator };

struct CommandResult {
  bool send_frame{false};
  network::json payload{};
  bool should_exit{false};
  std::string local_message{};
  std::optional<std::string> prompt_for_input{};
};

class ClientState {
 public:
  explicit ClientState(ClientRole role);

  [[nodiscard]] ClientRole role() const noexcept { return role_; }
  [[nodiscard]] std::optional<std::uint64_t> match_id() const noexcept { return match_id_; }
  void set_match_id(std::uint64_t id) noexcept { match_id_ = id; }

  [[nodiscard]] std::optional<PlayerId> player_id() const noexcept { return player_id_; }
  void set_player_id(PlayerId id) noexcept { player_id_ = id; }

  [[nodiscard]] std::optional<std::string> private_role() const noexcept { return private_role_; }
  void set_private_role(std::string role_name) { private_role_ = std::move(role_name); }

  [[nodiscard]] std::string prompt() const;

  [[nodiscard]] CommandResult process_input(std::string_view raw_line);
  [[nodiscard]] CommandResult process_prompt_response(std::string_view response);

  [[nodiscard]] std::string format_response(const network::json& response);

  [[nodiscard]] static std::string help_text(ClientRole role);

 private:
  enum class PendingPrompt { None, TradeAction, ClueTarget, VoteTarget, EventImpact, JoinMatch };

  [[nodiscard]] CommandResult handle_moderator_command(std::string_view command,
                                                       const std::vector<std::string>& args);
  [[nodiscard]] CommandResult handle_player_command(std::string_view command,
                                                    const std::vector<std::string>& args);

  ClientRole role_{ClientRole::Player};
  std::optional<std::uint64_t> match_id_{std::nullopt};
  std::optional<PlayerId> player_id_{std::nullopt};
  std::optional<std::string> private_role_{std::nullopt};
  PendingPrompt pending_prompt_{PendingPrompt::None};
};

}  // namespace darkpool::cli
