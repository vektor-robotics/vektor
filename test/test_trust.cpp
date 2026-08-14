#include "vektor/trust.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
constexpr auto kArtifact =
    "ghcr.io/vektor-robotics/demo@sha256:"
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

struct TrustFiles {
  std::filesystem::path policy{"vektor_test_trust.yaml"};
  std::filesystem::path key{"vektor_test_cosign.pub"};
  std::filesystem::path executable{"vektor_test_cosign.sh"};
  std::filesystem::path arguments{"vektor_test_cosign.sh.args"};

  TrustFiles() { cleanup(); }
  ~TrustFiles() { cleanup(); }

  void cleanup() const {
    std::filesystem::remove(policy);
    std::filesystem::remove(key);
    std::filesystem::remove(executable);
    std::filesystem::remove(arguments);
  }

  void make_executable() const {
    std::filesystem::permissions(executable,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_exec);
  }
};
} // namespace

TEST(TrustPolicy, LoadsKeylessIdentityPolicy) {
  TrustFiles files;
  std::ofstream policy(files.policy);
  policy << "schema_version: 1\n"
            "mode: keyless\n"
            "certificate_identity: release@example.com\n"
            "certificate_oidc_issuer: https://issuer.example.com\n"
            "timeout_ms: 1234\n";
  policy.close();

  const auto loaded = vektor::load_trust_policy(files.policy);
  EXPECT_EQ(loaded.mode, vektor::TrustMode::Keyless);
  EXPECT_EQ(loaded.certificate_identity, "release@example.com");
  EXPECT_EQ(loaded.certificate_oidc_issuer, "https://issuer.example.com");
  EXPECT_EQ(loaded.timeout, std::chrono::milliseconds(1234));
}

TEST(TrustPolicy, RejectsMixedAndUnknownPolicyFields) {
  TrustFiles files;
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nmode: keyless\nkey: cosign.pub\n"
              "certificate_identity: release@example.com\n"
              "certificate_oidc_issuer: https://issuer.example.com\n";
  }
  EXPECT_THROW(vektor::load_trust_policy(files.policy), std::runtime_error);
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nmode: public_key\nkey: cosign.pub\n"
              "allow_unsigned: true\n";
  }
  EXPECT_THROW(vektor::load_trust_policy(files.policy), std::runtime_error);
}

TEST(TrustPolicy, LoadsExplicitPrivateRegistryControls) {
  TrustFiles files;
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nmode: public_key\nkey: cosign.pub\n"
              "allow_http_registry: true\n"
              "ignore_transparency_log: true\n";
  }

  const auto loaded = vektor::load_trust_policy(files.policy);
  EXPECT_TRUE(loaded.allow_http_registry);
  EXPECT_TRUE(loaded.ignore_transparency_log);
}

TEST(TrustPolicy, KeepsPrivateRegistryControlsSecureByDefault) {
  TrustFiles files;
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nmode: public_key\nkey: cosign.pub\n";
  }

  const auto loaded = vektor::load_trust_policy(files.policy);
  EXPECT_FALSE(loaded.allow_http_registry);
  EXPECT_FALSE(loaded.ignore_transparency_log);
}

TEST(TrustPolicy, RejectsWeakenedKeylessPolicy) {
  TrustFiles files;
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nmode: keyless\n"
              "certificate_identity: release@example.com\n"
              "certificate_oidc_issuer: https://issuer.example.com\n"
              "ignore_transparency_log: true\n";
  }

  EXPECT_THROW(vektor::load_trust_policy(files.policy), std::runtime_error);
}

TEST(CosignVerifier, RejectsWeakenedKeylessPolicyConstructedInCode) {
  vektor::TrustPolicy policy;
  policy.mode = vektor::TrustMode::Keyless;
  policy.certificate_identity = "release@example.com";
  policy.certificate_oidc_issuer = "https://issuer.example.com";
  policy.ignore_transparency_log = true;

  EXPECT_THROW(vektor::CosignArtifactVerifier(std::move(policy)),
               std::invalid_argument);
}

