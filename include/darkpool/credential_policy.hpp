#pragma once

#include <cstddef>
#include <string_view>

namespace darkpool::security {

// Credential strings are byte sequences. Enforce a minimum length so accidental
// short passwords are not accepted as network admission/authentication secrets.
// This is a shape check, not an entropy estimator: callers must still generate
// credentials with a cryptographically secure random source.
inline constexpr std::size_t kMinimumCredentialBytes = 32U;
inline constexpr std::size_t kMaximumCredentialBytes = 256U;

[[nodiscard]] constexpr bool has_valid_credential_length(std::string_view value) noexcept {
  return value.size() >= kMinimumCredentialBytes &&
         value.size() <= kMaximumCredentialBytes;
}

}  // namespace darkpool::security
