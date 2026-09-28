/**
 * @file src/crypto.h
 * @brief Declarations for cryptography functions.
 */
#pragma once

// standard includes
#include <array>
#include <list>
#include <optional>

// lib includes
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/x509.h>

// local includes
#include "utility.h"

namespace crypto {
  struct creds_t {
    std::string x509;
    std::string pkey;
  };

  void md_ctx_destroy(EVP_MD_CTX *);

  using sha256_t = std::array<std::uint8_t, SHA256_DIGEST_LENGTH>;

  using aes_t = std::vector<std::uint8_t>;
  using x509_t = util::safe_ptr<X509, X509_free>;
  using x509_store_t = util::safe_ptr<X509_STORE, X509_STORE_free>;
  using x509_store_ctx_t = util::safe_ptr<X509_STORE_CTX, X509_STORE_CTX_free>;
  using cipher_ctx_t = util::safe_ptr<EVP_CIPHER_CTX, EVP_CIPHER_CTX_free>;
  using md_ctx_t = util::safe_ptr<EVP_MD_CTX, md_ctx_destroy>;
  using bio_t = util::safe_ptr<BIO, BIO_free_all>;
  using pkey_t = util::safe_ptr<EVP_PKEY, EVP_PKEY_free>;
  using pkey_ctx_t = util::safe_ptr<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
  using bignum_t = util::safe_ptr<BIGNUM, BN_free>;

  enum class PERM: std::uint32_t {
    _reserved = 1,

    _input = _reserved << 8,
    input_controller = _input << 0,
    input_touch = _input << 1,
    input_pen = _input << 2,
    input_mouse = _input << 3,
    input_kbd = _input << 4,
    _all_inputs = input_controller | input_touch | input_pen | input_mouse | input_kbd,

    _operation = _input << 8,
    clipboard_set = _operation << 0,
    clipboard_read = _operation << 1,
    file_upload = _operation << 2,
    file_download = _operation << 3,
    server_cmd = _operation << 4,
    client_microphone = _operation << 5,
    _all_operations = clipboard_set | clipboard_read | file_upload | file_download | server_cmd | client_microphone,

    _action = _operation << 8,
    list = _action << 0,
    view = _action << 1,
    launch = _action << 2,
    _allow_view = view | launch,
    _all_actions = list | view | launch,

    _default = view | list,
    _no = 0,
    _all = _all_inputs | _all_operations | _all_actions,
  };

  inline constexpr PERM operator&(PERM x, PERM y) {
    return static_cast<PERM>(static_cast<std::uint32_t>(x) & static_cast<std::uint32_t>(y));
  }

  inline constexpr PERM operator|(PERM x, PERM y) {
    return static_cast<PERM>(static_cast<std::uint32_t>(x) | static_cast<std::uint32_t>(y));
  }

  inline constexpr bool operator!(PERM p) {
    return static_cast<std::uint32_t>(p) == 0;
  }

  struct command_entry_t {
    std::string cmd;
    bool elevated = false;
  };

  /**
   * @brief Hashes the given plaintext using SHA-256.
   * @param plaintext
   * @return The SHA-256 hash of the plaintext.
   */
  sha256_t hash(const std::string_view &plaintext);

  aes_t gen_aes_key(const std::array<uint8_t, 16> &salt, const std::string_view &pin);

  inline constexpr std::size_t PAIRING_PASSWORD_MIN_LENGTH = 8;
  inline constexpr std::size_t PAIRING_PASSWORD_MAX_LENGTH = 128;
  inline constexpr int PAIRING_PASSWORD_ITERATIONS = 600'000;

  bool valid_pairing_password(std::string_view password);
  std::optional<sha256_t> derive_pairing_password_verifier(std::string_view password, std::string_view salt, int iterations);
  aes_t derive_pairing_session_key(std::string_view session_salt, std::string_view verifier);

  x509_t x509(const std::string_view &x);
  pkey_t pkey(const std::string_view &k);
  std::string pem(x509_t &x509);
  std::string pem(pkey_t &pkey);

