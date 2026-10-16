#if defined(__linux__)

#include "darkpool/tls_transport.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace darkpool::transport {

std::string get_openssl_error_string() {
  std::ostringstream oss;
  unsigned long err = 0;
  char buf[256];
  bool first = true;
  while ((err = ERR_get_error()) != 0) {
    if (!first) oss << "; ";
    ERR_error_string_n(err, buf, sizeof(buf));
    oss << buf;
    first = false;
  }
  return oss.str();
}

bool parse_fingerprint_sha256(
    std::string_view hex_str,
    std::array<std::uint8_t, 32>& out_digest,
    std::string& err_msg) {
  std::string cleaned;
  cleaned.reserve(hex_str.size());
  for (const char c : hex_str) {
    if (c == ':' || c == ' ' || c == '-' || c == '\n' || c == '\r') {
      continue;
    }
    cleaned.push_back(c);
  }

  if (cleaned.size() != 64) {
    err_msg = "invalid SHA-256 fingerprint length: expected exactly 64 hex characters (got " +
              std::to_string(cleaned.size()) + ")";
    return false;
  }

  for (std::size_t i = 0; i < 32; ++i) {
    const char hi_char = cleaned[2 * i];
    const char lo_char = cleaned[2 * i + 1];
    if (!std::isxdigit(static_cast<unsigned char>(hi_char)) ||
        !std::isxdigit(static_cast<unsigned char>(lo_char))) {
      err_msg = "invalid hex character in fingerprint at index " + std::to_string(2 * i);
      return false;
    }
    const auto parse_nibble = [](char ch) -> std::uint8_t {
      if (ch >= '0' && ch <= '9') return static_cast<std::uint8_t>(ch - '0');
      if (ch >= 'a' && ch <= 'f') return static_cast<std::uint8_t>(ch - 'a' + 10);
      return static_cast<std::uint8_t>(ch - 'A' + 10);
    };
    out_digest[i] = static_cast<std::uint8_t>((parse_nibble(hi_char) << 4) | parse_nibble(lo_char));
  }
  return true;
}

