#pragma once

#if defined(__linux__)

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace darkpool::transport {

// Custom deleters for OpenSSL C structures
struct SslCtxDeleter {
  void operator()(SSL_CTX* ctx) const noexcept {
    if (ctx != nullptr) {
      SSL_CTX_free(ctx);
    }
  }
};
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

struct SslDeleter {
  void operator()(SSL* ssl) const noexcept {
    if (ssl != nullptr) {
      SSL_free(ssl);
    }
  }
};
using SslPtr = std::unique_ptr<SSL, SslDeleter>;

struct X509Deleter {
  void operator()(X509* cert) const noexcept {
    if (cert != nullptr) {
      X509_free(cert);
    }
  }
};
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

struct EvpPkeyDeleter {
  void operator()(EVP_PKEY* pkey) const noexcept {
    if (pkey != nullptr) {
      EVP_PKEY_free(pkey);
    }
  }
};
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct BioDeleter {
  void operator()(BIO* bio) const noexcept {
    if (bio != nullptr) {
      BIO_free_all(bio);
    }
  }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

// Server TLS Configuration
struct ServerTlsConfig {
  std::string cert_file;
  std::string key_file;
  std::string bind_address{"127.0.0.1"};
  std::uint16_t port{4783};
};

// Client Certificate Verification Mode
enum class ClientVerifyMode {
  LocalCa,
  FingerprintPinning
};

// Client TLS Configuration
struct ClientTlsConfig {
  ClientVerifyMode mode{ClientVerifyMode::LocalCa};
  std::string ca_file;
  std::string pinned_fingerprint_hex;
  std::string target_ip{"127.0.0.1"};
};

// TLS Handshake Status
enum class TlsHandshakeStatus {
  InProgress,
  Established,
  Failed
};

// TLS Nonblocking I/O Status
enum class TlsIoStatus {
  Ok,
  WantRead,
  WantWrite,
  PeerClosed,
  Error
};

// Format OpenSSL error queue into a string
[[nodiscard]] std::string get_openssl_error_string();

// Parses a 64-hex-character SHA-256 fingerprint into a 32-byte array
[[nodiscard]] bool parse_fingerprint_sha256(
    std::string_view hex_str,
    std::array<std::uint8_t, 32>& out_digest,
    std::string& err_msg);

// Computes SHA-256 fingerprint of an X509 certificate as lowercase 64-hex string
[[nodiscard]] std::string compute_cert_fingerprint_sha256(X509* cert);

// Verifies if the certificate validity window is valid at the current time
[[nodiscard]] bool verify_cert_time_validity(X509* cert, std::string& err_msg);

// Verifies if the certificate contains an IP SAN matching the given IP
[[nodiscard]] bool verify_cert_ip_san(X509* cert, const std::string& expected_ip, std::string& err_msg);

// Creates an initialized OpenSSL server context enforcing strict TLS 1.3
[[nodiscard]] SslCtxPtr create_server_ssl_ctx(
    const ServerTlsConfig& config,
    std::string& err_msg);

// Creates an initialized OpenSSL client context enforcing strict TLS 1.3
[[nodiscard]] SslCtxPtr create_client_ssl_ctx(
    const ClientTlsConfig& config,
    std::string& err_msg);

// Drives one step of a nonblocking TLS handshake (SSL_accept or SSL_connect)
[[nodiscard]] TlsHandshakeStatus step_tls_handshake(
    SSL* ssl,
    bool is_server,
    bool& want_read,
    bool& want_write,
    std::string& err_msg);

// Performs nonblocking TLS read
[[nodiscard]] TlsIoStatus tls_read(
    SSL* ssl,
    char* buffer,
    std::size_t max_len,
    std::size_t& bytes_read,
    std::string& err_msg);

// Performs nonblocking TLS write
[[nodiscard]] TlsIoStatus tls_write(
    SSL* ssl,
    const char* data,
    std::size_t len,
    std::size_t& bytes_written,
    std::string& err_msg);

// Ephemeral certificate generator for deterministic tests (creates temp certs/keys)
struct EphemeralCertBundle {
  std::string ca_cert_pem_path;
  std::string server_cert_pem_path;
  std::string server_key_pem_path;
  std::string server_fingerprint_hex;
  std::string temp_dir_path;

  // Cleanup files and temp dir
  ~EphemeralCertBundle();
};

// Generates an ephemeral CA + server certificate bundle in a secure temporary directory
[[nodiscard]] std::unique_ptr<EphemeralCertBundle> generate_ephemeral_certs(
    const std::string& ip_san = "127.0.0.1",
    int validity_days = 1,
    bool self_signed = false,
    bool expired = false);

}  // namespace darkpool::transport

#endif  // defined(__linux__)