  std::vector<uint8_t> sign256(const pkey_t &pkey, const std::string_view &data);
  bool verify256(const x509_t &x509, const std::string_view &data, const std::string_view &signature);

  creds_t gen_creds(const std::string_view &cn, std::uint32_t key_bits);

  std::string_view signature(const x509_t &x);

  std::string rand(std::size_t bytes);
  std::string rand_alphabet(std::size_t bytes, const std::string_view &alphabet = std::string_view {"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!%&()=-"});

  class cert_chain_t {
  public:
    KITTY_DECL_CONSTR(cert_chain_t)

    void add(x509_t &&cert);

    void clear();

    const char *verify(x509_t::element_type *cert);

  private:
    std::vector<std::pair<x509_t, x509_store_t>> _certs;
    x509_store_ctx_t _cert_ctx;
  };

  namespace cipher {
    constexpr std::size_t tag_size = 16;

    constexpr std::size_t round_to_pkcs7_padded(std::size_t size) {
      return ((size + 15) / 16) * 16;
    }

    class cipher_t {
    public:
      cipher_ctx_t decrypt_ctx;
      cipher_ctx_t encrypt_ctx;

      aes_t key;

      bool padding;
    };

    class ecb_t: public cipher_t {
    public:
      ecb_t() = default;
      ecb_t(ecb_t &&) noexcept = default;
      ecb_t &operator=(ecb_t &&) noexcept = default;

      ecb_t(const aes_t &key, bool padding = true);

      int encrypt(const std::string_view &plaintext, std::vector<std::uint8_t> &cipher);
      int decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext);
    };

    class gcm_t: public cipher_t {
    public:
      gcm_t() = default;
      gcm_t(gcm_t &&) noexcept = default;
      gcm_t &operator=(gcm_t &&) noexcept = default;

      gcm_t(const crypto::aes_t &key, bool padding = true);

      /**
       * @brief Encrypts the plaintext using AES GCM mode.
       * @param plaintext The plaintext data to be encrypted.
       * @param tag The buffer where the GCM tag will be written.
       * @param ciphertext The buffer where the resulting ciphertext will be written.
       * @param iv The initialization vector to be used for the encryption.
       * @return The total length of the ciphertext and GCM tag. Returns -1 in case of an error.
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *tag, std::uint8_t *ciphertext, aes_t *iv);

      /**
       * @brief Encrypts the plaintext using AES GCM mode.
       * length of cipher must be at least: round_to_pkcs7_padded(plaintext.size()) + crypto::cipher::tag_size
       * @param plaintext The plaintext data to be encrypted.
       * @param tagged_cipher The buffer where the resulting ciphertext and GCM tag will be written.
       * @param iv The initialization vector to be used for the encryption.
       * @return The total length of the ciphertext and GCM tag written into tagged_cipher. Returns -1 in case of an error.
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *tagged_cipher, aes_t *iv);

      int decrypt(const std::string_view &cipher, std::vector<std::uint8_t> &plaintext, aes_t *iv);
    };

    class cbc_t: public cipher_t {
    public:
      cbc_t() = default;
      cbc_t(cbc_t &&) noexcept = default;
      cbc_t &operator=(cbc_t &&) noexcept = default;

      cbc_t(const crypto::aes_t &key, bool padding = true);

      /**
       * @brief Encrypts the plaintext using AES CBC mode.
       * length of cipher must be at least: round_to_pkcs7_padded(plaintext.size())
       * @param plaintext The plaintext data to be encrypted.
       * @param cipher The buffer where the resulting ciphertext will be written.
       * @param iv The initialization vector to be used for the encryption.
       * @return The total length of the ciphertext written into cipher. Returns -1 in case of an error.
       */
      int encrypt(const std::string_view &plaintext, std::uint8_t *cipher, aes_t *iv);
    };
  }  // namespace cipher
}  // namespace crypto