std::string compute_cert_fingerprint_sha256(X509* cert) {
  if (cert == nullptr) return {};
  unsigned char md[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  if (X509_digest(cert, EVP_sha256(), md, &len) != 1 || len != 32) {
    return {};
  }
  std::ostringstream oss;
  oss << std::hex << std::setfill('0');
  for (unsigned int i = 0; i < len; ++i) {
    oss << std::setw(2) << static_cast<int>(md[i]);
  }
  return oss.str();
}

bool verify_cert_time_validity(X509* cert, std::string& err_msg) {
  if (cert == nullptr) {
    err_msg = "null certificate presented";
    return false;
  }
  const ASN1_TIME* not_before = X509_get0_notBefore(cert);
  const ASN1_TIME* not_after = X509_get0_notAfter(cert);
  if (not_before == nullptr || not_after == nullptr) {
    err_msg = "certificate missing validity timestamps";
    return false;
  }

  if (X509_cmp_current_time(not_before) > 0) {
    err_msg = "certificate is not yet valid (notBefore in future)";
    return false;
  }
  if (X509_cmp_current_time(not_after) < 0) {
    err_msg = "certificate has expired (notAfter in past)";
    return false;
  }
  return true;
}

bool verify_cert_ip_san(X509* cert, const std::string& expected_ip, std::string& err_msg) {
  if (cert == nullptr) {
    err_msg = "null certificate presented";
    return false;
  }
  if (expected_ip.empty()) {
    err_msg = "target IP address cannot be empty";
    return false;
  }

  const int check = X509_check_ip_asc(cert, expected_ip.c_str(), 0);
  if (check == 1) {
    return true;
  }
  err_msg = "certificate Subject Alternative Name (SAN) does not match expected IP '" + expected_ip + "'";
  return false;
}

SslCtxPtr create_server_ssl_ctx(
    const ServerTlsConfig& config,
    std::string& err_msg) {
  if (config.cert_file.empty()) {
    err_msg = "--tls-cert is required when TLS listener is configured";
    return nullptr;
  }
  if (config.key_file.empty()) {
    err_msg = "--tls-key is required when TLS listener is configured";
    return nullptr;
  }

  struct stat cert_st {};
  if (::stat(config.cert_file.c_str(), &cert_st) != 0) {
    err_msg = "cannot open TLS certificate file '" + config.cert_file + "': " + std::strerror(errno);
    return nullptr;
  }
  struct stat key_st {};
  if (::stat(config.key_file.c_str(), &key_st) != 0) {
    err_msg = "cannot open TLS private key file '" + config.key_file + "': " + std::strerror(errno);
    return nullptr;
  }

  // Check private key permissions; warn if group or others have read permissions
  if ((key_st.st_mode & 0077) != 0) {
    std::cerr << "warning: private key file '" << config.key_file
              << "' permissions (" << std::oct << (key_st.st_mode & 0777) << std::dec
              << ") are insecure; recommended mode is 0600\n";
  }

  SSL_CTX* raw_ctx = SSL_CTX_new(TLS_server_method());
  if (raw_ctx == nullptr) {
    err_msg = "failed to create OpenSSL server context: " + get_openssl_error_string();
    return nullptr;
  }
  SslCtxPtr ctx(raw_ctx);

  // Enforce strict TLS 1.3 only (reject TLS 1.2 and earlier)
  if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) != 1) {
    err_msg = "failed to enforce TLS 1.3 minimum/maximum protocol version: " + get_openssl_error_string();
    return nullptr;
  }

  // Set nonblocking I/O modes: partial write and moving write buffer
  SSL_CTX_set_mode(ctx.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_AUTO_RETRY);

  // Disable 0-RTT / early data to prevent replay attacks on application commands
  SSL_CTX_set_max_early_data(ctx.get(), 0);

  // Load certificate chain
  if (SSL_CTX_use_certificate_chain_file(ctx.get(), config.cert_file.c_str()) != 1) {
    err_msg = "failed to load certificate file '" + config.cert_file + "': " + get_openssl_error_string();
    return nullptr;
  }

  // Load private key
  if (SSL_CTX_use_PrivateKey_file(ctx.get(), config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
    err_msg = "failed to load private key file '" + config.key_file + "': " + get_openssl_error_string();
    return nullptr;
  }

  // Verify private key matches certificate
  if (SSL_CTX_check_private_key(ctx.get()) != 1) {
    err_msg = "private key '" + config.key_file + "' does not match certificate '" + config.cert_file + "': " + get_openssl_error_string();
    return nullptr;
  }

  return ctx;
}

namespace {

int client_verify_callback(int preverify_ok, X509_STORE_CTX* x509_ctx) {
  SSL* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(x509_ctx, SSL_get_ex_data_X509_STORE_CTX_idx()));
  if (ssl == nullptr) {
    return 0;
  }
  const auto* cfg = static_cast<const ClientTlsConfig*>(SSL_get_app_data(ssl));
  if (cfg == nullptr) {
    return 0;
  }

  if (cfg->mode == ClientVerifyMode::LocalCa) {
    // In CA mode, let OpenSSL do standard X.509 chain verification
    if (preverify_ok != 1) {
      return 0;
    }
    // Verify leaf cert IP SAN explicitly if at depth 0
    const int depth = X509_STORE_CTX_get_error_depth(x509_ctx);
    if (depth == 0) {
      X509* cert = X509_STORE_CTX_get_current_cert(x509_ctx);
      std::string err;
      if (!verify_cert_time_validity(cert, err) || !verify_cert_ip_san(cert, cfg->target_ip, err)) {
        return 0;
      }
    }
    return 1;
  }

  if (cfg->mode == ClientVerifyMode::FingerprintPinning) {
    // In fingerprint mode, we only accept the leaf certificate
    const int depth = X509_STORE_CTX_get_error_depth(x509_ctx);
    if (depth != 0) {
      return 0;
    }
    X509* cert = X509_STORE_CTX_get_current_cert(x509_ctx);
    if (cert == nullptr) {
      return 0;
    }
    std::string err;
    if (!verify_cert_time_validity(cert, err)) {
      return 0;
    }
    if (!verify_cert_ip_san(cert, cfg->target_ip, err)) {
      return 0;
    }
    std::array<std::uint8_t, 32> expected_digest{};
    if (!parse_fingerprint_sha256(cfg->pinned_fingerprint_hex, expected_digest, err)) {
      return 0;
    }
    const std::string actual_fp = compute_cert_fingerprint_sha256(cert);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (const auto b : expected_digest) {
      oss << std::setw(2) << static_cast<int>(b);
    }
    if (actual_fp != oss.str()) {
      return 0;
    }
    // Pinned fingerprint matches exactly, certificate is time-valid, and IP SAN matches!
    return 1;
  }

