/**
 * @file tests/unit/test_pairing_password.cpp
 * @brief Tests for the password-based pairing derivation contract.
 */

#include "../tests_common.h"

#include <src/crypto.h>

TEST(PairingPasswordCrypto, MatchesPbkdf2AndSessionKeyFixture) {
  const auto verifier = crypto::derive_pairing_password_verifier("password", "salt", 1);
  ASSERT_TRUE(verifier.has_value());

  const std::string raw(reinterpret_cast<const char *>(verifier->data()), verifier->size());
  EXPECT_EQ(util::hex_vec(raw.begin(), raw.end(), true),
            "120FB6CFFCF8B32C43E7225256C4F837A86548C92CCC35480805987CB70BE17B");

  const auto key = crypto::derive_pairing_session_key("0123456789abcdef", raw);
  EXPECT_EQ(util::hex_vec(key.begin(), key.end(), true),
            "9FE75F91AAA8EB5F57B2DE599C101BEC");
}

TEST(PairingPasswordCrypto, EnforcesPasswordPolicy) {
  EXPECT_FALSE(crypto::valid_pairing_password("short"));
  EXPECT_TRUE(crypto::valid_pairing_password("correct horse battery staple"));
  EXPECT_FALSE(crypto::valid_pairing_password(std::string(129, 'x')));
  EXPECT_FALSE(crypto::valid_pairing_password(std::string("valid\0password", 14)));
}
