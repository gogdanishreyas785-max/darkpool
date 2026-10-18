#include "darkpool/token_store.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {
int failures = 0;

void check(bool condition, const char* expression, int line) {
  if (!condition) {
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    ++failures;
  }
}
#define CHECK(value) check(static_cast<bool>(value), #value, __LINE__)

unsigned int mode_of(const std::filesystem::path& path) {
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return 0U;
  return static_cast<unsigned int>(st.st_mode & 0777);
}
} // namespace

int main() {
  using darkpool::security::TokenStore;
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
      ("darkpool-token-store-test-" + std::to_string(::getpid()) + "-" + std::to_string(nonce));
  const auto parent = root / "parent";
  const auto tokens = parent / "tokens";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  ec.clear();
  std::filesystem::create_directories(parent, ec);
  CHECK(!ec);
  if (ec) return EXIT_FAILURE;
  CHECK(::chmod(parent.c_str(), 0755) == 0);

  // Secure the token directory without changing permissions on its ancestors.
  CHECK(TokenStore::ensure_secure_dir(tokens.string()));
  CHECK(mode_of(parent) == 0755U);
  CHECK(mode_of(tokens) == 0700U);

  CHECK(TokenStore::save_match_token(42U, "secret-token", tokens.string()));
  CHECK(mode_of(tokens / "match_42.token") == 0600U);
  CHECK(TokenStore::load_match_token(42U, tokens.string()).value_or("") == "secret-token");
  CHECK(TokenStore::latest_match_id(tokens.string()).value_or(0U) == 42U);

  CHECK(::chmod((tokens / "match_42.token").c_str(), 0644) == 0);
  CHECK(!TokenStore::load_match_token(42U, tokens.string()).has_value());
  CHECK(::chmod((tokens / "match_42.token").c_str(), 0600) == 0);

  const auto outside = root / "outside.token";
  {
    std::ofstream out(outside);
    out << "outside-secret\n";
  }
  CHECK(::symlink(outside.c_str(), (tokens / "match_43.token").c_str()) == 0);
  CHECK(!TokenStore::load_match_token(43U, tokens.string()).has_value());

  CHECK(TokenStore::remove_match_token(42U, tokens.string()));
  CHECK(!std::filesystem::exists(tokens / "match_42.token"));
  CHECK(!TokenStore::latest_match_id(tokens.string()).has_value());

  std::filesystem::remove_all(root, ec);
  if (failures != 0) return EXIT_FAILURE;
  std::cout << "Token store security tests passed.\n";
  return EXIT_SUCCESS;
}