  return 0;
}

}  // namespace

SslCtxPtr create_client_ssl_ctx(
    const ClientTlsConfig& config,
    std::string& err_msg) {
  SSL_CTX* raw_ctx = SSL_CTX_new(TLS_client_method());
  if (raw_ctx == nullptr) {
    err_msg = "failed to create OpenSSL client context: " + get_openssl_error_string();
    return nullptr;
  }
  SslCtxPtr ctx(raw_ctx);

  // Enforce strict TLS 1.3 only
  if (SSL_CTX_set_min_proto_version(ctx.get(), TLS1_3_VERSION) != 1 ||
      SSL_CTX_set_max_proto_version(ctx.get(), TLS1_3_VERSION) != 1) {
    err_msg = "failed to enforce TLS 1.3 minimum/maximum protocol version: " + get_openssl_error_string();
    return nullptr;
  }

  // Set nonblocking I/O modes
  SSL_CTX_set_mode(ctx.get(), SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER | SSL_MODE_AUTO_RETRY);

  // Disable 0-RTT / early data
  SSL_CTX_set_max_early_data(ctx.get(), 0);

  if (config.mode == ClientVerifyMode::LocalCa) {
    if (config.ca_file.empty()) {
      err_msg = "--ca-cert is required in local-CA verification mode";
      return nullptr;
    }
    struct stat st {};
    if (::stat(config.ca_file.c_str(), &st) != 0) {
      err_msg = "cannot open CA certificate file '" + config.ca_file + "': " + std::strerror(errno);
      return nullptr;
    }
    if (SSL_CTX_load_verify_locations(ctx.get(), config.ca_file.c_str(), nullptr) != 1) {
      err_msg = "failed to load CA certificate file '" + config.ca_file + "': " + get_openssl_error_string();
      return nullptr;
    }
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, client_verify_callback);

    X509_VERIFY_PARAM* param = SSL_CTX_get0_param(ctx.get());
    if (param != nullptr) {
      (void)X509_VERIFY_PARAM_set1_ip_asc(param, config.target_ip.c_str());
    }
  } else if (config.mode == ClientVerifyMode::FingerprintPinning) {
    std::array<std::uint8_t, 32> digest{};
    if (!parse_fingerprint_sha256(config.pinned_fingerprint_hex, digest, err_msg)) {
      return nullptr;
    }
    SSL_CTX_set_verify(ctx.get(), SSL_VERIFY_PEER, client_verify_callback);
  }

  return ctx;
}

TlsHandshakeStatus step_tls_handshake(
    SSL* ssl,
    bool is_server,
    bool& want_read,
    bool& want_write,
    std::string& err_msg) {
  want_read = false;
  want_write = false;
  if (ssl == nullptr) {
    err_msg = "null SSL session";
    return TlsHandshakeStatus::Failed;
  }

  const int res = is_server ? SSL_accept(ssl) : SSL_connect(ssl);
  if (res == 1) {
    return TlsHandshakeStatus::Established;
  }

  const int err = SSL_get_error(ssl, res);
  if (err == SSL_ERROR_WANT_READ) {
    want_read = true;
    return TlsHandshakeStatus::InProgress;
  }
  if (err == SSL_ERROR_WANT_WRITE) {
    want_write = true;
    return TlsHandshakeStatus::InProgress;
  }

  err_msg = get_openssl_error_string();
  if (err_msg.empty()) {
    err_msg = "TLS handshake failed with OpenSSL error code " + std::to_string(err);
  }
  return TlsHandshakeStatus::Failed;
}

