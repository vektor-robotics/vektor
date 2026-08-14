#include "vektor/trust.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <string>

namespace {
std::string required_environment(const char *name) {
  const auto *value = std::getenv(name);
  if (value == nullptr || *value == '\0')
    throw std::runtime_error(std::string("missing integration environment: ") +
                             name);
  return value;
}

vektor::CosignArtifactVerifier verifier_for(const std::string &key) {
  vektor::TrustPolicy policy;
  policy.mode = vektor::TrustMode::PublicKey;
  policy.cosign_executable = required_environment("VEKTOR_COSIGN_BIN");
  policy.key = key;
  policy.allow_http_registry = true;
  policy.ignore_transparency_log = true;
  policy.timeout = std::chrono::seconds(30);
  return vektor::CosignArtifactVerifier(std::move(policy));
}
} // namespace

TEST(RealCosignTrust, AcceptsSignedDigestWithTrustedKey) {
  auto verifier =
      verifier_for(required_environment("VEKTOR_TRUSTED_PUBLIC_KEY"));
  const auto result = verifier.verify(
      required_environment("VEKTOR_SIGNED_ARTIFACT"), std::chrono::seconds(30));

  EXPECT_TRUE(result.verified);
  EXPECT_EQ(result.method, "cosign_public_key");
  EXPECT_FALSE(result.signer.empty());
  EXPECT_FALSE(result.verified_at.empty());
}

TEST(RealCosignTrust, RejectsUnsignedDigest) {
  auto verifier =
      verifier_for(required_environment("VEKTOR_TRUSTED_PUBLIC_KEY"));
  EXPECT_THROW(verifier.verify(required_environment("VEKTOR_UNSIGNED_ARTIFACT"),
                               std::chrono::seconds(30)),
               std::runtime_error);
}

TEST(RealCosignTrust, RejectsSignatureFromUntrustedKey) {
  auto verifier =
      verifier_for(required_environment("VEKTOR_UNTRUSTED_PUBLIC_KEY"));
  EXPECT_THROW(verifier.verify(required_environment("VEKTOR_SIGNED_ARTIFACT"),
                               std::chrono::seconds(30)),
               std::runtime_error);
}

TEST(RealCosignTrust, RejectsTagMovedAfterSigning) {
  auto verifier =
      verifier_for(required_environment("VEKTOR_TRUSTED_PUBLIC_KEY"));
  EXPECT_THROW(verifier.verify(required_environment("VEKTOR_TAMPERED_ARTIFACT"),
                               std::chrono::seconds(30)),
               std::runtime_error);
}
