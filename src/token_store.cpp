#include "darkpool/token_store.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace darkpool::security {

namespace {

std::string trim_token(std::string_view sv) {
  while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r' || sv.front() == '\n')) {
    sv.remove_prefix(1);
  }
  while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r' || sv.back() == '\n')) {
    sv.remove_suffix(1);
  }
  return std::string(sv);
}

bool write_file_0600(const std::string& path, const std::string& content) {
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
  int flags = O_WRONLY | O_CREAT | O_NONBLOCK;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif
  const int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
  if (fd < 0) return false;

  struct stat st;
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      st.st_uid != ::getuid() ||
      ::fchmod(fd, S_IRUSR | S_IWUSR) != 0 ||
      ::ftruncate(fd, 0) != 0) {
    (void)::close(fd);
    return false;
  }

  std::size_t written = 0;
  while (written < content.size()) {
    const auto res = ::write(fd, content.data() + written, content.size() - written);
    if (res < 0) {
      if (errno == EINTR) continue;
      (void)::close(fd);
      (void)::unlink(path.c_str());
      return false;
    }
    if (res == 0) {
      (void)::close(fd);
      (void)::unlink(path.c_str());
      return false;
    }
    written += static_cast<std::size_t>(res);
  }
  if (::close(fd) != 0) return false;
  return true;
#else
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << content;
  return out.good();
#endif
}

bool check_permissions_secure(const std::string& path) {
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
  struct stat st;
  if (::lstat(path.c_str(), &st) != 0) return false;
  if (!S_ISREG(st.st_mode) || st.st_uid != ::getuid()) return false;
  const auto mode = static_cast<unsigned int>(st.st_mode & 0777);
  return mode == 0600U || mode == 0400U;
#else
  return true;
#endif
}

}  // namespace

std::string TokenStore::default_token_dir() {
  if (const char* env_dir = std::getenv("DARKPOOL_TOKEN_DIR")) {
    if (std::strlen(env_dir) > 0) return std::string(env_dir);
  }
  if (const char* home = std::getenv("HOME")) {
    if (std::strlen(home) > 0) return std::string(home) + "/.darkpool/tokens";
  }
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
  const uid_t uid = ::getuid();
  return "/tmp/darkpool_tokens_" + std::to_string(uid);
#else
  return "/tmp/darkpool_tokens";
#endif
}

bool TokenStore::ensure_secure_dir(const std::string& dir_path) {
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
  if (dir_path.empty()) return false;
  std::error_code ec;
  (void)std::filesystem::create_directories(dir_path, ec);
  if (ec && !std::filesystem::is_directory(dir_path, ec)) return false;

  struct stat st;
  if (::lstat(dir_path.c_str(), &st) != 0 ||
      !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
      st.st_uid != ::getuid()) {
    return false;
  }
  // Restrict only the token directory itself; never chmod ancestors such as
  // $HOME, /home, or /tmp while walking a path.
  if (::chmod(dir_path.c_str(), S_IRWXU) != 0) return false;
  if (::lstat(dir_path.c_str(), &st) != 0 ||
      !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) ||
      st.st_uid != ::getuid() ||
      (st.st_mode & 0777) != S_IRWXU) {
    return false;
  }
  return true;
#else
  return true;
#endif
}

bool TokenStore::save_match_token(std::uint64_t match_id,
                                 const std::string& token,
                                 const std::string& custom_dir) {
  if (token.empty()) return false;
  const std::string dir = custom_dir.empty() ? default_token_dir() : custom_dir;
  if (!ensure_secure_dir(dir)) return false;

  const std::string match_file = dir + "/match_" + std::to_string(match_id) + ".token";
  const std::string token_with_newline = token + "\n";
  if (!write_file_0600(match_file, token_with_newline)) {
    return false;
  }

  // Record as latest match for fast CLI discovery
  const std::string latest_file = dir + "/match_latest.token";
  (void)write_file_0600(latest_file, token_with_newline);

  const std::string latest_id_file = dir + "/match_latest_id.txt";
  (void)write_file_0600(latest_id_file, std::to_string(match_id) + "\n");

  return true;
}

std::optional<std::string> TokenStore::load_match_token(std::uint64_t match_id,
                                                       const std::string& custom_dir) {
  const std::string dir = custom_dir.empty() ? default_token_dir() : custom_dir;
  const std::string match_file = dir + "/match_" + std::to_string(match_id) + ".token";

  if (!check_permissions_secure(match_file)) {
    return std::nullopt;
  }

  std::ifstream in(match_file, std::ios::binary);
  if (!in) return std::nullopt;
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string raw = buf.str();
  const std::string trimmed = trim_token(raw);
  if (trimmed.empty()) return std::nullopt;
  return trimmed;
}

bool TokenStore::remove_match_token(std::uint64_t match_id,
                                   const std::string& custom_dir) {
  const std::string dir = custom_dir.empty() ? default_token_dir() : custom_dir;
  const std::string match_file = dir + "/match_" + std::to_string(match_id) + ".token";
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
  (void)::unlink(match_file.c_str());
#endif

  // If this was latest match, remove latest pointers
  const std::string latest_id_file = dir + "/match_latest_id.txt";
  std::ifstream in(latest_id_file);
  if (in) {
    std::string line;
    if (std::getline(in, line)) {
      const auto parsed = trim_token(line);
      if (parsed == std::to_string(match_id)) {
#if defined(__linux__) || defined(__unix__) || defined(__APPLE__)
        (void)::unlink(latest_id_file.c_str());
        (void)::unlink((dir + "/match_latest.token").c_str());
#endif
      }
    }
  }
  return true;
}

std::optional<std::uint64_t> TokenStore::latest_match_id(const std::string& custom_dir) {
  const std::string dir = custom_dir.empty() ? default_token_dir() : custom_dir;
  const std::string latest_id_file = dir + "/match_latest_id.txt";

  if (!check_permissions_secure(latest_id_file)) {
    return std::nullopt;
  }

  std::ifstream in(latest_id_file);
  if (!in) return std::nullopt;
  std::string line;
  if (!std::getline(in, line)) return std::nullopt;
  const auto trimmed = trim_token(line);
  if (trimmed.empty()) return std::nullopt;

  try {
    std::size_t parsed_chars = 0;
    const auto id = std::stoull(trimmed, &parsed_chars);
    if (parsed_chars != trimmed.size() || id == 0U) return std::nullopt;
    // Verify that the corresponding token file also exists and is secure
    const std::string token_file = dir + "/match_" + std::to_string(id) + ".token";
    if (check_permissions_secure(token_file)) {
      return id;
    }
  } catch (...) {
    return std::nullopt;
  }
  return std::nullopt;
}

}  // namespace darkpool::security