TlsIoStatus tls_read(
    SSL* ssl,
    char* buffer,
    std::size_t max_len,
    std::size_t& bytes_read,
    std::string& err_msg) {
  bytes_read = 0;
  if (ssl == nullptr) {
    err_msg = "null SSL session";
    return TlsIoStatus::Error;
  }

  const int count = SSL_read(ssl, buffer, static_cast<int>(max_len));
  if (count > 0) {
    bytes_read = static_cast<std::size_t>(count);
    return TlsIoStatus::Ok;
  }

  const int err = SSL_get_error(ssl, count);
  if (count == 0 || err == SSL_ERROR_ZERO_RETURN) {
    return TlsIoStatus::PeerClosed;
  }
  if (err == SSL_ERROR_WANT_READ) {
    return TlsIoStatus::WantRead;
  }
  if (err == SSL_ERROR_WANT_WRITE) {
    return TlsIoStatus::WantWrite;
  }

  err_msg = get_openssl_error_string();
  if (err_msg.empty()) {
    err_msg = "SSL_read error " + std::to_string(err);
  }
  return TlsIoStatus::Error;
}

TlsIoStatus tls_write(
    SSL* ssl,
    const char* data,
    std::size_t len,
    std::size_t& bytes_written,
    std::string& err_msg) {
  bytes_written = 0;
  if (ssl == nullptr) {
    err_msg = "null SSL session";
    return TlsIoStatus::Error;
  }
  if (len == 0) {
    return TlsIoStatus::Ok;
  }

  const int count = SSL_write(ssl, data, static_cast<int>(len));
  if (count > 0) {
    bytes_written = static_cast<std::size_t>(count);
    return TlsIoStatus::Ok;
  }

  const int err = SSL_get_error(ssl, count);
  if (err == SSL_ERROR_WANT_WRITE) {
    return TlsIoStatus::WantWrite;
  }
  if (err == SSL_ERROR_WANT_READ) {
    return TlsIoStatus::WantRead;
  }

  err_msg = get_openssl_error_string();
  if (err_msg.empty()) {
    err_msg = "SSL_write error " + std::to_string(err);
  }
  return TlsIoStatus::Error;
}

EphemeralCertBundle::~EphemeralCertBundle() {
  if (!temp_dir_path.empty()) {
    std::error_code ec;
    std::filesystem::remove_all(temp_dir_path, ec);
  }
}

