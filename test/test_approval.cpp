#include "vektor/approval.hpp"

#include <gtest/gtest.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace {
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using Digest = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using Bio = std::unique_ptr<BIO, decltype(&BIO_free)>;

Key generate_key() {
  KeyContext context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                     EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) != 1 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) != 1)
    throw std::runtime_error("failed to initialize test key generation");
  EVP_PKEY *raw = nullptr;
  if (EVP_PKEY_keygen(context.get(), &raw) != 1)
    throw std::runtime_error("failed to generate test key");
  return Key(raw, EVP_PKEY_free);
}

void write_public_key(const std::filesystem::path &path, EVP_PKEY *key) {
  Bio output(BIO_new_file(path.c_str(), "w"), BIO_free);
  if (!output || PEM_write_bio_PUBKEY(output.get(), key) != 1)
    throw std::runtime_error("failed to write test public key");
}

std::string sign(EVP_PKEY *key, const std::string &payload) {
  Digest digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
  if (!digest || EVP_DigestSignInit(digest.get(), nullptr, EVP_sha256(),
                                    nullptr, key) != 1)
    throw std::runtime_error("failed to initialize test signing");
  std::size_t size = 0;
  if (EVP_DigestSign(digest.get(), nullptr, &size,
                     reinterpret_cast<const unsigned char *>(payload.data()),
                     payload.size()) != 1)
    throw std::runtime_error("failed to size test signature");
  std::vector<unsigned char> signature(size);
  if (EVP_DigestSign(digest.get(), signature.data(), &size,
                     reinterpret_cast<const unsigned char *>(payload.data()),
                     payload.size()) != 1)
    throw std::runtime_error("failed to sign test approval");
  signature.resize(size);
  std::string encoded(4 * ((signature.size() + 2) / 3), '\0');
  EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),
                  signature.data(), static_cast<int>(signature.size()));
  return encoded;
}

struct ApprovalFiles {
  std::filesystem::path policy{"vektor_test_approval_policy.yaml"};
  std::filesystem::path alice{"vektor_test_approval_alice.pem"};
  std::filesystem::path bob{"vektor_test_approval_bob.pem"};
  ApprovalFiles() { cleanup(); }
  ~ApprovalFiles() { cleanup(); }
  void cleanup() const {
    std::filesystem::remove(policy);
    std::filesystem::remove(alice);
    std::filesystem::remove(bob);
  }
};
} // namespace

TEST(ApprovalPolicy, VerifiesDistinctBoundAndUnexpiredSignatures) {
  ApprovalFiles files;
  auto alice = generate_key();
  auto bob = generate_key();
  write_public_key(files.alice, alice.get());
  write_public_key(files.bob, bob.get());
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nsensitive_environments: [production]\n"
              "max_wave_size_without_approval: 10\nrequired_approvals: 2\n"
              "max_approval_lifetime_ms: 7200000\napprovers:\n"
              "  - identity: alice@example.com\n    public_key: "
           << files.alice.string()
           << "\n  - identity: bob@example.com\n    public_key: "
           << files.bob.string() << '\n';
  }
  const auto policy = vektor::load_approval_policy(files.policy);
  const vektor::ApprovalContext context{
      "release-42",
      "ghcr.io/vektor/"
      "demo@sha256:"
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "warehouse-prod",
      "picker",
      "production",
      "stable"};
  vektor::ApprovalRecord alice_record{context,
                                      "alice@example.com",
                                      "2026-08-15T10:00:00Z",
                                      "2026-08-15T12:00:00Z",
                                      {}};
  vektor::ApprovalRecord bob_record{context,
                                    "bob@example.com",
                                    "2026-08-15T10:00:00Z",
                                    "2026-08-15T12:00:00Z",
                                    {}};
  alice_record.signature =
      sign(alice.get(), vektor::approval_signing_payload(alice_record));
  bob_record.signature =
      sign(bob.get(), vektor::approval_signing_payload(bob_record));
  const auto now = std::chrono::system_clock::from_time_t(1786789800);

  const auto verified = vektor::verify_approval_records(
      policy, {alice_record, bob_record}, context, now);
  EXPECT_EQ(verified,
            (std::vector<std::string>{"alice@example.com", "bob@example.com"}));

  auto tampered = bob_record;
  tampered.context.wave = "canary";
  EXPECT_THROW(vektor::verify_approval_records(policy, {alice_record, tampered},
                                               context, now),
               std::runtime_error);
  tampered = bob_record;
  tampered.signature[0] = tampered.signature[0] == 'A' ? 'B' : 'A';
  EXPECT_THROW(vektor::verify_approval_records(policy, {alice_record, tampered},
                                               context, now),
               std::runtime_error);
  auto expired = bob_record;
  expired.issued_at = "2026-08-15T08:00:00Z";
  expired.expires_at = "2026-08-15T09:00:00Z";
  expired.signature =
      sign(bob.get(), vektor::approval_signing_payload(expired));
  EXPECT_THROW(vektor::verify_approval_records(policy, {alice_record, expired},
                                               context, now),
               std::runtime_error);
  EXPECT_THROW(vektor::verify_approval_records(
                   policy, {alice_record, alice_record}, context, now),
               std::runtime_error);
}

TEST(ApprovalPolicy, GatesSensitiveEnvironmentsAndOversizedWaves) {
  vektor::ApprovalPolicy policy;
  policy.sensitive_environments = {"production"};
  policy.max_wave_size_without_approval = 5;
  EXPECT_TRUE(vektor::approval_required(policy, "production", 1));
  EXPECT_TRUE(vektor::approval_required(policy, "development", 6));
  EXPECT_FALSE(vektor::approval_required(policy, "development", 5));
}

TEST(ApprovalPolicy, RejectsInvalidThresholdAndUnknownFields) {
  ApprovalFiles files;
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nsensitive_environments: []\n"
              "max_wave_size_without_approval: 10\nrequired_approvals: 2\n"
              "approvers:\n  - identity: alice\n    public_key: alice.pem\n";
  }
  EXPECT_THROW(vektor::load_approval_policy(files.policy), std::runtime_error);
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nsensitive_environments: []\n"
              "max_wave_size_without_approval: 10\nrequired_approvals: 2\n"
              "approvers:\n  - identity: alice\n    public_key: shared.pem\n"
              "  - identity: bob\n    public_key: shared.pem\n";
  }
  EXPECT_THROW(vektor::load_approval_policy(files.policy), std::runtime_error);
  {
    std::ofstream policy(files.policy);
    policy << "schema_version: 1\nsensitive_environments: []\n"
              "max_wave_size_without_approval: 10\nrequired_approvals: 1\n"
              "approvers:\n  - identity: alice\n    public_key: alice.pem\n"
              "allow_unsigned: true\n";
  }
  EXPECT_THROW(vektor::load_approval_policy(files.policy), std::runtime_error);
}
