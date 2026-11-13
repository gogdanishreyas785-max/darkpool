#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#else
#include <random>
#endif

namespace darkpool::security {
namespace detail {

// The injectable fill function makes the failure path testable. No token is
// encoded unless the source filled every byte successfully.
template <typename Fill>
[[nodiscard]] std::optional<std::string> make_hex_token_128_with(Fill&& fill) noexcept {
  std::array<std::uint8_t, 16> bytes{};
  bool filled = false;
  try {
    filled = std::forward<Fill>(fill)(std::span<std::uint8_t>(bytes));
  } catch (...) {
    return std::nullopt;
  }
  if (!filled) return std::nullopt;

  static constexpr char kHexDigits[] = "0123456789abcdef";
  std::string token;
  try {
    token.reserve(bytes.size() * 2U);
    for (const auto byte : bytes) {
      token.push_back(kHexDigits[(byte >> 4U) & 0x0FU]);
      token.push_back(kHexDigits[byte & 0x0FU]);
    }
  } catch (...) {
    return std::nullopt;
  }
  return token;
}

}  // namespace detail

// Generates a 128-bit opaque token. On Linux, getrandom() is the only source:
// entropy-source failure is fatal and never falls back to a weaker PRNG.
[[nodiscard]] inline std::optional<std::string> generate_token_128() noexcept {
  return detail::make_hex_token_128_with([](std::span<std::uint8_t> output) noexcept {
#if defined(__linux__)
    std::size_t offset = 0;
    while (offset < output.size()) {
      const auto received = ::getrandom(output.data() + offset, output.size() - offset, 0);
      if (received < 0 && errno == EINTR) continue;
      if (received <= 0) return false;
      offset += static_cast<std::size_t>(received);
    }
    return true;
#else
    try {
      std::random_device entropy;
      if (entropy.entropy() <= 0.0) return false;
      for (auto& byte : output) {
        byte = static_cast<std::uint8_t>(entropy());
      }
      return true;
    } catch (...) {
      return false;
    }
#endif
  });
}

}  // namespace darkpool::security