std::unique_ptr<EphemeralCertBundle> generate_ephemeral_certs(
    const std::string& ip_san,
    int validity_days,
    bool self_signed,
    bool expired) {
  char temp_template[] = "/tmp/darkpool_tls_XXXXXX";
  char* temp_dir = ::mkdtemp(temp_template);
  if (temp_dir == nullptr) {
    return nullptr;
  }

  auto bundle = std::make_unique<EphemeralCertBundle>();
  bundle->temp_dir_path = temp_dir;

  // Generate CA key and certificate (unless self_signed)
  EvpPkeyPtr ca_key;
  X509Ptr ca_cert;

  if (!self_signed) {
    EVP_PKEY* raw_ca_key = EVP_RSA_gen(2048);
    if (raw_ca_key == nullptr) return nullptr;
    ca_key.reset(raw_ca_key);

    X509* raw_ca_cert = X509_new();
    if (raw_ca_cert == nullptr) return nullptr;
    ca_cert.reset(raw_ca_cert);

    (void)X509_set_version(ca_cert.get(), 2);
    (void)ASN1_INTEGER_set(X509_get_serialNumber(ca_cert.get()), 1);
    (void)X509_gmtime_adj(X509_get_notBefore(ca_cert.get()), -3600);
    (void)X509_gmtime_adj(X509_get_notAfter(ca_cert.get()), 86400 * 365);
    (void)X509_set_pubkey(ca_cert.get(), ca_key.get());

    X509_NAME* name = X509_get_subject_name(ca_cert.get());
    (void)X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    (void)X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("DARKPOOL"), -1, -1, 0);
    (void)X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("DARKPOOL Ephemeral Root CA"), -1, -1, 0);
    (void)X509_set_issuer_name(ca_cert.get(), name);

    X509V3_CTX ctx{};
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, ca_cert.get(), ca_cert.get(), nullptr, nullptr, 0);
    X509_EXTENSION* ext_bc = X509V3_EXT_conf_nid(nullptr, &ctx, NID_basic_constraints, "critical,CA:TRUE");
    if (ext_bc != nullptr) {
      (void)X509_add_ext(ca_cert.get(), ext_bc, -1);
      X509_EXTENSION_free(ext_bc);
    }

    if (X509_sign(ca_cert.get(), ca_key.get(), EVP_sha256()) <= 0) {
      return nullptr;
    }

    bundle->ca_cert_pem_path = bundle->temp_dir_path + "/ca-cert.pem";
    BioPtr ca_bio(BIO_new_file(bundle->ca_cert_pem_path.c_str(), "w"));
    if (ca_bio == nullptr || PEM_write_bio_X509(ca_bio.get(), ca_cert.get()) != 1) {
      return nullptr;
    }
  }

  // Generate server key and certificate
  EVP_PKEY* raw_server_key = EVP_RSA_gen(2048);
  if (raw_server_key == nullptr) return nullptr;
  EvpPkeyPtr server_key(raw_server_key);

  X509* raw_server_cert = X509_new();
  if (raw_server_cert == nullptr) return nullptr;
  X509Ptr server_cert(raw_server_cert);

  (void)X509_set_version(server_cert.get(), 2);
  (void)ASN1_INTEGER_set(X509_get_serialNumber(server_cert.get()), 2);

  if (expired) {
    (void)X509_gmtime_adj(X509_get_notBefore(server_cert.get()), -7200);
    (void)X509_gmtime_adj(X509_get_notAfter(server_cert.get()), -3600);
  } else {
    (void)X509_gmtime_adj(X509_get_notBefore(server_cert.get()), -3600);
    (void)X509_gmtime_adj(X509_get_notAfter(server_cert.get()), 86400 * validity_days);
  }

  (void)X509_set_pubkey(server_cert.get(), server_key.get());

  X509_NAME* server_name = X509_get_subject_name(server_cert.get());
  (void)X509_NAME_add_entry_by_txt(server_name, "C", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
  (void)X509_NAME_add_entry_by_txt(server_name, "O", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("DARKPOOL"), -1, -1, 0);
  (void)X509_NAME_add_entry_by_txt(server_name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("darkpool-server"), -1, -1, 0);

  if (self_signed) {
    (void)X509_set_issuer_name(server_cert.get(), server_name);
  } else {
    (void)X509_set_issuer_name(server_cert.get(), X509_get_subject_name(ca_cert.get()));
  }

  // Set IP SAN extension
  X509V3_CTX ext_ctx{};
  X509V3_set_ctx_nodb(&ext_ctx);
  X509V3_set_ctx(&ext_ctx, self_signed ? server_cert.get() : ca_cert.get(), server_cert.get(), nullptr, nullptr, 0);
  const std::string san_str = "IP:" + ip_san;
  X509_EXTENSION* ext_san = X509V3_EXT_conf_nid(nullptr, &ext_ctx, NID_subject_alt_name, san_str.c_str());
  if (ext_san != nullptr) {
    (void)X509_add_ext(server_cert.get(), ext_san, -1);
    X509_EXTENSION_free(ext_san);
  }

  // Sign server certificate
  if (self_signed) {
    if (X509_sign(server_cert.get(), server_key.get(), EVP_sha256()) <= 0) {
      return nullptr;
    }
  } else {
    if (X509_sign(server_cert.get(), ca_key.get(), EVP_sha256()) <= 0) {
      return nullptr;
    }
  }

  bundle->server_cert_pem_path = bundle->temp_dir_path + "/server-cert.pem";
  BioPtr server_cert_bio(BIO_new_file(bundle->server_cert_pem_path.c_str(), "w"));
  if (server_cert_bio == nullptr || PEM_write_bio_X509(server_cert_bio.get(), server_cert.get()) != 1) {
    return nullptr;
  }

  bundle->server_key_pem_path = bundle->temp_dir_path + "/server-key.pem";
  BioPtr server_key_bio(BIO_new_file(bundle->server_key_pem_path.c_str(), "w"));
  if (server_key_bio == nullptr || PEM_write_bio_PrivateKey(server_key_bio.get(), server_key.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    return nullptr;
  }

  // Set file permissions on private key to 0600
  (void)::chmod(bundle->server_key_pem_path.c_str(), 0600);

  bundle->server_fingerprint_hex = compute_cert_fingerprint_sha256(server_cert.get());
  return bundle;
}

}  // namespace darkpool::transport

#endif  // defined(__linux__)
