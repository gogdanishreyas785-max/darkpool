#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace darkpool::security {

class TokenStore {
public:
  // Saves match join token to a secure owner-only (0600) file in the token directory.
  // Returns true on success, false on error.
  static bool save_match_token(std::uint64_t match_id,
                               const std::string& token,
                               const std::string& custom_dir = "");

  // Loads match join token from secure file.
  // Strictly checks permissions (must be 0600 or 0400; fails closed if group/others have permissions).
  static std::optional<std::string> load_match_token(std::uint64_t match_id,
                                                     const std::string& custom_dir = "");

  // Removes match token file when match completes or is aborted.
  static bool remove_match_token(std::uint64_t match_id,
                                 const std::string& custom_dir = "");

  // Returns the latest active match ID recorded in the token store, if any.
  static std::optional<std::uint64_t> latest_match_id(const std::string& custom_dir = "");

  // Resolves the default directory for tokens:
  // 1. $DARKPOOL_TOKEN_DIR (if set and non-empty)
  // 2. $HOME/.darkpool/tokens (if $HOME set)
  // 3. /tmp/darkpool_tokens_<uid> (or /tmp/darkpool_tokens)
  static std::string default_token_dir();

  // Ensures directory exists with 0700 permissions.
  static bool ensure_secure_dir(const std::string& dir_path);
};

}  // namespace darkpool::security
