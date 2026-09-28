/**
 * @file tests/unit/test_httpcommon.cpp
 * @brief Test src/httpcommon.*.
 */
// test imports
#include "../tests_common.h"

// standard imports
#include <filesystem>

// lib imports
#include <curl/curl.h>

// local imports
#include <src/config.h>
#include <src/crypto.h>
#include <src/file_handler.h>
#include <src/httpcommon.h>

struct UrlEscapeTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlEscapeTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_escape(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlEscapeTests,
  UrlEscapeTest,
  testing::Values(
    std::make_tuple("igdb_0123456789", "igdb_0123456789"),
    std::make_tuple("../../../", "..%2F..%2F..%2F"),
    std::make_tuple("..*\\", "..%2A%5C")
  )
);

struct UrlGetHostTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(UrlGetHostTest, Run) {
  const auto &[input, expected] = GetParam();
  ASSERT_EQ(http::url_get_host(input), expected);
}

INSTANTIATE_TEST_SUITE_P(
  UrlGetHostTests,
  UrlGetHostTest,
  testing::Values(
    std::make_tuple("https://images.igdb.com/example.txt", "images.igdb.com"),
    std::make_tuple("http://localhost:8080", "localhost"),
    std::make_tuple("nonsense!!}{::", "")
  )
);

struct DownloadFileTest: testing::TestWithParam<std::tuple<std::string, std::string>> {};

TEST_P(DownloadFileTest, Run) {
  const auto &[url, filename] = GetParam();
  const std::string test_dir = platf::appdata().string() + "/tests/";
  std::string path = test_dir + filename;
  ASSERT_TRUE(http::download_file(url, path, CURL_SSLVERSION_TLSv1_0));
}

#ifdef SUNSHINE_BUILD_FLATPAK
// requires running `npm run serve` prior to running the tests
constexpr const char *URL_1 = "http://0.0.0.0:3000/hello.txt";
constexpr const char *URL_2 = "http://0.0.0.0:3000/hello-redirect.txt";
#else
constexpr const char *URL_1 = "https://httpbin.org/base64/aGVsbG8h";
constexpr const char *URL_2 = "https://httpbin.org/redirect-to?url=/base64/aGVsbG8h";
#endif

INSTANTIATE_TEST_SUITE_P(
  DownloadFileTests,
  DownloadFileTest,
  testing::Values(
    std::make_tuple(URL_1, "hello.txt"),
    std::make_tuple(URL_2, "hello-redirect.txt")
  )
);

TEST(PairingPasswordCredentials, SavesVerifierWithoutPlaintext) {
  const auto path = std::filesystem::temp_directory_path() / "sunshine-pairing-password-test.json";
  std::filesystem::remove(path);

  constexpr std::string_view password = "correct horse battery staple";
  ASSERT_EQ(http::save_pairing_password(path.string(), password), 0);

  const auto contents = file_handler::read_file(path.string().c_str());
  EXPECT_NE(contents.find("pairing_password_salt"), std::string::npos);
  EXPECT_NE(contents.find("pairing_password_verifier"), std::string::npos);
  EXPECT_EQ(contents.find(password), std::string::npos);

  ASSERT_EQ(http::reload_pairing_password(path.string()), 0);
  EXPECT_EQ(config::sunshine.pairing_password_iterations, crypto::PAIRING_PASSWORD_ITERATIONS);
  EXPECT_EQ(config::sunshine.pairing_password_salt.size(), 32U);
  EXPECT_EQ(config::sunshine.pairing_password_verifier.size(), 64U);

  std::filesystem::remove(path);
}