TEST(CosignVerifier, EnforcesPublicKeyAndRecordsProvenance) {
  TrustFiles files;
  {
    std::ofstream key(files.key);
    key << "test public key";
    std::ofstream executable(files.executable);
    executable << "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.args\"\n";
  }
  files.make_executable();
  vektor::TrustPolicy policy;
  policy.mode = vektor::TrustMode::PublicKey;
  policy.cosign_executable = "./" + files.executable.string();
  policy.key = files.key.string();
  policy.allow_http_registry = true;
  policy.ignore_transparency_log = true;
  vektor::CosignArtifactVerifier verifier(policy);

  const auto result = verifier.verify(kArtifact, std::chrono::seconds(1));
  EXPECT_TRUE(result.verified);
  EXPECT_EQ(result.method, "cosign_public_key");
  EXPECT_EQ(result.signer, files.key.string());
  EXPECT_FALSE(result.verified_at.empty());
  std::ifstream arguments(files.arguments);
  const std::string recorded{std::istreambuf_iterator<char>(arguments), {}};
  EXPECT_NE(recorded.find("verify\n--key\n" + files.key.string()),
            std::string::npos);
  EXPECT_NE(recorded.find(kArtifact), std::string::npos);
  EXPECT_NE(recorded.find("--allow-http-registry"), std::string::npos);
  EXPECT_NE(recorded.find("--insecure-ignore-tlog"), std::string::npos);
}

TEST(CosignVerifier, EnforcesKeylessIdentityAndIssuer) {
  TrustFiles files;
  {
    std::ofstream executable(files.executable);
    executable << "#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$0.args\"\n";
  }
  files.make_executable();
  vektor::TrustPolicy policy;
  policy.mode = vektor::TrustMode::Keyless;
  policy.cosign_executable = "./" + files.executable.string();
  policy.certificate_identity = "release@example.com";
  policy.certificate_oidc_issuer = "https://issuer.example.com";
  vektor::CosignArtifactVerifier verifier(policy);

  const auto result = verifier.verify(kArtifact, std::chrono::seconds(1));
  EXPECT_TRUE(result.verified);
  EXPECT_EQ(result.method, "cosign_keyless");
  EXPECT_EQ(result.signer, "release@example.com");
  EXPECT_EQ(result.issuer, "https://issuer.example.com");
  std::ifstream arguments(files.arguments);
  const std::string recorded{std::istreambuf_iterator<char>(arguments), {}};
  EXPECT_NE(recorded.find("--certificate-identity\nrelease@example.com"),
            std::string::npos);
  EXPECT_NE(
      recorded.find("--certificate-oidc-issuer\nhttps://issuer.example.com"),
      std::string::npos);
}

TEST(CosignVerifier, RejectsUnsignedArtifactWithBoundedOutput) {
  TrustFiles files;
  {
    std::ofstream executable(files.executable);
    executable << "#!/bin/sh\nprintf 'no matching signatures' >&2\nexit 1\n";
  }
  files.make_executable();
  vektor::TrustPolicy policy;
  policy.mode = vektor::TrustMode::Keyless;
  policy.cosign_executable = "./" + files.executable.string();
  policy.certificate_identity = "release@example.com";
  policy.certificate_oidc_issuer = "https://issuer.example.com";
  vektor::CosignArtifactVerifier verifier(policy);

  try {
    verifier.verify(kArtifact, std::chrono::seconds(1));
    FAIL() << "verification unexpectedly succeeded";
  } catch (const std::runtime_error &error) {
    EXPECT_NE(std::string(error.what()).find("no matching signatures"),
              std::string::npos);
  }
}

TEST(CosignVerifier, TerminatesVerificationAtDeadline) {
  using namespace std::chrono_literals;
  TrustFiles files;
  {
    std::ofstream executable(files.executable);
    executable << "#!/bin/sh\nsleep 5\n";
  }
  files.make_executable();
  vektor::TrustPolicy policy;
  policy.cosign_executable = "./" + files.executable.string();
  policy.key = files.key.string();
  vektor::CosignArtifactVerifier verifier(policy);

  const auto started = std::chrono::steady_clock::now();
  EXPECT_THROW(verifier.verify(kArtifact, 40ms), std::runtime_error);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
}
